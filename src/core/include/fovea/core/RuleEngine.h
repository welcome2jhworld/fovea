#pragma once
#include "fovea/Api.h"
#include "fovea/core/Analytics.h"
#include "fovea/rules/RuleEvaluator.h"
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace fovea::core {

class EventService;
class Store;
struct AnalysisResult;

// Zones, rules and one rules::RuleEvaluator per rule (a rule names one
// camera). Every zone and rule change is stored as a new revision and applied
// with setRule, so an open event survives edits; deleting a rule clears its
// open event (note rule_deleted), deleting a zone disables the rules on it,
// deleting a camera deletes its rules (note camera_deleted) and zones.
// Analysis results and a 1 Hz tick feed the evaluators; each evaluator first
// learns the camera's current generation, so results from an older one
// (session or rule-set change) are reported stale and their span is frozen.
// A triggered evaluation is stored only after its event row committed; when
// the event cannot be stored the evaluator forgets it and triggers again on
// the next frame. At start every rule's rearm counts from its latest cleared
// event. Transitions are stored in rule_evaluations: lifecycle transitions
// always, repeated per-frame reports (unknown, suppressed, stale,
// out_of_schedule, zone_mismatch, disabled) once until the report changes,
// none and still_active never.
class RuleEngine : public QObject {
  Q_OBJECT
public:
  using CameraLookup = std::function<std::optional<Camera>(const QString& cameraId)>;
  using Generation = std::function<uint64_t(const QString& cameraId)>;
  using BumpGeneration = std::function<void(const QString& cameraId)>;

  RuleEngine(Store& store, EventService& events, CameraLookup cameras, QObject* parent = nullptr);
  void setGenerations(Generation current, BumpGeneration bump);
  void start();

  std::optional<ZoneRecord> createZone(const QJsonObject& body, ServiceError* error);
  std::optional<ZoneRecord> updateZone(const QString& id, const QJsonObject& body, ServiceError* error);
  bool deleteZone(const QString& id, ServiceError* error);
  std::optional<RuleRecord> createRule(const QJsonObject& body, ServiceError* error);
  std::optional<RuleRecord> updateRule(const QString& id, const QJsonObject& body, ServiceError* error);
  bool deleteRule(const QString& id, ServiceError* error);
  void onCameraDeleted(const QString& cameraId);
  DetectHints detectHints(const QString& cameraId) const;
  QJsonObject ruleJson(const RuleRecord& rule) const;

  void onAnalysis(const AnalysisResult& result);
  void tick(int64_t nowMonoNs, int64_t nowUtcMs);

private:
  struct Entry {
    RuleRecord rule;
    ZoneRecord zone;
    std::unique_ptr<rules::RuleEvaluator> evaluator;
    QString lastRepeatKey;
  };

  std::optional<RuleRecord> storeRevision(RuleRecord rule, const ZoneRecord& zone, bool created, ServiceError* error);
  std::optional<ZoneRecord> zoneForRule(const RuleRecord& rule, ServiceError* error);
  void apply(const RuleRecord& rule, const ZoneRecord& zone);
  void removeRule(std::map<QString, Entry>::iterator it, const QString& note, const QString& actor, int64_t nowUtcMs);
  // result is the analysed frame behind e, null for ticks and rule changes.
  void handle(Entry& entry, const rules::Evaluation& e, const AnalysisResult* result);
  void persist(Entry& entry, const rules::Evaluation& e);

  Store& store_;
  EventService& events_;
  CameraLookup cameras_;
  Generation generation_;
  BumpGeneration bumpGeneration_;
  QTimer tickTimer_;
  std::map<QString, Entry> entries_;
};

}
