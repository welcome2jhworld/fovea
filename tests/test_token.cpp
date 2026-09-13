#include "fovea/Token.h"
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class TestToken : public QObject {
  Q_OBJECT
private slots:
  void createsThenReloadsSameToken() {
    QTemporaryDir dir;
    const QString path = dir.path() + "/core.token";
    const QString t1 = fovea::loadOrCreateToken(path);
    QCOMPARE(t1.size(), 64);
    QCOMPARE(fovea::loadOrCreateToken(path), t1);
    QCOMPARE(fovea::loadToken(path), t1);
    const auto perms = QFile(path).permissions();
    QVERIFY(!(perms & QFileDevice::ReadGroup));
    QVERIFY(!(perms & QFileDevice::ReadOther));
  }
  void constantTimeCompare() {
    QVERIFY(fovea::tokenEquals("abc", "abc"));
    QVERIFY(!fovea::tokenEquals("abc", "abd"));
    QVERIFY(!fovea::tokenEquals("abc", "abcd"));
    QVERIFY(!fovea::tokenEquals("", "a"));
  }
};

QTEST_GUILESS_MAIN(TestToken)
#include "test_token.moc"
