#include "fovea/Backoff.h"
#include <QTest>

class TestBackoff : public QObject {
  Q_OBJECT
private slots:
  void growsAndCaps() {
    fovea::Backoff b(1000, 30000, 2.0);
    QCOMPARE(b.nextDelayMs(), 1000);
    QCOMPARE(b.nextDelayMs(), 2000);
    QCOMPARE(b.nextDelayMs(), 4000);
    QCOMPARE(b.nextDelayMs(), 8000);
    QCOMPARE(b.nextDelayMs(), 16000);
    QCOMPARE(b.nextDelayMs(), 30000);
    QCOMPARE(b.nextDelayMs(), 30000);
    QCOMPARE(b.attempts(), 7);
    b.reset();
    QCOMPARE(b.nextDelayMs(), 1000);
  }
};

QTEST_GUILESS_MAIN(TestBackoff)
#include "test_backoff.moc"
