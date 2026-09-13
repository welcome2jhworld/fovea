#include "core/AlertTypes.h"
#include <QJsonArray>
#include <QJsonValue>

namespace fovea::ui {

namespace {
int64_t i64(const QJsonObject& o, const char* k, int64_t d = 0) {
  const QJsonValue v = o.value(QLatin1StringView(k));
  return v.isDouble() ? static_cast<int64_t>(v.toDouble()) : d;
}
int i32(const QJsonObject& o, const char* k, int d = 0) { return o.value(QLatin1StringView(k)).toInt(d); }
double dbl(const QJsonObject& o, const char* k, double d = 0) { return o.value(QLatin1StringView(k)).toDouble(d); }
bool bl(const QJsonObject& o, const char* k, bool d = false) { return o.value(QLatin1StringView(k)).toBool(d); }
QString str(const QJsonObject& o, const char* k) { return o.value(QLatin1StringView(k)).toString(); }

const char* const kPreservedRuleKeys[] = {"max_observation_gap_ns", "clear_after_ns", "rearm_ns", "result_ttl_ns",
                                          "evidence_pre_ns", "evidence_post_ns", "vlm_role"};
}

ZoneInfo ZoneInfo::fromJson(const QJsonObject& o) {
  ZoneInfo z;
  z.id = str(o, "id");
  z.cameraId = str(o, "camera_id");
  z.name = str(o, "name");
  z.revision = i32(o, "revision");
  for (const QJsonValue& v : o.value(QLatin1StringView("points")).toArray()) {
    const QJsonArray xy = v.toArray();
    if (xy.size() == 2) z.points.push_back(QPointF(xy.at(0).toDouble(), xy.at(1).toDouble()));
  }
  const QString anchor = str(o, "anchor");
  if (!anchor.isEmpty()) z.anchor = anchor;
  z.refWidth = i32(o, "ref_width");
  z.refHeight = i32(o, "ref_height");
  return z;
}

RuleInfo RuleInfo::fromJson(const QJsonObject& o) {
  RuleInfo r;
  r.id = str(o, "id");
  r.name = str(o, "name");
  r.revision = i32(o, "revision");
  r.enabled = bl(o, "enabled", true);
  r.cameraId = str(o, "camera_id");
  r.zoneId = str(o, "zone_id");
  r.zoneRevision = i32(o, "zone_revision");
  const QJsonArray windows = o.value(QLatin1StringView("schedule")).toArray();
  if (!windows.isEmpty()) {
    r.schedule.clear();
    for (const QJsonValue& v : windows) {
      const QJsonObject w = v.toObject();
      r.schedule.push_back(ScheduleWindow{i32(w, "days", 0x7f), i32(w, "start_minute", 0), i32(w, "end_minute", 24 * 60)});
    }
  }
  r.timeZone = str(o, "time_zone");
  const QString cls = str(o, "target_class");
  if (!cls.isEmpty()) r.targetClass = cls;
  r.minConfidence = dbl(o, "min_confidence", r.minConfidence);
  r.dwellNs = i64(o, "dwell_ns", r.dwellNs);
  const QString severity = str(o, "severity");
  if (!severity.isEmpty()) r.severity = severity;
  const QJsonObject actions = o.value(QLatin1StringView("actions")).toObject();
  r.soundAction = bl(actions, "sound", false);
  r.popAction = bl(actions, "pop_to_main_view", false);
  for (const char* key : kPreservedRuleKeys) {
    const QJsonValue v = o.value(QLatin1StringView(key));
    if (!v.isUndefined() && !v.isNull()) r.preserved.insert(QLatin1StringView(key), v);
  }
  return r;
}

QJsonObject RuleInfo::revisionJson() const {
  QJsonArray windows;
  for (const ScheduleWindow& w : schedule)
    windows.push_back(QJsonObject{{"days", w.days}, {"start_minute", w.startMinute}, {"end_minute", w.endMinute}});
  QJsonObject o = preserved;
  o.insert("name", name);
  o.insert("enabled", enabled);
  o.insert("camera_id", cameraId);
  o.insert("zone_id", zoneId);
  o.insert("zone_revision", zoneRevision);
  o.insert("schedule", windows);
  o.insert("time_zone", timeZone);
  o.insert("target_class", targetClass);
  o.insert("min_confidence", minConfidence);
  o.insert("dwell_ns", static_cast<double>(dwellNs));
  o.insert("severity", severity);
  o.insert("actions", QJsonObject{{"sound", soundAction}, {"pop_to_main_view", popAction}});
  return o;
}

DeliveryInfo DeliveryInfo::fromJson(const QJsonObject& o) {
  DeliveryInfo d;
  d.id = str(o, "id");
  d.eventId = str(o, "event_id");
  d.channel = str(o, "channel");
  d.state = str(o, "state");
  d.lastError = str(o, "last_error");
  d.attempts = i32(o, "attempts");
  d.createdUtcMs = i64(o, "created_utc_ms");
  d.deliveredUtcMs = i64(o, "delivered_utc_ms");
  return d;
}

EventInfo EventInfo::fromJson(const QJsonObject& o) {
  EventInfo e;
  e.id = str(o, "id");
  e.ruleId = str(o, "rule_id");
  e.ruleRevision = i32(o, "rule_revision");
  e.cameraId = str(o, "camera_id");
  e.sessionId = str(o, "session_id");
  e.severity = str(o, "severity");
  e.condition = str(o, "condition");
  e.operatorState = str(o, "operator_state");
  e.title = str(o, "title");
  e.detail = str(o, "detail");
  e.openedUtcMs = i64(o, "opened_utc_ms");
  e.clearedUtcMs = i64(o, "cleared_utc_ms");
  e.late = o.value(QLatin1StringView("late")).toVariant().toBool();
  const QJsonValue review = o.value(QLatin1StringView("review"));
  if (review.isObject()) {
    const QJsonObject r = review.toObject();
    e.review = ReviewInfo{str(r, "label"), str(r, "note"), str(r, "operator"), i64(r, "utc_ms")};
  }
  const QJsonValue evidence = o.value(QLatin1StringView("evidence"));
  if (evidence.isObject()) {
    const QJsonObject ev = evidence.toObject();
    e.evidence = EvidenceInfo{str(ev, "id"), str(ev, "state"), str(ev, "reason"), i64(ev, "from_utc_ms"),
                              i64(ev, "to_utc_ms")};
  }
  for (const QJsonValue& v : o.value(QLatin1StringView("deliveries")).toArray())
    e.deliveries.push_back(DeliveryInfo::fromJson(v.toObject()));
  for (const QJsonValue& v : o.value(QLatin1StringView("evaluations")).toArray()) {
    const QJsonObject ev = v.toObject();
    EvaluationInfo info;
    info.utcMs = i64(ev, "utc_ms");
    info.ruleRevision = i32(ev, "rule_revision");
    info.transition = str(ev, "transition");
    info.before = str(ev, "before");
    info.after = str(ev, "after");
    info.quality = str(ev, "quality");
    info.note = str(ev, "note");
    info.dwellNs = i64(ev, "dwell_ns");
    for (const QJsonValue& t : ev.value(QLatin1StringView("track_ids")).toArray()) info.trackIds.push_back(t.toString());
    e.evaluations.push_back(std::move(info));
  }
  return e;
}

DetectionFrame DetectionFrame::fromJson(const QJsonObject& o) {
  DetectionFrame f;
  f.cameraId = str(o, "camera_id");
  f.sessionId = str(o, "session_id");
  f.frameId = str(o, "frame_id");
  f.ptsNs = i64(o, "pts_ns");
  f.utcMs = i64(o, "utc_ms");
  f.width = i32(o, "width");
  f.height = i32(o, "height");
  for (const QJsonValue& v : o.value(QLatin1StringView("detections")).toArray()) {
    const QJsonObject d = v.toObject();
    const QJsonArray bbox = d.value(QLatin1StringView("bbox")).toArray();
    if (bbox.size() != 4) continue;
    Detection det;
    det.trackId = d.value(QLatin1StringView("track_id")).toVariant().toString();
    det.cls = str(d, "cls");
    det.confidence = dbl(d, "confidence");
    det.box = QRectF(QPointF(bbox.at(0).toDouble(), bbox.at(1).toDouble()),
                     QPointF(bbox.at(2).toDouble(), bbox.at(3).toDouble()));
    f.detections.push_back(std::move(det));
  }
  return f;
}

}
