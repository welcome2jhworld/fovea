#include "dialogs/FfmpegCommand.h"
#include <QTest>

using namespace fovea::ui;

class TestFfmpegCommand : public QObject {
  Q_OBJECT
private slots:
  void rtspWithCredentialsAndTransport();
  void rtspWithoutCredentials();
  void fileSource();
  void quotesSingleQuotes();
};

void TestFfmpegCommand::rtspWithCredentialsAndTransport() {
  const QString cmd = ffplayCommand({QStringLiteral("rtsp"), QStringLiteral("rtsp://10.4.18.62:554/Streaming/Channels/101"),
                                     QStringLiteral("svc_vms"), QStringLiteral("p@ss word"), QStringLiteral("udp")});
  QCOMPARE(cmd, QStringLiteral("ffplay -rtsp_transport udp -i 'rtsp://svc_vms:p%40ss%20word@10.4.18.62:554/Streaming/Channels/101'"));
}

void TestFfmpegCommand::rtspWithoutCredentials() {
  const QString cmd = ffplayCommand({QStringLiteral("rtsp"), QStringLiteral("rtsp://cam.local/live"), {}, {}, QStringLiteral("tcp")});
  QCOMPARE(cmd, QStringLiteral("ffplay -rtsp_transport tcp -i 'rtsp://cam.local/live'"));
}

void TestFfmpegCommand::fileSource() {
  const QString cmd = ffplayCommand({QStringLiteral("file"), QStringLiteral("/recordings/a b.mkv"), QStringLiteral("ignored"),
                                     QStringLiteral("ignored"), QStringLiteral("tcp")});
  QCOMPARE(cmd, QStringLiteral("ffplay -i '/recordings/a b.mkv'"));
}

void TestFfmpegCommand::quotesSingleQuotes() {
  QCOMPARE(shellQuote(QStringLiteral("it's")), QStringLiteral("'it'\\''s'"));
}

QTEST_GUILESS_MAIN(TestFfmpegCommand)
#include "test_ffmpeg_command.moc"
