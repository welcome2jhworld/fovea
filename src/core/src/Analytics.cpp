#include "fovea/core/Analytics.h"
#include <QJsonArray>
#include <QTimeZone>
#include <cmath>

namespace fovea::core {
namespace {

constexpr int kMinZonePoints = 3;
constexpr int kMaxZonePoints = 32;
constexpr int kMaxRefSize = 16384;
constexpr int kMaxScheduleWindows = 16;
constexpr int kMinutesPerDay = 24 * 60;

const QStringList kTargetClasses{QStringLiteral("person"),     QStringLiteral("car"),      QStringLiteral("truck"),
                                 QStringLiteral("bus"),        QStringLiteral("motorcycle"), QStringLiteral("bicycle")};
const QStringList kSeverities{QStringLiteral("critical"), QStringLiteral("review"), QStringLiteral("info")};

double num(int64_t v) { return static_cast<double>(v); }

QJsonArray pointsJson(const QVector<QPointF>& points) {
  QJsonArray out;
  for (const QPointF& p : points) out.push_back(QJsonArray{p.x(), p.y()});
  return out;
}

QJsonArray scheduleJson(const QVector<rules::TimeWindow>& schedule) {
  QJsonArray out;
  for (const rules::TimeWindow& w : schedule)
    out.push_back(QJsonObject{{"days", w.days}, {"start_minute", w.startMinute}, {"end_minute", w.endMinute}});
  return out;
}

QString typeError(const QString& key, const char* expected) { return QStringLiteral("%1 must be %2").arg(key, QLatin1String(expected)); }

// Reads an optional field; the error is set when the field exists with the wrong type.
bool readString(const QJsonObject& o, const QString& key, QString& out, QString& error) {
  if (!o.contains(key)) return false;
  if (!o.value(key).isString()) {
    error = typeError(key, "a string");
    return false;
  }
  out = o.value(key).toString();
  return true;
}

bool readBool(const QJsonObject& o, const QString& key, bool& out, QString& error) {
  if (!o.contains(key)) return false;
  if (!o.value(key).isBool()) {
    error = typeError(key, "a boolean");
    return false;
  }
  out = o.value(key).toBool();
  return true;
}

bool readNumber(const QJsonObject& o, const QString& key, double& out, QString& error) {
  if (!o.contains(key)) return false;
  const QJsonValue v = o.value(key);
  if (!v.isDouble() || !std::isfinite(v.toDouble())) {
    error = typeError(key, "a number");
    return false;
  }
  out = v.toDouble();
  return true;
}

bool readInt(const QJsonObject& o, const QString& key, int& out, QString& error) {
  double v = 0;
  if (!readNumber(o, key, v, error)) return false;
  if (std::floor(v) != v || std::fabs(v) > 1e9) {
    error = typeError(key, "an integer");
    return false;
  }
  out = static_cast<int>(v);
  return true;
}

void readNs(const QJsonObject& o, const QString& key, int64_t& out, QString& error) {
  double v = 0;
  if (readNumber(o, key, v, error)) out = static_cast<int64_t>(std::llround(v));
}

QString checkRange(const QString& key, int64_t valueNs, double minSeconds, double maxSeconds) {
  const double s = static_cast<double>(valueNs) / 1e9;
  if (s < minSeconds || s > maxSeconds)
    return QStringLiteral("%1 must be between %2 and %3 s").arg(key).arg(minSeconds).arg(maxSeconds);
  return {};
}

QString parsePoints(const QJsonValue& value, QVector<QPointF>& out) {
  if (!value.isArray()) return QStringLiteral("points must be an array of [x, y] pairs");
  const QJsonArray arr = value.toArray();
  if (arr.size() < kMinZonePoints || arr.size() > kMaxZonePoints)
    return QStringLiteral("points must have between %1 and %2 vertices").arg(kMinZonePoints).arg(kMaxZonePoints);
  QVector<QPointF> points;
  for (const QJsonValue& p : arr) {
    const QJsonArray xy = p.toArray();
    if (!p.isArray() || xy.size() != 2 || !xy.at(0).isDouble() || !xy.at(1).isDouble())
      return QStringLiteral("points must be an array of [x, y] pairs");
    const double x = xy.at(0).toDouble();
    const double y = xy.at(1).toDouble();
    if (!(x >= 0.0 && x <= 1.0 && y >= 0.0 && y <= 1.0)) return QStringLiteral("points must be normalized to [0, 1]");
    points.push_back(QPointF(x, y));
  }
  out = points;
  return {};
}

QString parseSchedule(const QJsonValue& value, QVector<rules::TimeWindow>& out) {
  if (!value.isArray()) return QStringLiteral("schedule must be an array");
  const QJsonArray arr = value.toArray();
  if (arr.size() > kMaxScheduleWindows) return QStringLiteral("schedule has more than %1 windows").arg(kMaxScheduleWindows);
  QVector<rules::TimeWindow> windows;
  for (const QJsonValue& v : arr) {
    if (!v.isObject()) return QStringLiteral("schedule entries must be objects");
    const QJsonObject w = v.toObject();
    rules::TimeWindow tw;
    QString error;
    readInt(w, QStringLiteral("days"), tw.days, error);
    readInt(w, QStringLiteral("start_minute"), tw.startMinute, error);
    readInt(w, QStringLiteral("end_minute"), tw.endMinute, error);
    if (!error.isEmpty()) return QStringLiteral("schedule: ") + error;
    if (tw.days < 1 || tw.days > 0x7f) return QStringLiteral("schedule days must be a mask between 1 and 127 (bit 0 = Monday)");
    if (tw.startMinute < 0 || tw.startMinute >= kMinutesPerDay) return QStringLiteral("schedule start_minute must be 0..1439");
    if (tw.endMinute < 1 || tw.endMinute > kMinutesPerDay) return QStringLiteral("schedule end_minute must be 1..1440");
    windows.push_back(tw);
  }
  out = windows;
  return {};
}

}

QJsonObject ZoneRecord::toJson() const {
  return {{"id", id},
          {"camera_id", cameraId},
          {"name", name},
          {"revision", revision},
          {"points", pointsJson(points)},
          {"anchor", anchor},
          {"ref_width", refWidth},
          {"ref_height", refHeight},
          {"created_utc_ms", num(createdUtcMs)},
          {"revision_utc_ms", num(revisionUtcMs)}};
}

rules::ZoneRevision ZoneRecord::toRevision() const {
  rules::ZoneRevision z;
  z.zoneId = id;
  z.revision = revision;
  z.points = points;
  z.anchor = anchor == QLatin1String("center") ? rules::Anchor::Center : rules::Anchor::Foot;
  z.refWidth = refWidth;
  z.refHeight = refHeight;
  return z;
}

QString applyZoneBody(ZoneRecord& zone, const QJsonObject& body) {
  QString error;
  QString name = zone.name;
  readString(body, QStringLiteral("name"), name, error);
  if (body.contains(QStringLiteral("points"))) {
    const QString pointsError = parsePoints(body.value(QStringLiteral("points")), zone.points);
    if (!pointsError.isEmpty()) return pointsError;
  }
  readString(body, QStringLiteral("anchor"), zone.anchor, error);
  readInt(body, QStringLiteral("ref_width"), zone.refWidth, error);
  readInt(body, QStringLiteral("ref_height"), zone.refHeight, error);
  if (!error.isEmpty()) return error;
  zone.name = name.trimmed();
  if (zone.name.isEmpty()) return QStringLiteral("name is required");
  if (zone.points.size() < kMinZonePoints) return QStringLiteral("points must have between %1 and %2 vertices").arg(kMinZonePoints).arg(kMaxZonePoints);
  if (zone.anchor != QLatin1String("foot") && zone.anchor != QLatin1String("center")) return QStringLiteral("anchor must be foot or center");
  if (zone.refWidth < 0 || zone.refHeight < 0 || zone.refWidth > kMaxRefSize || zone.refHeight > kMaxRefSize)
    return QStringLiteral("ref_width and ref_height must be between 0 and %1").arg(kMaxRefSize);
  if ((zone.refWidth == 0) != (zone.refHeight == 0)) return QStringLiteral("ref_width and ref_height must both be set or both be 0");
  return {};
}

QJsonObject RuleRecord::revisionJson() const {
  return {{"camera_id", cameraId},
          {"zone_id", zoneId},
          {"zone_revision", zoneRevision},
          {"enabled", enabled},
          {"schedule", scheduleJson(schedule)},
          {"time_zone", timeZone},
          {"target_class", targetClass},
          {"min_confidence", minConfidence},
          {"dwell_ns", num(dwellNs)},
          {"max_observation_gap_ns", num(maxObservationGapNs)},
          {"clear_after_ns", num(clearAfterNs)},
          {"rearm_ns", num(rearmNs)},
          {"result_ttl_ns", num(resultTtlNs)},
          {"evidence_pre_ns", num(evidencePreNs)},
          {"evidence_post_ns", num(evidencePostNs)},
          {"vlm_role", vlmRole},
          {"severity", severity},
          {"actions", QJsonObject{{"sound", soundAction}, {"pop_to_main_view", popAction}}}};
}

RuleRecord RuleRecord::fromRevisionJson(const QJsonObject& o) {
  RuleRecord r;
  QString ignored;
  readString(o, QStringLiteral("camera_id"), r.cameraId, ignored);
  readString(o, QStringLiteral("zone_id"), r.zoneId, ignored);
  readInt(o, QStringLiteral("zone_revision"), r.zoneRevision, ignored);
  readBool(o, QStringLiteral("enabled"), r.enabled, ignored);
  if (o.contains(QStringLiteral("schedule"))) parseSchedule(o.value(QStringLiteral("schedule")), r.schedule);
  readString(o, QStringLiteral("time_zone"), r.timeZone, ignored);
  readString(o, QStringLiteral("target_class"), r.targetClass, ignored);
  readNumber(o, QStringLiteral("min_confidence"), r.minConfidence, ignored);
  readNs(o, QStringLiteral("dwell_ns"), r.dwellNs, ignored);
  readNs(o, QStringLiteral("max_observation_gap_ns"), r.maxObservationGapNs, ignored);
  readNs(o, QStringLiteral("clear_after_ns"), r.clearAfterNs, ignored);
  readNs(o, QStringLiteral("rearm_ns"), r.rearmNs, ignored);
  readNs(o, QStringLiteral("result_ttl_ns"), r.resultTtlNs, ignored);
  readNs(o, QStringLiteral("evidence_pre_ns"), r.evidencePreNs, ignored);
  readNs(o, QStringLiteral("evidence_post_ns"), r.evidencePostNs, ignored);
  readString(o, QStringLiteral("vlm_role"), r.vlmRole, ignored);
  readString(o, QStringLiteral("severity"), r.severity, ignored);
  const QJsonObject actions = o.value(QStringLiteral("actions")).toObject();
  readBool(actions, QStringLiteral("sound"), r.soundAction, ignored);
  readBool(actions, QStringLiteral("pop_to_main_view"), r.popAction, ignored);
  return r;
}

QJsonObject RuleRecord::toJson() const {
  QJsonObject o = revisionJson();
  o.insert("id", id);
  o.insert("name", name);
  o.insert("revision", revision);
  o.insert("created_utc_ms", num(createdUtcMs));
  o.insert("revision_utc_ms", num(revisionUtcMs));
  return o;
}

rules::RuleRevision RuleRecord::toEvaluatorRevision(const ZoneRecord& zone) const {
  rules::RuleRevision r;
  r.ruleId = id;
  r.revision = revision;
  r.cameraId = cameraId;
  r.name = name;
  r.enabled = enabled;
  r.zone = zone.toRevision();
  r.schedule = schedule;
  r.timeZoneId = timeZone;
  r.targetClass = targetClass;
  r.minConfidence = minConfidence;
  r.dwellNs = dwellNs;
  r.maxObservationGapNs = maxObservationGapNs;
  r.clearAfterNs = clearAfterNs;
  r.rearmNs = rearmNs;
  r.resultTtlNs = resultTtlNs;
  r.evidencePreNs = evidencePreNs;
  r.evidencePostNs = evidencePostNs;
  r.vlmRole = rules::VlmRole::None;
  return r;
}

QString applyRuleBody(RuleRecord& rule, const QJsonObject& body) {
  QString error;
  readString(body, QStringLiteral("name"), rule.name, error);
  readBool(body, QStringLiteral("enabled"), rule.enabled, error);
  readString(body, QStringLiteral("camera_id"), rule.cameraId, error);
  readString(body, QStringLiteral("zone_id"), rule.zoneId, error);
  readString(body, QStringLiteral("time_zone"), rule.timeZone, error);
  readString(body, QStringLiteral("target_class"), rule.targetClass, error);
  readNumber(body, QStringLiteral("min_confidence"), rule.minConfidence, error);
  readNs(body, QStringLiteral("dwell_ns"), rule.dwellNs, error);
  readNs(body, QStringLiteral("max_observation_gap_ns"), rule.maxObservationGapNs, error);
  readNs(body, QStringLiteral("clear_after_ns"), rule.clearAfterNs, error);
  readNs(body, QStringLiteral("rearm_ns"), rule.rearmNs, error);
  readNs(body, QStringLiteral("result_ttl_ns"), rule.resultTtlNs, error);
  readNs(body, QStringLiteral("evidence_pre_ns"), rule.evidencePreNs, error);
  readNs(body, QStringLiteral("evidence_post_ns"), rule.evidencePostNs, error);
  readString(body, QStringLiteral("vlm_role"), rule.vlmRole, error);
  readString(body, QStringLiteral("severity"), rule.severity, error);
  if (body.contains(QStringLiteral("actions"))) {
    if (!body.value(QStringLiteral("actions")).isObject()) return typeError(QStringLiteral("actions"), "an object");
    const QJsonObject actions = body.value(QStringLiteral("actions")).toObject();
    readBool(actions, QStringLiteral("sound"), rule.soundAction, error);
    readBool(actions, QStringLiteral("pop_to_main_view"), rule.popAction, error);
  }
  if (!error.isEmpty()) return error;
  if (body.contains(QStringLiteral("schedule"))) {
    const QString scheduleError = parseSchedule(body.value(QStringLiteral("schedule")), rule.schedule);
    if (!scheduleError.isEmpty()) return scheduleError;
  }
  rule.name = rule.name.trimmed();
  if (rule.name.isEmpty()) return QStringLiteral("name is required");
  if (rule.cameraId.isEmpty()) return QStringLiteral("camera_id is required");
  if (rule.zoneId.isEmpty()) return QStringLiteral("zone_id is required");
  if (!QTimeZone(rule.timeZone.toUtf8()).isValid()) return QStringLiteral("time_zone %1 is not a valid IANA time zone").arg(rule.timeZone);
  if (!kTargetClasses.contains(rule.targetClass))
    return QStringLiteral("target_class must be one of %1").arg(kTargetClasses.join(QStringLiteral(", ")));
  if (!(rule.minConfidence >= 0.0 && rule.minConfidence <= 1.0)) return QStringLiteral("min_confidence must be between 0 and 1");
  for (const QString& e : {checkRange(QStringLiteral("dwell_ns"), rule.dwellNs, 1, 3600),
                           checkRange(QStringLiteral("max_observation_gap_ns"), rule.maxObservationGapNs, 0.5, 60),
                           checkRange(QStringLiteral("clear_after_ns"), rule.clearAfterNs, 0, 3600),
                           checkRange(QStringLiteral("rearm_ns"), rule.rearmNs, 0, 86400),
                           checkRange(QStringLiteral("result_ttl_ns"), rule.resultTtlNs, 0.5, 60),
                           checkRange(QStringLiteral("evidence_pre_ns"), rule.evidencePreNs, 0, 600),
                           checkRange(QStringLiteral("evidence_post_ns"), rule.evidencePostNs, 0, 600)})
    if (!e.isEmpty()) return e;
  if (rule.vlmRole != QLatin1String("none")) return QStringLiteral("vlm_role %1 is not implemented (M5); use none").arg(rule.vlmRole);
  if (!kSeverities.contains(rule.severity)) return QStringLiteral("severity must be critical, review or info");
  return {};
}

QJsonObject EventRecord::toJson() const {
  return {{"id", id},
          {"rule_id", ruleId},
          {"rule_revision", ruleRevision},
          {"camera_id", cameraId},
          {"session_id", sessionId},
          {"severity", severity},
          {"condition", condition},
          {"operator_state", operatorState},
          {"opened_utc_ms", num(openedUtcMs)},
          {"opened_pts_ns", num(openedPtsNs)},
          {"trigger_pts_ns", num(triggerPtsNs)},
          {"cleared_utc_ms", num(clearedUtcMs)},
          {"title", title},
          {"detail", detail},
          {"late", late ? 1 : 0}};
}

QJsonObject EventReview::toJson() const {
  return {{"id", id}, {"event_id", eventId}, {"label", label}, {"note", note}, {"operator", operatorName}, {"utc_ms", num(utcMs)}};
}

QJsonObject AlertDelivery::toJson() const {
  return {{"id", id},
          {"event_id", eventId},
          {"channel", channel},
          {"state", state},
          {"attempts", attempts},
          {"last_error", lastError},
          {"created_utc_ms", num(createdUtcMs)},
          {"delivered_utc_ms", num(deliveredUtcMs)}};
}

QJsonObject EvidenceRef::toJson() const {
  return {{"id", id},
          {"event_id", eventId},
          {"camera_id", cameraId},
          {"from_utc_ms", num(fromUtcMs)},
          {"to_utc_ms", num(toUtcMs)},
          {"state", state},
          {"reason", reason},
          {"segment_ids", QJsonArray::fromStringList(segmentIds)},
          {"has_thumbnail", !thumbnailPath.isEmpty()},
          {"updated_utc_ms", num(updatedUtcMs)}};
}

EvaluationRecord EvaluationRecord::from(const rules::Evaluation& e) {
  EvaluationRecord r;
  r.ruleId = e.ruleId;
  r.ruleRevision = e.ruleRevision;
  r.cameraId = e.cameraId;
  r.sessionId = e.sessionId;
  r.generation = e.generation;
  r.windowStartPtsNs = e.windowStartPtsNs;
  r.windowEndPtsNs = e.windowEndPtsNs;
  r.utcMs = e.utcMs;
  r.before = rules::toString(e.before);
  r.after = rules::toString(e.after);
  r.quality = rules::toString(e.quality);
  r.transition = rules::toString(e.transition);
  r.eventId = e.eventId;
  for (const QString& t : e.trackIds) r.trackIds.push_back(t);
  r.dwellNs = e.dwellNs;
  r.note = e.note;
  return r;
}

QJsonObject EvaluationRecord::toJson() const {
  return {{"id", num(id)},
          {"rule_id", ruleId},
          {"rule_revision", ruleRevision},
          {"camera_id", cameraId},
          {"session_id", sessionId},
          {"generation", static_cast<double>(generation)},
          {"window_start_pts_ns", num(windowStartPtsNs)},
          {"window_end_pts_ns", num(windowEndPtsNs)},
          {"utc_ms", num(utcMs)},
          {"before", before},
          {"after", after},
          {"quality", quality},
          {"transition", transition},
          {"event_id", eventId},
          {"track_ids", QJsonArray::fromStringList(trackIds)},
          {"dwell_ns", num(dwellNs)},
          {"note", note}};
}

}
