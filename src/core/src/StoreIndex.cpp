#include "StoreSql.h"
#include "fovea/core/Store.h"
#include "fovea/core/VectorStore.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

namespace fovea::core {

using sql::integer;
using sql::text;

namespace sql {

QStringList indexSchemaStatements() {
  return {
      QStringLiteral("ALTER TABLE cameras ADD COLUMN index_enabled INTEGER NOT NULL DEFAULT 1"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS index_versions(hash TEXT PRIMARY KEY, name TEXT NOT NULL, model_id TEXT NOT NULL,"
                     " model_revision TEXT NOT NULL DEFAULT '', dims INTEGER NOT NULL, dtype TEXT NOT NULL DEFAULT '',"
                     " sample_interval_ms INTEGER NOT NULL, descriptor_json TEXT NOT NULL, created_utc_ms INTEGER NOT NULL,"
                     " deleted_utc_ms INTEGER NOT NULL DEFAULT 0)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS index_jobs(id INTEGER PRIMARY KEY AUTOINCREMENT, segment_id TEXT NOT NULL,"
                     " camera_id TEXT NOT NULL, index_version TEXT NOT NULL, state TEXT NOT NULL, reason TEXT NOT NULL DEFAULT '',"
                     " attempts INTEGER NOT NULL DEFAULT 0, generation INTEGER NOT NULL DEFAULT 0, sample_interval_ms INTEGER NOT NULL,"
                     " frames_expected INTEGER NOT NULL DEFAULT 0, frames_indexed INTEGER NOT NULL DEFAULT 0,"
                     " next_attempt_utc_ms INTEGER NOT NULL DEFAULT 0, compute_ms INTEGER NOT NULL DEFAULT 0,"
                     " footage_ms INTEGER NOT NULL DEFAULT 0, created_utc_ms INTEGER NOT NULL, updated_utc_ms INTEGER NOT NULL,"
                     " UNIQUE(segment_id, index_version))"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_index_jobs_pick ON index_jobs(index_version, state, id)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_index_jobs_camera ON index_jobs(camera_id, state)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS embedding_records(id INTEGER PRIMARY KEY AUTOINCREMENT, index_version TEXT NOT NULL,"
                     " camera_id TEXT NOT NULL, session_id TEXT NOT NULL, segment_id TEXT NOT NULL, job_id INTEGER NOT NULL,"
                     " generation INTEGER NOT NULL, pts_ns INTEGER NOT NULL, utc_ms INTEGER NOT NULL,"
                     " vector_file TEXT NOT NULL DEFAULT '', vector_offset INTEGER NOT NULL DEFAULT -1,"
                     " thumbnail_path TEXT NOT NULL DEFAULT '', deleted INTEGER NOT NULL DEFAULT 0)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_embeddings_search ON embedding_records(index_version, camera_id, utc_ms) WHERE deleted=0"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_embeddings_version_time ON embedding_records(index_version, utc_ms) WHERE deleted=0"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_embeddings_segment ON embedding_records(segment_id)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_embeddings_job ON embedding_records(job_id, generation)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_embeddings_file ON embedding_records(vector_file, vector_offset)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS search_sessions(id TEXT PRIMARY KEY, query TEXT NOT NULL, filters_json TEXT NOT NULL,"
                     " index_version TEXT NOT NULL, model TEXT NOT NULL, created_utc_ms INTEGER NOT NULL, stats_json TEXT NOT NULL,"
                     " results_json TEXT NOT NULL)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS imports(id TEXT PRIMARY KEY, camera_id TEXT NOT NULL, session_id TEXT NOT NULL DEFAULT '',"
                     " source_path TEXT NOT NULL, start_utc_ms INTEGER NOT NULL, state TEXT NOT NULL, error TEXT NOT NULL DEFAULT '',"
                     " codec TEXT NOT NULL DEFAULT '', duration_ns INTEGER NOT NULL DEFAULT 0, progress REAL NOT NULL DEFAULT 0,"
                     " segments INTEGER NOT NULL DEFAULT 0, bytes INTEGER NOT NULL DEFAULT 0, created_utc_ms INTEGER NOT NULL,"
                     " started_utc_ms INTEGER NOT NULL DEFAULT 0, finished_utc_ms INTEGER NOT NULL DEFAULT 0)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_imports_created ON imports(created_utc_ms)"),
  };
}

}

namespace {

// Grid points of [a, b] for positive a and b, as SQL.
QString gridSql(const QString& a, const QString& b) {
  return QStringLiteral("(CASE WHEN %2>=%1 THEN %2/:interval-(%1-1)/:interval ELSE 0 END)").arg(a, b);
}

QString compactJson(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }
QString compactJson(const QJsonArray& a) { return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)); }

// ":c0,:c1,..." for an IN list, with the binds.
QString inList(const QStringList& values, const QString& prefix, QVariantMap* binds) {
  QStringList names;
  for (qsizetype i = 0; i < values.size(); ++i) {
    const QString name = QStringLiteral(":%1%2").arg(prefix).arg(i);
    names.push_back(name);
    binds->insert(name, text(values[i]));
  }
  return names.join(QLatin1Char(','));
}

// Binds the placeholders the prepared statement uses.
void bindAll(QSqlQuery& q, const QVariantMap& binds) {
  const QString statement = q.lastQuery();
  for (auto it = binds.cbegin(); it != binds.cend(); ++it)
    if (statement.contains(it.key())) q.bindValue(it.key(), it.value());
}

IndexVersion readVersion(const QSqlQuery& q) {
  IndexVersion v;
  v.hash = q.value("hash").toString();
  v.name = q.value("name").toString();
  v.modelId = q.value("model_id").toString();
  v.modelRevision = q.value("model_revision").toString();
  v.dims = q.value("dims").toInt();
  v.dtype = q.value("dtype").toString();
  v.sampleIntervalMs = q.value("sample_interval_ms").toInt();
  v.descriptor = QJsonDocument::fromJson(q.value("descriptor_json").toString().toUtf8()).object();
  v.createdUtcMs = q.value("created_utc_ms").toLongLong();
  v.deletedUtcMs = q.value("deleted_utc_ms").toLongLong();
  return v;
}

IndexJob readJob(const QSqlQuery& q) {
  IndexJob j;
  j.id = q.value("id").toLongLong();
  j.segmentId = q.value("segment_id").toString();
  j.cameraId = q.value("camera_id").toString();
  j.indexVersion = q.value("index_version").toString();
  j.state = q.value("state").toString();
  j.reason = q.value("reason").toString();
  j.attempts = q.value("attempts").toInt();
  j.generation = q.value("generation").toLongLong();
  j.sampleIntervalMs = q.value("sample_interval_ms").toInt();
  j.framesExpected = q.value("frames_expected").toInt();
  j.framesIndexed = q.value("frames_indexed").toInt();
  j.nextAttemptUtcMs = q.value("next_attempt_utc_ms").toLongLong();
  j.computeMs = q.value("compute_ms").toLongLong();
  j.footageMs = q.value("footage_ms").toLongLong();
  j.createdUtcMs = q.value("created_utc_ms").toLongLong();
  j.updatedUtcMs = q.value("updated_utc_ms").toLongLong();
  return j;
}

EmbeddingRecord readRecord(const QSqlQuery& q) {
  EmbeddingRecord r;
  r.id = q.value("id").toLongLong();
  r.indexVersion = q.value("index_version").toString();
  r.cameraId = q.value("camera_id").toString();
  r.sessionId = q.value("session_id").toString();
  r.segmentId = q.value("segment_id").toString();
  r.jobId = q.value("job_id").toLongLong();
  r.generation = q.value("generation").toLongLong();
  r.ptsNs = q.value("pts_ns").toLongLong();
  r.utcMs = q.value("utc_ms").toLongLong();
  r.vectorFile = q.value("vector_file").toString();
  r.vectorOffset = q.value("vector_offset").toLongLong();
  r.thumbnailPath = q.value("thumbnail_path").toString();
  r.deleted = q.value("deleted").toBool();
  return r;
}

ImportRecord readImport(const QSqlQuery& q) {
  ImportRecord r;
  r.id = q.value("id").toString();
  r.cameraId = q.value("camera_id").toString();
  r.sessionId = q.value("session_id").toString();
  r.sourcePath = q.value("source_path").toString();
  r.startUtcMs = q.value("start_utc_ms").toLongLong();
  r.state = q.value("state").toString();
  r.error = q.value("error").toString();
  r.codec = q.value("codec").toString();
  r.durationNs = q.value("duration_ns").toLongLong();
  r.progress = q.value("progress").toDouble();
  r.segments = q.value("segments").toInt();
  r.bytes = q.value("bytes").toLongLong();
  r.createdUtcMs = q.value("created_utc_ms").toLongLong();
  r.startedUtcMs = q.value("started_utc_ms").toLongLong();
  r.finishedUtcMs = q.value("finished_utc_ms").toLongLong();
  return r;
}

bool insideDir(const QString& path, const QString& dir) {
  if (dir.isEmpty() || path.isEmpty()) return false;
  const QString root = QDir::cleanPath(QFileInfo(dir).absoluteFilePath()) + QLatin1Char('/');
  const QString target = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef _WIN32
  return target.startsWith(root, Qt::CaseInsensitive);
#else
  return target.startsWith(root);
#endif
}

}

bool Store::markEmbeddingsDeleted(const QString& condition, const QVariantMap& binds, MarkedEvidence* marked) {
  QStringList thumbnails;
  {
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("SELECT thumbnail_path FROM embedding_records WHERE deleted=0 AND thumbnail_path!='' AND ") + condition);
    bindAll(q, binds);
    if (!q.exec()) {
      lastError_ = q.lastError().text();
      return false;
    }
    while (q.next()) thumbnails.push_back(q.value(0).toString());
  }
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE embedding_records SET deleted=1, thumbnail_path='' WHERE deleted=0 AND ") + condition);
  bindAll(q, binds);
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return false;
  }
  if (marked) {
    marked->embeddings += q.numRowsAffected();
    marked->indexThumbnails += thumbnails;
  }
  return true;
}

bool Store::skipJobsOfDeletedSegments(const QString& condition, const QVariantMap& binds, int64_t nowUtcMs) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE index_jobs SET state='skipped', reason='segment_deleted', updated_utc_ms=:now"
                           " WHERE state IN ('queued','failed') AND ") + condition);
  bindAll(q, binds);
  q.bindValue(":now", integer(nowUtcMs));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return false;
  }
  return true;
}

bool Store::upsertIndexVersion(const IndexVersion& v) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral(
      "INSERT INTO index_versions(hash,name,model_id,model_revision,dims,dtype,sample_interval_ms,descriptor_json,created_utc_ms)"
      " VALUES(:hash,:name,:model,:revision,:dims,:dtype,:interval,:descriptor,:created)"
      " ON CONFLICT(hash) DO UPDATE SET deleted_utc_ms=0"));
  q.bindValue(":hash", text(v.hash));
  q.bindValue(":name", text(v.name));
  q.bindValue(":model", text(v.modelId));
  q.bindValue(":revision", text(v.modelRevision));
  q.bindValue(":dims", v.dims);
  q.bindValue(":dtype", text(v.dtype));
  q.bindValue(":interval", v.sampleIntervalMs);
  q.bindValue(":descriptor", text(compactJson(v.descriptor)));
  q.bindValue(":created", integer(v.createdUtcMs));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return false;
  }
  return true;
}

std::optional<IndexVersion> Store::getIndexVersion(const QString& hash) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM index_versions WHERE hash=:hash AND deleted_utc_ms=0"));
  q.bindValue(":hash", text(hash));
  if (!q.exec() || !q.next()) return std::nullopt;
  return readVersion(q);
}

std::optional<IndexVersion> Store::findIndexVersion(const QString& name, int sampleIntervalMs) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM index_versions WHERE name=:name AND sample_interval_ms=:interval AND deleted_utc_ms=0"
                           " ORDER BY created_utc_ms DESC LIMIT 1"));
  q.bindValue(":name", text(name));
  q.bindValue(":interval", sampleIntervalMs);
  if (!q.exec() || !q.next()) return std::nullopt;
  return readVersion(q);
}

QVector<IndexVersion> Store::listIndexVersions() {
  QVector<IndexVersion> out;
  QSqlQuery q(db_);
  if (!q.exec(QStringLiteral("SELECT * FROM index_versions WHERE deleted_utc_ms=0 ORDER BY created_utc_ms"))) {
    lastError_ = q.lastError().text();
    return out;
  }
  while (q.next()) out.push_back(readVersion(q));
  return out;
}

bool Store::deleteIndexVersion(const QString& hash, int64_t nowUtcMs) {
  return transact([&] {
    QSqlQuery version(db_);
    version.prepare(QStringLiteral("UPDATE index_versions SET deleted_utc_ms=:now WHERE hash=:hash AND deleted_utc_ms=0"));
    version.bindValue(":now", integer(nowUtcMs));
    version.bindValue(":hash", text(hash));
    if (!version.exec() || version.numRowsAffected() != 1) {
      lastError_ = version.lastError().isValid() ? version.lastError().text() : QStringLiteral("no such index version");
      return false;
    }
    for (const QString& table : {QStringLiteral("embedding_records"), QStringLiteral("index_jobs")}) {
      QSqlQuery del(db_);
      del.prepare(QStringLiteral("DELETE FROM %1 WHERE index_version=:hash").arg(table));
      del.bindValue(":hash", text(hash));
      if (!del.exec()) {
        lastError_ = del.lastError().text();
        return false;
      }
    }
    return true;
  });
}

int Store::queueIndexJobs(const IndexVersion& v, int64_t nowUtcMs) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral(
      "INSERT INTO index_jobs(segment_id,camera_id,index_version,state,sample_interval_ms,frames_expected,created_utc_ms,updated_utc_ms)"
      " SELECT s.id, s.camera_id, :version, 'queued', :interval, ") +
            gridSql(QStringLiteral("s.start_utc_ms"), QStringLiteral("(s.end_utc_ms-1)")) +
            QStringLiteral(", :now, :now FROM recording_segments s JOIN cameras c ON c.id=s.camera_id"
                           " WHERE s.state='finalized' AND s.start_utc_ms>0 AND s.end_utc_ms>s.start_utc_ms AND c.deleted_utc_ms=0"
                           " AND c.index_enabled=1 AND NOT EXISTS(SELECT 1 FROM index_jobs j WHERE j.segment_id=s.id"
                           " AND j.index_version=:version) ORDER BY s.start_utc_ms, s.id"));
  q.bindValue(":version", text(v.hash));
  q.bindValue(":interval", v.sampleIntervalMs);
  q.bindValue(":now", integer(nowUtcMs));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return 0;
  }
  return q.numRowsAffected();
}

std::optional<IndexJob> Store::nextIndexJob(const QString& version, int64_t nowUtcMs, int maxAttempts) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral(
      "SELECT j.* FROM index_jobs j JOIN recording_segments s ON s.id=j.segment_id JOIN cameras c ON c.id=j.camera_id"
      " WHERE j.index_version=:version AND s.state='finalized' AND c.deleted_utc_ms=0 AND c.index_enabled=1"
      " AND (j.state='queued' OR (j.state='failed' AND j.attempts<:max AND j.next_attempt_utc_ms<=:now))"
      " ORDER BY j.id LIMIT 1"));
  q.bindValue(":version", text(version));
  q.bindValue(":max", maxAttempts);
  q.bindValue(":now", integer(nowUtcMs));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return std::nullopt;
  }
  if (!q.next()) return std::nullopt;
  return readJob(q);
}

std::optional<IndexJob> Store::beginIndexJob(int64_t jobId, int64_t nowUtcMs, MarkedEvidence* stale) {
  MarkedEvidence local;
  const bool ok = transact([&] {
    if (!markEmbeddingsDeleted(QStringLiteral("job_id=:job"), {{QStringLiteral(":job"), integer(jobId)}}, &local)) return false;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("UPDATE index_jobs SET state='running', reason='', generation=generation+1, frames_indexed=0,"
                             " updated_utc_ms=:now WHERE id=:id AND state IN ('queued','failed')"));
    q.bindValue(":now", integer(nowUtcMs));
    q.bindValue(":id", integer(jobId));
    if (!q.exec() || q.numRowsAffected() != 1) {
      lastError_ = q.lastError().isValid() ? q.lastError().text() : QStringLiteral("job is not waiting");
      return false;
    }
    return true;
  });
  if (!ok) return std::nullopt;
  if (stale) {
    stale->embeddings += local.embeddings;
    stale->indexThumbnails += local.indexThumbnails;
  }
  return getIndexJob(jobId);
}

bool Store::finishIndexJob(const IndexJob& job, const QString& state, const QString& reason, int64_t nowUtcMs, MarkedEvidence* stale) {
  MarkedEvidence local;
  const bool ok = transact([&] {
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("UPDATE index_jobs SET state=:state, reason=:reason, attempts=:attempts, next_attempt_utc_ms=:next,"
                             " frames_expected=:expected, compute_ms=:compute, footage_ms=:footage, updated_utc_ms=:now"
                             " WHERE id=:id AND generation=:gen AND state='running'"));
    q.bindValue(":state", text(state));
    q.bindValue(":reason", text(reason));
    q.bindValue(":attempts", job.attempts);
    q.bindValue(":next", integer(job.nextAttemptUtcMs));
    q.bindValue(":expected", job.framesExpected);
    q.bindValue(":compute", integer(job.computeMs));
    q.bindValue(":footage", integer(job.footageMs));
    q.bindValue(":now", integer(nowUtcMs));
    q.bindValue(":id", integer(job.id));
    q.bindValue(":gen", integer(job.generation));
    if (!q.exec() || q.numRowsAffected() != 1) {
      lastError_ = q.lastError().isValid() ? q.lastError().text() : QStringLiteral("attempt is no longer running");
      return false;
    }
    if (state == QLatin1String("done")) return true;
    return markEmbeddingsDeleted(QStringLiteral("job_id=:job AND generation=:gen"),
                                 {{QStringLiteral(":job"), integer(job.id)}, {QStringLiteral(":gen"), integer(job.generation)}}, &local);
  });
  if (ok && stale) {
    stale->embeddings += local.embeddings;
    stale->indexThumbnails += local.indexThumbnails;
  }
  return ok;
}

int Store::recoverIndexJobs(int64_t nowUtcMs, int maxAttempts, MarkedEvidence* stale) {
  int requeued = 0;
  MarkedEvidence local;
  const bool ok = transact([&] {
    QSqlQuery q(db_);
    // A crash counts as an attempt: a segment whose decode takes the core down
    // would otherwise be picked first again at every start.
    q.prepare(QStringLiteral("UPDATE index_jobs SET state=CASE WHEN attempts+1>=:max THEN 'failed' ELSE 'queued' END,"
                             " reason='core_restart', attempts=attempts+1, next_attempt_utc_ms=:now, updated_utc_ms=:now"
                             " WHERE state='running'"));
    q.bindValue(":now", integer(nowUtcMs));
    q.bindValue(":max", maxAttempts);
    if (!q.exec()) {
      lastError_ = q.lastError().text();
      return false;
    }
    requeued = q.numRowsAffected();
    // A segment deleted while its job was running leaves a job that nextIndexJob
    // never picks (it wants a finalized segment) and that no deletion will skip.
    if (!skipJobsOfDeletedSegments(
            QStringLiteral("segment_id IN (SELECT id FROM recording_segments WHERE state='deleted')"), {}, nowUtcMs))
      return false;
    return markEmbeddingsDeleted(QStringLiteral("job_id IN (SELECT id FROM index_jobs WHERE state!='done')"), {}, &local);
  });
  if (!ok) return -1;
  if (stale) {
    stale->embeddings += local.embeddings;
    stale->indexThumbnails += local.indexThumbnails;
  }
  return requeued;
}

std::optional<IndexJob> Store::getIndexJob(int64_t id) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM index_jobs WHERE id=:id"));
  q.bindValue(":id", integer(id));
  if (!q.exec() || !q.next()) return std::nullopt;
  return readJob(q);
}

QVector<IndexJob> Store::listIndexJobs(const QString& version, const QString& state, int limit) {
  QVector<IndexJob> out;
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM index_jobs WHERE index_version=:version AND (:state='' OR state=:state)"
                           " ORDER BY updated_utc_ms DESC, id DESC LIMIT :limit"));
  q.bindValue(":version", text(version));
  q.bindValue(":state", text(state));
  q.bindValue(":limit", limit);
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return out;
  }
  while (q.next()) out.push_back(readJob(q));
  return out;
}

IndexQueueStats Store::indexQueueStats(const QString& version) {
  IndexQueueStats s;
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT state, COUNT(*), COALESCE(SUM(compute_ms),0), COALESCE(SUM(footage_ms),0),"
                           " COALESCE(SUM(frames_indexed),0) FROM index_jobs WHERE index_version=:version GROUP BY state"));
  q.bindValue(":version", text(version));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return s;
  }
  while (q.next()) {
    const QString state = q.value(0).toString();
    const int count = q.value(1).toInt();
    if (state == QLatin1String("queued")) s.queued = count;
    else if (state == QLatin1String("running")) s.running = count;
    else if (state == QLatin1String("failed")) s.failed = count;
    else if (state == QLatin1String("skipped")) s.skipped = count;
    else if (state == QLatin1String("done")) {
      s.done = count;
      s.computeMs = q.value(2).toLongLong();
      s.footageMs = q.value(3).toLongLong();
      s.framesIndexed = q.value(4).toLongLong();
    }
  }
  return s;
}

bool Store::appendEmbeddings(const IndexJob& job, QVector<EmbeddingRecord>& records,
                             const std::function<bool(QVector<EmbeddingRecord>&)>& writeVectors) {
  return transact([&] {
    QSqlQuery check(db_);
    check.prepare(QStringLiteral("SELECT 1 FROM index_jobs j JOIN recording_segments s ON s.id=j.segment_id"
                                 " WHERE j.id=:id AND j.generation=:gen AND j.state='running' AND s.state='finalized'"));
    check.bindValue(":id", integer(job.id));
    check.bindValue(":gen", integer(job.generation));
    if (!check.exec() || !check.next()) {
      lastError_ = check.lastError().isValid() ? check.lastError().text() : QStringLiteral("attempt is no longer running");
      return false;
    }
    QSqlQuery insert(db_);
    insert.prepare(QStringLiteral(
        "INSERT INTO embedding_records(index_version,camera_id,session_id,segment_id,job_id,generation,pts_ns,utc_ms,thumbnail_path)"
        " VALUES(:version,:cam,:session,:segment,:job,:gen,:pts,:utc,:thumb)"));
    for (EmbeddingRecord& r : records) {
      insert.bindValue(":version", text(job.indexVersion));
      insert.bindValue(":cam", text(job.cameraId));
      insert.bindValue(":session", text(r.sessionId));
      insert.bindValue(":segment", text(job.segmentId));
      insert.bindValue(":job", integer(job.id));
      insert.bindValue(":gen", integer(job.generation));
      insert.bindValue(":pts", integer(r.ptsNs));
      insert.bindValue(":utc", integer(r.utcMs));
      insert.bindValue(":thumb", text(r.thumbnailPath));
      if (!insert.exec()) {
        lastError_ = insert.lastError().text();
        return false;
      }
      r.id = insert.lastInsertId().toLongLong();
      r.indexVersion = job.indexVersion;
      r.cameraId = job.cameraId;
      r.segmentId = job.segmentId;
      r.jobId = job.id;
      r.generation = job.generation;
    }
    if (!writeVectors(records)) {
      lastError_ = QStringLiteral("cannot write vectors");
      return false;
    }
    QSqlQuery place(db_);
    place.prepare(QStringLiteral("UPDATE embedding_records SET vector_file=:file, vector_offset=:offset WHERE id=:id"));
    for (const EmbeddingRecord& r : records) {
      place.bindValue(":file", text(r.vectorFile));
      place.bindValue(":offset", integer(r.vectorOffset));
      place.bindValue(":id", integer(r.id));
      if (!place.exec()) {
        lastError_ = place.lastError().text();
        return false;
      }
    }
    QSqlQuery count(db_);
    count.prepare(QStringLiteral("UPDATE index_jobs SET frames_indexed=frames_indexed+:n WHERE id=:id"));
    count.bindValue(":n", static_cast<int>(records.size()));
    count.bindValue(":id", integer(job.id));
    if (!count.exec()) {
      lastError_ = count.lastError().text();
      return false;
    }
    return true;
  });
}

std::optional<EmbeddingRecord> Store::getEmbeddingRecord(int64_t id) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM embedding_records WHERE id=:id"));
  q.bindValue(":id", integer(id));
  if (!q.exec() || !q.next()) return std::nullopt;
  return readRecord(q);
}

QSet<int64_t> Store::liveEmbeddingIds(const QVector<int64_t>& ids) {
  QSet<int64_t> live;
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT deleted FROM embedding_records WHERE id=:id"));
  for (const int64_t id : ids) {
    q.bindValue(":id", integer(id));
    if (q.exec() && q.next() && !q.value(0).toBool()) live.insert(id);
  }
  return live;
}

bool Store::forEachSearchRow(const QString& version, const QStringList& cameraIds, int64_t fromUtcMs, int64_t toUtcMs,
                            const std::function<void(const QString&, int64_t, int64_t, int64_t)>& row) {
  QStringList cameras = cameraIds;
  if (cameras.isEmpty()) {
    QSqlQuery all(db_);
    if (!all.exec(QStringLiteral("SELECT id FROM cameras WHERE deleted_utc_ms=0 ORDER BY created_utc_ms"))) {
      lastError_ = all.lastError().text();
      return false;
    }
    while (all.next()) cameras.push_back(all.value(0).toString());
  }
  // One camera at a time in time order: that is how idx_embeddings_search
  // stores the rows, so nothing is sorted and the rows arrive grouped by the
  // vector file they live in. The whole range is never held in memory.
  QSqlQuery q(db_);
  q.setForwardOnly(true);
  q.prepare(QStringLiteral("SELECT vector_file, id, utc_ms, vector_offset FROM embedding_records"
                           " WHERE index_version=:version AND camera_id=:camera AND deleted=0 AND vector_file!=''"
                           " AND utc_ms>=:from AND utc_ms<=:to ORDER BY utc_ms"));
  for (const QString& camera : std::as_const(cameras)) {
    q.bindValue(":version", text(version));
    q.bindValue(":camera", text(camera));
    q.bindValue(":from", integer(fromUtcMs));
    q.bindValue(":to", integer(toUtcMs));
    if (!q.exec()) {
      lastError_ = q.lastError().text();
      return false;
    }
    while (q.next()) row(q.value(0).toString(), q.value(1).toLongLong(), q.value(2).toLongLong(), q.value(3).toLongLong());
  }
  return true;
}

QVector<VectorRow> Store::embeddingRowsByIds(const QVector<int64_t>& ids) {
  QVector<VectorRow> out;
  if (ids.isEmpty()) return out;
  QStringList names;
  QSqlQuery q(db_);
  for (qsizetype i = 0; i < ids.size(); ++i) names.push_back(QStringLiteral(":i%1").arg(i));
  q.prepare(QStringLiteral("SELECT id, camera_id, segment_id, utc_ms, vector_file, vector_offset FROM embedding_records"
                           " WHERE id IN (%1)").arg(names.join(QLatin1Char(','))));
  for (qsizetype i = 0; i < ids.size(); ++i) q.bindValue(names[i], integer(ids[i]));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return out;
  }
  while (q.next())
    out.push_back({q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toString(), q.value(3).toLongLong(),
                   q.value(4).toString(), q.value(5).toLongLong()});
  return out;
}

int Store::dropUnreadableRecords(const QVector<int64_t>& ids, MarkedEvidence* marked) {
  if (ids.isEmpty()) return 0;
  QVariantMap binds;
  QStringList names;
  for (qsizetype i = 0; i < ids.size(); ++i) {
    const QString name = QStringLiteral(":i%1").arg(i);
    names.push_back(name);
    binds.insert(name, integer(ids[i]));
  }
  const QString condition = QStringLiteral("id IN (%1)").arg(names.join(QLatin1Char(',')));
  MarkedEvidence local;
  const bool ok = transact([&] {
    if (!markEmbeddingsDeleted(condition, binds, &local)) return false;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("UPDATE embedding_records SET vector_file='', vector_offset=-1 WHERE ") + condition);
    bindAll(q, binds);
    if (!q.exec()) {
      lastError_ = q.lastError().text();
      return false;
    }
    return true;
  });
  if (!ok) return -1;
  if (marked) {
    marked->embeddings += local.embeddings;
    marked->indexThumbnails += local.indexThumbnails;
  }
  return local.embeddings;
}

int Store::requeueJobsMissingRecords(int64_t nowUtcMs) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral(
      "UPDATE index_jobs SET state='queued', reason='vector_repair', attempts=0, next_attempt_utc_ms=0, updated_utc_ms=:now"
      " WHERE state='done' AND frames_indexed>0 AND id IN (SELECT j.id FROM index_jobs j"
      " JOIN recording_segments s ON s.id=j.segment_id WHERE j.state='done' AND s.state='finalized'"
      " AND (SELECT COUNT(*) FROM embedding_records e WHERE e.job_id=j.id AND e.generation=j.generation AND e.deleted=0)"
      " < j.frames_indexed)"));
  q.bindValue(":now", integer(nowUtcMs));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return 0;
  }
  return q.numRowsAffected();
}

QStringList Store::deletedIndexVersions() {
  QStringList out;
  QSqlQuery q(db_);
  if (!q.exec(QStringLiteral("SELECT hash FROM index_versions WHERE deleted_utc_ms!=0"))) {
    lastError_ = q.lastError().text();
    return out;
  }
  while (q.next()) out.push_back(q.value(0).toString());
  return out;
}

std::pair<int64_t, int64_t> Store::footageRange(const QStringList& cameraIds) {
  QVariantMap binds;
  const QString cameras = cameraIds.isEmpty() ? QString() : QStringLiteral(" AND camera_id IN (%1)").arg(inList(cameraIds, "c", &binds));
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT COALESCE(MIN(start_utc_ms),0), COALESCE(MAX(end_utc_ms),0) FROM recording_segments"
                           " WHERE state='finalized' AND start_utc_ms>0 AND end_utc_ms>start_utc_ms") +
            cameras);
  bindAll(q, binds);
  if (!q.exec() || !q.next()) {
    lastError_ = q.lastError().text();
    return {0, 0};
  }
  return {q.value(0).toLongLong(), q.value(1).toLongLong()};
}

std::pair<int64_t, int64_t> Store::embeddingRowStats(const QString& version) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT COUNT(*), COALESCE(SUM(length(index_version)+length(camera_id)+length(session_id)+length(segment_id)"
                           "+length(vector_file)+length(thumbnail_path)+56),0) FROM embedding_records WHERE index_version=:version AND deleted=0"));
  q.bindValue(":version", text(version));
  if (!q.exec() || !q.next()) {
    lastError_ = q.lastError().text();
    return {0, 0};
  }
  return {q.value(0).toLongLong(), q.value(1).toLongLong()};
}

CoverageCount Store::indexCoverage(const IndexVersion& v, const QStringList& cameraIds, int64_t fromUtcMs, int64_t toUtcMs) {
  CoverageCount c;
  QVariantMap binds{{QStringLiteral(":interval"), v.sampleIntervalMs},
                    {QStringLiteral(":from"), integer(fromUtcMs)},
                    {QStringLiteral(":to"), integer(toUtcMs)},
                    {QStringLiteral(":version"), text(v.hash)}};
  const QString cameras = cameraIds.isEmpty() ? QString() : QStringLiteral(" AND camera_id IN (%1)").arg(inList(cameraIds, "c", &binds));
  QSqlQuery expected(db_);
  expected.prepare(QStringLiteral("SELECT COALESCE(SUM(") +
                   gridSql(QStringLiteral("MAX(start_utc_ms,:from)"), QStringLiteral("MIN(end_utc_ms-1,:to)")) +
                   QStringLiteral("),0) FROM recording_segments WHERE state='finalized' AND start_utc_ms>0 AND end_utc_ms>start_utc_ms"
                                  " AND start_utc_ms<=:to AND end_utc_ms>:from") +
                   cameras);
  bindAll(expected, binds);
  if (!expected.exec() || !expected.next()) {
    lastError_ = expected.lastError().text();
    return c;
  }
  c.expected = expected.value(0).toLongLong();
  QSqlQuery indexed(db_);
  indexed.prepare(QStringLiteral("SELECT COUNT(*) FROM embedding_records WHERE index_version=:version AND deleted=0"
                                 " AND utc_ms>=:from AND utc_ms<=:to") +
                  cameras);
  bindAll(indexed, binds);
  if (!indexed.exec() || !indexed.next()) {
    lastError_ = indexed.lastError().text();
    return c;
  }
  c.indexed = indexed.value(0).toLongLong();
  return c;
}

QVector<CameraCoverage> Store::coverageByCamera(const IndexVersion& v) {
  QVector<CameraCoverage> out;
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT c.id, COALESCE((SELECT SUM(") +
            gridSql(QStringLiteral("s.start_utc_ms"), QStringLiteral("(s.end_utc_ms-1)")) +
            QStringLiteral(") FROM recording_segments s WHERE s.camera_id=c.id AND s.state='finalized' AND s.start_utc_ms>0"
                           " AND s.end_utc_ms>s.start_utc_ms),0), (SELECT COUNT(*) FROM embedding_records e WHERE e.camera_id=c.id"
                           " AND e.index_version=:version AND e.deleted=0) FROM cameras c WHERE c.deleted_utc_ms=0"
                           " ORDER BY c.created_utc_ms"));
  q.bindValue(":interval", v.sampleIntervalMs);
  q.bindValue(":version", text(v.hash));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return out;
  }
  while (q.next()) out.push_back({q.value(0).toString(), {q.value(1).toLongLong(), q.value(2).toLongLong()}});
  return out;
}

QVector<VectorFileUsage> Store::vectorFileUsage() {
  QVector<VectorFileUsage> out;
  QSqlQuery q(db_);
  if (!q.exec(QStringLiteral("SELECT vector_file, MIN(index_version), SUM(CASE WHEN deleted=0 THEN 1 ELSE 0 END)"
                             " FROM embedding_records WHERE vector_file!='' GROUP BY vector_file"))) {
    lastError_ = q.lastError().text();
    return out;
  }
  while (q.next()) out.push_back({q.value(0).toString(), q.value(1).toString(), q.value(2).toLongLong()});
  return out;
}

QVector<std::pair<int64_t, int64_t>> Store::liveFileRecords(const QString& vectorFile) {
  QVector<std::pair<int64_t, int64_t>> out;
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT id, vector_offset FROM embedding_records WHERE vector_file=:file AND deleted=0 ORDER BY vector_offset"));
  q.bindValue(":file", text(vectorFile));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return out;
  }
  while (q.next()) out.push_back({q.value(0).toLongLong(), q.value(1).toLongLong()});
  return out;
}

bool Store::moveFileRecords(const QString& fromFile, const QString& toFile, const QVector<std::pair<int64_t, int64_t>>& moved,
                            const QVector<int64_t>& newOffsets) {
  if (moved.size() != newOffsets.size()) {
    lastError_ = QStringLiteral("offset count mismatch");
    return false;
  }
  return transact([&] {
    QSqlQuery move(db_);
    move.prepare(QStringLiteral("UPDATE embedding_records SET vector_file=:to, vector_offset=:new WHERE id=:id AND vector_file=:from"
                                " AND vector_offset=:old"));
    for (qsizetype i = 0; i < moved.size(); ++i) {
      move.bindValue(":to", text(toFile));
      move.bindValue(":new", integer(newOffsets[i]));
      move.bindValue(":id", integer(moved[i].first));
      move.bindValue(":from", text(fromFile));
      move.bindValue(":old", integer(moved[i].second));
      if (!move.exec()) {
        lastError_ = move.lastError().text();
        return false;
      }
    }
    QSqlQuery rest(db_);
    rest.prepare(QStringLiteral("UPDATE embedding_records SET vector_file='', vector_offset=-1, deleted=1 WHERE vector_file=:from"));
    rest.bindValue(":from", text(fromFile));
    if (!rest.exec()) {
      lastError_ = rest.lastError().text();
      return false;
    }
    return true;
  });
}

int Store::dropRecordsPastEnd(const QString& vectorFile, int64_t fileBytes, int dims, MarkedEvidence* marked) {
  const QString condition = fileBytes < 0 ? QStringLiteral("vector_file=:file")
                                          : QStringLiteral("vector_file=:file AND vector_offset+:rec>:bytes");
  const QVariantMap binds{{QStringLiteral(":file"), text(vectorFile)},
                          {QStringLiteral(":rec"), integer(VectorStore::recordBytes(dims))},
                          {QStringLiteral(":bytes"), integer(fileBytes)}};
  MarkedEvidence local;
  const bool ok = transact([&] {
    if (!markEmbeddingsDeleted(condition, binds, &local)) return false;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("UPDATE embedding_records SET vector_file='', vector_offset=-1 WHERE ") + condition);
    bindAll(q, binds);
    if (!q.exec()) {
      lastError_ = q.lastError().text();
      return false;
    }
    return true;
  });
  if (!ok) return -1;
  if (marked) {
    marked->embeddings += local.embeddings;
    marked->indexThumbnails += local.indexThumbnails;
  }
  return local.embeddings;
}

QStringList Store::referencedVectorFiles() {
  QStringList out;
  QSqlQuery q(db_);
  if (!q.exec(QStringLiteral("SELECT DISTINCT vector_file FROM embedding_records WHERE vector_file!=''"))) {
    lastError_ = q.lastError().text();
    return out;
  }
  while (q.next()) out.push_back(q.value(0).toString());
  return out;
}

int Store::removeIndexFiles(const QStringList& paths, const QString& indexDir) {
  int removed = 0;
  QSet<QString> dirs;
  for (const QString& path : paths) {
    if (!insideDir(path, indexDir) || !QFile::remove(path)) continue;
    ++removed;
    dirs.insert(QFileInfo(path).path());
  }
  for (const QString& dir : std::as_const(dirs)) QDir().rmdir(dir);
  return removed;
}

bool Store::insertSearchSession(const SearchSessionRecord& s) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("INSERT INTO search_sessions(id,query,filters_json,index_version,model,created_utc_ms,stats_json,results_json)"
                           " VALUES(:id,:query,:filters,:version,:model,:created,:stats,:results)"));
  q.bindValue(":id", text(s.id));
  q.bindValue(":query", text(s.query));
  q.bindValue(":filters", text(compactJson(s.filters)));
  q.bindValue(":version", text(s.indexVersion));
  q.bindValue(":model", text(s.model));
  q.bindValue(":created", integer(s.createdUtcMs));
  q.bindValue(":stats", text(compactJson(s.stats)));
  q.bindValue(":results", text(compactJson(s.results)));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return false;
  }
  return true;
}

std::optional<SearchSessionRecord> Store::getSearchSession(const QString& id) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM search_sessions WHERE id=:id"));
  q.bindValue(":id", text(id));
  if (!q.exec() || !q.next()) return std::nullopt;
  SearchSessionRecord s;
  s.id = q.value("id").toString();
  s.query = q.value("query").toString();
  s.filters = QJsonDocument::fromJson(q.value("filters_json").toString().toUtf8()).object();
  s.indexVersion = q.value("index_version").toString();
  s.model = q.value("model").toString();
  s.createdUtcMs = q.value("created_utc_ms").toLongLong();
  s.stats = QJsonDocument::fromJson(q.value("stats_json").toString().toUtf8()).object();
  s.results = QJsonDocument::fromJson(q.value("results_json").toString().toUtf8()).array();
  return s;
}

bool Store::insertImport(const ImportRecord& r) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("INSERT INTO imports(id,camera_id,session_id,source_path,start_utc_ms,state,error,codec,duration_ns,progress,"
                           "segments,bytes,created_utc_ms,started_utc_ms,finished_utc_ms) VALUES(:id,:cam,:session,:path,:start,:state,"
                           ":error,:codec,:duration,:progress,:segments,:bytes,:created,:started,:finished)"));
  q.bindValue(":id", text(r.id));
  q.bindValue(":cam", text(r.cameraId));
  q.bindValue(":session", text(r.sessionId));
  q.bindValue(":path", text(r.sourcePath));
  q.bindValue(":start", integer(r.startUtcMs));
  q.bindValue(":state", text(r.state));
  q.bindValue(":error", text(r.error));
  q.bindValue(":codec", text(r.codec));
  q.bindValue(":duration", integer(r.durationNs));
  q.bindValue(":progress", r.progress);
  q.bindValue(":segments", r.segments);
  q.bindValue(":bytes", integer(r.bytes));
  q.bindValue(":created", integer(r.createdUtcMs));
  q.bindValue(":started", integer(r.startedUtcMs));
  q.bindValue(":finished", integer(r.finishedUtcMs));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return false;
  }
  return true;
}

bool Store::updateImport(const ImportRecord& r) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("UPDATE imports SET session_id=:session, state=:state, error=:error, codec=:codec, duration_ns=:duration,"
                           " progress=:progress, segments=:segments, bytes=:bytes, started_utc_ms=:started, finished_utc_ms=:finished"
                           " WHERE id=:id"));
  q.bindValue(":session", text(r.sessionId));
  q.bindValue(":state", text(r.state));
  q.bindValue(":error", text(r.error));
  q.bindValue(":codec", text(r.codec));
  q.bindValue(":duration", integer(r.durationNs));
  q.bindValue(":progress", r.progress);
  q.bindValue(":segments", r.segments);
  q.bindValue(":bytes", integer(r.bytes));
  q.bindValue(":started", integer(r.startedUtcMs));
  q.bindValue(":finished", integer(r.finishedUtcMs));
  q.bindValue(":id", text(r.id));
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return false;
  }
  return q.numRowsAffected() == 1;
}

std::optional<ImportRecord> Store::getImport(const QString& id) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM imports WHERE id=:id"));
  q.bindValue(":id", text(id));
  if (!q.exec() || !q.next()) return std::nullopt;
  return readImport(q);
}

QVector<ImportRecord> Store::listImports(int limit) {
  QVector<ImportRecord> out;
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT * FROM imports ORDER BY created_utc_ms DESC, id LIMIT :limit"));
  q.bindValue(":limit", limit);
  if (!q.exec()) {
    lastError_ = q.lastError().text();
    return out;
  }
  while (q.next()) out.push_back(readImport(q));
  return out;
}

bool Store::cameraHasFootage(const QString& cameraId, int64_t fromUtcMs, int64_t toUtcMs) {
  QSqlQuery q(db_);
  q.prepare(QStringLiteral("SELECT 1 FROM recording_segments WHERE camera_id=:cam AND state!='deleted' AND start_utc_ms>0"
                           " AND start_utc_ms<:to AND (CASE WHEN end_utc_ms>0 THEN end_utc_ms ELSE start_utc_ms+1 END)>:from LIMIT 1"));
  q.bindValue(":cam", text(cameraId));
  q.bindValue(":from", integer(fromUtcMs));
  q.bindValue(":to", integer(toUtcMs));
  return !q.exec() || q.next();
}

bool Store::deleteSessionSegments(const QString& sessionId, const QString& reason, int64_t nowUtcMs, MarkedEvidence* marked) {
  MarkedEvidence local;
  const bool ok = transact([&] {
    const QVariantMap session{{QStringLiteral(":session"), text(sessionId)}};
    const QString bySegment = QStringLiteral("segment_id IN (SELECT id FROM recording_segments WHERE session_id=:session)");
    QStringList segmentIds;
    {
      QSqlQuery ids(db_);
      ids.prepare(QStringLiteral("SELECT id FROM recording_segments WHERE session_id=:session AND state!='deleted'"));
      ids.bindValue(":session", text(sessionId));
      if (!ids.exec()) {
        lastError_ = ids.lastError().text();
        return false;
      }
      while (ids.next()) segmentIds.push_back(ids.value(0).toString());
    }
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("UPDATE recording_segments SET state='deleted', deleted_utc_ms=:now, delete_reason=:reason"
                             " WHERE session_id=:session AND state!='deleted'"));
    q.bindValue(":now", integer(nowUtcMs));
    q.bindValue(":reason", text(reason));
    q.bindValue(":session", text(sessionId));
    if (!q.exec()) {
      lastError_ = q.lastError().text();
      return false;
    }
    for (const QString& id : segmentIds)
      if (!markEvidenceDeleted(QStringLiteral("segment_ids_json LIKE :pattern"),
                               {{QStringLiteral(":pattern"), QStringLiteral("%\"") + id + QStringLiteral("\"%")}},
                               QStringLiteral("segment %1 deleted (%2)").arg(id, reason), nowUtcMs, &local))
        return false;
    return markEmbeddingsDeleted(bySegment, session, &local) && skipJobsOfDeletedSegments(bySegment, session, nowUtcMs);
  });
  if (ok && marked) *marked = local;
  return ok;
}

}
