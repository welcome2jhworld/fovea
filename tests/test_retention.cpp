#include "fovea/core/RetentionManager.h"
#include "fovea/core/Store.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

using namespace fovea;
using namespace fovea::core;

namespace {

constexpr int64_t kNow = 1'757'700'000'000;

struct Fixture {
  QTemporaryDir dir;
  Store store;
  CoreConfig config;
  QVector<Camera> cameras;
  int64_t freeBytes = int64_t{1} << 40;

  Fixture() {
    config.recordingsDir = dir.path() + "/recordings";
    config.minFreeBytes = 1000;
    store.open(dir.path() + "/t.sqlite");
  }

  RetentionManager manager() {
    return RetentionManager(store, config, [this] { return cameras; }, [this] { return freeBytes; });
  }

  void addCamera(const QString& id, int retentionDays = 7, int64_t maxBytes = 0) {
    Camera c;
    c.id = id;
    c.retentionDays = retentionDays;
    c.maxBytes = maxBytes;
    cameras.push_back(c);
  }

  QString addSegment(const QString& cameraId, const QString& id, int64_t startUtcMs, int64_t bytes,
                     const QString& state = QStringLiteral("finalized")) {
    RecordingSegment seg;
    seg.id = id;
    seg.cameraId = cameraId;
    seg.sessionId = "s-" + cameraId;
    seg.path = config.recordingsDir + "/" + cameraId + "/" + id + ".mkv";
    seg.startUtcMs = startUtcMs;
    seg.createdUtcMs = startUtcMs;
    if (!store.insertSegment(seg)) return {};
    if (state == QLatin1String("finalized")) store.finalizeSegment(id, 1, startUtcMs + 30'000, bytes, startUtcMs + 30'000);
    else if (state == QLatin1String("damaged")) store.markSegmentDamaged(id, bytes);
    QDir().mkpath(QFileInfo(seg.path).path());
    QFile f(seg.path);
    if (f.open(QIODevice::WriteOnly)) f.write(QByteArray(static_cast<qsizetype>(bytes), 'x'));
    return seg.path;
  }

  QString state(const QString& id) { return store.getSegment(id)->state; }
};

}

class TestRetention : public QObject {
  Q_OBJECT
private slots:
  void deletesByAgeExceptHeldAndOpen();
  void enforcesCameraByteLimit();
  void freesSpaceAcrossCamerasOldestFirst();
  void leavesRecordingsWhenFloorIsUnreachable();
  void boundsDeletionsPerPass();
  void reportsStorage();
  void evidenceHoldsOfUnresolvedAndRecentEvents();
};

void TestRetention::deletesByAgeExceptHeldAndOpen() {
  Fixture fx;
  fx.config.retentionSecondsOverride = 100;
  fx.addCamera("cam1");
  const QString oldPath = fx.addSegment("cam1", "old", 1'000'000, 10);
  const QString heldPath = fx.addSegment("cam1", "held", 1'100'000, 10);
  const QString damagedPath = fx.addSegment("cam1", "damaged", 1'200'000, 10, "damaged");
  fx.addSegment("cam1", "open", 1'300'000, 0, "recording");
  const QString recentPath = fx.addSegment("cam1", "recent", kNow - 60'000, 10);
  QVERIFY(fx.store.insertEvidenceHold("held", 0, "event"));
  RetentionManager rm = fx.manager();
  const RetentionReport r = rm.runPass(kNow);
  QCOMPARE(r.deleted, 2);
  QCOMPARE(r.deletedBytes, 20);
  QCOMPARE(r.held, 1);
  QCOMPARE(fx.state("old"), QString("deleted"));
  QCOMPARE(fx.state("damaged"), QString("deleted"));
  QCOMPARE(fx.state("held"), QString("finalized"));
  QCOMPARE(fx.state("open"), QString("recording"));
  QCOMPARE(fx.state("recent"), QString("finalized"));
  QVERIFY(!QFile::exists(oldPath));
  QVERIFY(!QFile::exists(damagedPath));
  QVERIFY(QFile::exists(heldPath));
  QVERIFY(QFile::exists(recentPath));
  QCOMPARE(rm.runPass(kNow).deleted, 0);

  fx.config.retentionSecondsOverride = 0;
  RetentionManager days = fx.manager();
  QCOMPARE(days.ageLimitMs(fx.cameras.first()), 7 * 86'400'000LL);
}

void TestRetention::enforcesCameraByteLimit() {
  Fixture fx;
  fx.addCamera("cam1", 7, 250);
  fx.addCamera("cam2");
  for (int i = 0; i < 5; ++i) fx.addSegment("cam1", QStringLiteral("a%1").arg(i), kNow - 50'000 + i * 1000, 100);
  fx.addSegment("cam2", "b0", kNow - 90'000, 100);
  QVERIFY(fx.store.insertEvidenceHold("a1", 0, "event"));
  RetentionManager rm = fx.manager();
  const RetentionReport r = rm.runPass(kNow);
  QCOMPARE(r.deleted, 3);
  QCOMPARE(fx.state("a0"), QString("deleted"));
  QCOMPARE(fx.state("a1"), QString("finalized"));
  QCOMPARE(fx.state("a2"), QString("deleted"));
  QCOMPARE(fx.state("a3"), QString("deleted"));
  QCOMPARE(fx.state("a4"), QString("finalized"));
  QCOMPARE(fx.state("b0"), QString("finalized"));
}

void TestRetention::freesSpaceAcrossCamerasOldestFirst() {
  Fixture fx;
  fx.addCamera("cam1");
  fx.addCamera("cam2");
  fx.addSegment("cam1", "a0", kNow - 90'000, 400);
  fx.addSegment("cam2", "b0", kNow - 80'000, 400);
  fx.addSegment("cam1", "a1", kNow - 70'000, 400);
  fx.addSegment("cam2", "b1", kNow - 60'000, 400);
  fx.config.minFreeBytes = 100LL * 1024 * 1024;
  fx.freeBytes = fx.config.minFreeBytes + fx.config.diskHeadroomBytes() - 700;
  RetentionManager rm = fx.manager();
  const RetentionReport r = rm.runPass(kNow);
  QCOMPARE(r.deleted, 2);
  QVERIFY(!r.floorUnreachable);
  QCOMPARE(fx.state("a0"), QString("deleted"));
  QCOMPARE(fx.state("b0"), QString("deleted"));
  QCOMPARE(fx.state("a1"), QString("finalized"));
  QCOMPARE(fx.state("b1"), QString("finalized"));
}

void TestRetention::leavesRecordingsWhenFloorIsUnreachable() {
  Fixture fx;
  fx.addCamera("cam1");
  fx.addSegment("cam1", "a0", kNow - 90'000, 400);
  fx.addSegment("cam1", "a1", kNow - 80'000, 400);
  fx.config.minFreeBytes = 100LL * 1024 * 1024;
  fx.freeBytes = 0;
  RetentionManager rm = fx.manager();
  const RetentionReport r = rm.runPass(kNow);
  QVERIFY(r.floorUnreachable);
  QCOMPARE(r.deleted, 0);
  QCOMPARE(fx.state("a0"), QString("finalized"));
}

void TestRetention::boundsDeletionsPerPass() {
  Fixture fx;
  fx.config.retentionSecondsOverride = 10;
  fx.addCamera("cam1");
  for (int i = 0; i < 230; ++i) fx.addSegment("cam1", QStringLiteral("s%1").arg(i, 3, 10, QLatin1Char('0')), 1000 + i, 1);
  RetentionManager rm = fx.manager();
  const RetentionReport first = rm.runPass(kNow);
  QCOMPARE(first.deleted, 200);
  QVERIFY(first.incomplete);
  QCOMPARE(fx.state("s199"), QString("deleted"));
  QCOMPARE(fx.state("s200"), QString("finalized"));
  const RetentionReport second = rm.runPass(kNow);
  QCOMPARE(second.deleted, 30);
  QVERIFY(!second.incomplete);
}

void TestRetention::reportsStorage() {
  Fixture fx;
  fx.addCamera("cam1", 3, 5000);
  fx.addSegment("cam1", "a0", kNow - 90'000, 400);
  fx.addSegment("cam1", "a1", kNow - 60'000, 300);
  fx.freeBytes = 123456;
  RetentionManager rm = fx.manager();
  const QJsonObject o = rm.storageJson();
  QCOMPARE(o.value("free_bytes").toDouble(), 123456.0);
  QCOMPARE(o.value("floor_bytes").toDouble(), 1000.0);
  const QJsonObject cam = o.value("per_camera").toArray().first().toObject();
  QCOMPARE(cam.value("camera_id").toString(), QString("cam1"));
  QCOMPARE(cam.value("bytes").toDouble(), 700.0);
  QCOMPARE(cam.value("oldest_utc_ms").toDouble(), static_cast<double>(kNow - 90'000));
  QCOMPARE(cam.value("retention_days").toInt(), 3);
  QCOMPARE(cam.value("max_bytes").toDouble(), 5000.0);
}

void TestRetention::evidenceHoldsOfUnresolvedAndRecentEvents() {
  Fixture fx;
  fx.config.retentionSecondsOverride = 100;
  fx.addCamera("cam1");
  fx.addSegment("cam1", "open", 1'000'000, 10);
  fx.addSegment("cam1", "resolved", 1'100'000, 10);
  fx.addSegment("cam1", "free", 1'200'000, 10);
  const int64_t openedUtcMs = kNow - 86'400'000;
  for (const char* id : {"open", "resolved"}) {
    EventRecord e;
    e.id = QStringLiteral("e-") + QLatin1String(id);
    e.cameraId = "cam1";
    e.openedUtcMs = openedUtcMs;
    EvidenceRef ref;
    ref.id = QStringLiteral("ref-") + QLatin1String(id);
    ref.eventId = e.id;
    ref.cameraId = "cam1";
    QVERIFY(fx.store.openEvent(e, ref, {}));
    ref.state = "available";
    ref.segmentIds = {QLatin1String(id)};
    QVERIFY(fx.store.updateEvidence(ref, 0));
  }
  QVERIFY(fx.store.setEventHoldsUntil("e-resolved", openedUtcMs + kEvidenceRetentionMs));
  RetentionManager rm = fx.manager();
  RetentionReport r = rm.runPass(kNow);
  QCOMPARE(r.deleted, 1);
  QCOMPARE(r.held, 2);
  QCOMPARE(fx.state("free"), QString("deleted"));
  QCOMPARE(fx.state("open"), QString("finalized"));
  QCOMPARE(fx.state("resolved"), QString("finalized"));
  r = rm.runPass(openedUtcMs + kEvidenceRetentionMs);
  QCOMPARE(r.deleted, 1);
  QCOMPARE(fx.state("resolved"), QString("deleted"));
  QCOMPARE(fx.state("open"), QString("finalized"));
  QCOMPARE(fx.store.getEvidence("ref-resolved")->state, QString("deleted"));
  QCOMPARE(fx.store.getEvidence("ref-open")->state, QString("available"));
}

QTEST_GUILESS_MAIN(TestRetention)
#include "test_retention.moc"
