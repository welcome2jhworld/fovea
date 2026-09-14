#include "fovea/core/ImportManager.h"
#include "fovea/core/Index.h"
#include "fovea/core/MediaProbe.h"
#include "fovea/core/SegmentSampler.h"
#include "fovea/core/Store.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <gst/gst.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>

using namespace fovea;
using namespace fovea::core;

namespace {

constexpr int64_t kStartUtcMs = 1'757'700'000'500;

bool runToEos(const QString& launch) {
  GError* err = nullptr;
  GstElement* pipeline = gst_parse_launch(launch.toUtf8().constData(), &err);
  if (!pipeline || err) {
    if (err) g_error_free(err);
    if (pipeline) gst_object_unref(pipeline);
    return false;
  }
  GstBus* bus = gst_element_get_bus(pipeline);
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  GstMessage* msg = gst_bus_timed_pop_filtered(bus, 60 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  const bool ok = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
  if (msg) gst_message_unref(msg);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  return ok;
}

// An H.264 encoder element with a keyframe every 25 frames, or empty when none is installed.
QString h264Encoder() {
  if (GstElementFactory* f = gst_element_factory_find("x264enc")) {
    gst_object_unref(f);
    return QStringLiteral("x264enc key-int-max=25 tune=zerolatency");
  }
  if (GstElementFactory* f = gst_element_factory_find("openh264enc")) {
    gst_object_unref(f);
    return QStringLiteral("openh264enc gop-size=25");
  }
  return {};
}

struct Fixture {
  QTemporaryDir dir;
  Store store;
  CoreConfig config;

  Fixture() {
    config.dataDir = dir.path();
    config.recordingsDir = dir.path() + "/recordings";
    store.open(dir.path() + "/fovea.sqlite");
    Camera c;
    c.id = "cam1";
    c.name = "Import";
    c.kind = "file";
    c.mainUrl = "clip.mp4";
    c.enabled = false;
    c.segmentSeconds = 5;
    store.insertCamera(c);
  }

  QString clip(const QString& name, int frames) {
    const QString path = dir.path() + "/" + name;
    const QString launch = QStringLiteral("videotestsrc num-buffers=%1 pattern=ball ! video/x-raw,width=320,height=240,framerate=25/1"
                                          " ! %2 ! h264parse ! mp4mux ! filesink location=\"%3\"")
                               .arg(frames)
                               .arg(h264Encoder(), path);
    return runToEos(launch) ? path : QString();
  }
};

struct Submitted {
  std::optional<ImportRecord> record;
  ServiceError error;
  bool done = false;
};

Submitted submit(ImportManager& imports, const QString& path, const QString& cameraId, int64_t startUtcMs) {
  auto result = std::make_shared<Submitted>();
  imports.submit(path, cameraId, startUtcMs, [result](std::optional<ImportRecord> record, const ServiceError& error) {
    result->record = std::move(record);
    result->error = error;
    result->done = true;
  });
  if (!QTest::qWaitFor([&result] { return result->done; }, 30'000)) result->error.code = QStringLiteral("no_answer");
  return *result;
}

}

class TestImportSample : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void importsAndSamplesSegments();
  void refusesBadImports();
  void recoveryFailsUnfinishedImports();
};

void TestImportSample::initTestCase() {
  gst_init(nullptr, nullptr);
  if (h264Encoder().isEmpty()) QSKIP("no H.264 encoder (x264enc or openh264enc) to make test media");
}

void TestImportSample::importsAndSamplesSegments() {
  Fixture fx;
  const QString path = fx.clip("clip.mp4", 300);
  QVERIFY(!path.isEmpty());
  ImportManager imports(fx.store, fx.config);
  const Submitted queued = submit(imports, path, "cam1", kStartUtcMs);
  QVERIFY2(queued.record, qPrintable(queued.error.message));
  QCOMPARE(queued.record->codec, QString("h264"));
  QVERIFY(queued.record->durationNs > 11'900'000'000 && queued.record->durationNs < 12'100'000'000);
  const QString id = queued.record->id;
  QTRY_VERIFY_WITH_TIMEOUT(fx.store.getImport(id)->state != QLatin1String("queued") &&
                               fx.store.getImport(id)->state != QLatin1String("running"),
                           30'000);
  const ImportRecord done = *fx.store.getImport(id);
  QCOMPARE(done.state, QString("done"));
  QCOMPARE(done.progress, 1.0);

  QVector<RecordingSegment> segments = fx.store.listSegments("cam1", 0, 0);
  std::sort(segments.begin(), segments.end(), [](const RecordingSegment& a, const RecordingSegment& b) { return a.startUtcMs < b.startUtcMs; });
  QCOMPARE(segments.size(), 3);
  QCOMPARE(done.segments, 3);
  QCOMPARE(segments.first().startUtcMs, kStartUtcMs);
  QVERIFY(qAbs(segments.last().endUtcMs - (kStartUtcMs + 12'000)) <= 40);
  for (qsizetype i = 0; i < segments.size(); ++i) {
    QCOMPARE(segments[i].state, QString("finalized"));
    QCOMPARE(segments[i].sessionId, done.sessionId);
    QCOMPARE(QFileInfo(segments[i].path).suffix(), QString("mkv"));
    if (i > 0) QVERIFY(qAbs(segments[i].startUtcMs - segments[i - 1].endUtcMs) <= 1);
  }
  QCOMPARE(fx.store.getSession(done.sessionId)->captureClock, QString("imported"));
  QCOMPARE(fx.store.getSession(done.sessionId)->endReason, QString("eos"));
  QCOMPARE(probeVideoCodec(segments.first().path), QString("h264"));

  const Submitted overlap = submit(imports, path, "cam1", kStartUtcMs + 6000);
  QVERIFY(!overlap.record);
  QCOMPARE(overlap.error.code, QString("footage_overlap"));

  const RecordingSegment& seg = segments.first();
  SampleRequest request;
  request.segmentPath = seg.path;
  request.startUtcMs = seg.startUtcMs;
  request.endUtcMs = seg.endUtcMs;
  request.startPtsNs = seg.startPtsNs;
  request.intervalMs = 1000;
  request.spoolDir = fx.dir.path() + "/spool";
  request.thumbnailDir = fx.dir.path() + "/thumbs";
  request.thumbnailPrefix = "g1";
  const SampleResult sampled = sampleSegment(request, nullptr);
  QVERIFY2(sampled.error.isEmpty(), qPrintable(sampled.error));
  QCOMPARE(sampled.expected, expectedSamples(seg.startUtcMs, seg.endUtcMs, 1000));
  QCOMPARE(sampled.frames.size(), sampled.expected);
  QVERIFY(sampled.decodedFrames >= 120);
  for (const SampledFrame& f : sampled.frames) {
    const int64_t grid = (f.utcMs / 1000 + (f.utcMs % 1000 == 0 ? 0 : 1)) * 1000;
    QVERIFY2(grid - f.utcMs >= 0 && grid - f.utcMs < 40, qPrintable(QString::number(f.utcMs)));
    QVERIFY(qAbs(f.ptsNs - seg.startPtsNs - (f.utcMs - seg.startUtcMs) * 1'000'000) < 1'000'000);
    QVERIFY(QFileInfo(f.jpegPath).size() > 0);
    QVERIFY(QFileInfo(f.thumbnailPath).size() > 0);
  }

  std::atomic<bool> cancel{true};
  QCOMPARE(sampleSegment(request, &cancel).error, QString("cancelled"));
  request.segmentPath = fx.dir.path() + "/missing.mkv";
  QVERIFY(!sampleSegment(request, nullptr).error.isEmpty());
}

void TestImportSample::refusesBadImports() {
  Fixture fx;
  ImportManager imports(fx.store, fx.config);
  QCOMPARE(submit(imports, fx.dir.path() + "/missing.mp4", "cam1", kStartUtcMs).error.code, QString("file_not_found"));
  QCOMPARE(submit(imports, "relative.mp4", "cam1", kStartUtcMs).error.code, QString("invalid_import"));
  QFile text(fx.dir.path() + "/notes.txt");
  QVERIFY(text.open(QIODevice::WriteOnly));
  text.write("not a video");
  text.close();
  QCOMPARE(submit(imports, text.fileName(), "cam1", kStartUtcMs).error.code, QString("unsupported_container"));
  QFile garbage(fx.dir.path() + "/garbage.mp4");
  QVERIFY(garbage.open(QIODevice::WriteOnly));
  garbage.write(QByteArray(4096, 'x'));
  garbage.close();
  QCOMPARE(submit(imports, garbage.fileName(), "cam1", kStartUtcMs).error.code, QString("unreadable_media"));
  const QString clip = fx.clip("short.mp4", 25);
  QVERIFY(!clip.isEmpty());
  QCOMPARE(submit(imports, clip, "nope", kStartUtcMs).error.code, QString("not_found"));
  QCOMPARE(submit(imports, clip, "cam1", 0).error.code, QString("invalid_import"));
  QVERIFY(fx.store.listImports(10).isEmpty());
}

void TestImportSample::recoveryFailsUnfinishedImports() {
  Fixture fx;
  StreamSession session;
  session.id = "s-import";
  session.cameraId = "cam1";
  session.startedUtcMs = kStartUtcMs;
  session.captureClock = "imported";
  QVERIFY(fx.store.insertSession(session));
  RecordingSegment seg;
  seg.id = "part";
  seg.cameraId = "cam1";
  seg.sessionId = session.id;
  seg.path = fx.config.recordingsDir + "/cam1/s-import_00000.mkv";
  seg.startUtcMs = kStartUtcMs;
  seg.createdUtcMs = kStartUtcMs;
  QVERIFY(fx.store.insertSegment(seg));
  QVERIFY(fx.store.finalizeSegment(seg.id, 5'000'000'000, kStartUtcMs + 5000, 10, kStartUtcMs + 5000));
  QDir().mkpath(QFileInfo(seg.path).path());
  QFile file(seg.path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write("partial");
  file.close();
  ImportRecord running;
  running.id = "imp";
  running.cameraId = "cam1";
  running.sessionId = session.id;
  running.sourcePath = "/videos/clip.mp4";
  running.startUtcMs = kStartUtcMs;
  running.state = "running";
  running.createdUtcMs = kStartUtcMs;
  QVERIFY(fx.store.insertImport(running));

  // A fragment the core never saw opened has no row and would be adopted as
  // finalized footage at the next start.
  const QString stray = fx.config.recordingsDir + "/cam1/" + session.id + "_00002.mkv";
  QVERIFY(QDir().mkpath(QFileInfo(stray).path()));
  {
    QFile f(stray);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("not a real fragment");
  }

  ImportManager imports(fx.store, fx.config);
  imports.recover();
  QVERIFY(!QFile::exists(stray));
  const ImportRecord failed = *fx.store.getImport("imp");
  QCOMPARE(failed.state, QString("failed"));
  QCOMPARE(failed.error, QString("core_restart"));
  QCOMPARE(fx.store.getSegment("part")->state, QString("deleted"));
  QVERIFY(!QFile::exists(seg.path));
  QVERIFY(!fx.store.cameraHasFootage("cam1", kStartUtcMs, kStartUtcMs + 5000));
}

QTEST_GUILESS_MAIN(TestImportSample)
#include "test_import_sample.moc"
