#include "fovea/core/EmbedClient.h"
#include "fovea/core/Index.h"
#include "fovea/core/IndexScheduler.h"
#include "fovea/core/RetentionManager.h"
#include "fovea/core/SearchService.h"
#include "fovea/core/Store.h"
#include "fovea/core/VectorStore.h"
#include "fovea/core/WorkerSupervisor.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <QUuid>
#include <cmath>
#include <limits>

using namespace fovea;
using namespace fovea::core;

namespace {

constexpr int64_t kNow = 1'757'700'000'000;
constexpr int kDims = 4;

QVector<float> axis(int i, float weight = 1.0f) {
  QVector<float> v(kDims, 0.0f);
  v[i % kDims] = weight;
  if (weight < 1.0f) v[(i + 1) % kDims] = std::sqrt(1.0f - weight * weight);
  return v;
}

int countRows(const QString& dbPath, const QString& sql) {
  const QString name = QUuid::createUuid().toString();
  int n = -1;
  {
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
    db.setDatabaseName(dbPath);
    QSqlQuery q(db);
    if (db.open() && q.exec(sql) && q.next()) n = q.value(0).toInt();
  }
  QSqlDatabase::removeDatabase(name);
  return n;
}

struct Fixture {
  QTemporaryDir dir;
  Store store;
  CoreConfig config;
  VectorStore vectors;
  IndexVersion version;

  Fixture() : vectors(dir.path() + "/index") {
    config.dataDir = dir.path();
    config.recordingsDir = dir.path() + "/recordings";
    config.minFreeBytes = 1000;
    store.open(dir.path() + "/fovea.sqlite");
    version.name = "siglip2-b16-224";
    version.modelId = "google/siglip2-base-patch16-224";
    version.modelRevision = "rev1";
    version.dims = kDims;
    version.dtype = "float32";
    version.sampleIntervalMs = 1000;
    version.descriptor = QJsonObject{{"name", version.name}, {"dims", kDims}};
    version.hash = descriptorHash(version.descriptor);
    version.createdUtcMs = kNow;
    store.upsertIndexVersion(version);
  }

  QString dbPath() const { return dir.path() + "/fovea.sqlite"; }

  void addCamera(const QString& id) {
    Camera c;
    c.id = id;
    c.name = id;
    c.kind = "file";
    c.mainUrl = "clip.mp4";
    c.createdUtcMs = kNow;
    c.updatedUtcMs = kNow;
    store.insertCamera(c);
  }

  void addSegment(const QString& cameraId, const QString& id, int64_t startUtcMs, int64_t durationMs) {
    RecordingSegment seg;
    seg.id = id;
    seg.cameraId = cameraId;
    seg.sessionId = "s-" + cameraId;
    seg.path = config.recordingsDir + "/" + cameraId + "/" + id + ".mkv";
    seg.startUtcMs = startUtcMs;
    seg.createdUtcMs = startUtcMs;
    store.insertSegment(seg);
    store.finalizeSegment(id, durationMs * 1'000'000, startUtcMs + durationMs, 100, startUtcMs + durationMs);
    QDir().mkpath(QFileInfo(seg.path).path());
    QFile f(seg.path);
    if (f.open(QIODevice::WriteOnly)) f.write(QByteArray(100, 'x'));
  }

  IndexJob jobFor(const QString& segmentId) {
    for (const IndexJob& j : store.listIndexJobs(version.hash, QString(), 1000))
      if (j.segmentId == segmentId) return j;
    return {};
  }

  // Stores one record per utc for a running attempt, the way the scheduler does.
  bool append(const IndexJob& job, const QVector<int64_t>& utcs, const QVector<QVector<float>>& vecs) {
    QVector<EmbeddingRecord> rows;
    for (qsizetype i = 0; i < utcs.size(); ++i) {
      EmbeddingRecord r;
      r.sessionId = "s-" + job.cameraId;
      r.utcMs = utcs[i];
      r.ptsNs = (utcs[i] - kNow) * 1'000'000;
      r.thumbnailPath = QStringLiteral("%1/index/%2/thumbs/%3/%4/g%5-%6.jpg")
                            .arg(dir.path(), version.hash, job.cameraId, job.segmentId)
                            .arg(job.generation)
                            .arg(i);
      QDir().mkpath(QFileInfo(r.thumbnailPath).path());
      QFile thumb(r.thumbnailPath);
      if (thumb.open(QIODevice::WriteOnly)) thumb.write("jpeg");
      rows.push_back(r);
    }
    return store.appendEmbeddings(job, rows, [&](QVector<EmbeddingRecord>& stored) {
      for (qsizetype i = 0; i < stored.size(); ++i) {
        const QString file = vectors.appendFile(version.hash, job.cameraId, stored[i].utcMs);
        QVector<int64_t> offsets;
        if (!vectors.append(file, kDims, {{stored[i].id, stored[i].utcMs, vecs[i]}}, &offsets, nullptr)) return false;
        stored[i].vectorFile = file;
        stored[i].vectorOffset = offsets[0];
      }
      return true;
    });
  }

  // Queues, begins and completes the job of a segment with the given records.
  bool index(const QString& segmentId, const QVector<int64_t>& utcs, const QVector<QVector<float>>& vecs) {
    store.queueIndexJobs(version, kNow);
    const std::optional<IndexJob> job = store.beginIndexJob(jobFor(segmentId).id, kNow, nullptr);
    if (!job || !append(*job, utcs, vecs)) return false;
    IndexJob done = *job;
    done.framesExpected = static_cast<int>(utcs.size());
    return store.finishIndexJob(done, "done", QString(), kNow, nullptr);
  }

  // The live rows a search would score, id order.
  QVector<VectorRow> rows(const QStringList& cameras = {}, int64_t from = 0, int64_t to = kNow * 2) {
    QVector<int64_t> ids;
    store.forEachSearchRow(version.hash, cameras, from, to, [&](const QString&, int64_t id, int64_t, int64_t) { ids.push_back(id); });
    std::sort(ids.begin(), ids.end());
    return store.embeddingRowsByIds(ids);
  }

  ScanRequest scan(const QVector<float>& query, const QStringList& cameras = {}, int64_t from = 0, int64_t to = kNow * 2) {
    ScanRequest r;
    r.databasePath = dbPath();
    r.indexRoot = dir.path() + "/index";
    r.version = version;
    r.cameraIds = cameras;
    r.fromUtcMs = from;
    r.toUtcMs = to;
    r.query = query;
    r.candidates = 16;
    r.candidateFraction = 1.0;
    return r;
  }
};

QJsonObject embedBody(const EmbedRequest& request, const QJsonObject& descriptor, const QVector<QVector<float>>& vecs) {
  QByteArray raw;
  for (const QVector<float>& v : vecs) raw.append(reinterpret_cast<const char*>(v.constData()), v.size() * 4);
  QJsonArray ids;
  for (const EmbedFrame& f : request.frames) ids.push_back(f.frameId);
  return QJsonObject{{"job_id", request.jobId},
                     {"generation", static_cast<double>(request.generation)},
                     {"status", "ok"},
                     {"model", request.versionName},
                     {"model_version", "google/siglip2-base-patch16-224@rev1"},
                     {"index_version", descriptorHash(descriptor)},
                     {"descriptor", descriptor},
                     {"dims", descriptor.value("dims").toInt()},
                     {"count", static_cast<int>(vecs.size())},
                     {"vectors", QString::fromLatin1(raw.toBase64())},
                     {"frame_ids", ids},
                     {"contract_violations", QJsonArray{}}};
}

}

class TestIndex : public QObject {
  Q_OBJECT
private slots:
  void mergesSamplesIntoRanges();
  void countsGridPoints();
  void migratesVersionThreeDatabase();
  void queuesAndPicksJobs();
  void restartsJobWithoutDuplicates();
  void skipsRestartedJobsOfDeletedSegments();
  void keepsRangesNextToDeletedFootageAvailable();
  void answersEmptyFilterWithoutEmbedding();
  void retentionDeletesRecords();
  void searchesWithFilters();
  void revalidatesStoredSessions();
  void checksEmbedReplies();
  void startupCheckRepairsVectorFiles();
  void compactsDeadFiles();
  void deletesImportSessions();
};

void TestIndex::mergesSamplesIntoRanges() {
  const QVector<SearchSample> samples{
      {1, "camA", "s1", 10'000, 0.5f}, {2, "camA", "s1", 11'000, 0.9f}, {3, "camA", "s1", 13'500, 0.4f},
      {4, "camA", "s1", 16'500, 0.8f}, {5, "camB", "s2", 11'500, 0.7f}, {6, "camB", "s2", 12'000, 0.2f},
  };
  const QVector<SearchRange> ranges = mergeSamples(samples, 3000, 1000);
  QCOMPARE(ranges.size(), 3);
  QCOMPARE(ranges[0].cameraId, QString("camA"));
  QCOMPARE(ranges[0].startUtcMs, 10'000);
  QCOMPARE(ranges[0].endUtcMs, 14'500);
  QCOMPARE(ranges[0].samples.size(), 3);
  QCOMPARE(ranges[0].representative.recordId, 2);
  QCOMPARE(ranges[0].relevance, static_cast<double>(0.9f));
  QCOMPARE(ranges[1].representative.recordId, 4);
  QCOMPARE(ranges[1].startUtcMs, 16'500);
  QCOMPARE(ranges[1].endUtcMs, 17'500);
  QCOMPARE(ranges[2].cameraId, QString("camB"));
  QCOMPARE(ranges[2].samples.size(), 2);
  QCOMPARE(mergeSamples(samples, 0, 1000).size(), 6);
  QCOMPARE(mergeSamples(samples, 10'000, 1000).size(), 2);
  QVERIFY(mergeSamples({}, 3000, 1000).isEmpty());

  const SearchRange back = SearchRange::fromJson(ranges[0].toJson());
  QCOMPARE(back.samples.size(), 3);
  QCOMPARE(back.representative.recordId, 2);
  QCOMPARE(ranges[0].toJson().value("representative").toObject().value("thumbnail").toString(), QString("/v1/search/thumbnails/2"));
}

void TestIndex::countsGridPoints() {
  QCOMPARE(gridPoints(1000, 1000, 1000), 1);
  QCOMPARE(gridPoints(1001, 1999, 1000), 0);
  QCOMPARE(gridPoints(999, 3000, 1000), 3);
  QCOMPARE(expectedSamples(10'000, 40'000, 1000), 30);
  QCOMPARE(expectedSamples(10'500, 40'160, 1000), 30);
  QCOMPARE(expectedSamples(10'000, 10'000, 1000), 0);
  QCOMPARE(gridPoints(0, 5000, 1000), 0);
  QVector<float> v{3, 4};
  QVERIFY(normalizeVector(v));
  QCOMPARE(v[0], 0.6f);
  QVector<float> zero{0, 0};
  QVERIFY(!normalizeVector(zero));
  QVector<float> nan{std::numeric_limits<float>::quiet_NaN(), 1};
  QVERIFY(!normalizeVector(nan));
}

void TestIndex::migratesVersionThreeDatabase() {
  QTemporaryDir dir;
  const QString db = dir.path() + "/v3.sqlite";
  {
    Store store;
    QVERIFY(store.open(db));
    Camera c;
    c.id = "cam1";
    c.name = "Gate";
    c.mainUrl = "rtsp://x/1";
    QVERIFY(store.insertCamera(c));
  }
  const QString name = QUuid::createUuid().toString();
  {
    QSqlDatabase raw = QSqlDatabase::addDatabase("QSQLITE", name);
    raw.setDatabaseName(db);
    QVERIFY(raw.open());
    QSqlQuery q(raw);
    for (const char* sql : {"DROP TABLE index_versions", "DROP TABLE index_jobs", "DROP TABLE embedding_records", "DROP TABLE search_sessions",
                            "DROP TABLE imports", "ALTER TABLE cameras DROP COLUMN index_enabled", "UPDATE schema_version SET version=3"})
      QVERIFY2(q.exec(QString::fromLatin1(sql)), sql);
  }
  QSqlDatabase::removeDatabase(name);
  Store store;
  QVERIFY2(store.open(db), qPrintable(store.lastError()));
  QCOMPARE(countRows(db, "SELECT version FROM schema_version"), 4);
  QVERIFY(store.getCamera("cam1")->indexEnabled);
  QCOMPARE(countRows(db, "SELECT COUNT(*) FROM embedding_records"), 0);
  Camera off = *store.getCamera("cam1");
  off.indexEnabled = false;
  QVERIFY(store.updateCamera(off));
  QVERIFY(!store.getCamera("cam1")->indexEnabled);
  QVERIFY(!Camera::fromJson(off.toJson()).indexEnabled);
  QVERIFY(Camera::fromJson(QJsonObject{}).indexEnabled);
}

void TestIndex::queuesAndPicksJobs() {
  Fixture fx;
  fx.addCamera("cam1");
  fx.addCamera("cam2");
  fx.addSegment("cam1", "a", kNow + 60'000, 30'000);
  fx.addSegment("cam1", "b", kNow, 30'500);
  fx.addSegment("cam2", "c", kNow + 10'000, 10'000);
  Camera cam2 = *fx.store.getCamera("cam2");
  cam2.indexEnabled = false;
  QVERIFY(fx.store.updateCamera(cam2));
  QCOMPARE(fx.store.queueIndexJobs(fx.version, kNow), 2);
  QCOMPARE(fx.store.queueIndexJobs(fx.version, kNow), 0);
  QCOMPARE(fx.jobFor("b").framesExpected, 31);
  std::optional<IndexJob> next = fx.store.nextIndexJob(fx.version.hash, kNow, kIndexMaxAttempts);
  QVERIFY(next);
  QCOMPARE(next->segmentId, QString("b"));

  const std::optional<IndexJob> begun = fx.store.beginIndexJob(next->id, kNow, nullptr);
  QCOMPARE(begun->state, QString("running"));
  QCOMPARE(begun->generation, 1);
  QVERIFY(!fx.store.beginIndexJob(next->id, kNow, nullptr));
  QCOMPARE(fx.store.nextIndexJob(fx.version.hash, kNow, kIndexMaxAttempts)->segmentId, QString("a"));
  IndexJob failed = *begun;
  failed.attempts = 1;
  failed.nextAttemptUtcMs = kNow + 10'000;
  QVERIFY(fx.store.finishIndexJob(failed, "failed", "sample: broken", kNow, nullptr));
  QVERIFY(!fx.store.finishIndexJob(failed, "done", QString(), kNow, nullptr));
  QCOMPARE(fx.store.nextIndexJob(fx.version.hash, kNow, kIndexMaxAttempts)->segmentId, QString("a"));
  const std::optional<IndexJob> a = fx.store.beginIndexJob(fx.jobFor("a").id, kNow, nullptr);
  QVERIFY(fx.store.finishIndexJob(*a, "done", QString(), kNow, nullptr));
  QVERIFY(!fx.store.nextIndexJob(fx.version.hash, kNow, kIndexMaxAttempts));
  QCOMPARE(fx.store.nextIndexJob(fx.version.hash, kNow + 10'000, kIndexMaxAttempts)->segmentId, QString("b"));
  QVERIFY(!fx.store.nextIndexJob(fx.version.hash, kNow + 10'000, 1));

  cam2.indexEnabled = true;
  QVERIFY(fx.store.updateCamera(cam2));
  QCOMPARE(fx.store.queueIndexJobs(fx.version, kNow), 1);
  const IndexQueueStats stats = fx.store.indexQueueStats(fx.version.hash);
  QCOMPARE(stats.queued, 1);
  QCOMPARE(stats.failed, 1);
  QCOMPARE(stats.done, 1);
}

void TestIndex::restartsJobWithoutDuplicates() {
  Fixture fx;
  fx.addCamera("cam1");
  fx.addSegment("cam1", "seg", kNow, 3000);
  QCOMPARE(fx.store.queueIndexJobs(fx.version, kNow), 1);
  std::optional<IndexJob> attempt = fx.store.beginIndexJob(fx.jobFor("seg").id, kNow, nullptr);
  QVERIFY(attempt);
  QVERIFY2(fx.append(*attempt, {kNow, kNow + 1000}, {axis(0), axis(1)}), qPrintable(fx.store.lastError()));
  QCOMPARE(fx.rows().size(), 2);

  MarkedEvidence stale;
  QCOMPARE(fx.store.recoverIndexJobs(kNow + 5000, kIndexMaxAttempts, &stale), 1);
  QCOMPARE(stale.embeddings, 2);
  QCOMPARE(stale.indexThumbnails.size(), 2);
  QCOMPARE(fx.jobFor("seg").state, QString("queued"));
  QVERIFY(fx.rows().isEmpty());
  QVERIFY(!fx.append(*attempt, {kNow + 2000}, {axis(2)}));

  attempt = fx.store.beginIndexJob(fx.jobFor("seg").id, kNow + 6000, nullptr);
  QCOMPARE(attempt->generation, 2);
  QVERIFY(fx.append(*attempt, {kNow, kNow + 1000}, {axis(0), axis(1)}));
  QVERIFY(fx.append(*attempt, {kNow + 2000}, {axis(2)}));
  IndexJob done = *attempt;
  done.framesExpected = 3;
  QVERIFY(fx.store.finishIndexJob(done, "done", QString(), kNow + 7000, nullptr));
  QCOMPARE(fx.store.recoverIndexJobs(kNow + 8000, kIndexMaxAttempts, nullptr), 0);

  const QVector<VectorRow> rows = fx.rows();
  QCOMPARE(rows.size(), 3);
  QSet<int64_t> utcs;
  for (const VectorRow& r : rows) utcs.insert(r.utcMs);
  QCOMPARE(utcs.size(), 3);
  QCOMPARE(fx.jobFor("seg").framesIndexed, 3);
  const CoverageCount coverage = fx.store.indexCoverage(fx.version, {}, 0, kNow * 2);
  QCOMPARE(coverage.expected, 3);
  QCOMPARE(coverage.indexed, 3);
  QCOMPARE(coverage.ratio(), 1.0);
  QCOMPARE(fx.store.queueIndexJobs(fx.version, kNow), 0);
  QCOMPARE(countRows(fx.dbPath(), "SELECT COUNT(*) FROM embedding_records WHERE deleted=0"), 3);
  QCOMPARE(countRows(fx.dbPath(), "SELECT COUNT(*) FROM embedding_records"), 5);
}

void TestIndex::retentionDeletesRecords() {
  Fixture fx;
  fx.addCamera("cam1");
  fx.addSegment("cam1", "old", kNow - 600'000, 2000);
  fx.addSegment("cam1", "queued", kNow - 500'000, 2000);
  fx.addSegment("cam1", "recent", kNow - 5000, 2000);
  QVERIFY(fx.index("old", {kNow - 600'000, kNow - 599'000}, {axis(0), axis(1)}));
  QVERIFY(fx.index("recent", {kNow - 5000}, {axis(0)}));
  const std::optional<EmbeddingRecord> oldRecord = fx.store.getEmbeddingRecord(fx.rows({}, 0, kNow - 590'000).first().id);
  QVERIFY(QFile::exists(oldRecord->thumbnailPath));
  QCOMPARE(fx.jobFor("queued").state, QString("queued"));

  fx.config.retentionSecondsOverride = 60;
  QVector<Camera> cameras{*fx.store.getCamera("cam1")};
  RetentionManager retention(fx.store, fx.config, [&cameras] { return cameras; }, [] { return int64_t{1} << 40; });
  const RetentionReport report = retention.runPass(kNow);
  QCOMPARE(report.deleted, 2);
  QCOMPARE(fx.store.getSegment("old")->state, QString("deleted"));
  QVERIFY(fx.store.getEmbeddingRecord(oldRecord->id)->deleted);
  QVERIFY(!QFile::exists(oldRecord->thumbnailPath));
  QCOMPARE(fx.jobFor("queued").state, QString("skipped"));
  QCOMPARE(fx.jobFor("queued").reason, QString("segment_deleted"));
  QCOMPARE(fx.jobFor("old").state, QString("done"));
  const QVector<VectorRow> rows = fx.rows({}, 0, kNow);
  QCOMPARE(rows.size(), 1);
  QCOMPARE(rows.first().segmentId, QString("recent"));
  const ScanResult result = scanIndex(fx.scan(axis(1)));
  QVERIFY(result.error.isEmpty());
  QCOMPARE(result.scanned, 1);
  QCOMPARE(result.samples.first().segmentId, QString("recent"));

  QVERIFY(fx.store.softDeleteCamera("cam1", kNow, nullptr));
  QVERIFY(fx.rows({}, 0, kNow).isEmpty());
}

void TestIndex::searchesWithFilters() {
  Fixture fx;
  fx.addCamera("camA");
  fx.addCamera("camB");
  fx.addSegment("camA", "a1", kNow, 4000);
  fx.addSegment("camB", "b1", kNow + 2000, 4000);
  QVERIFY(fx.index("a1", {kNow, kNow + 1000, kNow + 2000, kNow + 3000}, {axis(0), axis(0, 0.8f), axis(1), axis(2)}));
  QVERIFY(fx.index("b1", {kNow + 2000, kNow + 3000, kNow + 4000}, {axis(0, 0.9f), axis(3), axis(0, 0.6f)}));

  ScanResult all = scanIndex(fx.scan(axis(0)));
  QVERIFY2(all.error.isEmpty(), qPrintable(all.error));
  QCOMPARE(all.scanned, 7);
  QCOMPARE(all.coverage.expected, 8);
  QCOMPARE(all.coverage.indexed, 7);
  QCOMPARE(all.samples.first().utcMs, kNow);
  QCOMPARE(all.samples.first().cameraId, QString("camA"));
  QCOMPARE(all.samples[1].cameraId, QString("camB"));
  QVERIFY(all.samples[0].score >= all.samples[1].score && all.samples[1].score >= all.samples[2].score);

  ScanRequest share = fx.scan(axis(0));
  share.candidateFraction = 0.3;
  const ScanResult capped = scanIndex(share);
  QCOMPARE(capped.scanned, 7);
  QCOMPARE(capped.samples.size(), 3);
  QCOMPARE(capped.samples[0].utcMs, all.samples[0].utcMs);
  share.candidateFraction = 0.01;
  QCOMPARE(scanIndex(share).samples.size(), 1);

  ScanResult camB = scanIndex(fx.scan(axis(0), {"camB"}));
  QCOMPARE(camB.scanned, 3);
  for (const SearchSample& s : camB.samples) QCOMPARE(s.cameraId, QString("camB"));
  QCOMPARE(camB.coverage.expected, 4);

  ScanRequest window = fx.scan(axis(0), {}, kNow + 1000, kNow + 3000);
  window.candidates = 2;
  ScanResult timed = scanIndex(window);
  QCOMPARE(timed.scanned, 5);
  QCOMPARE(timed.samples.size(), 2);
  QCOMPARE(timed.samples[0].utcMs, kNow + 2000);
  QCOMPARE(timed.samples[1].utcMs, kNow + 1000);
  QCOMPARE(timed.coverage.expected, 5);

  const auto [oldest, newest] = fx.store.footageRange({"camB"});
  QCOMPARE(oldest, kNow + 2000);
  QCOMPARE(newest, kNow + 6000);

  IndexVersion other = fx.version;
  other.hash = "000000000000";
  QVERIFY(fx.store.upsertIndexVersion(other));
  ScanRequest otherVersion = fx.scan(axis(0));
  otherVersion.version = other;
  QCOMPARE(scanIndex(otherVersion).scanned, 0);
  ScanRequest badQuery = fx.scan(QVector<float>{1, 0});
  QVERIFY(!scanIndex(badQuery).error.isEmpty());
}

void TestIndex::revalidatesStoredSessions() {
  Fixture fx;
  fx.addCamera("cam1");
  fx.addSegment("cam1", "s1", kNow, 2000);
  fx.addSegment("cam1", "s2", kNow + 2000, 1500);
  QVERIFY(fx.index("s1", {kNow, kNow + 1000}, {axis(0), axis(0, 0.9f)}));
  QVERIFY(fx.index("s2", {kNow + 2000, kNow + 3000}, {axis(0, 0.8f), axis(1)}));
  const ScanResult scanned = scanIndex(fx.scan(axis(0)));
  const QVector<SearchRange> ranges = mergeSamples(scanned.samples, 3000, 1000);
  QCOMPARE(ranges.size(), 1);
  QCOMPARE(ranges[0].samples.size(), 4);
  SearchSessionRecord session;
  session.id = "session1";
  session.query = "a person";
  session.indexVersion = fx.version.hash;
  session.model = fx.version.modelId;
  session.createdUtcMs = kNow;
  session.stats = QJsonObject{{"sample_interval_ms", 1000}};
  session.results = QJsonArray{ranges[0].toJson()};
  QVERIFY(fx.store.insertSearchSession(session));

  WorkerSupervisor worker(fx.dir.path(), fx.dir.path() + "/worker.log", std::nullopt);
  EmbedClient embed(worker);
  IndexScheduler index(fx.store, embed, fx.dir.path());
  SearchService search(fx.store, embed, index);
  std::optional<QJsonObject> stored = search.session("session1");
  QVERIFY(stored);
  QCOMPARE(stored->value("results").toArray().size(), 1);
  QCOMPARE(stored->value("results").toArray()[0].toObject().value("evidence_state").toString(), QString("available"));
  QCOMPARE(static_cast<int64_t>(stored->value("results").toArray()[0].toObject().value("end_utc_ms").toDouble()), kNow + 3500);

  QVERIFY(fx.store.deleteSegmentUnlessHeld("s2", "age", kNow, nullptr) == SegmentDeletion::Deleted);
  stored = search.session("session1");
  const QJsonObject partial = stored->value("results").toArray()[0].toObject();
  QCOMPARE(partial.value("evidence_state").toString(), QString("partial"));
  QCOMPARE(partial.value("samples").toArray().size(), 2);
  QCOMPARE(static_cast<int64_t>(partial.value("end_utc_ms").toDouble()), kNow + 2000);
  QVERIFY(fx.store.deleteSegmentUnlessHeld("s1", "age", kNow, nullptr) == SegmentDeletion::Deleted);
  QVERIFY(search.session("session1")->value("results").toArray().isEmpty());
  ServiceError error;
  QVERIFY(!search.thumbnail(ranges[0].representative.recordId, &error));
  QCOMPARE(error.code, QString("record_deleted"));
  QVERIFY(!search.session("missing"));
}

void TestIndex::keepsRangesNextToDeletedFootageAvailable() {
  Fixture fx;
  fx.addCamera("cam1");
  fx.addSegment("cam1", "s1", kNow, 2000);
  fx.addSegment("cam1", "s2", kNow + 2000, 2000);
  QVERIFY(fx.index("s1", {kNow, kNow + 1000}, {axis(1), axis(1, 0.9f)}));
  QVERIFY(fx.index("s2", {kNow + 2000, kNow + 3000}, {axis(0), axis(0, 0.9f)}));
  const ScanResult scanned = scanIndex(fx.scan(axis(0)));
  QVector<SearchRange> ranges = mergeSamples(scanned.samples, 500, 1000);
  ranges.removeIf([](const SearchRange& r) { return r.startUtcMs != kNow + 2000; });
  QCOMPARE(ranges.size(), 1);
  SearchSessionRecord session;
  session.id = "session2";
  session.query = "a person";
  session.indexVersion = fx.version.hash;
  session.model = fx.version.modelId;
  session.createdUtcMs = kNow;
  session.stats = QJsonObject{{"sample_interval_ms", 1000}};
  session.results = QJsonArray{ranges[0].toJson()};
  QVERIFY(fx.store.insertSearchSession(session));

  WorkerSupervisor worker(fx.dir.path(), fx.dir.path() + "/worker.log", std::nullopt);
  EmbedClient embed(worker);
  IndexScheduler index(fx.store, embed, fx.dir.path());
  SearchService search(fx.store, embed, index);
  // s1 ends exactly where the range starts, so deleting it takes no footage
  // out of the range: sample instants lie in [segment start, segment end).
  QCOMPARE(fx.store.deleteSegmentUnlessHeld("s1", "age", kNow, nullptr), SegmentDeletion::Deleted);
  const QJsonObject result = search.session("session2")->value("results").toArray()[0].toObject();
  QCOMPARE(result.value("evidence_state").toString(), QString("available"));
  QCOMPARE(static_cast<int64_t>(result.value("start_utc_ms").toDouble()), kNow + 2000);
}

void TestIndex::skipsRestartedJobsOfDeletedSegments() {
  Fixture fx;
  fx.addCamera("cam1");
  fx.addSegment("cam1", "gone", kNow, 2000);
  fx.addSegment("cam1", "kept", kNow + 2000, 2000);
  fx.store.queueIndexJobs(fx.version, kNow);
  const std::optional<IndexJob> job = fx.store.beginIndexJob(fx.jobFor("gone").id, kNow, nullptr);
  QVERIFY(job);
  // Retention deletes the segment while its job is still running, then the
  // core is killed before the job notices.
  QCOMPARE(fx.store.deleteSegmentUnlessHeld("gone", "age", kNow, nullptr), SegmentDeletion::Deleted);
  QCOMPARE(fx.jobFor("gone").state, QString("running"));
  MarkedEvidence stale;
  QCOMPARE(fx.store.recoverIndexJobs(kNow, kIndexMaxAttempts, &stale), 1);
  QCOMPARE(fx.jobFor("gone").state, QString("skipped"));
  QCOMPARE(fx.jobFor("gone").reason, QString("segment_deleted"));
  QCOMPARE(fx.store.indexQueueStats(fx.version.hash).queued, 1);
  QCOMPARE(fx.store.nextIndexJob(fx.version.hash, kNow, kIndexMaxAttempts)->segmentId, QString("kept"));

  // A crash counts as an attempt, so a segment that takes the core down does
  // not hold the head of the queue for ever.
  for (int attempt = 1; attempt <= kIndexMaxAttempts; ++attempt) {
    QVERIFY(fx.store.beginIndexJob(fx.jobFor("kept").id, kNow, nullptr));
    QCOMPARE(fx.store.recoverIndexJobs(kNow, kIndexMaxAttempts, &stale), 1);
    QCOMPARE(fx.jobFor("kept").attempts, attempt);
  }
  QCOMPARE(fx.jobFor("kept").state, QString("failed"));
  QVERIFY(!fx.store.nextIndexJob(fx.version.hash, kNow, kIndexMaxAttempts));
}

void TestIndex::answersEmptyFilterWithoutEmbedding() {
  Fixture fx;
  fx.addCamera("cam1");
  fx.addSegment("cam1", "s1", kNow, 2000);
  QVERIFY(fx.index("s1", {kNow, kNow + 1000}, {axis(0), axis(1)}));
  fx.addCamera("camEmpty");
  WorkerSupervisor worker(fx.dir.path(), fx.dir.path() + "/worker.log", std::nullopt);
  EmbedClient embed(worker);
  IndexScheduler index(fx.store, embed, fx.dir.path());
  index.noteVersion(fx.version);
  SearchService search(fx.store, embed, index);
  QJsonObject answer;
  ServiceError error;
  // No worker is running: a filter that leaves no footage must answer anyway.
  search.search(QJsonObject{{"query", "a person"}, {"camera_ids", QJsonArray{"camEmpty"}}},
                [&](const QJsonObject& response, const ServiceError& e) {
                  answer = response;
                  error = e;
                });
  QVERIFY2(error.code.isEmpty(), qPrintable(error.code + ": " + error.message));
  QVERIFY(answer.value("results").toArray().isEmpty());
  QCOMPARE(answer.value("stats").toObject().value("samples_scanned").toInt(), 0);
  QCOMPARE(answer.value("stats").toObject().value("coverage_ratio").toDouble(), 0.0);
  QVERIFY(!answer.value("session_id").toString().isEmpty());
  QVERIFY(search.session(answer.value("session_id").toString()));
}

void TestIndex::checksEmbedReplies() {
  const QJsonObject descriptor{{"name", "siglip2-b16-224"}, {"model_id", "google/siglip2-base-patch16-224"}, {"model_revision", "abc"},
                               {"preprocessing", QJsonObject{{"image_size", "224x224"}}}, {"dims", kDims}, {"dtype", "float32"},
                               {"sample_interval_ms", 1000}, {"prompt_template", "query: this is a photo of {text}."}};
  QCOMPARE(QString::fromUtf8(QJsonDocument(QJsonObject{{"b", 1}, {"a", "x"}}).toJson(QJsonDocument::Compact)), QString("{\"a\":\"x\",\"b\":1}"));
  EmbedRequest request;
  request.kind = "embed_frames";
  request.jobId = "job1";
  request.versionName = "siglip2-b16-224";
  request.sampleIntervalMs = 1000;
  request.generation = 3;
  request.frames = {{"f0", 0, kNow, "/tmp/0.jpg"}, {"f1", 1, kNow + 1000, "/tmp/1.jpg"}};
  const QVector<QVector<float>> vecs{axis(0), axis(1, 0.6f)};
  QJsonObject body = embedBody(request, descriptor, vecs);
  EmbedReply reply = parseEmbedReply(body, request);
  QVERIFY2(reply.ok, qPrintable(reply.error));
  QCOMPARE(reply.version.hash, descriptorHash(descriptor));
  QCOMPARE(reply.version.modelRevision, QString("abc"));
  QCOMPARE(reply.vectors.size(), 2);
  QCOMPARE(reply.vectors[1][1], 0.6f);

  const auto rejected = [&](QJsonObject changed, const char* expected) {
    const EmbedReply r = parseEmbedReply(changed, request);
    QVERIFY2(!r.ok && r.error.contains(QLatin1String(expected)), qPrintable(r.error));
  };
  QJsonObject b = body;
  b.insert("index_version", "000000000000");
  rejected(b, "descriptor hash");
  b = body;
  b.insert("frame_ids", QJsonArray{"f1", "f0"});
  rejected(b, "frame ids");
  b = body;
  b.insert("status", "dry_run");
  rejected(b, "dry_run");
  b = body;
  b.insert("generation", 2);
  rejected(b, "generation");
  b = embedBody(request, descriptor, {axis(0), QVector<float>{2, 0, 0, 0}});
  rejected(b, "unit length");
  QJsonObject otherInterval = descriptor;
  otherInterval.insert("sample_interval_ms", 500);
  rejected(embedBody(request, otherInterval, vecs), "sample interval");
  b = body;
  b.insert("vectors", QString::fromLatin1(QByteArray(12, 'a').toBase64()));
  rejected(b, "vector bytes");
  b = body;
  b.insert("status", "error");
  b.insert("error", "RuntimeError: boom");
  rejected(b, "worker_error");

  EmbedRequest text;
  text.kind = "embed_text";
  text.jobId = "q1";
  text.versionName = "siglip2-b16-224";
  text.text = "a person";
  QJsonObject textBody = embedBody(text, descriptor, {axis(2)});
  textBody.insert("frame_ids", QJsonArray{});
  QVERIFY(parseEmbedReply(textBody, text).ok);
}

void TestIndex::startupCheckRepairsVectorFiles() {
  Fixture fx;
  fx.addCamera("cam1");
  fx.addSegment("cam1", "seg", kNow, 3000);
  QVERIFY(fx.index("seg", {kNow, kNow + 1000, kNow + 2000}, {axis(0), axis(1), axis(2)}));
  const QVector<VectorRow> rows = fx.rows();
  QCOMPARE(rows.size(), 3);
  const QString file = rows.first().vectorFile;
  const QString path = fx.vectors.absolutePath(file);
  QVERIFY(QFile::resize(path, QFileInfo(path).size() - VectorStore::recordBytes(kDims) + 5));
  const QString orphan = fx.version.hash + "/cam1/20200101.vec";
  QVERIFY(fx.vectors.append(orphan, kDims, {{99, kNow, axis(0)}}, nullptr, nullptr));
  const QString foreign = "ffffffffffff/cam1/20200101.vec";
  QVERIFY(fx.vectors.append(foreign, kDims, {{98, kNow, axis(0)}}, nullptr, nullptr));

  WorkerSupervisor worker(fx.dir.path(), fx.dir.path() + "/worker.log", std::nullopt);
  EmbedClient embed(worker);
  IndexScheduler index(fx.store, embed, fx.dir.path());
  index.start();
  index.stop();
  QCOMPARE(QFileInfo(path).size(), VectorStore::kHeaderBytes + 2 * VectorStore::recordBytes(kDims));
  QVERIFY(!QFile::exists(fx.vectors.absolutePath(orphan)));
  QVERIFY(!QFile::exists(fx.vectors.absolutePath(foreign)));
  const QVector<VectorRow> left = fx.rows();
  QCOMPARE(left.size(), 2);
  QVERIFY(fx.store.getEmbeddingRecord(rows[2].id)->deleted);
  QCOMPARE(scanIndex(fx.scan(axis(2))).scanned, 2);
  QCOMPARE(index.activeName(), QString("siglip2-b16-224"));
  QCOMPARE(index.activeVersion()->hash, fx.version.hash);
  QJsonObject status = index.statusJson(true);
  QCOMPARE(status.value("index_version").toString(), fx.version.hash);
  QCOMPARE(status.value("cameras").toArray()[0].toObject().value("frames_indexed").toInt(), 2);
  QCOMPARE(status.value("versions").toArray()[0].toObject().value("storage").toObject().value("rows").toInt(), 2);

  ServiceError error;
  QVERIFY(!index.setActive("unknown", &error));
  QCOMPARE(error.code, QString("unknown_index_version"));
  QVERIFY(!index.deleteVersion(fx.version.hash, &error));
  QCOMPARE(error.code, QString("active_index_version"));
  QVERIFY(index.setActive("qwen3vl-emb-2b-1024", &error));
  QCOMPARE(index.previousName(), QString("siglip2-b16-224"));
  QVERIFY(index.deleteVersion(fx.version.hash, &error));
  QVERIFY(!QFileInfo::exists(fx.dir.path() + "/index/" + fx.version.hash));
  QVERIFY(fx.rows().isEmpty());
  QVERIFY(!fx.store.getIndexVersion(fx.version.hash));
}

void TestIndex::compactsDeadFiles() {
  Fixture fx;
  fx.addCamera("cam1");
  for (int i = 0; i < 20; ++i) {
    fx.addSegment("cam1", QStringLiteral("seg%1").arg(i), kNow + i * 1000, 1000);
    QVERIFY(fx.index(QStringLiteral("seg%1").arg(i), {kNow + i * 1000}, {axis(i)}));
  }
  for (int i = 0; i < 12; ++i)
    QVERIFY(fx.store.deleteSegmentUnlessHeld(QStringLiteral("seg%1").arg(i), "age", kNow, nullptr) == SegmentDeletion::Deleted);
  const QString file = fx.rows().first().vectorFile;
  const QVector<VectorFileUsage> usage = fx.store.vectorFileUsage();
  QCOMPARE(usage.size(), 1);
  QCOMPARE(usage[0].liveRows, 8);

  WorkerSupervisor worker(fx.dir.path(), fx.dir.path() + "/worker.log", std::nullopt);
  EmbedClient embed(worker);
  IndexScheduler index(fx.store, embed, fx.dir.path());
  index.start();
  const QVector<std::pair<int64_t, int64_t>> live = fx.store.liveFileRecords(file);
  QCOMPARE(live.size(), 8);
  QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(fx.vectors.absolutePath(file)), 10'000);
  index.stop();
  const QVector<VectorRow> rows = fx.rows();
  QCOMPARE(rows.size(), 8);
  QVERIFY(rows.first().vectorFile != file);
  QCOMPARE(QFileInfo(fx.vectors.absolutePath(rows.first().vectorFile)).size(), VectorStore::kHeaderBytes + 8 * VectorStore::recordBytes(kDims));
  const ScanResult result = scanIndex(fx.scan(axis(13)));
  QCOMPARE(result.scanned, 8);
  QCOMPARE(result.unreadable, 0);
  QCOMPARE(result.samples[0].score, 1.0f);
  QCOMPARE(result.samples[1].score, 1.0f);
  QCOMPARE(result.samples[0].utcMs + result.samples[1].utcMs, 2 * kNow + 30'000);
}

void TestIndex::deletesImportSessions() {
  Fixture fx;
  fx.addCamera("cam1");
  fx.addSegment("cam1", "i1", kNow, 2000);
  fx.addSegment("cam1", "i2", kNow + 2000, 2000);
  QVERIFY(fx.index("i1", {kNow, kNow + 1000}, {axis(0), axis(1)}));
  fx.store.queueIndexJobs(fx.version, kNow);
  QVERIFY(fx.store.cameraHasFootage("cam1", kNow + 1500, kNow + 2500));
  QVERIFY(!fx.store.cameraHasFootage("cam1", kNow + 4000, kNow + 5000));
  MarkedEvidence marked;
  QVERIFY(fx.store.deleteSessionSegments("s-cam1", "import_failed", kNow, &marked));
  QCOMPARE(marked.embeddings, 2);
  QCOMPARE(fx.store.getSegment("i1")->state, QString("deleted"));
  QCOMPARE(fx.store.getSegment("i2")->state, QString("deleted"));
  QCOMPARE(fx.jobFor("i2").state, QString("skipped"));
  QVERIFY(!fx.store.cameraHasFootage("cam1", kNow, kNow + 4000));
  QCOMPARE(fx.store.purgeDeletedSegmentFiles(fx.config.recordingsDir, kNow, 10), 2);

  ImportRecord r;
  r.id = "imp1";
  r.cameraId = "cam1";
  r.sourcePath = "/videos/clip.mp4";
  r.startUtcMs = kNow;
  r.createdUtcMs = kNow;
  QVERIFY(fx.store.insertImport(r));
  r.state = "done";
  r.segments = 2;
  r.progress = 1;
  QVERIFY(fx.store.updateImport(r));
  QCOMPARE(fx.store.getImport("imp1")->segments, 2);
  QCOMPARE(fx.store.listImports(10).size(), 1);
}

QTEST_GUILESS_MAIN(TestIndex)
#include "test_index.moc"
