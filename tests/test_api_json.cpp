#include "fovea/Api.h"
#include <QTest>

using namespace fovea;

class TestApiJson : public QObject {
  Q_OBJECT
private slots:
  void cameraRoundTrip() {
    Camera c;
    c.id = "id1";
    c.name = "North Gate";
    c.groupName = "Harbour";
    c.mainUrl = "rtsp://h/1";
    c.transport = "udp";
    c.timeoutMs = 5000;
    c.jitterMs = 900;
    c.segmentSeconds = 30;
    c.recordEnabled = false;
    c.createdUtcMs = 1757700000000LL;
    const Camera d = Camera::fromJson(c.toJson());
    QCOMPARE(d.id, c.id);
    QCOMPARE(d.groupName, c.groupName);
    QCOMPARE(d.transport, c.transport);
    QCOMPARE(d.jitterMs, 900);
    QCOMPARE(d.segmentSeconds, 30);
    QCOMPARE(d.recordEnabled, false);
    QCOMPARE(d.createdUtcMs, c.createdUtcMs);
  }
  void statusWithRing() {
    CameraStatus s;
    s.cameraId = "c";
    s.state = "online";
    s.fpsNew = 24.5;
    s.latency.p95Ms = 120.5;
    s.frameRing = RingRef{"fv-abc", 3, 3686400, 1280, 720, "BGRA"};
    const CameraStatus d = CameraStatus::fromJson(s.toJson());
    QCOMPARE(d.state, QString("online"));
    QCOMPARE(d.fpsNew, 24.5);
    QCOMPARE(d.latency.p95Ms, 120.5);
    QVERIFY(d.frameRing.has_value());
    QCOMPARE(d.frameRing->slotBytes, 3686400u);
  }
  void segmentLargeNumbers() {
    RecordingSegment seg;
    seg.startPtsNs = 9000000000000LL;
    seg.bytes = 5000000000LL;
    const RecordingSegment d = RecordingSegment::fromJson(seg.toJson());
    QCOMPARE(d.startPtsNs, seg.startPtsNs);
    QCOMPARE(d.bytes, seg.bytes);
  }
  void errorShape() {
    const QJsonObject e = errorJson("not_found", "no camera");
    QCOMPARE(e.value("error").toObject().value("code").toString(), QString("not_found"));
  }
};

QTEST_GUILESS_MAIN(TestApiJson)
#include "test_api_json.moc"
