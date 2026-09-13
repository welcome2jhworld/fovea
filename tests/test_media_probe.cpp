#include "fovea/core/MediaProbe.h"
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <gst/gst.h>

using namespace fovea::core;

namespace {

bool runToEos(const QString& launch) {
  GError* err = nullptr;
  GstElement* pipeline = gst_parse_launch(launch.toUtf8().constData(), &err);
  if (!pipeline || err) {
    if (err) g_error_free(err);
    return false;
  }
  GstBus* bus = gst_element_get_bus(pipeline);
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  GstMessage* msg = gst_bus_timed_pop_filtered(bus, 20 * GST_SECOND,
                                               static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  const bool ok = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
  if (msg) gst_message_unref(msg);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  return ok;
}

}

class TestMediaProbe : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { gst_init(nullptr, nullptr); }

  void finalizedMkvIsReadableWithDuration() {
    QTemporaryDir dir;
    const QString path = dir.path() + "/clip.mkv";
    QVERIFY(runToEos(QStringLiteral("videotestsrc num-buffers=25 ! video/x-raw,width=160,height=120,framerate=25/1"
                                    " ! x264enc tune=zerolatency ! h264parse ! matroskamux ! filesink location=\"%1\"")
                         .arg(path)));
    const SegmentProbe p = probeSegmentFile(path);
    QVERIFY(p.readable);
    QVERIFY(p.bytes > 0);
    QVERIFY2(p.durationNs > 900'000'000 && p.durationNs < 1'100'000'000, qPrintable(QString::number(p.durationNs)));
  }

  void missingFileHasZeroBytes() {
    QTemporaryDir dir;
    const SegmentProbe p = probeSegmentFile(dir.path() + "/nope.mkv");
    QVERIFY(!p.readable);
    QCOMPARE(p.bytes, 0);
    QCOMPARE(p.durationNs, 0);
  }

  void textFileIsNotReadable() {
    QTemporaryDir dir;
    const QString path = dir.path() + "/text.mkv";
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(4096, 'x'));
    f.close();
    const SegmentProbe p = probeSegmentFile(path);
    QVERIFY(!p.readable);
    QCOMPARE(p.bytes, 4096);
  }

  void codecNames() {
    QCOMPARE(codecDisplayName("video/x-h264"), QString("H.264"));
    QCOMPARE(codecDisplayName("H265"), QString("H.265"));
    QCOMPARE(codecDisplayName("image/jpeg"), QString("MJPEG"));
    QCOMPARE(codecDisplayName("video/x-other"), QString("video/x-other"));
  }
};

QTEST_GUILESS_MAIN(TestMediaProbe)
#include "test_media_probe.moc"
