#include "fovea/core/SessionClock.h"
#include <QTest>

using fovea::core::SessionClock;

class TestSessionClock : public QObject {
  Q_OBJECT
private slots:
  void mapsPtsToUtc() {
    SessionClock c;
    QVERIFY(!c.started());
    QCOMPARE(c.utcForPts(5'000'000'000), 0);
    c.start(1'000'000'000, 1'700'000'000'000);
    QVERIFY(c.started());
    QCOMPARE(c.firstPtsNs(), 1'000'000'000);
    QCOMPARE(c.startedUtcMs(), 1'700'000'000'000);
    QCOMPARE(c.utcForPts(1'000'000'000), 1'700'000'000'000);
    QCOMPARE(c.utcForPts(3'500'000'000), 1'700'000'002'500);
    QCOMPARE(c.utcForPts(0), 1'699'999'999'000);
  }

  void monotonicGuard() {
    SessionClock c;
    QVERIFY(c.observe(42));
    c.start(0, 1000);
    QVERIFY(c.observe(40'000'000));
    QVERIFY(c.observe(80'000'000));
    QCOMPARE(c.lastPtsNs(), 80'000'000);
    QVERIFY(c.observe(80'000'000 - 400'000'000 + 400'000'000 - 40'000'000));
    QCOMPARE(c.lastPtsNs(), 80'000'000);
    QVERIFY(c.observe(2'000'000'000));
    QVERIFY(c.observe(2'000'000'000 - SessionClock::kBackwardsToleranceNs));
    QVERIFY(!c.observe(2'000'000'000 - SessionClock::kBackwardsToleranceNs - 1));
    QCOMPARE(c.lastPtsNs(), 2'000'000'000);
    QCOMPARE(QString(SessionClock::kBackwardsReason), QString("pts_backwards"));
  }
};

QTEST_GUILESS_MAIN(TestSessionClock)
#include "test_session_clock.moc"
