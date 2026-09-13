#include "fovea/core/Store.h"
#include "fovea/Ids.h"
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace fovea::core {
namespace {

constexpr int kSchemaVersion = 1;

QVariant text(const QString& s) { return s.isNull() ? QVariant(QStringLiteral("")) : QVariant(s); }

Camera readCamera(const QSqlQuery& q) {
  Camera c;
  c.id = q.value("id").toString();
  c.code = q.value("code").toString();
  c.name = q.value("name").toString();
  c.groupName = q.value("group_name").toString();
  c.kind = q.value("kind").toString();
  c.mainUrl = q.value("main_url").toString();
  c.subUrl = q.value("sub_url").toString();
  c.transport = q.value("transport").toString();
  c.timeoutMs = q.value("timeout_ms").toInt();
  c.jitterMs = q.value("jitter_ms").toInt();
  c.segmentSeconds = q.value("segment_seconds").toInt();
  c.analyticsEnabled = q.value("analytics_enabled").toBool();
  c.recordEnabled = q.value("record_enabled").toBool();
  c.enabled = q.value("enabled").toBool();
  c.createdUtcMs = q.value("created_utc_ms").toLongLong();
  c.updatedUtcMs = q.value("updated_utc_ms").toLongLong();
  return c;
}

StreamSession readSession(const QSqlQuery& q) {
  StreamSession s;
  s.id = q.value("id").toString();
  s.cameraId = q.value("camera_id").toString();
  s.startedMonoNs = q.value("started_mono_ns").toLongLong();
  s.startedUtcMs = q.value("started_utc_ms").toLongLong();
  s.firstPtsNs = q.value("first_pts_ns").toLongLong();
  s.endedUtcMs = q.value("ended_utc_ms").toLongLong();
  s.endReason = q.value("end_reason").toString();
  s.codec = q.value("codec").toString();
  s.width = q.value("width").toInt();
  s.height = q.value("height").toInt();
  s.fps = q.value("fps").toDouble();
  s.transport = q.value("transport").toString();
  s.captureClock = q.value("capture_clock").toString();
  return s;
}

RecordingSegment readSegment(const QSqlQuery& q) {
  RecordingSegment s;
  s.id = q.value("id").toString();
  s.cameraId = q.value("camera_id").toString();
  s.sessionId = q.value("session_id").toString();
  s.path = q.value("path").toString();
  s.state = q.value("state").toString();
  s.startPtsNs = q.value("start_pts_ns").toLongLong();
  s.endPtsNs = q.value("end_pts_ns").toLongLong();
  s.startUtcMs = q.value("start_utc_ms").toLongLong();
  s.endUtcMs = q.value("end_utc_ms").toLongLong();
  s.bytes = q.value("bytes").toLongLong();
  s.createdUtcMs = q.value("created_utc_ms").toLongLong();
  s.finalizedUtcMs = q.value("finalized_utc_ms").toLongLong();
  return s;
}

ReceiveGap readGap(const QSqlQuery& q) {
  ReceiveGap g;
  g.id = q.value("id").toString();
  g.cameraId = q.value("camera_id").toString();
  g.sessionId = q.value("session_id").toString();
  g.fromUtcMs = q.value("from_utc_ms").toLongLong();
  g.toUtcMs = q.value("to_utc_ms").toLongLong();
  g.reason = q.value("reason").toString();
  return g;
}

}

Store::Store() : connectionName_(QStringLiteral("fovea-") + QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

Store::~Store() { close(); }

bool Store::open(const QString& path) {
  db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
  db_.setDatabaseName(path);
  if (!db_.open()) {
    lastError_ = db_.lastError().text();
    return false;
  }
  exec(QStringLiteral("PRAGMA journal_mode=WAL"));
  exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
  exec(QStringLiteral("PRAGMA foreign_keys=ON"));
  exec(QStringLiteral("PRAGMA busy_timeout=5000"));
  return migrate();
}

void Store::close() {
  if (db_.isValid()) {
    db_.close();
    db_ = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName_);
  }
}

bool Store::isOpen() const { return db_.isValid() && db_.isOpen(); }

bool Store::exec(const QString& sql) {
  QSqlQuery q(db_);
  if (!q.exec(sql)) {
    lastError_ = q.lastError().text() + " [" + sql.left(80) + "]";
    return false;
  }
  return true;
}

bool Store::migrate() {
  if (!exec(QStringLiteral("CREATE TABLE IF NOT EXISTS schema_version(version INTEGER NOT NULL)"))) return false;
  QSqlQuery v(db_);
  int current = 0;
  if (v.exec(QStringLiteral("SELECT version FROM schema_version LIMIT 1")) && v.next()) current = v.value(0).toInt();
  if (current >= kSchemaVersion) return true;
  const QStringList statements = {
      QStringLiteral(
          "CREATE TABLE IF NOT EXISTS cameras(id TEXT PRIMARY KEY, code TEXT NOT NULL DEFAULT '', name TEXT NOT NULL, group_name TEXT NOT NULL DEFAULT '',"
          " kind TEXT NOT NULL, main_url TEXT NOT NULL, sub_url TEXT NOT NULL DEFAULT '', transport TEXT NOT NULL DEFAULT 'tcp',"
          " timeout_ms INTEGER NOT NULL DEFAULT 8000, jitter_ms INTEGER NOT NULL DEFAULT 1000,"
          " segment_seconds INTEGER NOT NULL DEFAULT 60, analytics_enabled INTEGER NOT NULL DEFAULT 0,"
          " record_enabled INTEGER NOT NULL DEFAULT 1, enabled INTEGER NOT NULL DEFAULT 1,"
          " created_utc_ms INTEGER NOT NULL, updated_utc_ms INTEGER NOT NULL, deleted_utc_ms INTEGER NOT NULL DEFAULT 0)"),
      QStringLiteral(
          "CREATE TABLE IF NOT EXISTS stream_sessions(id TEXT PRIMARY KEY, camera_id TEXT NOT NULL,"
          " started_mono_ns INTEGER NOT NULL DEFAULT 0, started_utc_ms INTEGER NOT NULL, first_pts_ns INTEGER NOT NULL DEFAULT -1,"
          " ended_utc_ms INTEGER NOT NULL DEFAULT 0, end_reason TEXT NOT NULL DEFAULT '', codec TEXT NOT NULL DEFAULT '',"
          " width INTEGER NOT NULL DEFAULT 0, height INTEGER NOT NULL DEFAULT 0, fps REAL NOT NULL DEFAULT 0,"
          " transport TEXT NOT NULL DEFAULT '', capture_clock TEXT NOT NULL DEFAULT 'none')"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_sessions_camera ON stream_sessions(camera_id, started_utc_ms)"),
      QStringLiteral(
          "CREATE TABLE IF NOT EXISTS recording_segments(id TEXT PRIMARY KEY, camera_id TEXT NOT NULL, session_id TEXT NOT NULL,"
          " path TEXT NOT NULL UNIQUE, state TEXT NOT NULL, start_pts_ns INTEGER NOT NULL DEFAULT 0,"
          " end_pts_ns INTEGER NOT NULL DEFAULT 0, start_utc_ms INTEGER NOT NULL DEFAULT 0, end_utc_ms INTEGER NOT NULL DEFAULT 0,"
          " bytes INTEGER NOT NULL DEFAULT 0, created_utc_ms INTEGER NOT NULL, finalized_utc_ms INTEGER NOT NULL DEFAULT 0)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_segments_camera_time ON recording_segments(camera_id, start_utc_ms)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_segments_state ON recording_segments(state)"),
      QStringLiteral(
          "CREATE TABLE IF NOT EXISTS receive_gaps(id TEXT PRIMARY KEY, camera_id TEXT NOT NULL, session_id TEXT NOT NULL,"
          " from_utc_ms INTEGER NOT NULL, to_utc_ms INTEGER NOT NULL DEFAULT 0, reason TEXT NOT NULL)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_gaps_camera_time ON receive_gaps(camera_id, from_utc_ms)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value_json TEXT NOT NULL)"),
      QStringLiteral(
          "CREATE TABLE IF NOT EXISTS audit_log(id INTEGER PRIMARY KEY AUTOINCREMENT, utc_ms INTEGER NOT NULL,"
          " actor TEXT NOT NULL, action TEXT NOT NULL, target TEXT NOT NULL, detail TEXT NOT NULL DEFAULT '')"),
      QStringLiteral("DELETE FROM schema_version"),
      QStringLiteral("INSERT INTO schema_version(version) VALUES(%1)").arg(kSchemaVersion),
  };
  db_.transaction();
  for (const QString& s : statements) {
    if (!exec(s)) {
      db_.rollback();
      return false;
    }
  }
  return db_.commit();
}

RecoveryReport Store::recoverOnStartup(const std::function<SegmentProbe(const QString&)>& probe, int64_t nowUtcMs) {
  RecoveryReport r;
  for (const RecordingSegment& seg : listSegmentsByState(QStringLiteral("recording"))) {
    const SegmentProbe p = probe(seg.path);
    if (p.bytes == 0 && !p.readable) {
      setSegmentState(seg.id, QStringLiteral("damaged"));
      ++r.segmentsMissing;
      continue;
    }
    if (p.readable) {
      const int64_t endPts = seg.startPtsNs + p.durationNs;
      const int64_t endUtc = seg.startUtcMs + p.durationNs / 1000000;
      finalizeSegment(seg.id, endPts, endUtc, p.bytes, nowUtcMs);
      ++r.segmentsFinalized;
    } else {
      setSegmentState(seg.id, QStringLiteral("damaged"));
      ++r.segmentsDamaged;
    }
  }
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE stream_sessions SET ended_utc_ms=:now, end_reason='shutdown' WHERE ended_utc_ms=0"));
  q.bindValue(":now", static_cast<qlonglong>(nowUtcMs));
  if (q.exec()) r.sessionsClosed = q.numRowsAffected();
  return r;
}

QVector<Camera> Store::listCameras(bool includeDeleted) {
  QVector<Camera> out;
  QSqlQuery q(db_);
  const QString sql = includeDeleted ? QStringLiteral("SELECT * FROM cameras ORDER BY created_utc_ms")
                                     : QStringLiteral("SELECT * FROM cameras WHERE deleted_utc_ms=0 ORDER BY created_utc_ms");
  if (!q.exec(sql)) { lastError_ = q.lastError().text(); return out; }
  while (q.next()) out.push_back(readCamera(q));
  return out;
}

std::optional<Camera> Store::getCamera(const QString& id) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM cameras WHERE id=:id AND deleted_utc_ms=0"));
  q.bindValue(":id", text(id));
  if (!q.exec() || !q.next()) return std::nullopt;
  return readCamera(q);
}

bool Store::insertCamera(const Camera& c) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral(
      "INSERT INTO cameras(id,code,name,group_name,kind,main_url,sub_url,transport,timeout_ms,jitter_ms,segment_seconds,"
      "analytics_enabled,record_enabled,enabled,created_utc_ms,updated_utc_ms) VALUES(:id,:code,:name,:group_name,:kind,:main_url,"
      ":sub_url,:transport,:timeout_ms,:jitter_ms,:segment_seconds,:analytics_enabled,:record_enabled,:enabled,:created,:updated)"));
  q.bindValue(":id", text(c.id));
  q.bindValue(":code", text(c.code));
  q.bindValue(":name", text(c.name));
  q.bindValue(":group_name", text(c.groupName));
  q.bindValue(":kind", text(c.kind));
  q.bindValue(":main_url", text(c.mainUrl));
  q.bindValue(":sub_url", text(c.subUrl));
  q.bindValue(":transport", text(c.transport));
  q.bindValue(":timeout_ms", c.timeoutMs);
  q.bindValue(":jitter_ms", c.jitterMs);
  q.bindValue(":segment_seconds", c.segmentSeconds);
  q.bindValue(":analytics_enabled", c.analyticsEnabled ? 1 : 0);
  q.bindValue(":record_enabled", c.recordEnabled ? 1 : 0);
  q.bindValue(":enabled", c.enabled ? 1 : 0);
  q.bindValue(":created", static_cast<qlonglong>(c.createdUtcMs));
  q.bindValue(":updated", static_cast<qlonglong>(c.updatedUtcMs));
  if (!q.exec()) { lastError_ = q.lastError().text(); return false; }
  return true;
}

bool Store::updateCamera(const Camera& c) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral(
      "UPDATE cameras SET code=:code,name=:name,group_name=:group_name,kind=:kind,main_url=:main_url,sub_url=:sub_url,transport=:transport,"
      "timeout_ms=:timeout_ms,jitter_ms=:jitter_ms,segment_seconds=:segment_seconds,analytics_enabled=:analytics_enabled,"
      "record_enabled=:record_enabled,enabled=:enabled,updated_utc_ms=:updated WHERE id=:id AND deleted_utc_ms=0"));
  q.bindValue(":id", text(c.id));
  q.bindValue(":code", text(c.code));
  q.bindValue(":name", text(c.name));
  q.bindValue(":group_name", text(c.groupName));
  q.bindValue(":kind", text(c.kind));
  q.bindValue(":main_url", text(c.mainUrl));
  q.bindValue(":sub_url", text(c.subUrl));
  q.bindValue(":transport", text(c.transport));
  q.bindValue(":timeout_ms", c.timeoutMs);
  q.bindValue(":jitter_ms", c.jitterMs);
  q.bindValue(":segment_seconds", c.segmentSeconds);
  q.bindValue(":analytics_enabled", c.analyticsEnabled ? 1 : 0);
  q.bindValue(":record_enabled", c.recordEnabled ? 1 : 0);
  q.bindValue(":enabled", c.enabled ? 1 : 0);
  q.bindValue(":updated", static_cast<qlonglong>(c.updatedUtcMs));
  if (!q.exec()) { lastError_ = q.lastError().text(); return false; }
  return q.numRowsAffected() == 1;
}

bool Store::softDeleteCamera(const QString& id, int64_t nowUtcMs) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE cameras SET deleted_utc_ms=:now, enabled=0 WHERE id=:id AND deleted_utc_ms=0"));
  q.bindValue(":now", static_cast<qlonglong>(nowUtcMs));
  q.bindValue(":id", text(id));
  if (!q.exec()) { lastError_ = q.lastError().text(); return false; }
  if (q.numRowsAffected() != 1) return false;
  QSqlQuery s(db_);
  s.prepare(QStringLiteral("UPDATE recording_segments SET state='deleted' WHERE camera_id=:id AND state!='deleted'"));
  s.bindValue(":id", id);
  return s.exec();
}

bool Store::insertSession(const StreamSession& s) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral(
      "INSERT INTO stream_sessions(id,camera_id,started_mono_ns,started_utc_ms,first_pts_ns,transport,capture_clock)"
      " VALUES(:id,:camera_id,:mono,:utc,:first,:transport,:clock)"));
  q.bindValue(":id", text(s.id));
  q.bindValue(":camera_id", text(s.cameraId));
  q.bindValue(":mono", static_cast<qlonglong>(s.startedMonoNs));
  q.bindValue(":utc", static_cast<qlonglong>(s.startedUtcMs));
  q.bindValue(":first", static_cast<qlonglong>(s.firstPtsNs));
  q.bindValue(":transport", text(s.transport));
  q.bindValue(":clock", text(s.captureClock));
  if (!q.exec()) { lastError_ = q.lastError().text(); return false; }
  return true;
}

bool Store::setSessionFirstPts(const QString& id, int64_t firstPtsNs) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE stream_sessions SET first_pts_ns=:pts WHERE id=:id AND first_pts_ns<0"));
  q.bindValue(":pts", static_cast<qlonglong>(firstPtsNs));
  q.bindValue(":id", text(id));
  return q.exec();
}

bool Store::setSessionMedia(const QString& id, const QString& codec, int width, int height, double fps, const QString& captureClock) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE stream_sessions SET codec=:codec,width=:w,height=:h,fps=:fps,capture_clock=:clock WHERE id=:id"));
  q.bindValue(":codec", text(codec));
  q.bindValue(":w", width);
  q.bindValue(":h", height);
  q.bindValue(":fps", fps);
  q.bindValue(":clock", text(captureClock));
  q.bindValue(":id", text(id));
  return q.exec();
}

bool Store::endSession(const QString& id, const QString& reason, int64_t endedUtcMs) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE stream_sessions SET ended_utc_ms=:utc,end_reason=:reason WHERE id=:id AND ended_utc_ms=0"));
  q.bindValue(":utc", static_cast<qlonglong>(endedUtcMs));
  q.bindValue(":reason", text(reason));
  q.bindValue(":id", text(id));
  return q.exec();
}

QVector<StreamSession> Store::listSessions(const QString& cameraId, int limit) {
  QVector<StreamSession> out;
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM stream_sessions WHERE camera_id=:id ORDER BY started_utc_ms DESC LIMIT :lim"));
  q.bindValue(":id", text(cameraId));
  q.bindValue(":lim", limit);
  if (!q.exec()) return out;
  while (q.next()) out.push_back(readSession(q));
  return out;
}

std::optional<StreamSession> Store::getSession(const QString& id) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM stream_sessions WHERE id=:id"));
  q.bindValue(":id", text(id));
  if (!q.exec() || !q.next()) return std::nullopt;
  return readSession(q);
}

bool Store::insertSegment(const RecordingSegment& s) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral(
      "INSERT INTO recording_segments(id,camera_id,session_id,path,state,start_pts_ns,start_utc_ms,created_utc_ms)"
      " VALUES(:id,:camera_id,:session_id,:path,:state,:spts,:sutc,:created)"));
  q.bindValue(":id", text(s.id));
  q.bindValue(":camera_id", text(s.cameraId));
  q.bindValue(":session_id", text(s.sessionId));
  q.bindValue(":path", text(s.path));
  q.bindValue(":state", text(s.state));
  q.bindValue(":spts", static_cast<qlonglong>(s.startPtsNs));
  q.bindValue(":sutc", static_cast<qlonglong>(s.startUtcMs));
  q.bindValue(":created", static_cast<qlonglong>(s.createdUtcMs));
  if (!q.exec()) { lastError_ = q.lastError().text(); return false; }
  return true;
}

bool Store::finalizeSegment(const QString& id, int64_t endPtsNs, int64_t endUtcMs, int64_t bytes, int64_t finalizedUtcMs) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral(
      "UPDATE recording_segments SET state='finalized',end_pts_ns=:epts,end_utc_ms=:eutc,bytes=:bytes,finalized_utc_ms=:fin"
      " WHERE id=:id AND state='recording'"));
  q.bindValue(":epts", static_cast<qlonglong>(endPtsNs));
  q.bindValue(":eutc", static_cast<qlonglong>(endUtcMs));
  q.bindValue(":bytes", static_cast<qlonglong>(bytes));
  q.bindValue(":fin", static_cast<qlonglong>(finalizedUtcMs));
  q.bindValue(":id", text(id));
  return q.exec() && q.numRowsAffected() == 1;
}

bool Store::setSegmentStart(const QString& id, int64_t startPtsNs, int64_t startUtcMs) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE recording_segments SET start_pts_ns=:spts,start_utc_ms=:sutc WHERE id=:id"));
  q.bindValue(":spts", static_cast<qlonglong>(startPtsNs));
  q.bindValue(":sutc", static_cast<qlonglong>(startUtcMs));
  q.bindValue(":id", text(id));
  return q.exec();
}

bool Store::setSegmentState(const QString& id, const QString& state) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE recording_segments SET state=:state WHERE id=:id"));
  q.bindValue(":state", text(state));
  q.bindValue(":id", text(id));
  return q.exec();
}

QVector<RecordingSegment> Store::listSegments(const QString& cameraId, int64_t fromUtcMs, int64_t toUtcMs, int limit) {
  QVector<RecordingSegment> out;
  QSqlQuery q(db_);
  q.prepare(QStringLiteral(
      "SELECT * FROM recording_segments WHERE camera_id=:id AND state!='deleted'"
      " AND (:to=0 OR start_utc_ms<=:to) AND (:from=0 OR end_utc_ms>=:from OR state='recording')"
      " ORDER BY start_utc_ms DESC LIMIT :lim"));
  q.bindValue(":id", text(cameraId));
  q.bindValue(":from", static_cast<qlonglong>(fromUtcMs));
  q.bindValue(":to", static_cast<qlonglong>(toUtcMs));
  q.bindValue(":lim", limit);
  if (!q.exec()) { lastError_ = q.lastError().text(); return out; }
  while (q.next()) out.push_back(readSegment(q));
  return out;
}

QVector<RecordingSegment> Store::listSegmentsByState(const QString& state) {
  QVector<RecordingSegment> out;
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM recording_segments WHERE state=:state ORDER BY created_utc_ms"));
  q.bindValue(":state", text(state));
  if (!q.exec()) return out;
  while (q.next()) out.push_back(readSegment(q));
  return out;
}

std::optional<RecordingSegment> Store::getSegment(const QString& id) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM recording_segments WHERE id=:id"));
  q.bindValue(":id", text(id));
  if (!q.exec() || !q.next()) return std::nullopt;
  return readSegment(q);
}

std::optional<RecordingSegment> Store::getSegmentByPath(const QString& path) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM recording_segments WHERE path=:path"));
  q.bindValue(":path", text(path));
  if (!q.exec() || !q.next()) return std::nullopt;
  return readSegment(q);
}

int64_t Store::totalSegmentBytes(const QString& cameraId) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT COALESCE(SUM(bytes),0) FROM recording_segments WHERE camera_id=:id AND state='finalized'"));
  q.bindValue(":id", text(cameraId));
  if (!q.exec() || !q.next()) return 0;
  return q.value(0).toLongLong();
}

bool Store::insertGap(const ReceiveGap& g) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral(
      "INSERT INTO receive_gaps(id,camera_id,session_id,from_utc_ms,to_utc_ms,reason) VALUES(:id,:cam,:sess,:from,:to,:reason)"));
  q.bindValue(":id", text(g.id));
  q.bindValue(":cam", text(g.cameraId));
  q.bindValue(":sess", text(g.sessionId));
  q.bindValue(":from", static_cast<qlonglong>(g.fromUtcMs));
  q.bindValue(":to", static_cast<qlonglong>(g.toUtcMs));
  q.bindValue(":reason", text(g.reason));
  if (!q.exec()) { lastError_ = q.lastError().text(); return false; }
  return true;
}

bool Store::closeGap(const QString& id, int64_t toUtcMs) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE receive_gaps SET to_utc_ms=:to WHERE id=:id AND to_utc_ms=0"));
  q.bindValue(":to", static_cast<qlonglong>(toUtcMs));
  q.bindValue(":id", text(id));
  return q.exec();
}

QVector<ReceiveGap> Store::listGaps(const QString& cameraId, int64_t fromUtcMs, int64_t toUtcMs, int limit) {
  QVector<ReceiveGap> out;
  QSqlQuery q(db_);
  q.prepare(QStringLiteral(
      "SELECT * FROM receive_gaps WHERE camera_id=:id AND (:to=0 OR from_utc_ms<=:to) AND (:from=0 OR to_utc_ms>=:from OR to_utc_ms=0)"
      " ORDER BY from_utc_ms DESC LIMIT :lim"));
  q.bindValue(":id", text(cameraId));
  q.bindValue(":from", static_cast<qlonglong>(fromUtcMs));
  q.bindValue(":to", static_cast<qlonglong>(toUtcMs));
  q.bindValue(":lim", limit);
  if (!q.exec()) return out;
  while (q.next()) out.push_back(readGap(q));
  return out;
}

std::optional<QString> Store::getSetting(const QString& key) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT value_json FROM settings WHERE key=:key"));
  q.bindValue(":key", text(key));
  if (!q.exec() || !q.next()) return std::nullopt;
  return q.value(0).toString();
}

bool Store::setSetting(const QString& key, const QString& valueJson) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("INSERT INTO settings(key,value_json) VALUES(:key,:value) ON CONFLICT(key) DO UPDATE SET value_json=excluded.value_json"));
  q.bindValue(":key", text(key));
  q.bindValue(":value", text(valueJson));
  return q.exec();
}

bool Store::appendAudit(const QString& actor, const QString& action, const QString& target, const QString& detail, int64_t utcMs) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("INSERT INTO audit_log(utc_ms,actor,action,target,detail) VALUES(:utc,:actor,:action,:target,:detail)"));
  q.bindValue(":utc", static_cast<qlonglong>(utcMs));
  q.bindValue(":actor", text(actor));
  q.bindValue(":action", text(action));
  q.bindValue(":target", text(target));
  q.bindValue(":detail", text(detail));
  return q.exec();
}

}
