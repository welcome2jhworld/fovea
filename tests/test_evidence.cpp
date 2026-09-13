#include "fovea/core/EvidenceService.h"
#include "fovea/core/Store.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace fovea;
using namespace fovea::core;

namespace {

constexpr int64_t kGraceMs = EvidenceService::kGraceMs;

RecordingSegment segment(const QString& id, const QString& state, int64_t startUtcMs, int64_t endUtcMs,
                         const QString& sessionId = "s1") {
  RecordingSegment s;
  s.id = id;
  s.cameraId = "cam1";
  s.sessionId = sessionId;
  s.path = "/r/" + id + ".mkv";
  s.state = state;
  s.startUtcMs = startUtcMs;
  s.endUtcMs = endUtcMs;
  return s;
}

ReceiveGap gap(int64_t fromUtcMs, int64_t toUtcMs) {
  ReceiveGap g;
  g.id = "g";
  g.cameraId = "cam1";
  g.fromUtcMs = fromUtcMs;
  g.toUtcMs = toUtcMs;
  g.reason = "timeout";
  return g;
}

EvidenceRef window(int64_t fromUtcMs, int64_t toUtcMs) {
  EvidenceRef r;
  r.id = "ev1";
  r.eventId = "e1";
  r.cameraId = "cam1";
  r.fromUtcMs = fromUtcMs;
  r.toUtcMs = toUtcMs;
  return r;
}

void addSegment(Store& store, const QString& id, int64_t startUtcMs, int64_t endUtcMs) {
  RecordingSegment s = segment(id, "recording", startUtcMs, 0);
  s.createdUtcMs = startUtcMs;
  QVERIFY(store.insertSegment(s));
  if (endUtcMs > 0) QVERIFY(store.finalizeSegment(id, 1, endUtcMs, 100, endUtcMs));
}

void openEvent(Store& store, const QString& id, int64_t openedUtcMs) {
  EventRecord e;
  e.id = id;
  e.ruleId = "r1";
  e.cameraId = "cam1";
  e.openedUtcMs = openedUtcMs;
  EvidenceRef r = window(openedUtcMs - 10'000, openedUtcMs + 10'000);
  r.id = "ref-" + id;
  r.eventId = id;
  r.updatedUtcMs = openedUtcMs;
  QVERIFY(store.openEvent(e, r, {}));
}

}

class TestEvidence : public QObject {
  Q_OBJECT
private slots:
  void pendingUntilSomethingCovers();
  void availableWhenFinalizedSegmentsCover();
  void partialReasons();
  void deletedSegment();
  void windowEdgesAndSessionJoins();
  void longRecordingSegmentKeepsTheRefWaiting();
  void servicePassKeepsHoldsAndStopsAfterGrace();
  void deletedRefLosesItsThumbnail();
};

void TestEvidence::pendingUntilSomethingCovers() {
  const EvidenceRef ref = window(10'000, 30'000);
  EvidenceRef r = assessEvidence(ref, {}, {}, 20'000, kGraceMs);
  QCOMPARE(r.state, QString("pending"));
  QCOMPARE(r.updatedUtcMs, 20'000);
  r = assessEvidence(ref, {}, {}, 30'000 + kGraceMs + 1, kGraceMs);
  QCOMPARE(r.state, QString("partial"));
  QCOMPARE(r.reason, QString("no recording covers the window"));
}

void TestEvidence::availableWhenFinalizedSegmentsCover() {
  const EvidenceRef ref = window(10'000, 30'000);
  const QVector<RecordingSegment> joined{segment("a", "finalized", 5'000, 20'000), segment("b", "finalized", 20'300, 40'000)};
  EvidenceRef r = assessEvidence(ref, joined, {}, 45'000, kGraceMs);
  QCOMPARE(r.state, QString("available"));
  QCOMPARE(r.reason, QString());
  QCOMPARE(r.segmentIds, QStringList({"a", "b"}));
  r = assessEvidence(ref, joined, {gap(1'000, 9'000), gap(31'000, 0)}, 45'000, kGraceMs);
  QCOMPARE(r.state, QString("available"));
}

void TestEvidence::partialReasons() {
  const EvidenceRef ref = window(10'000, 30'000);
  const RecordingSegment before = segment("a", "finalized", 5'000, 20'000);
  EvidenceRef r = assessEvidence(ref, {before, segment("b", "recording", 20'000, 0)}, {}, 25'000, kGraceMs);
  QCOMPARE(r.state, QString("partial"));
  QCOMPARE(r.reason, QString("the window ends in the future"));
  QCOMPARE(r.segmentIds.size(), 2);
  r = assessEvidence(ref, {before, segment("b", "recording", 20'000, 0)}, {}, 35'000, kGraceMs);
  QCOMPARE(r.reason, QString("the window ends inside the segment still recording"));
  r = assessEvidence(ref, {before, segment("b", "finalized", 20'000, 40'000)}, {gap(22'000, 24'000)}, 45'000, kGraceMs);
  QCOMPARE(r.reason, QString("a receive gap intersects the window"));
  r = assessEvidence(ref, {before, segment("b", "finalized", 20'000, 40'000)}, {gap(29'000, 0)}, 45'000, kGraceMs);
  QCOMPARE(r.reason, QString("a receive gap intersects the window"));
  r = assessEvidence(ref, {before, segment("b", "damaged", 20'000, 0)}, {}, 45'000, kGraceMs);
  QCOMPARE(r.reason, QString("a damaged segment intersects the window"));
  r = assessEvidence(ref, {before, segment("b", "finalized", 25'000, 40'000)}, {}, 45'000, kGraceMs);
  QCOMPARE(r.state, QString("partial"));
  QCOMPARE(r.reason, QString("recordings do not cover the whole window"));
}

void TestEvidence::deletedSegment() {
  const EvidenceRef ref = window(10'000, 30'000);
  const EvidenceRef r =
      assessEvidence(ref, {segment("a", "deleted", 5'000, 20'000), segment("b", "finalized", 20'000, 40'000)}, {}, 45'000, kGraceMs);
  QCOMPARE(r.state, QString("deleted"));
  QCOMPARE(r.reason, QString("segment a was deleted"));
  QCOMPARE(r.segmentIds, QStringList{"b"});
}

void TestEvidence::windowEdgesAndSessionJoins() {
  const EvidenceRef ref = window(10'000, 30'000);
  const auto state = [&ref](const QVector<RecordingSegment>& segments) {
    return assessEvidence(ref, segments, {}, 45'000, kGraceMs).state;
  };
  QCOMPARE(state({segment("a", "finalized", 10'490, 19'000), segment("b", "finalized", 19'000, 40'000)}), QString("partial"));
  QCOMPARE(state({segment("a", "finalized", 5'000, 19'000), segment("b", "finalized", 19'000, 29'510)}), QString("partial"));
  QCOMPARE(state({segment("a", "finalized", 10'000, 19'000), segment("b", "finalized", 19'000, 30'000)}), QString("available"));
  QCOMPARE(state({segment("a", "finalized", 5'000, 19'000), segment("b", "finalized", 19'300, 40'000, "s2")}), QString("partial"));
  QCOMPARE(state({segment("a", "finalized", 5'000, 19'000), segment("b", "finalized", 19'600, 40'000)}), QString("partial"));
  QCOMPARE(state({segment("a", "finalized", 5'000, 9'900), segment("b", "finalized", 10'100, 40'000, "s2")}), QString("partial"));
  QCOMPARE(state({segment("a", "finalized", 5'000, 9'900), segment("b", "finalized", 10'100, 40'000)}), QString("available"));
  QCOMPARE(state({segment("a", "finalized", 5'000, 25'000), segment("b", "finalized", 8'000, 12'000, "s2"),
                  segment("c", "finalized", 25'000, 40'000, "s3")}),
           QString("available"));
}

void TestEvidence::longRecordingSegmentKeepsTheRefWaiting() {
  const int64_t trigger = 1'000'000;
  const EvidenceRef ref = window(trigger - 10'000, trigger + 10'000);
  const RecordingSegment recording = segment("long", "recording", trigger - 100'000, 0);
  for (const int64_t after : {20'000, 131'000, 190'000}) {
    const EvidenceRef r = assessEvidence(ref, {recording}, {}, trigger + after, kGraceMs);
    QCOMPARE(r.state, QString("partial"));
    QCOMPARE(r.reason, QString("the window ends inside the segment still recording"));
  }
  const RecordingSegment finalized = segment("long", "finalized", trigger - 100'000, trigger + 200'000);
  QCOMPARE(assessEvidence(ref, {finalized}, {}, trigger + 205'000, kGraceMs).state, QString("available"));

  QTemporaryDir dir;
  Store store;
  QVERIFY(store.open(dir.path() + "/t.sqlite"));
  EvidenceService service(store, dir.path() + "/evidence");
  addSegment(store, "long", trigger - 100'000, 0);
  openEvent(store, "e1", trigger);
  QVERIFY(store.hasEvidenceHold("long", trigger));
  service.runPass(trigger + 20'000);
  service.runPass(trigger + 131'000);
  QCOMPARE(store.evidenceToEvaluate(kGraceMs).size(), 1);
  QCOMPARE(store.evidenceForEvent("e1")->reason, QString("the window ends inside the segment still recording"));
  QVERIFY(store.finalizeSegment("long", 1, trigger + 200'000, 100, trigger + 200'000));
  service.runPass(trigger + 205'000);
  QCOMPARE(store.evidenceForEvent("e1")->state, QString("available"));
  QCOMPARE(store.evidenceToEvaluate(kGraceMs).size(), 0);
}

void TestEvidence::deletedRefLosesItsThumbnail() {
  QTemporaryDir dir;
  Store store;
  QVERIFY(store.open(dir.path() + "/t.sqlite"));
  const QString evidenceDir = dir.path() + "/evidence";
  EvidenceService service(store, evidenceDir);
  const QString thumbnail = evidenceDir + "/e1.jpg";
  QVERIFY(QDir().mkpath(evidenceDir));
  QFile file(thumbnail);
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write("jpeg");
  file.close();
  EventRecord e;
  e.id = "e1";
  e.cameraId = "cam1";
  e.openedUtcMs = 100'000;
  EvidenceRef r = window(90'000, 110'000);
  r.id = "ref-e1";
  r.thumbnailPath = thumbnail;
  QVERIFY(store.openEvent(e, r, {}));
  addSegment(store, "gone", 85'000, 115'000);
  QCOMPARE(store.deleteSegmentUnlessHeld("gone", "age", 116'000), SegmentDeletion::Deleted);
  QCOMPARE(service.runPass(120'000), 1);
  const EvidenceRef after = *store.evidenceForEvent("e1");
  QCOMPARE(after.state, QString("deleted"));
  QVERIFY(after.thumbnailPath.isEmpty());
  QVERIFY(!QFile::exists(thumbnail));
}

void TestEvidence::servicePassKeepsHoldsAndStopsAfterGrace() {
  QTemporaryDir dir;
  Store store;
  QVERIFY(store.open(dir.path() + "/t.sqlite"));
  EvidenceService service(store, dir.path() + "/evidence");

  openEvent(store, "e1", 100'000);
  addSegment(store, "s1", 80'000, 100'000);
  addSegment(store, "s2", 100'000, 0);
  addSegment(store, "old", 10'000, 20'000);
  QCOMPARE(service.runPass(105'000), 1);
  EvidenceRef ref = *store.evidenceForEvent("e1");
  QCOMPARE(ref.state, QString("partial"));
  QCOMPARE(ref.reason, QString("the window ends in the future"));
  QCOMPARE(ref.segmentIds, QStringList({"s1", "s2"}));
  QVERIFY(store.hasEvidenceHold("s1", 105'000));
  QVERIFY(store.hasEvidenceHold("s2", 105'000));
  QVERIFY(!store.hasEvidenceHold("old", 105'000));
  QCOMPARE(store.deleteSegmentUnlessHeld("s1", "age", 105'000), SegmentDeletion::Held);
  QCOMPARE(store.deleteSegmentUnlessHeld("old", "age", 105'000), SegmentDeletion::Deleted);

  QVERIFY(store.finalizeSegment("s2", 1, 120'000, 100, 120'000));
  QCOMPARE(service.runPass(125'000), 1);
  QCOMPARE(store.evidenceForEvent("e1")->state, QString("available"));
  QCOMPARE(service.runPass(126'000), 0);

  openEvent(store, "e2", 200'000);
  addSegment(store, "s3", 185'000, 200'000);
  addSegment(store, "s4", 203'000, 215'000);
  ReceiveGap g = gap(201'000, 203'000);
  g.id = "g1";
  QVERIFY(store.insertGap(g));
  QVERIFY(store.setEventOperatorState("e2", "resolved"));
  QCOMPARE(service.runPass(220'000), 1);
  ref = *store.evidenceForEvent("e2");
  QCOMPARE(ref.state, QString("partial"));
  QCOMPARE(ref.reason, QString("a receive gap intersects the window"));
  const int64_t until = 200'000 + kEvidenceRetentionMs;
  QVERIFY(store.hasEvidenceHold("s3", until - 1));
  QVERIFY(!store.hasEvidenceHold("s3", until));
  QCOMPARE(service.runPass(210'000 + kGraceMs + 1), 0);
  QCOMPARE(store.evidenceForEvent("e2")->updatedUtcMs, 210'000 + kGraceMs + 1);
  QCOMPARE(service.runPass(210'000 + kGraceMs + 2), 0);
  QCOMPARE(store.evidenceForEvent("e2")->updatedUtcMs, 210'000 + kGraceMs + 1);
}

QTEST_GUILESS_MAIN(TestEvidence)
#include "test_evidence.moc"
