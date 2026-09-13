#include "fovea/Redact.h"
#include <QTest>

class TestRedact : public QObject {
  Q_OBJECT
private slots:
  void stripsCredentialsFromUrls() {
    QCOMPARE(fovea::redactUrl("rtsp://admin:s3cr3t@10.0.0.5:554/stream"), QString("rtsp://***@10.0.0.5:554/stream"));
    QCOMPARE(fovea::redactUrl("rtsp://10.0.0.5:554/stream"), QString("rtsp://10.0.0.5:554/stream"));
    QCOMPARE(fovea::redactUrl("http://user@host/x"), QString("http://***@host/x"));
  }
  void stripsPasswordFieldsInJson() {
    QCOMPARE(fovea::redactText(R"({"username":"a","password":"hunter2"})"), QString(R"({"username":"a","password":"***"})"));
  }
  void leavesPlainTextAlone() {
    QCOMPARE(fovea::redactText("pipeline error: not-linked"), QString("pipeline error: not-linked"));
  }
};

QTEST_GUILESS_MAIN(TestRedact)
#include "test_redact.moc"
