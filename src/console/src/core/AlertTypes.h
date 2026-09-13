#pragma once
#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdint>
#include <optional>

// Console views of the M3 API payloads (docs/M3_DESIGN.md "API additions").
// Field names follow the storage columns in snake_case.
namespace fovea::ui {

struct ZoneInfo {
  QString id;
  QString cameraId;
  QString name;
  int revision = 0;
  QVector<QPointF> points;
  QString anchor = QStringLiteral("foot");
  int refWidth = 0;
  int refHeight = 0;

  static ZoneInfo fromJson(const QJsonObject& o);
};

// Operating window in the rule's time zone; days bit 0 = Monday, end <= start wraps past midnight.
struct ScheduleWindow {
  int days = 0x7f;
  int startMinute = 0;
  int endMinute = 24 * 60;
};

struct RuleInfo {
  static constexpr int64_t kNsPerSecond = 1'000'000'000;

  QString id;
  QString name;
  int revision = 0;
  bool enabled = true;
  QString cameraId;
  QString zoneId;
  int zoneRevision = 0;
  QVector<ScheduleWindow> schedule{ScheduleWindow{}};
  QString timeZone;
  QString targetClass = QStringLiteral("person");
  double minConfidence = 0.5;
  int64_t dwellNs = 10 * kNsPerSecond;
  QString severity = QStringLiteral("critical");
  bool soundAction = true;
  bool popAction = false;
  // Revision fields the console does not edit (gap, clear, rearm, TTL, evidence window, vlm_role);
  // sent back unchanged on update.
  QJsonObject preserved;

  static RuleInfo fromJson(const QJsonObject& o);
  QJsonObject revisionJson() const;
};

struct EvidenceInfo {
  QString id;
  QString state;
  QString reason;
  int64_t fromUtcMs = 0;
  int64_t toUtcMs = 0;
};

struct ReviewInfo {
  QString label;
  QString note;
  QString operatorName;
  int64_t utcMs = 0;
};

struct DeliveryInfo {
  QString id;
  QString eventId;
  QString channel;
  QString state;
  QString lastError;
  int attempts = 0;
  int64_t createdUtcMs = 0;
  int64_t deliveredUtcMs = 0;

  static DeliveryInfo fromJson(const QJsonObject& o);
};

struct EvaluationInfo {
  int64_t utcMs = 0;
  int ruleRevision = 0;
  QString transition;
  QString before;
  QString after;
  QString quality;
  QString note;
  int64_t dwellNs = 0;
  QStringList trackIds;
};

struct EventInfo {
  QString id;
  QString ruleId;
  int ruleRevision = 0;
  QString cameraId;
  QString sessionId;
  QString severity;
  QString condition;
  QString operatorState;
  QString title;
  QString detail;
  int64_t openedUtcMs = 0;
  int64_t clearedUtcMs = 0;
  bool late = false;
  std::optional<ReviewInfo> review;
  std::optional<EvidenceInfo> evidence;
  QVector<DeliveryInfo> deliveries;
  // Only GET /v1/events/{id} carries the evaluation timeline.
  QVector<EvaluationInfo> evaluations;

  static EventInfo fromJson(const QJsonObject& o);
};

struct Detection {
  QString trackId;
  QString cls;
  double confidence = 0;
  // Normalized to the detection frame (x1, y1, x2, y2 in 0..1).
  QRectF box;
};

struct DetectionFrame {
  QString cameraId;
  QString sessionId;
  QString frameId;
  int64_t ptsNs = 0;
  int64_t utcMs = 0;
  int width = 0;
  int height = 0;
  QVector<Detection> detections;

  static DetectionFrame fromJson(const QJsonObject& o);
};

}
