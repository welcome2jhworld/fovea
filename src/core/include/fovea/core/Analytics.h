#pragma once
#include "fovea/rules/Types.h"
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdint>
#include <optional>

namespace fovea::core {

inline constexpr int64_t kNsPerSecond = 1'000'000'000;
inline constexpr int64_t kEvidenceRetentionMs = 30LL * 86'400'000;

struct ZoneRecord {
  QString id;
  QString cameraId;
  QString name;
  int revision = 0;
  QVector<QPointF> points;
  QString anchor = QStringLiteral("foot");
  int refWidth = 0;
  int refHeight = 0;
  int64_t createdUtcMs = 0;
  int64_t revisionUtcMs = 0;
  int64_t deletedUtcMs = 0;

  QJsonObject toJson() const;
  rules::ZoneRevision toRevision() const;
};

struct RuleRecord {
  QString id;
  QString name;
  int revision = 0;
  bool enabled = true;
  QString cameraId;
  QString zoneId;
  int zoneRevision = 0;
  QVector<rules::TimeWindow> schedule;
  QString timeZone = QStringLiteral("UTC");
  QString targetClass = QStringLiteral("person");
  double minConfidence = 0.3;
  int64_t dwellNs = 10 * kNsPerSecond;
  int64_t maxObservationGapNs = 1'500'000'000;
  int64_t clearAfterNs = 5 * kNsPerSecond;
  int64_t rearmNs = 30 * kNsPerSecond;
  int64_t resultTtlNs = 5 * kNsPerSecond;
  int64_t evidencePreNs = 10 * kNsPerSecond;
  int64_t evidencePostNs = 10 * kNsPerSecond;
  QString vlmRole = QStringLiteral("none");
  QString severity = QStringLiteral("critical");
  bool soundAction = true;
  bool popAction = false;
  int64_t createdUtcMs = 0;
  int64_t revisionUtcMs = 0;

  // The stored rule_revisions.json: every revision field, no id or name.
  QJsonObject revisionJson() const;
  static RuleRecord fromRevisionJson(const QJsonObject& o);
  QJsonObject toJson() const;
  rules::RuleRevision toEvaluatorRevision(const ZoneRecord& zone) const;
};

// Applies the fields present in body over base and validates the result.
// Returns the error message, empty when valid. Camera and zone existence are
// checked by the caller, which has the store.
QString applyRuleBody(RuleRecord& rule, const QJsonObject& body);
QString applyZoneBody(ZoneRecord& zone, const QJsonObject& body);

struct EventRecord {
  QString id;
  QString ruleId;
  int ruleRevision = 0;
  QString cameraId;
  QString sessionId;
  QString severity;
  QString condition = QStringLiteral("active");
  QString operatorState = QStringLiteral("new");
  int64_t openedUtcMs = 0;
  int64_t openedPtsNs = 0;
  int64_t triggerPtsNs = 0;
  int64_t clearedUtcMs = 0;
  QString title;
  QString detail;
  bool late = false;

  QJsonObject toJson() const;
};

struct EventReview {
  QString id;
  QString eventId;
  QString label;
  QString note;
  QString operatorName;
  int64_t utcMs = 0;

  QJsonObject toJson() const;
};

struct AlertDelivery {
  QString id;
  QString eventId;
  QString channel;
  QString state = QStringLiteral("pending");
  int attempts = 0;
  QString lastError;
  int64_t createdUtcMs = 0;
  int64_t deliveredUtcMs = 0;

  QJsonObject toJson() const;
};

struct EvidenceRef {
  QString id;
  QString eventId;
  QString cameraId;
  int64_t fromUtcMs = 0;
  int64_t toUtcMs = 0;
  QString state = QStringLiteral("pending");
  QString reason;
  QStringList segmentIds;
  QString thumbnailPath;
  int64_t updatedUtcMs = 0;

  QJsonObject toJson() const;
};

struct EvaluationRecord {
  int64_t id = 0;
  QString ruleId;
  int ruleRevision = 0;
  QString cameraId;
  QString sessionId;
  uint64_t generation = 0;
  int64_t windowStartPtsNs = 0;
  int64_t windowEndPtsNs = 0;
  int64_t utcMs = 0;
  QString before;
  QString after;
  QString quality;
  QString transition;
  QString eventId;
  QStringList trackIds;
  int64_t dwellNs = 0;
  QString note;

  static EvaluationRecord from(const rules::Evaluation& e);
  QJsonObject toJson() const;
};

struct CoverageRecord {
  QString cameraId;
  QString sessionId;
  int64_t fromUtcMs = 0;
  int64_t toUtcMs = 0;
  int framesSent = 0;
  int framesKnown = 0;
  int framesUnknown = 0;
  double detectFpsTarget = 0;
};

// Detector job options derived from a camera's enabled rules: threshold is the
// lowest min_confidence within [kMinDetectThreshold, kDetectThreshold], maxGapNs
// the longest max_observation_gap_ns. Both 0 when no enabled rule names the camera.
inline constexpr double kDetectThreshold = 0.3;
inline constexpr double kMinDetectThreshold = 0.1;
struct DetectHints {
  double threshold = 0;
  int64_t maxGapNs = 0;
};

// A refused API operation: HTTP status, error code and message.
struct ServiceError {
  int status = 400;
  QString code;
  QString message;
};

struct EventCounts {
  int unresolved = 0;
  int acknowledged = 0;
  int dismissed = 0;
};

struct EventQuery {
  QString state;
  QString cameraId;
  int64_t fromUtcMs = 0;
  int64_t toUtcMs = 0;
  int limit = 100;
};

}
