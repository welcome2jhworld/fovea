#include "fovea/core/Store.h"
#include "fovea/core/SecretStore.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace fovea;
using namespace fovea::core;

class TestStore : public QObject {
  Q_OBJECT
private slots:
  void cameraCrudAndSoftDelete();
  void sessionsAndSegmentsRoundTrip();
  void startupRecoveryFinalizesOrDamages();
  void startupRecoveryAdoptsOrQuarantinesFiles();
  void gapsQuery();
  void secretsFilePermissionsAndRoundTrip();
};

void TestStore::cameraCrudAndSoftDelete() {
  QTemporaryDir dir;
  Store store;
  QVERIFY2(store.open(dir.path() + "/t.sqlite"), qPrintable(store.lastError()));
  Camera c;
  c.id = "cam1";
  c.code = "CAM-01";
  c.name = "Gate";
  c.mainUrl = "rtsp://x/1";
  c.createdUtcMs = 10;
  c.updatedUtcMs = 10;
  QVERIFY2(store.insertCamera(c), qPrintable(store.lastError()));
  QCOMPARE(store.listCameras().size(), 1);
  c.name = "Gate 2";
  c.updatedUtcMs = 20;
  QVERIFY(store.updateCamera(c));
  QCOMPARE(store.getCamera("cam1")->name, QString("Gate 2"));
  QCOMPARE(store.getCamera("cam1")->code, QString("CAM-01"));
  RecordingSegment seg;
  seg.id = "seg1";
  seg.cameraId = "cam1";
  seg.sessionId = "s1";
  seg.path = dir.path() + "/seg1.mkv";
  seg.createdUtcMs = 25;
  QVERIFY(store.insertSegment(seg));
  QVERIFY(store.softDeleteCamera("cam1", 30));
  QCOMPARE(store.getSegment("seg1")->state, QString("deleted"));
  QVERIFY(!store.getCamera("cam1").has_value());
  QCOMPARE(store.listCameras().size(), 0);
  QCOMPARE(store.listCameras(true).size(), 1);
  QVERIFY(!store.softDeleteCamera("cam1", 31));
}

void TestStore::sessionsAndSegmentsRoundTrip() {
  QTemporaryDir dir;
  Store store;
  QVERIFY(store.open(dir.path() + "/t.sqlite"));
  StreamSession s;
  s.id = "s1";
  s.cameraId = "cam1";
  s.startedUtcMs = 1000;
  s.transport = "tcp";
  QVERIFY2(store.insertSession(s), qPrintable(store.lastError()));
  QVERIFY(store.setSessionAnchor("s1", 500, 1205));
  QVERIFY(store.setSessionAnchor("s1", 900, 1300));
  QCOMPARE(store.getSession("s1")->firstPtsNs, 500);
  QCOMPARE(store.getSession("s1")->startedUtcMs, 1205);
  QVERIFY(store.setSessionMedia("s1", "h264", 640, 360, 25.0, "none"));
  RecordingSegment seg;
  seg.id = "seg1";
  seg.cameraId = "cam1";
  seg.sessionId = "s1";
  seg.path = dir.path() + "/seg1.mp4";
  seg.startPtsNs = 0;
  seg.startUtcMs = 1000;
  seg.createdUtcMs = 1000;
  QVERIFY2(store.insertSegment(seg), qPrintable(store.lastError()));
  QCOMPARE(store.listSegments("cam1", 0, 0).size(), 1);
  QVERIFY(store.finalizeSegment("seg1", 10000000000LL, 11000, 12345, 11001));
  QVERIFY(!store.finalizeSegment("seg1", 1, 1, 1, 1));
  const auto got = store.getSegment("seg1");
  QCOMPARE(got->state, QString("finalized"));
  QCOMPARE(got->bytes, 12345);
  QCOMPARE(store.listSegments("cam1", 12000, 0).size(), 0);
  QCOMPARE(store.listSegments("cam1", 5000, 20000).size(), 1);
  QCOMPARE(store.totalSegmentBytes("cam1"), 12345);
  QVERIFY(store.endSession("s1", "eos", 20000));
  QCOMPARE(store.listSessions("cam1").first().endReason, QString("eos"));
}

void TestStore::startupRecoveryFinalizesOrDamages() {
  QTemporaryDir dir;
  const QString db = dir.path() + "/t.sqlite";
  {
    Store store;
    QVERIFY(store.open(db));
    for (const char* id : {"s1", "s2"}) {
      StreamSession s;
      s.id = id;
      s.cameraId = "cam1";
      s.startedUtcMs = 1000;
      QVERIFY2(store.insertSession(s), qPrintable(store.lastError()));
    }
    for (const char* id : {"good", "bad", "gone"}) {
      RecordingSegment seg;
      seg.id = id;
      seg.cameraId = "cam1";
      seg.sessionId = "s1";
      seg.path = dir.path() + "/" + id + ".mp4";
      seg.startUtcMs = 1000;
      seg.createdUtcMs = 1000;
      QVERIFY2(store.insertSegment(seg), qPrintable(store.lastError()));
    }
    ReceiveGap openGap;
    openGap.id = "g-open";
    openGap.cameraId = "cam1";
    openGap.sessionId = "s1";
    openGap.fromUtcMs = 4000;
    openGap.reason = "eos";
    QVERIFY(store.insertGap(openGap));
    ReceiveGap closedGap = openGap;
    closedGap.id = "g-closed";
    closedGap.toUtcMs = 4500;
    QVERIFY(store.insertGap(closedGap));
  }
  Store store;
  QVERIFY(store.open(db));
  const RecoveryReport r = store.recoverOnStartup(
      [](const QString& path) {
        if (path.endsWith("good.mp4")) return SegmentProbe{true, 4000000000LL, 777};
        if (path.endsWith("bad.mp4")) return SegmentProbe{false, 0, 10};
        return SegmentProbe{false, 0, 0};
      },
      99000, QString());
  QCOMPARE(r.sessionsClosed, 2);
  QCOMPARE(r.gapsClosed, 1);
  QCOMPARE(r.segmentsFinalized, 1);
  QCOMPARE(r.segmentsDamaged, 1);
  QCOMPARE(r.segmentsMissing, 1);
  QCOMPARE(store.getSegment("good")->state, QString("finalized"));
  QCOMPARE(store.getSegment("good")->endUtcMs, 5000);
  QCOMPARE(store.getSegment("bad")->state, QString("damaged"));
  QCOMPARE(store.getSegment("bad")->bytes, 10);
  QCOMPARE(store.getSegment("gone")->state, QString("damaged"));
  QCOMPARE(store.getSession("s1")->endReason, QString("shutdown"));
  QCOMPARE(store.getSession("s1")->endedUtcMs, 5000);
  QCOMPARE(store.getSession("s2")->endedUtcMs, 1000);
  QCOMPARE(store.listSegmentsByState("recording").size(), 0);
  QCOMPARE(store.listSegments("cam1", 6000, 7000).size(), 2);
  const auto gaps = store.listGaps("cam1", 0, 0);
  QCOMPARE(gaps.size(), 2);
  for (const ReceiveGap& g : gaps) QCOMPARE(g.toUtcMs, g.id == "g-open" ? 99000 : 4500);
}

void TestStore::startupRecoveryAdoptsOrQuarantinesFiles() {
  QTemporaryDir dir;
  const QString recordings = dir.path() + "/recordings";
  const QString camDir = recordings + "/cam1";
  QVERIFY(QDir().mkpath(camDir));
  const QString session = "7d0b9a51-3f0e-4c4e-9a53-2f5d1f0c1d11";
  const QString first = camDir + "/" + session + "_00000.mkv";
  const QString second = camDir + "/" + session + "_00001.mkv";
  const QString stray = camDir + "/unknown_00000.mkv";
  for (const QString& path : {first, second, stray}) {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(64, 'x'));
  }
  Store store;
  QVERIFY(store.open(dir.path() + "/t.sqlite"));
  StreamSession s;
  s.id = session;
  s.cameraId = "cam1";
  s.startedUtcMs = 10000;
  s.firstPtsNs = 2000000000LL;
  QVERIFY(store.insertSession(s));
  RecordingSegment known;
  known.id = "known";
  known.cameraId = "cam1";
  known.sessionId = session;
  known.path = first;
  known.startPtsNs = 2000000000LL;
  known.startUtcMs = 10000;
  known.createdUtcMs = 10000;
  QVERIFY(store.insertSegment(known));
  QVERIFY(store.finalizeSegment("known", 12000000000LL, 20000, 64, 20001));

  const RecoveryReport r = store.recoverOnStartup(
      [](const QString&) { return SegmentProbe{true, 3000000000LL, 64}; }, 99000, recordings);
  QCOMPARE(r.filesAdopted, 1);
  QCOMPARE(r.filesQuarantined, 1);
  const auto adopted = store.getSegmentByPath(second);
  QVERIFY(adopted.has_value());
  QCOMPARE(adopted->state, QString("finalized"));
  QCOMPARE(adopted->sessionId, session);
  QCOMPARE(adopted->startPtsNs, 12000000000LL);
  QCOMPARE(adopted->startUtcMs, 20000);
  QCOMPARE(adopted->endUtcMs, 23000);
  QVERIFY(!QFile::exists(stray));
  QVERIFY(QFile::exists(recordings + "/quarantine/cam1/unknown_00000.mkv"));
  const RecoveryReport again = store.recoverOnStartup(
      [](const QString&) { return SegmentProbe{true, 3000000000LL, 64}; }, 99000, recordings);
  QCOMPARE(again.filesAdopted, 0);
  QCOMPARE(again.filesQuarantined, 0);
}

void TestStore::gapsQuery() {
  QTemporaryDir dir;
  Store store;
  QVERIFY(store.open(dir.path() + "/t.sqlite"));
  ReceiveGap g;
  g.id = "g1";
  g.cameraId = "cam1";
  g.sessionId = "s1";
  g.fromUtcMs = 5000;
  g.reason = "timeout";
  QVERIFY2(store.insertGap(g), qPrintable(store.lastError()));
  QCOMPARE(store.listGaps("cam1", 0, 0).size(), 1);
  QCOMPARE(store.listGaps("cam1", 0, 0).first().toUtcMs, 0);
  QVERIFY(store.closeGap("g1", 7000));
  QCOMPARE(store.listGaps("cam1", 6000, 8000).size(), 1);
  QCOMPARE(store.listGaps("cam1", 8000, 9000).size(), 0);
  QVERIFY(store.setSetting("wall.layout", "\"2x2\""));
  QCOMPARE(store.getSetting("wall.layout").value(), QString("\"2x2\""));
  QVERIFY(store.appendAudit("console", "camera.create", "cam1", "", 1));
}

void TestStore::secretsFilePermissionsAndRoundTrip() {
  QTemporaryDir dir;
  FileSecretStore secrets(dir.path() + "/secrets.json");
  QVERIFY(!secrets.get("cam1").has_value());
  QVERIFY(secrets.set("cam1", {"admin", "pw"}));
  QCOMPARE(secrets.get("cam1")->password, QString("pw"));
  const auto perms = QFile(dir.path() + "/secrets.json").permissions();
#ifndef Q_OS_WIN
  QVERIFY(!(perms & QFileDevice::ReadGroup));
  QVERIFY(!(perms & QFileDevice::ReadOther));
#endif
  QVERIFY(secrets.remove("cam1"));
  QVERIFY(!secrets.get("cam1").has_value());
}

QTEST_GUILESS_MAIN(TestStore)
#include "test_store.moc"
