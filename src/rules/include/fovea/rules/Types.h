#pragma once
#include <QPointF>
#include <QString>
#include <QVector>
#include <cstdint>
#include <optional>

namespace fovea::rules {

enum class ConditionState { Inactive, Pending, Active, Clearing };
enum class Quality { Known, Unknown };
enum class VlmRole { None, Describe, Verify };
enum class Anchor { Foot, Center };

QString toString(ConditionState s);
QString toString(Quality q);

// Operating window in the rule's time zone. Minutes since midnight; a window
// with end <= start wraps past midnight. days is a 7-bit mask, bit 0 = Monday.
struct TimeWindow {
  int startMinute = 0;
  int endMinute = 24 * 60;
  int days = 0x7f;
};

// Polygon in normalized image coordinates (0..1) of a reference frame size.
// A different reference size means the field of view changed and the zone
// must be re-validated; the evaluator refuses frames whose size differs.
struct ZoneRevision {
  QString zoneId;
  int revision = 1;
  QVector<QPointF> points;
  Anchor anchor = Anchor::Foot;
  int refWidth = 0;
  int refHeight = 0;
  bool contains(const QPointF& normalized) const;
};

struct RuleRevision {
  QString ruleId;
  int revision = 1;
  QString cameraId;
  QString name;
  bool enabled = true;
  ZoneRevision zone;
  QVector<TimeWindow> schedule;
  QString timeZoneId = QStringLiteral("UTC");
  QString targetClass = QStringLiteral("person");
  double minConfidence = 0.3;
  int64_t dwellNs = 10'000'000'000;
  int64_t maxObservationGapNs = 1'500'000'000;
  int64_t clearAfterNs = 5'000'000'000;
  int64_t rearmNs = 30'000'000'000;
  int64_t resultTtlNs = 5'000'000'000;
  int64_t evidencePreNs = 10'000'000'000;
  int64_t evidencePostNs = 10'000'000'000;
  VlmRole vlmRole = VlmRole::None;
};

struct TrackObservation {
  QString trackId;
  QString cls;
  QPointF anchor;
  double confidence = 0;
};

// One evaluated observation of a camera at a media time. quality Unknown means
// detection or tracking did not run or failed for this frame; tracks are then
// ignored and timers freeze. frameWidth/frameHeight are the size of the
// analysed frame; utcMs 0 means the capture time is unknown.
struct ObservationFrame {
  QString cameraId;
  QString sessionId;
  int64_t ptsNs = 0;
  int64_t utcMs = 0;
  int64_t recvMonoNs = 0;
  uint64_t generation = 0;
  Quality quality = Quality::Known;
  int frameWidth = 0;
  int frameHeight = 0;
  QVector<TrackObservation> tracks;
};

enum class Transition {
  None,
  BecamePending,
  Triggered,
  StillActive,
  BecameClearing,
  Cleared,
  PendingReset,
  Unknown,
  Suppressed,
  Stale,
  OutOfSchedule,
  ZoneMismatch,
  Disabled
};

QString toString(Transition t);

struct Evaluation {
  QString ruleId;
  int ruleRevision = 0;
  QString cameraId;
  QString sessionId;
  int64_t windowStartPtsNs = 0;
  int64_t windowEndPtsNs = 0;
  int64_t utcMs = 0;
  uint64_t generation = 0;
  ConditionState before = ConditionState::Inactive;
  ConditionState after = ConditionState::Inactive;
  Quality quality = Quality::Known;
  Transition transition = Transition::None;
  QString eventId;
  QVector<QString> trackIds;
  int64_t dwellNs = 0;
  int64_t unknownNs = 0;
  QString note;
};

}
