#include "StoreSql.h"
#include "fovea/core/Store.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

namespace fovea::core {
namespace {

using sql::integer;
using sql::text;

QString compactJson(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }
QString compactJson(const QJsonArray& a) { return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)); }
QJsonArray parseArray(const QString& s) { return QJsonDocument::fromJson(s.toUtf8()).array(); }

QStringList stringList(const QJsonArray& a) {
  QStringList out;
  for (const QJsonValue& v : a) out.push_back(v.toString());
  return out;
}

ZoneRecord readZone(const QSqlQuery& q) {
  ZoneRecord z;
  z.id = q.value("id").toString();
  z.cameraId = q.value("camera_id").toString();
  z.name = q.value("name").toString();
  z.createdUtcMs = q.value("created_utc_ms").toLongLong();
  z.deletedUtcMs = q.value("deleted_utc_ms").toLongLong();
  z.revision = q.value("revision").toInt();
  for (const QJsonValue& p : parseArray(q.value("points_json").toString())) {
    const QJsonArray xy = p.toArray();
    z.points.push_back(QPointF(xy.at(0).toDouble(), xy.at(1).toDouble()));
  }
  z.anchor = q.value("anchor").toString();
  z.refWidth = q.value("ref_width").toInt();
  z.refHeight = q.value("ref_height").toInt();
  z.revisionUtcMs = q.value("revision_utc_ms").toLongLong();
  return z;
}

RuleRecord readRule(const QSqlQuery& q) {
  RuleRecord r = RuleRecord::fromRevisionJson(QJsonDocument::fromJson(q.value("json").toString().toUtf8()).object());
  r.id = q.value("id").toString();
  r.name = q.value("name").toString();
  r.revision = q.value("revision").toInt();
  r.createdUtcMs = q.value("created_utc_ms").toLongLong();
  r.revisionUtcMs = q.value("revision_utc_ms").toLongLong();
  return r;
}

EventRecord readEvent(const QSqlQuery& q) {
  EventRecord e;
  e.id = q.value("id").toString();
  e.ruleId = q.value("rule_id").toString();
  e.ruleRevision = q.value("rule_revision").toInt();
  e.cameraId = q.value("camera_id").toString();
  e.sessionId = q.value("session_id").toString();
  e.severity = q.value("severity").toString();
  e.condition = q.value("condition").toString();
  e.operatorState = q.value("operator_state").toString();
  e.openedUtcMs = q.value("opened_utc_ms").toLongLong();
  e.openedPtsNs = q.value("opened_pts_ns").toLongLong();
  e.triggerPtsNs = q.value("trigger_pts_ns").toLongLong();
  e.clearedUtcMs = q.value("cleared_utc_ms").toLongLong();
  e.title = q.value("title").toString();
  e.detail = q.value("detail").toString();
  e.late = q.value("late").toInt() != 0;
  return e;
}

AlertDelivery readDelivery(const QSqlQuery& q) {
  AlertDelivery d;
  d.id = q.value("id").toString();
  d.eventId = q.value("event_id").toString();
  d.channel = q.value("channel").toString();
  d.state = q.value("state").toString();
  d.attempts = q.value("attempts").toInt();
  d.lastError = q.value("last_error").toString();
  d.createdUtcMs = q.value("created_utc_ms").toLongLong();
  d.deliveredUtcMs = q.value("delivered_utc_ms").toLongLong();
  return d;
}

EvidenceRef readEvidence(const QSqlQuery& q) {
  EvidenceRef r;
  r.id = q.value("id").toString();
  r.eventId = q.value("event_id").toString();
  r.cameraId = q.value("camera_id").toString();
  r.fromUtcMs = q.value("from_utc_ms").toLongLong();
  r.toUtcMs = q.value("to_utc_ms").toLongLong();
  r.state = q.value("state").toString();
  r.reason = q.value("reason").toString();
  r.segmentIds = stringList(parseArray(q.value("segment_ids_json").toString()));
  r.thumbnailPath = q.value("thumbnail_path").toString();
  r.updatedUtcMs = q.value("updated_utc_ms").toLongLong();
  return r;
}

EvaluationRecord readEvaluation(const QSqlQuery& q) {
  EvaluationRecord e;
  e.id = q.value("id").toLongLong();
  e.ruleId = q.value("rule_id").toString();
  e.ruleRevision = q.value("rule_revision").toInt();
  e.cameraId = q.value("camera_id").toString();
  e.sessionId = q.value("session_id").toString();
  e.generation = static_cast<uint64_t>(q.value("generation").toLongLong());
  e.windowStartPtsNs = q.value("window_start_pts_ns").toLongLong();
  e.windowEndPtsNs = q.value("window_end_pts_ns").toLongLong();
  e.utcMs = q.value("utc_ms").toLongLong();
  e.before = q.value("before").toString();
  e.after = q.value("after").toString();
  e.quality = q.value("quality").toString();
  e.transition = q.value("transition").toString();
  e.eventId = q.value("event_id").toString();
  e.trackIds = stringList(parseArray(q.value("track_ids_json").toString()));
  e.dwellNs = q.value("dwell_ns").toLongLong();
  e.note = q.value("note").toString();
  return e;
}

const QString kZoneSelect = QStringLiteral(
    "SELECT z.id, z.camera_id, z.name, z.created_utc_ms, z.deleted_utc_ms, r.revision, r.points_json, r.anchor, r.ref_width,"
    " r.ref_height, r.created_utc_ms AS revision_utc_ms FROM zones z JOIN zone_revisions r ON r.zone_id=z.id");
const QString kRuleSelect = QStringLiteral(
    "SELECT u.id, u.name, u.created_utc_ms, v.revision, v.json, v.created_utc_ms AS revision_utc_ms FROM rules u"
    " JOIN rule_revisions v ON v.rule_id=u.id AND v.revision=u.current_revision");

}

namespace sql {

QStringList analyticsSchemaStatements() {
  return {
      QStringLiteral("CREATE TABLE IF NOT EXISTS zones(id TEXT PRIMARY KEY, camera_id TEXT NOT NULL, name TEXT NOT NULL,"
                     " created_utc_ms INTEGER NOT NULL, deleted_utc_ms INTEGER NOT NULL DEFAULT 0)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_zones_camera ON zones(camera_id)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS zone_revisions(zone_id TEXT NOT NULL, revision INTEGER NOT NULL, points_json TEXT NOT NULL,"
                     " anchor TEXT NOT NULL, ref_width INTEGER NOT NULL, ref_height INTEGER NOT NULL, created_utc_ms INTEGER NOT NULL,"
                     " PRIMARY KEY(zone_id, revision))"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS rules(id TEXT PRIMARY KEY, name TEXT NOT NULL, created_utc_ms INTEGER NOT NULL,"
                     " deleted_utc_ms INTEGER NOT NULL DEFAULT 0, current_revision INTEGER NOT NULL)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS rule_revisions(rule_id TEXT NOT NULL, revision INTEGER NOT NULL, json TEXT NOT NULL,"
                     " created_utc_ms INTEGER NOT NULL, PRIMARY KEY(rule_id, revision))"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS rule_evaluations(id INTEGER PRIMARY KEY AUTOINCREMENT, rule_id TEXT NOT NULL,"
                     " rule_revision INTEGER NOT NULL, camera_id TEXT NOT NULL, session_id TEXT NOT NULL, generation INTEGER NOT NULL,"
                     " window_start_pts_ns INTEGER NOT NULL, window_end_pts_ns INTEGER NOT NULL, utc_ms INTEGER NOT NULL,"
                     " \"before\" TEXT NOT NULL, \"after\" TEXT NOT NULL, quality TEXT NOT NULL, transition TEXT NOT NULL,"
                     " event_id TEXT NOT NULL DEFAULT '', track_ids_json TEXT NOT NULL DEFAULT '[]', dwell_ns INTEGER NOT NULL DEFAULT 0,"
                     " note TEXT NOT NULL DEFAULT '')"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_evaluations_rule_time ON rule_evaluations(rule_id, utc_ms)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS events(id TEXT PRIMARY KEY, rule_id TEXT NOT NULL, rule_revision INTEGER NOT NULL,"
                     " camera_id TEXT NOT NULL, session_id TEXT NOT NULL, severity TEXT NOT NULL, condition TEXT NOT NULL,"
                     " operator_state TEXT NOT NULL, opened_utc_ms INTEGER NOT NULL, opened_pts_ns INTEGER NOT NULL,"
                     " trigger_pts_ns INTEGER NOT NULL, cleared_utc_ms INTEGER NOT NULL DEFAULT 0, title TEXT NOT NULL,"
                     " detail TEXT NOT NULL DEFAULT '', late INTEGER NOT NULL DEFAULT 0)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_events_opened ON events(opened_utc_ms)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_events_camera ON events(camera_id, opened_utc_ms)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS event_reviews(id TEXT PRIMARY KEY, event_id TEXT NOT NULL, label TEXT NOT NULL,"
                     " note TEXT NOT NULL DEFAULT '', operator TEXT NOT NULL DEFAULT '', utc_ms INTEGER NOT NULL)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_reviews_event ON event_reviews(event_id, utc_ms)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS alert_deliveries(id TEXT PRIMARY KEY, event_id TEXT NOT NULL, channel TEXT NOT NULL,"
                     " state TEXT NOT NULL, attempts INTEGER NOT NULL DEFAULT 0, last_error TEXT NOT NULL DEFAULT '',"
                     " created_utc_ms INTEGER NOT NULL, delivered_utc_ms INTEGER NOT NULL DEFAULT 0)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_deliveries_event ON alert_deliveries(event_id)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_deliveries_open ON alert_deliveries(created_utc_ms) WHERE state!='delivered'"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS evidence_refs(id TEXT PRIMARY KEY, event_id TEXT NOT NULL, camera_id TEXT NOT NULL,"
                     " from_utc_ms INTEGER NOT NULL, to_utc_ms INTEGER NOT NULL, state TEXT NOT NULL, reason TEXT NOT NULL DEFAULT '',"
                     " segment_ids_json TEXT NOT NULL DEFAULT '[]', thumbnail_path TEXT NOT NULL DEFAULT '', updated_utc_ms INTEGER NOT NULL)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_evidence_event ON evidence_refs(event_id)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_evidence_open ON evidence_refs(to_utc_ms) WHERE state IN ('pending','partial')"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS analysis_coverage(id INTEGER PRIMARY KEY AUTOINCREMENT, camera_id TEXT NOT NULL,"
                     " session_id TEXT NOT NULL, from_utc_ms INTEGER NOT NULL, to_utc_ms INTEGER NOT NULL, frames_sent INTEGER NOT NULL,"
                     " frames_known INTEGER NOT NULL, frames_unknown INTEGER NOT NULL, detect_fps_target REAL NOT NULL)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_coverage_camera ON analysis_coverage(camera_id, from_utc_ms)"),
      QStringLiteral("DELETE FROM evidence_holds WHERE rowid NOT IN (SELECT MIN(rowid) FROM evidence_holds GROUP BY segment_id, reason)"),
      QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_evidence_holds_segment_reason ON evidence_holds(segment_id, reason)"),
  };
}

}

bool Store::insertZone(const ZoneRecord& zone) {
  if (!db_.transaction()) {
    lastError_ = db_.lastError().text();
    return false;
  }
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("INSERT INTO zones(id,camera_id,name,created_utc_ms) VALUES(:id,:cam,:name,:created)"));
  q.bindValue(":id", text(zone.id));
  q.bindValue(":cam", text(zone.cameraId));
  q.bindValue(":name", text(zone.name));
  q.bindValue(":created", integer(zone.createdUtcMs));
  if (!q.exec() || !insertZoneRevision(zone)) {
    if (q.lastError().isValid()) lastError_ = q.lastError().text();
    db_.rollback();
    return false;
  }
  if (db_.commit()) return true;
  lastError_ = db_.lastError().text();
  db_.rollback();
  return false;
}

bool Store::insertZoneRevision(const ZoneRecord& zone) {
  QJsonArray points;
  for (const QPointF& p : zone.points) points.push_back(QJsonArray{p.x(), p.y()});
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("INSERT INTO zone_revisions(zone_id,revision,points_json,anchor,ref_width,ref_height,created_utc_ms)"
                           " VALUES(:id,:rev,:points,:anchor,:w,:h,:created)"));
  q.bindValue(":id", text(zone.id));
  q.bindValue(":rev", zone.revision);
  q.bindValue(":points", compactJson(points));
  q.bindValue(":anchor", text(zone.anchor));
  q.bindValue(":w", zone.refWidth);
  q.bindValue(":h", zone.refHeight);
  q.bindValue(":created", integer(zone.revisionUtcMs));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return false;
  }
  if (zone.revision > 1) {
    QSqlQuery name(db_);
    name.prepare(QStringLiteral("UPDATE zones SET name=:name WHERE id=:id"));
    name.bindValue(":name", text(zone.name));
    name.bindValue(":id", text(zone.id));
    if (!name.exec()) {
      lastError_ = name.lastError().text();
      return false;
    }
  }
  return true;
}

bool Store::softDeleteZone(const QString& id, int64_t nowUtcMs) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE zones SET deleted_utc_ms=:now WHERE id=:id AND deleted_utc_ms=0"));
  q.bindValue(":now", integer(nowUtcMs));
  q.bindValue(":id", text(id));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return false;
  }
  return q.numRowsAffected() == 1;
}

QVector<ZoneRecord> Store::listZones(const QString& cameraId) {
  QVector<ZoneRecord> out;
  QSqlQuery q(db_);
  q.prepare(kZoneSelect + QStringLiteral(" WHERE z.deleted_utc_ms=0 AND (:cam='' OR z.camera_id=:cam)"
                                         " AND r.revision=(SELECT MAX(revision) FROM zone_revisions WHERE zone_id=z.id)"
                                         " ORDER BY z.created_utc_ms, z.id"));
  q.bindValue(":cam", text(cameraId));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return out;
  }
  while (q.next()) out.push_back(readZone(q));
  return out;
}

std::optional<ZoneRecord> Store::getZone(const QString& id, int revision) {
  QSqlQuery q(db_);
  q.prepare(kZoneSelect + QStringLiteral(" WHERE z.id=:id AND r.revision=(CASE WHEN :rev>0 THEN :rev ELSE"
                                         " (SELECT MAX(revision) FROM zone_revisions WHERE zone_id=z.id) END)"));
  q.bindValue(":id", text(id));
  q.bindValue(":rev", revision);
  if (!q.exec() || !q.next()) return std::nullopt;
  return readZone(q);
}

bool Store::insertRule(const RuleRecord& rule) {
  if (!db_.transaction()) {
    lastError_ = db_.lastError().text();
    return false;
  }
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("INSERT INTO rules(id,name,created_utc_ms,current_revision) VALUES(:id,:name,:created,0)"));
  q.bindValue(":id", text(rule.id));
  q.bindValue(":name", text(rule.name));
  q.bindValue(":created", integer(rule.createdUtcMs));
  if (!q.exec() || !insertRuleRevision(rule)) {
    if (q.lastError().isValid()) lastError_ = q.lastError().text();
    db_.rollback();
    return false;
  }
  if (db_.commit()) return true;
  lastError_ = db_.lastError().text();
  db_.rollback();
  return false;
}

bool Store::insertRuleRevision(const RuleRecord& rule) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("INSERT INTO rule_revisions(rule_id,revision,json,created_utc_ms) VALUES(:id,:rev,:json,:created)"));
  q.bindValue(":id", text(rule.id));
  q.bindValue(":rev", rule.revision);
  q.bindValue(":json", compactJson(rule.revisionJson()));
  q.bindValue(":created", integer(rule.revisionUtcMs));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return false;
  }
  QSqlQuery current(db_);
  current.prepare(QStringLiteral("UPDATE rules SET current_revision=:rev, name=:name WHERE id=:id AND current_revision<:rev"));
  current.bindValue(":rev", rule.revision);
  current.bindValue(":name", text(rule.name));
  current.bindValue(":id", text(rule.id));
  if (!current.exec() || current.numRowsAffected() != 1) {
    lastError_ = current.lastError().isValid() ? current.lastError().text() : QStringLiteral("stale rule revision");
    return false;
  }
  return true;
}

bool Store::softDeleteRule(const QString& id, int64_t nowUtcMs) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE rules SET deleted_utc_ms=:now WHERE id=:id AND deleted_utc_ms=0"));
  q.bindValue(":now", integer(nowUtcMs));
  q.bindValue(":id", text(id));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return false;
  }
  return q.numRowsAffected() == 1;
}

QVector<RuleRecord> Store::listRules() {
  QVector<RuleRecord> out;
  QSqlQuery q(db_);
  if (!q.exec(kRuleSelect + QStringLiteral(" WHERE u.deleted_utc_ms=0 ORDER BY u.created_utc_ms, u.id"))) {
    lastError_ = q.lastError().text();
    return out;
  }
  while (q.next()) out.push_back(readRule(q));
  return out;
}

std::optional<RuleRecord> Store::getRule(const QString& id) {
  QSqlQuery q(db_);
  q.prepare(kRuleSelect + QStringLiteral(" WHERE u.id=:id AND u.deleted_utc_ms=0"));
  q.bindValue(":id", text(id));
  if (!q.exec() || !q.next()) return std::nullopt;
  return readRule(q);
}

bool Store::insertEvaluation(const EvaluationRecord& e) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral(
      "INSERT INTO rule_evaluations(rule_id,rule_revision,camera_id,session_id,generation,window_start_pts_ns,window_end_pts_ns,"
      "utc_ms,\"before\",\"after\",quality,transition,event_id,track_ids_json,dwell_ns,note) VALUES(:rule,:rev,:cam,:sess,:gen,"
      ":wstart,:wend,:utc,:before,:after,:quality,:transition,:event,:tracks,:dwell,:note)"));
  q.bindValue(":rule", text(e.ruleId));
  q.bindValue(":rev", e.ruleRevision);
  q.bindValue(":cam", text(e.cameraId));
  q.bindValue(":sess", text(e.sessionId));
  q.bindValue(":gen", integer(static_cast<int64_t>(e.generation)));
  q.bindValue(":wstart", integer(e.windowStartPtsNs));
  q.bindValue(":wend", integer(e.windowEndPtsNs));
  q.bindValue(":utc", integer(e.utcMs));
  q.bindValue(":before", text(e.before));
  q.bindValue(":after", text(e.after));
  q.bindValue(":quality", text(e.quality));
  q.bindValue(":transition", text(e.transition));
  q.bindValue(":event", text(e.eventId));
  q.bindValue(":tracks", compactJson(QJsonArray::fromStringList(e.trackIds)));
  q.bindValue(":dwell", integer(e.dwellNs));
  q.bindValue(":note", text(e.note));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return false;
  }
  return true;
}

QVector<EvaluationRecord> Store::listEvaluations(const QString& ruleId, int64_t fromUtcMs, int64_t toUtcMs, int limit) {
  QVector<EvaluationRecord> out;
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM rule_evaluations WHERE rule_id=:rule AND utc_ms>=:from AND (:to=0 OR utc_ms<=:to)"
                           " ORDER BY id LIMIT :lim"));
  q.bindValue(":rule", text(ruleId));
  q.bindValue(":from", integer(fromUtcMs));
  q.bindValue(":to", integer(toUtcMs));
  q.bindValue(":lim", limit);
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return out;
  }
  while (q.next()) out.push_back(readEvaluation(q));
  return out;
}

bool Store::openEvent(const EventRecord& event, const EvidenceRef& evidence, const QVector<AlertDelivery>& deliveries) {
  if (!db_.transaction()) {
    lastError_ = db_.lastError().text();
    return false;
  }
  const auto fail = [this](const QSqlQuery& q) {
    lastError_ = q.lastError().text();
    db_.rollback();
    return false;
  };
  QSqlQuery e(db_);
  e.prepare(QStringLiteral(
      "INSERT INTO events(id,rule_id,rule_revision,camera_id,session_id,severity,condition,operator_state,opened_utc_ms,opened_pts_ns,"
      "trigger_pts_ns,cleared_utc_ms,title,detail,late) VALUES(:id,:rule,:rev,:cam,:sess,:sev,:cond,:op,:opened,:opts,:tpts,:cleared,"
      ":title,:detail,:late)"));
  e.bindValue(":id", text(event.id));
  e.bindValue(":rule", text(event.ruleId));
  e.bindValue(":rev", event.ruleRevision);
  e.bindValue(":cam", text(event.cameraId));
  e.bindValue(":sess", text(event.sessionId));
  e.bindValue(":sev", text(event.severity));
  e.bindValue(":cond", text(event.condition));
  e.bindValue(":op", text(event.operatorState));
  e.bindValue(":opened", integer(event.openedUtcMs));
  e.bindValue(":opts", integer(event.openedPtsNs));
  e.bindValue(":tpts", integer(event.triggerPtsNs));
  e.bindValue(":cleared", integer(event.clearedUtcMs));
  e.bindValue(":title", text(event.title));
  e.bindValue(":detail", text(event.detail));
  e.bindValue(":late", event.late ? 1 : 0);
  if (!e.exec()) return fail(e);
  QSqlQuery r(db_);
  r.prepare(QStringLiteral(
      "INSERT INTO evidence_refs(id,event_id,camera_id,from_utc_ms,to_utc_ms,state,reason,segment_ids_json,thumbnail_path,updated_utc_ms)"
      " VALUES(:id,:event,:cam,:from,:to,:state,:reason,:segs,:thumb,:updated)"));
  r.bindValue(":id", text(evidence.id));
  r.bindValue(":event", text(evidence.eventId));
  r.bindValue(":cam", text(evidence.cameraId));
  r.bindValue(":from", integer(evidence.fromUtcMs));
  r.bindValue(":to", integer(evidence.toUtcMs));
  r.bindValue(":state", text(evidence.state));
  r.bindValue(":reason", text(evidence.reason));
  r.bindValue(":segs", compactJson(QJsonArray::fromStringList(evidence.segmentIds)));
  r.bindValue(":thumb", text(evidence.thumbnailPath));
  r.bindValue(":updated", integer(evidence.updatedUtcMs));
  if (!r.exec()) return fail(r);
  for (const AlertDelivery& d : deliveries) {
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("INSERT INTO alert_deliveries(id,event_id,channel,state,attempts,last_error,created_utc_ms,delivered_utc_ms)"
                             " VALUES(:id,:event,:channel,:state,:attempts,:error,:created,:delivered)"));
    q.bindValue(":id", text(d.id));
    q.bindValue(":event", text(d.eventId));
    q.bindValue(":channel", text(d.channel));
    q.bindValue(":state", text(d.state));
    q.bindValue(":attempts", d.attempts);
    q.bindValue(":error", text(d.lastError));
    q.bindValue(":created", integer(d.createdUtcMs));
    q.bindValue(":delivered", integer(d.deliveredUtcMs));
    if (!q.exec()) return fail(q);
  }
  QSqlQuery h(db_);
  h.prepare(QStringLiteral(
      "INSERT OR IGNORE INTO evidence_holds(segment_id,until_utc_ms,reason) SELECT id, 0, :reason FROM recording_segments"
      " WHERE camera_id=:cam AND state IN ('recording','finalized','damaged') AND start_utc_ms>0 AND start_utc_ms<=:to"
      " AND (CASE WHEN end_utc_ms>0 THEN end_utc_ms WHEN state='recording' THEN :to ELSE start_utc_ms END)>=:from"));
  h.bindValue(":reason", QStringLiteral("event:") + event.id);
  h.bindValue(":cam", text(evidence.cameraId));
  h.bindValue(":from", integer(evidence.fromUtcMs));
  h.bindValue(":to", integer(evidence.toUtcMs));
  if (!h.exec()) return fail(h);
  if (db_.commit()) return true;
  lastError_ = db_.lastError().text();
  db_.rollback();
  return false;
}

bool Store::setEventCondition(const QString& id, const QString& condition, int64_t clearedUtcMs) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE events SET condition=:cond, cleared_utc_ms=:cleared WHERE id=:id AND condition!='cleared'"));
  q.bindValue(":cond", text(condition));
  q.bindValue(":cleared", integer(clearedUtcMs));
  q.bindValue(":id", text(id));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return false;
  }
  return q.numRowsAffected() == 1;
}

bool Store::setEventOperatorState(const QString& id, const QString& state) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE events SET operator_state=:state WHERE id=:id"));
  q.bindValue(":state", text(state));
  q.bindValue(":id", text(id));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return false;
  }
  return q.numRowsAffected() == 1;
}

bool Store::applyOperatorAction(const QString& eventId, const QString& state, std::optional<int64_t> holdsUntilUtcMs,
                                const AuditEntry& audit) {
  lastError_.clear();
  return transact([&] {
    if (!setEventOperatorState(eventId, state)) {
      if (lastError_.isEmpty()) lastError_ = QStringLiteral("no such event");
      return false;
    }
    if (holdsUntilUtcMs && !setEventHoldsUntil(eventId, *holdsUntilUtcMs)) return false;
    return appendAudit(audit.actor, audit.action, audit.target, audit.detail, audit.utcMs);
  });
}

QStringList Store::clearOpenEvents(int64_t nowUtcMs) {
  QVector<EventRecord> open;
  {
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral("SELECT * FROM events WHERE condition!='cleared' ORDER BY opened_utc_ms, id"))) {
      lastError_ = q.lastError().text();
      return {};
    }
    while (q.next()) open.push_back(readEvent(q));
  }
  QStringList ids;
  if (open.isEmpty()) return ids;
  const bool ok = transact([&] {
    for (const EventRecord& ev : open) {
      if (!setEventCondition(ev.id, QStringLiteral("cleared"), nowUtcMs)) return false;
      EvaluationRecord e;
      e.ruleId = ev.ruleId;
      e.ruleRevision = ev.ruleRevision;
      e.cameraId = ev.cameraId;
      e.sessionId = ev.sessionId;
      e.windowStartPtsNs = ev.openedPtsNs;
      e.windowEndPtsNs = ev.triggerPtsNs;
      e.utcMs = nowUtcMs;
      e.before = ev.condition;
      e.after = QStringLiteral("inactive");
      e.quality = QStringLiteral("unknown");
      e.transition = QStringLiteral("cleared");
      e.eventId = ev.id;
      e.note = QStringLiteral("core_restart");
      if (!insertEvaluation(e) ||
          !appendAudit(QStringLiteral("rules"), QStringLiteral("event.clear"), ev.id, QStringLiteral("{\"note\":\"core_restart\"}"),
                       nowUtcMs))
        return false;
      ids.push_back(ev.id);
    }
    return true;
  });
  return ok ? ids : QStringList{};
}

int64_t Store::lastClearedUtcMs(const QString& ruleId) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT COALESCE(MAX(cleared_utc_ms),0) FROM events WHERE rule_id=:rule AND condition='cleared'"));
  q.bindValue(":rule", text(ruleId));
  if (!q.exec() || !q.next()) return 0;
  return q.value(0).toLongLong();
}

std::optional<EventRecord> Store::getEvent(const QString& id) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM events WHERE id=:id"));
  q.bindValue(":id", text(id));
  if (!q.exec() || !q.next()) return std::nullopt;
  return readEvent(q);
}

// state is an operator state, "unresolved" (new or acknowledged) or a condition.
QVector<EventRecord> Store::listEvents(const EventQuery& query) {
  QString sql = QStringLiteral("SELECT * FROM events WHERE (:cam='' OR camera_id=:cam) AND (:from=0 OR opened_utc_ms>=:from)"
                               " AND (:to=0 OR opened_utc_ms<=:to)");
  if (query.state == QLatin1String("unresolved")) sql += QStringLiteral(" AND operator_state!='resolved'");
  else if (!query.state.isEmpty()) sql += QStringLiteral(" AND (operator_state=:state OR condition=:state)");
  sql += QStringLiteral(" ORDER BY opened_utc_ms DESC, id LIMIT :lim");
  QVector<EventRecord> out;
  QSqlQuery q(db_);
  q.prepare(sql);
  q.bindValue(":cam", text(query.cameraId));
  q.bindValue(":from", integer(query.fromUtcMs));
  q.bindValue(":to", integer(query.toUtcMs));
  if (!query.state.isEmpty() && query.state != QLatin1String("unresolved")) q.bindValue(":state", text(query.state));
  q.bindValue(":lim", query.limit);
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return out;
  }
  while (q.next()) out.push_back(readEvent(q));
  return out;
}

EventCounts Store::eventCounts() {
  EventCounts counts;
  QSqlQuery q(db_);
  if (!q.exec(QStringLiteral(
          "SELECT COALESCE(SUM(dismissed=0 AND operator_state='new'),0), COALESCE(SUM(dismissed=0 AND operator_state='acknowledged'),0),"
          " COALESCE(SUM(dismissed),0) FROM (SELECT operator_state, (operator_state='resolved' OR COALESCE((SELECT label FROM"
          " event_reviews r WHERE r.event_id=e.id ORDER BY utc_ms DESC, rowid DESC LIMIT 1),'')='false_alarm') AS dismissed"
          " FROM events e)")) ||
      !q.next()) {
    lastError_ = q.lastError().text();
    return counts;
  }
  counts.unresolved = q.value(0).toInt();
  counts.acknowledged = q.value(1).toInt();
  counts.dismissed = q.value(2).toInt();
  return counts;
}

bool Store::insertReview(const EventReview& review, const AuditEntry& audit) {
  return transact([&] {
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("INSERT INTO event_reviews(id,event_id,label,note,operator,utc_ms) VALUES(:id,:event,:label,:note,:op,:utc)"));
    q.bindValue(":id", text(review.id));
    q.bindValue(":event", text(review.eventId));
    q.bindValue(":label", text(review.label));
    q.bindValue(":note", text(review.note));
    q.bindValue(":op", text(review.operatorName));
    q.bindValue(":utc", integer(review.utcMs));
    if (!q.exec()) {
      lastError_ = q.lastError().text();
      return false;
    }
    return appendAudit(audit.actor, audit.action, audit.target, audit.detail, audit.utcMs);
  });
}

std::optional<EventReview> Store::latestReview(const QString& eventId) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM event_reviews WHERE event_id=:event ORDER BY utc_ms DESC, rowid DESC LIMIT 1"));
  q.bindValue(":event", text(eventId));
  if (!q.exec() || !q.next()) return std::nullopt;
  EventReview r;
  r.id = q.value("id").toString();
  r.eventId = q.value("event_id").toString();
  r.label = q.value("label").toString();
  r.note = q.value("note").toString();
  r.operatorName = q.value("operator").toString();
  r.utcMs = q.value("utc_ms").toLongLong();
  return r;
}

QVector<AlertDelivery> Store::deliveriesForEvent(const QString& eventId) {
  QVector<AlertDelivery> out;
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM alert_deliveries WHERE event_id=:event ORDER BY created_utc_ms, channel"));
  q.bindValue(":event", text(eventId));
  if (!q.exec()) return out;
  while (q.next()) out.push_back(readDelivery(q));
  return out;
}

std::optional<AlertDelivery> Store::getDelivery(const QString& id) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM alert_deliveries WHERE id=:id"));
  q.bindValue(":id", text(id));
  if (!q.exec() || !q.next()) return std::nullopt;
  return readDelivery(q);
}

QVector<AlertDelivery> Store::openDeliveries() {
  QVector<AlertDelivery> out;
  QSqlQuery q(db_);
  if (!q.exec(QStringLiteral("SELECT d.* FROM alert_deliveries d JOIN events e ON e.id=d.event_id WHERE d.state!='delivered'"
                             " AND e.operator_state!='resolved' ORDER BY d.created_utc_ms, d.channel"))) {
    lastError_ = q.lastError().text();
    return out;
  }
  while (q.next()) out.push_back(readDelivery(q));
  return out;
}

bool Store::markDelivered(const QString& id, int64_t nowUtcMs) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE alert_deliveries SET state='delivered', delivered_utc_ms=:now, attempts=attempts+1, last_error=''"
                           " WHERE id=:id AND state!='delivered'"));
  q.bindValue(":now", integer(nowUtcMs));
  q.bindValue(":id", text(id));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return false;
  }
  return q.numRowsAffected() == 1;
}

bool Store::recordDeliveryAttempt(const QString& id, int attempts, const QString& state, const QString& error) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE alert_deliveries SET attempts=:attempts, state=:state, last_error=:error WHERE id=:id AND state!='delivered'"));
  q.bindValue(":attempts", attempts);
  q.bindValue(":state", text(state));
  q.bindValue(":error", text(error));
  q.bindValue(":id", text(id));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return false;
  }
  return q.numRowsAffected() == 1;
}

std::optional<EvidenceRef> Store::getEvidence(const QString& id) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM evidence_refs WHERE id=:id"));
  q.bindValue(":id", text(id));
  if (!q.exec() || !q.next()) return std::nullopt;
  return readEvidence(q);
}

std::optional<EvidenceRef> Store::evidenceForEvent(const QString& eventId) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM evidence_refs WHERE event_id=:event LIMIT 1"));
  q.bindValue(":event", text(eventId));
  if (!q.exec() || !q.next()) return std::nullopt;
  return readEvidence(q);
}

// A partial ref past its grace stays selected while a segment of its camera
// that started before the window ended is still recording, and once more after
// such a segment finalized: the window may yet be covered.
QVector<EvidenceRef> Store::evidenceToEvaluate(int64_t graceMs) {
  QVector<EvidenceRef> out;
  QSqlQuery q(db_);
  q.prepare(QStringLiteral(
      "SELECT * FROM evidence_refs r WHERE state='pending' OR (state='partial' AND (updated_utc_ms<=to_utc_ms+:grace OR EXISTS("
      "SELECT 1 FROM recording_segments s WHERE s.camera_id=r.camera_id AND s.start_utc_ms>0 AND s.start_utc_ms<=r.to_utc_ms"
      " AND (s.state='recording' OR (s.state='finalized' AND s.finalized_utc_ms>=r.updated_utc_ms AND s.end_utc_ms>=r.from_utc_ms)))))"
      " ORDER BY to_utc_ms"));
  q.bindValue(":grace", integer(graceMs));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return out;
  }
  while (q.next()) out.push_back(readEvidence(q));
  return out;
}

bool Store::updateEvidence(const EvidenceRef& ref, int64_t holdUntilUtcMs) {
  if (!db_.transaction()) {
    lastError_ = db_.lastError().text();
    return false;
  }
  QSqlQuery r(db_);
  r.prepare(QStringLiteral("UPDATE evidence_refs SET state=:state, reason=:reason, segment_ids_json=:segs, updated_utc_ms=:updated,"
                           " thumbnail_path=:thumb WHERE id=:id"));
  r.bindValue(":state", text(ref.state));
  r.bindValue(":thumb", text(ref.thumbnailPath));
  r.bindValue(":reason", text(ref.reason));
  r.bindValue(":segs", compactJson(QJsonArray::fromStringList(ref.segmentIds)));
  r.bindValue(":updated", integer(ref.updatedUtcMs));
  r.bindValue(":id", text(ref.id));
  if (!r.exec()) {
    lastError_ = r.lastError().text();
    db_.rollback();
    return false;
  }
  const QString reason = QStringLiteral("event:") + ref.eventId;
  for (const QString& segmentId : ref.segmentIds) {
    QSqlQuery h(db_);
    h.prepare(QStringLiteral("INSERT OR IGNORE INTO evidence_holds(segment_id,until_utc_ms,reason) VALUES(:seg,:until,:reason)"));
    h.bindValue(":seg", text(segmentId));
    h.bindValue(":until", integer(holdUntilUtcMs));
    h.bindValue(":reason", reason);
    if (!h.exec()) {
      lastError_ = h.lastError().text();
      db_.rollback();
      return false;
    }
  }
  if (db_.commit()) return true;
  lastError_ = db_.lastError().text();
  db_.rollback();
  return false;
}

bool Store::setEventHoldsUntil(const QString& eventId, int64_t untilUtcMs) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE evidence_holds SET until_utc_ms=:until WHERE reason=:reason"));
  q.bindValue(":until", integer(untilUtcMs));
  q.bindValue(":reason", QStringLiteral("event:") + eventId);
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return false;
  }
  return true;
}

bool Store::insertCoverage(const CoverageRecord& c) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("INSERT INTO analysis_coverage(camera_id,session_id,from_utc_ms,to_utc_ms,frames_sent,frames_known,"
                           "frames_unknown,detect_fps_target) VALUES(:cam,:sess,:from,:to,:sent,:known,:unknown,:fps)"));
  q.bindValue(":cam", text(c.cameraId));
  q.bindValue(":sess", text(c.sessionId));
  q.bindValue(":from", integer(c.fromUtcMs));
  q.bindValue(":to", integer(c.toUtcMs));
  q.bindValue(":sent", c.framesSent);
  q.bindValue(":known", c.framesKnown);
  q.bindValue(":unknown", c.framesUnknown);
  q.bindValue(":fps", c.detectFpsTarget);
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return false;
  }
  return true;
}

}
