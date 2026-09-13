#include "fovea/core/Store.h"
#include "fovea/core/SecretStore.h"
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace fovea;
using namespace fovea::core;

namespace {

void writeFile(const QString& path, qsizetype bytes) {
  QDir().mkpath(QFileInfo(path).path());
  QFile f(path);
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write(QByteArray(bytes, 'x'));
}

int countRows(const QString& dbPath, const char* sql) {
  const QString name = QUuid::createUuid().toString();
  int n = -1;
  {
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
    db.setDatabaseName(dbPath);
    QSqlQuery q(db);
    if (db.open() && q.exec(QString::fromLatin1(sql)) && q.next()) n = q.value(0).toInt();
  }
  QSqlDatabase::removeDatabase(name);
  return n;
}

RecordingSegment finalizedSegment(Store& store, const QString& id, const QString& path, int64_t startUtcMs, int64_t bytes) {
  RecordingSegment seg;
  seg.id = id;
  seg.cameraId = "cam1";
  seg.sessionId = "s1";
  seg.path = path;
  seg.startUtcMs = startUtcMs;
  seg.createdUtcMs = startUtcMs;
  if (!store.insertSegment(seg) || !store.finalizeSegment(id, 10, startUtcMs + 10000, bytes, startUtcMs + 10000)) return {};
  return *store.getSegment(id);
}

}

class TestStore : public QObject {
  Q_OBJECT
private slots:
  void cameraCrudAndSoftDelete();
  void sessionsAndSegmentsRoundTrip();
  void startupRecoveryFinalizesOrDamages();
  void startupRecoveryAdoptsOrQuarantinesFiles();
  void gapsQuery();
  void secretsFilePermissionsAndRoundTrip();
  void migratesVersionOneDatabase();
  void retentionSettingsRoundTrip();
  void deletionRespectsHoldsAndStates();
  void recoveryPurgesRetentionDeletedFiles();
  void storageByCamera();
  void zonesAndRulesKeepRevisions();
  void eventOpensWithEvidenceAndDeliveries();
  void evidenceHoldsProtectSegmentsUntilExpiry();
  void migratesVersionTwoHolds();
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
  QCOMPARE(r.closedSessions.size(), 2);
  QCOMPARE(r.closedSessions.first().cameraId, QString("cam1"));
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

  const QString third = camDir + "/" + session + "_00003.mkv";
  QFile orphan(third);
  QVERIFY(orphan.open(QIODevice::WriteOnly));
  orphan.write(QByteArray(64, 'x'));
  orphan.close();
  QCOMPARE(store.recoverOnStartup([](const QString&) { return SegmentProbe{true, 60000000000LL, 64}; }, 99500, recordings)
               .filesAdopted,
           1);
  const auto unanchored = store.getSegmentByPath(third);
  QVERIFY(unanchored.has_value());
  QCOMPARE(unanchored->state, QString("finalized"));
  QCOMPARE(unanchored->startUtcMs, 0);
  QCOMPARE(unanchored->endUtcMs, 0);
  QCOMPARE(store.segmentsOverlapping("cam1", 10000, 90000).size(), 2);
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

void TestStore::migratesVersionOneDatabase() {
  QTemporaryDir dir;
  const QString db = dir.path() + "/v1.sqlite";
  {
    const QString name = QUuid::createUuid().toString();
    {
      QSqlDatabase raw = QSqlDatabase::addDatabase("QSQLITE", name);
      raw.setDatabaseName(db);
      QVERIFY(raw.open());
      QSqlQuery q(raw);
      for (const char* sql :
           {"CREATE TABLE schema_version(version INTEGER NOT NULL)", "INSERT INTO schema_version(version) VALUES(1)",
            "CREATE TABLE cameras(id TEXT PRIMARY KEY, code TEXT NOT NULL DEFAULT '', name TEXT NOT NULL, group_name TEXT NOT NULL"
            " DEFAULT '', kind TEXT NOT NULL, main_url TEXT NOT NULL, sub_url TEXT NOT NULL DEFAULT '', transport TEXT NOT NULL"
            " DEFAULT 'tcp', timeout_ms INTEGER NOT NULL DEFAULT 8000, jitter_ms INTEGER NOT NULL DEFAULT 1000, segment_seconds"
            " INTEGER NOT NULL DEFAULT 60, analytics_enabled INTEGER NOT NULL DEFAULT 0, record_enabled INTEGER NOT NULL DEFAULT 1,"
            " enabled INTEGER NOT NULL DEFAULT 1, created_utc_ms INTEGER NOT NULL, updated_utc_ms INTEGER NOT NULL, deleted_utc_ms"
            " INTEGER NOT NULL DEFAULT 0)",
            "CREATE TABLE recording_segments(id TEXT PRIMARY KEY, camera_id TEXT NOT NULL, session_id TEXT NOT NULL, path TEXT NOT"
            " NULL UNIQUE, state TEXT NOT NULL, start_pts_ns INTEGER NOT NULL DEFAULT 0, end_pts_ns INTEGER NOT NULL DEFAULT 0,"
            " start_utc_ms INTEGER NOT NULL DEFAULT 0, end_utc_ms INTEGER NOT NULL DEFAULT 0, bytes INTEGER NOT NULL DEFAULT 0,"
            " created_utc_ms INTEGER NOT NULL, finalized_utc_ms INTEGER NOT NULL DEFAULT 0)",
            "INSERT INTO cameras(id,name,kind,main_url,created_utc_ms,updated_utc_ms) VALUES('cam1','Gate','rtsp','rtsp://x/1',1,1)",
            "INSERT INTO recording_segments(id,camera_id,session_id,path,state,created_utc_ms) VALUES('old','cam1','s1','/r/a.mkv',"
            "'finalized',1)"})
        QVERIFY2(q.exec(QString::fromLatin1(sql)), sql);
    }
    QSqlDatabase::removeDatabase(name);
  }
  Store store;
  QVERIFY2(store.open(db), qPrintable(store.lastError()));
  const auto cam = store.getCamera("cam1");
  QVERIFY(cam.has_value());
  QCOMPARE(cam->retentionDays, 7);
  QCOMPARE(cam->maxBytes, 0);
  QVERIFY(!store.hasEvidenceHold("old", 5));
  QVERIFY(store.insertEvidenceHold("old", 0, "test"));
  QVERIFY(store.hasEvidenceHold("old", 5));
  Store again;
  QVERIFY2(again.open(db), qPrintable(again.lastError()));
  QVERIFY(again.hasEvidenceHold("old", 5));
}

void TestStore::retentionSettingsRoundTrip() {
  QTemporaryDir dir;
  Store store;
  QVERIFY(store.open(dir.path() + "/t.sqlite"));
  Camera c;
  c.id = "cam1";
  c.name = "Gate";
  c.mainUrl = "rtsp://x/1";
  c.retentionDays = 30;
  c.maxBytes = 7000000000LL;
  QVERIFY2(store.insertCamera(c), qPrintable(store.lastError()));
  QCOMPARE(store.getCamera("cam1")->retentionDays, 30);
  QCOMPARE(store.getCamera("cam1")->maxBytes, 7000000000LL);
  c.retentionDays = 1;
  c.maxBytes = 0;
  QVERIFY(store.updateCamera(c));
  QCOMPARE(store.getCamera("cam1")->retentionDays, 1);
  QCOMPARE(store.getCamera("cam1")->maxBytes, 0);
}

void TestStore::deletionRespectsHoldsAndStates() {
  QTemporaryDir dir;
  Store store;
  QVERIFY(store.open(dir.path() + "/t.sqlite"));
  const QString recordings = dir.path() + "/recordings";
  const RecordingSegment free = finalizedSegment(store, "free", recordings + "/cam1/free.mkv", 1000, 10);
  const RecordingSegment held = finalizedSegment(store, "held", recordings + "/cam1/held.mkv", 2000, 10);
  const RecordingSegment expired = finalizedSegment(store, "expired", recordings + "/cam1/expired.mkv", 3000, 10);
  RecordingSegment open;
  open.id = "open";
  open.cameraId = "cam1";
  open.sessionId = "s1";
  open.path = recordings + "/cam1/open.mkv";
  open.startUtcMs = 4000;
  open.createdUtcMs = 4000;
  QVERIFY(store.insertSegment(open));
  for (const QString& path : {free.path, held.path, expired.path, open.path}) writeFile(path, 10);
  QVERIFY(store.insertEvidenceHold("held", 0, "event"));
  QVERIFY(store.insertEvidenceHold("expired", 50000, "event"));
  QVERIFY(store.hasEvidenceHold("expired", 49999));
  QVERIFY(!store.hasEvidenceHold("expired", 50000));

  QCOMPARE(store.deleteSegmentUnlessHeld("held", "age", 60000), SegmentDeletion::Held);
  QCOMPARE(store.deleteSegmentUnlessHeld("open", "age", 60000), SegmentDeletion::NotDeletable);
  QCOMPARE(store.deleteSegmentUnlessHeld("missing", "age", 60000), SegmentDeletion::NotDeletable);
  QCOMPARE(store.deleteSegmentUnlessHeld("free", "age", 60000), SegmentDeletion::Deleted);
  QCOMPARE(store.deleteSegmentUnlessHeld("free", "age", 60000), SegmentDeletion::NotDeletable);
  QCOMPARE(store.deleteSegmentUnlessHeld("expired", "max_bytes", 60000), SegmentDeletion::Deleted);
  QCOMPARE(store.getSegment("held")->state, QString("finalized"));
  QCOMPARE(store.getSegment("open")->state, QString("recording"));
  QCOMPARE(store.getSegment("free")->state, QString("deleted"));
  QVERIFY(QFile::exists(free.path));

  const SegmentCursor start;
  QCOMPARE(store.listRetentionCandidates("cam1", std::nullopt, start, 10).size(), 1);
  QCOMPARE(store.listRetentionCandidates(QString(), 12000, start, 10).size(), 0);
  QCOMPARE(store.listRetentionCandidates(QString(), 12001, start, 10).first().id, QString("held"));
  QCOMPARE(store.reclaimableBytes(60000), 0);

  QVERIFY(store.removeDeletedSegmentFile("free", free.path, recordings, 61000));
  QVERIFY(!QFile::exists(free.path));
  QVERIFY(QFile::exists(held.path));
  QCOMPARE(store.purgeDeletedSegmentFiles(recordings, 62000, 10), 1);
  QVERIFY(!QFile::exists(expired.path));
  QCOMPARE(store.purgeDeletedSegmentFiles(recordings, 63000, 10), 0);

  QCOMPARE(countRows(dir.path() + "/t.sqlite",
                    "SELECT COUNT(*) FROM audit_log WHERE actor='retention' AND action='segment.delete'"), 2);
}

void TestStore::recoveryPurgesRetentionDeletedFiles() {
  QTemporaryDir dir;
  const QString recordings = dir.path() + "/recordings";
  const QString outside = dir.path() + "/elsewhere/cam1/outside.mkv";
  Store store;
  QVERIFY(store.open(dir.path() + "/t.sqlite"));
  StreamSession s;
  s.id = "s1";
  s.cameraId = "cam1";
  s.startedUtcMs = 1000;
  QVERIFY(store.insertSession(s));
  const RecordingSegment crashed = finalizedSegment(store, "crashed", recordings + "/cam1/s1_00000.mkv", 1000, 10);
  const RecordingSegment kept = finalizedSegment(store, "kept", outside, 2000, 10);
  const RecordingSegment cameraGone = finalizedSegment(store, "gone", recordings + "/cam2/s1_00000.mkv", 3000, 10);
  for (const QString& path : {crashed.path, kept.path, cameraGone.path}) writeFile(path, 10);
  QCOMPARE(store.deleteSegmentUnlessHeld("crashed", "age", 5000), SegmentDeletion::Deleted);
  QCOMPARE(store.deleteSegmentUnlessHeld("kept", "age", 5000), SegmentDeletion::Deleted);
  Camera c2;
  c2.id = "cam1";
  c2.name = "Gate";
  c2.mainUrl = "rtsp://x/1";
  QVERIFY(store.insertCamera(c2));
  EventRecord ev;
  ev.id = "e1";
  ev.cameraId = "cam1";
  EvidenceRef ref;
  ref.id = "ref1";
  ref.eventId = "e1";
  ref.cameraId = "cam1";
  ref.state = "available";
  ref.thumbnailPath = dir.path() + "/evidence/e1.jpg";
  QVERIFY(store.openEvent(ev, ref, {}));
  MarkedEvidence marked;
  QVERIFY(store.softDeleteCamera("cam1", 6000, &marked));
  QCOMPARE(store.getSegment("gone")->state, QString("deleted"));
  QCOMPARE(store.getEvidence("ref1")->state, QString("deleted"));
  QCOMPARE(store.getEvidence("ref1")->reason, QString("camera deleted"));
  QVERIFY(store.getEvidence("ref1")->thumbnailPath.isEmpty());
  QCOMPARE(marked.refs, 1);
  QCOMPARE(marked.thumbnails, QStringList{ref.thumbnailPath});

  const RecoveryReport r = store.recoverOnStartup([](const QString&) { return SegmentProbe{true, 1, 10}; }, 9000, recordings);
  QCOMPARE(r.filesPurged, 2);
  QVERIFY(!QFile::exists(crashed.path));
  QVERIFY(QFile::exists(outside));
  QVERIFY(QFile::exists(cameraGone.path));
  QCOMPARE(store.recoverOnStartup([](const QString&) { return SegmentProbe{true, 1, 10}; }, 9500, recordings).filesPurged, 0);
}

void TestStore::storageByCamera() {
  QTemporaryDir dir;
  Store store;
  QVERIFY(store.open(dir.path() + "/t.sqlite"));
  finalizedSegment(store, "a", dir.path() + "/a.mkv", 5000, 100);
  finalizedSegment(store, "b", dir.path() + "/b.mkv", 3000, 50);
  finalizedSegment(store, "c", dir.path() + "/c.mkv", 1000, 25);
  QCOMPARE(store.deleteSegmentUnlessHeld("c", "age", 9000), SegmentDeletion::Deleted);
  const QVector<CameraStorage> stats = store.storageByCamera();
  QCOMPARE(stats.size(), 1);
  QCOMPARE(stats.first().cameraId, QString("cam1"));
  QCOMPARE(stats.first().bytes, 150);
  QCOMPARE(stats.first().segments, 2);
  QCOMPARE(stats.first().oldestUtcMs, 3000);
}

void TestStore::zonesAndRulesKeepRevisions() {
  QTemporaryDir dir;
  Store store;
  QVERIFY(store.open(dir.path() + "/t.sqlite"));
  ZoneRecord zone;
  zone.id = "z1";
  zone.cameraId = "cam1";
  zone.name = "Gate";
  zone.revision = 1;
  zone.points = {QPointF(0.1, 0.1), QPointF(0.9, 0.1), QPointF(0.5, 0.9)};
  zone.refWidth = 960;
  zone.refHeight = 540;
  zone.createdUtcMs = 100;
  zone.revisionUtcMs = 100;
  QVERIFY2(store.insertZone(zone), qPrintable(store.lastError()));
  zone.revision = 2;
  zone.name = "Gate apron";
  zone.points[2] = QPointF(0.5, 0.8);
  zone.revisionUtcMs = 200;
  QVERIFY(store.insertZoneRevision(zone));
  QCOMPARE(store.getZone("z1")->revision, 2);
  QCOMPARE(store.getZone("z1")->name, QString("Gate apron"));
  QCOMPARE(store.getZone("z1", 1)->points[2], QPointF(0.5, 0.9));
  QCOMPARE(store.getZone("z1")->points[2], QPointF(0.5, 0.8));
  QCOMPARE(store.listZones("cam1").size(), 1);
  QCOMPARE(store.listZones("cam2").size(), 0);
  QCOMPARE(store.listZones().size(), 1);

  RuleRecord rule;
  rule.id = "r1";
  rule.name = "Dwell";
  rule.revision = 1;
  rule.cameraId = "cam1";
  rule.zoneId = "z1";
  rule.zoneRevision = 2;
  rule.schedule = {rules::TimeWindow{420, 1140, 0x1f}};
  rule.timeZone = "Asia/Seoul";
  rule.dwellNs = 15 * kNsPerSecond;
  rule.createdUtcMs = 300;
  rule.revisionUtcMs = 300;
  QVERIFY2(store.insertRule(rule), qPrintable(store.lastError()));
  rule.revision = 2;
  rule.enabled = false;
  rule.soundAction = false;
  QVERIFY2(store.insertRuleRevision(rule), qPrintable(store.lastError()));
  QVERIFY(!store.insertRuleRevision(rule));
  const std::optional<RuleRecord> got = store.getRule("r1");
  QVERIFY(got.has_value());
  QCOMPARE(got->revision, 2);
  QCOMPARE(got->enabled, false);
  QCOMPARE(got->soundAction, false);
  QCOMPARE(got->dwellNs, 15 * kNsPerSecond);
  QCOMPARE(got->schedule.size(), 1);
  QCOMPARE(got->schedule.first().startMinute, 420);
  QCOMPARE(got->schedule.first().days, 0x1f);
  QCOMPARE(got->timeZone, QString("Asia/Seoul"));
  QCOMPARE(got->createdUtcMs, 300);
  QVERIFY(store.softDeleteZone("z1", 400));
  QCOMPARE(store.listZones().size(), 0);
  QCOMPARE(store.getZone("z1")->deletedUtcMs, 400);
  QVERIFY(store.softDeleteRule("r1", 500));
  QVERIFY(!store.getRule("r1").has_value());
  QCOMPARE(store.listRules().size(), 0);
}

void TestStore::eventOpensWithEvidenceAndDeliveries() {
  QTemporaryDir dir;
  Store store;
  QVERIFY(store.open(dir.path() + "/t.sqlite"));
  EventRecord ev;
  ev.id = "e1";
  ev.ruleId = "r1";
  ev.ruleRevision = 1;
  ev.cameraId = "cam1";
  ev.sessionId = "s1";
  ev.severity = "critical";
  ev.openedUtcMs = 10'000;
  ev.title = "Person in Gate for 10 s";
  EvidenceRef ref;
  ref.id = "ev1";
  ref.eventId = "e1";
  ref.cameraId = "cam1";
  ref.fromUtcMs = 0;
  ref.toUtcMs = 20'000;
  ref.updatedUtcMs = 10'000;
  AlertDelivery console;
  console.id = "d1";
  console.eventId = "e1";
  console.channel = "console";
  console.createdUtcMs = 10'000;
  AlertDelivery sound = console;
  sound.id = "d2";
  sound.channel = "sound";
  QVERIFY2(store.openEvent(ev, ref, {console, sound}), qPrintable(store.lastError()));
  EventRecord duplicate = ev;
  EvidenceRef otherRef = ref;
  otherRef.id = "ev2";
  QVERIFY(!store.openEvent(duplicate, otherRef, {}));
  QVERIFY(!store.getEvidence("ev2").has_value());
  QCOMPARE(store.evidenceForEvent("e1")->id, QString("ev1"));
  QCOMPARE(store.deliveriesForEvent("e1").size(), 2);
  QCOMPARE(store.openDeliveries().size(), 2);

  QVERIFY(store.markDelivered("d1", 11'000));
  QVERIFY(!store.markDelivered("d1", 12'000));
  QCOMPARE(store.getDelivery("d1")->state, QString("delivered"));
  QCOMPARE(store.getDelivery("d1")->deliveredUtcMs, 11'000);
  QVERIFY(store.recordDeliveryAttempt("d2", 3, "failed", "no console connected"));
  QCOMPARE(store.openDeliveries().size(), 1);
  QCOMPARE(store.openDeliveries().first().state, QString("failed"));

  EventReview first{"rv1", "e1", "undecided", "", "op", 12'000};
  EventReview second{"rv2", "e1", "confirmed", "seen on camera", "op", 13'000};
  QCOMPARE(store.eventCounts().unresolved, 1);
  QVERIFY(store.insertReview(first, {"op", "event.review", "e1", "", 12'000}));
  QVERIFY(store.insertReview(second, {"op", "event.review", "e1", "", 13'000}));
  QVERIFY(!store.insertReview(first, {"op", "event.review", "e1", "", 13'500}));
  QCOMPARE(store.latestReview("e1")->label, QString("confirmed"));
  QCOMPARE(countRows(dir.path() + "/t.sqlite", "SELECT COUNT(*) FROM audit_log WHERE action='event.review'"), 2);

  QVERIFY(store.setEventCondition("e1", "clearing", 0));
  QCOMPARE(store.listEvents({"clearing", "", 0, 0, 10}).size(), 1);
  QCOMPARE(store.listEvents({"unresolved", "cam1", 5'000, 15'000, 10}).size(), 1);
  QCOMPARE(store.listEvents({"", "cam2", 0, 0, 10}).size(), 0);
  QCOMPARE(store.lastClearedUtcMs("r1"), 0);
  QCOMPARE(store.clearOpenEvents(14'000), QStringList{"e1"});
  QCOMPARE(store.getEvent("e1")->condition, QString("cleared"));
  QCOMPARE(store.getEvent("e1")->clearedUtcMs, 14'000);
  QCOMPARE(store.lastClearedUtcMs(ev.ruleId), 14'000);
  const QVector<EvaluationRecord> restartRows = store.listEvaluations(ev.ruleId, 14'000, 14'000, 10);
  QCOMPARE(restartRows.size(), 1);
  QCOMPARE(restartRows.first().transition, QString("cleared"));
  QCOMPARE(restartRows.first().note, QString("core_restart"));
  QCOMPARE(restartRows.first().eventId, QString("e1"));
  QCOMPARE(restartRows.first().before, QString("clearing"));
  QCOMPARE(countRows(dir.path() + "/t.sqlite", "SELECT COUNT(*) FROM audit_log WHERE action='event.clear'"), 1);
  QCOMPARE(store.clearOpenEvents(15'000), QStringList{});
  QVERIFY(!store.setEventCondition("e1", "active", 0));
  QVERIFY(!store.applyOperatorAction("missing", "resolved", 1, {"op", "event.resolve", "missing", "", 14'500}));
  EventReview falseAlarm{"rv3", "e1", "false_alarm", "", "op", 14'100};
  QVERIFY(store.insertReview(falseAlarm, {"op", "event.review", "e1", "", 14'100}));
  QCOMPARE(store.eventCounts().unresolved, 0);
  QCOMPARE(store.eventCounts().dismissed, 1);
  QVERIFY(store.insertReview({"rv4", "e1", "confirmed", "", "op", 14'200}, {"op", "event.review", "e1", "", 14'200}));
  QCOMPARE(store.eventCounts().unresolved, 1);
  QVERIFY(store.applyOperatorAction("e1", "resolved", std::nullopt, {"op", "event.resolve", "e1", "", 14'500}));
  QCOMPARE(store.eventCounts().dismissed, 1);
  QCOMPARE(store.eventCounts().unresolved, 0);
  QCOMPARE(countRows(dir.path() + "/t.sqlite", "SELECT COUNT(*) FROM audit_log WHERE action='event.resolve'"), 1);
  QCOMPARE(store.openDeliveries().size(), 0);
  QCOMPARE(store.listEvents({"unresolved", "", 0, 0, 10}).size(), 0);
  QCOMPARE(store.listEvents({"resolved", "", 0, 0, 10}).size(), 1);

  EvaluationRecord eval;
  eval.ruleId = "r1";
  eval.ruleRevision = 1;
  eval.cameraId = "cam1";
  eval.sessionId = "s1";
  eval.utcMs = 10'000;
  eval.before = "pending";
  eval.after = "active";
  eval.quality = "known";
  eval.transition = "triggered";
  eval.eventId = "e1";
  eval.trackIds = {"7", "9"};
  eval.dwellNs = 10 * kNsPerSecond;
  QVERIFY2(store.insertEvaluation(eval), qPrintable(store.lastError()));
  const QVector<EvaluationRecord> evals = store.listEvaluations("r1", 9'000, 11'000, 10);
  QCOMPARE(evals.size(), 1);
  QCOMPARE(evals.first().trackIds, QStringList({"7", "9"}));
  QCOMPARE(evals.first().before, QString("pending"));
  QCOMPARE(store.listEvaluations("r1", 10'001, 0, 10).size(), 1);
}

void TestStore::evidenceHoldsProtectSegmentsUntilExpiry() {
  QTemporaryDir dir;
  Store store;
  QVERIFY(store.open(dir.path() + "/t.sqlite"));
  finalizedSegment(store, "a", dir.path() + "/a.mkv", 1000, 10);
  finalizedSegment(store, "b", dir.path() + "/b.mkv", 11000, 10);
  EventRecord ev;
  ev.id = "e1";
  ev.cameraId = "cam1";
  ev.openedUtcMs = 12'000;
  EvidenceRef ref;
  ref.id = "ev1";
  ref.eventId = "e1";
  ref.cameraId = "cam1";
  ref.fromUtcMs = 2'000;
  ref.toUtcMs = 22'000;
  QVERIFY(store.openEvent(ev, ref, {}));
  QCOMPARE(store.evidenceToEvaluate(120'000).size(), 1);

  QCOMPARE(store.segmentsOverlapping("cam1", 2'000, 22'000).size(), 2);
  QCOMPARE(store.segmentsOverlapping("cam1", 21'001, 30'000).size(), 0);
  ref.state = "available";
  ref.segmentIds = {"a", "b"};
  ref.updatedUtcMs = 30'000;
  QVERIFY2(store.updateEvidence(ref, 0), qPrintable(store.lastError()));
  QVERIFY(store.updateEvidence(ref, 0));
  QCOMPARE(countRows(dir.path() + "/t.sqlite", "SELECT COUNT(*) FROM evidence_holds WHERE reason='event:e1'"), 2);
  QCOMPARE(store.evidenceToEvaluate(120'000).size(), 0);
  QCOMPARE(store.deleteSegmentUnlessHeld("a", "age", 100'000'000), SegmentDeletion::Held);

  const int64_t until = ev.openedUtcMs + kEvidenceRetentionMs;
  QVERIFY(store.setEventHoldsUntil("e1", until));
  QCOMPARE(store.deleteSegmentUnlessHeld("a", "age", until - 1), SegmentDeletion::Held);
  const QString evidenceDir = dir.path() + "/evidence";
  const QString thumbnail = evidenceDir + "/e1.jpg";
  const QString outside = dir.path() + "/elsewhere.jpg";
  writeFile(thumbnail, 10);
  writeFile(outside, 10);
  ref.thumbnailPath = thumbnail;
  QVERIFY(store.updateEvidence(ref, until));
  MarkedEvidence marked;
  QCOMPARE(store.deleteSegmentUnlessHeld("a", "age", until, &marked), SegmentDeletion::Deleted);
  QCOMPARE(marked.refs, 1);
  QCOMPARE(marked.thumbnails, QStringList{thumbnail});
  QCOMPARE(store.getEvidence("ev1")->state, QString("deleted"));
  QVERIFY(store.getEvidence("ev1")->reason.contains("a"));
  QVERIFY(store.getEvidence("ev1")->thumbnailPath.isEmpty());
  QCOMPARE(Store::removeEvidenceFiles({thumbnail, outside}, evidenceDir), 1);
  QVERIFY(!QFile::exists(thumbnail));
  QVERIFY(QFile::exists(outside));
  MarkedEvidence none;
  QCOMPARE(store.deleteSegmentUnlessHeld("b", "age", until, &none), SegmentDeletion::Deleted);
  QCOMPARE(none.refs, 0);

  finalizedSegment(store, "c", dir.path() + "/c.mkv", 40'000, 10);
  EventRecord later = ev;
  later.id = "e2";
  EvidenceRef laterRef = ref;
  laterRef.id = "ev2";
  laterRef.eventId = "e2";
  laterRef.fromUtcMs = 45'000;
  laterRef.toUtcMs = 65'000;
  QVERIFY(store.openEvent(later, laterRef, {}));
  QVERIFY(store.hasEvidenceHold("c", until * 2));
  QCOMPARE(store.deleteSegmentUnlessHeld("c", "max_bytes", until * 2), SegmentDeletion::Held);
}

void TestStore::migratesVersionTwoHolds() {
  QTemporaryDir dir;
  const QString db = dir.path() + "/v2.sqlite";
  {
    Store store;
    QVERIFY(store.open(db));
  }
  const QString name = QUuid::createUuid().toString();
  {
    QSqlDatabase raw = QSqlDatabase::addDatabase("QSQLITE", name);
    raw.setDatabaseName(db);
    QVERIFY(raw.open());
    QSqlQuery q(raw);
    for (const char* sql : {"DROP INDEX idx_evidence_holds_segment_reason", "DROP TABLE zones", "DROP TABLE events",
                            "UPDATE schema_version SET version=2",
                            "INSERT INTO evidence_holds(segment_id,until_utc_ms,reason) VALUES('s',0,'verify'),('s',0,'verify')"})
      QVERIFY2(q.exec(QString::fromLatin1(sql)), sql);
  }
  QSqlDatabase::removeDatabase(name);
  Store store;
  QVERIFY2(store.open(db), qPrintable(store.lastError()));
  QCOMPARE(countRows(db, "SELECT version FROM schema_version"), 3);
  QCOMPARE(countRows(db, "SELECT COUNT(*) FROM evidence_holds WHERE segment_id='s'"), 1);
  QVERIFY(store.hasEvidenceHold("s", 1));
  QCOMPARE(store.listZones().size(), 0);
  QCOMPARE(store.listEvents({}).size(), 0);
}

QTEST_GUILESS_MAIN(TestStore)
#include "test_store.moc"
