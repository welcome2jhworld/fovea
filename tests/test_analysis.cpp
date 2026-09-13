#include "fovea/core/AnalysisTap.h"
#include "fovea/core/WorkerSupervisor.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <vector>

using namespace fovea::core;

class TestAnalysis : public QObject {
  Q_OBJECT
private slots:
  void tapKeepsNewestFrameAndRateLimitsCopies();
  void tapDropsFramesWhenDisabled();
  void discoversWorkerCommand();
  void restartsWorkerWithStuckOrFailedDetector();
};

void TestAnalysis::restartsWorkerWithStuckOrFailedDetector() {
  int failures = 0;
  const QJsonObject ready{{"detector_state", "ready"}, {"detector_busy_ms", 80}};
  QVERIFY(detectorRestartReason(ready, &failures).isEmpty());
  QVERIFY(detectorRestartReason(QJsonObject{{"detector_state", "ready"}, {"detector_busy_ms", kDetectorStuckMs}}, &failures).isEmpty());
  QVERIFY(detectorRestartReason(QJsonObject{{"detector_state", "ready"}, {"detector_busy_ms", kDetectorStuckMs + 1}}, &failures)
              .startsWith("detector lane stuck"));
  const QJsonObject failed{{"detector_state", "failed"}, {"detector_load_error", "OSError: no weights"}};
  QVERIFY(detectorRestartReason(failed, &failures).isEmpty());
  QVERIFY(detectorRestartReason(failed, &failures).isEmpty());
  QVERIFY(detectorRestartReason(QJsonObject{{"detector_state", "loading"}}, &failures).isEmpty());
  QCOMPARE(failures, 0);
  for (int i = 1; i < kMaxDetectorFailures; ++i) QVERIFY(detectorRestartReason(failed, &failures).isEmpty());
  QCOMPARE(detectorRestartReason(failed, &failures), QString("detector failed: OSError: no weights"));
}

void TestAnalysis::tapKeepsNewestFrameAndRateLimitsCopies() {
  AnalysisTap tap;
  tap.setEnabled(true);
  const int w = 4;
  const int h = 2;
  const size_t stride = 20;
  std::vector<uint8_t> pixels(stride * h, 0);
  for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<uint8_t>(i);
  const int64_t t0 = 5'000'000'000;
  QVERIFY(tap.wants(t0));
  tap.offer("s1", 100, 90, 1000, w, h, pixels.data(), stride, t0);
  QVERIFY(!tap.wants(t0 + AnalysisTap::kMinCopyIntervalNs - 1));
  QVERIFY(tap.wants(t0 + AnalysisTap::kMinCopyIntervalNs));
  pixels[stride] = 200;
  tap.offer("s1", 200, 190, 1100, w, h, pixels.data(), stride, t0 + AnalysisTap::kMinCopyIntervalNs);

  std::optional<AnalysisFrame> frame = tap.takeNewer(0);
  QVERIFY(frame.has_value());
  QCOMPARE(frame->ptsNs, 200);
  QCOMPARE(frame->recvMonoNs, 190);
  QCOMPARE(frame->utcMs, 1100);
  QCOMPARE(frame->sessionId, QString("s1"));
  QCOMPARE(frame->bgra.size(), w * h * 4);
  QCOMPARE(static_cast<uint8_t>(frame->bgra.at(15)), uint8_t{15});
  QCOMPARE(static_cast<uint8_t>(frame->bgra.at(16)), uint8_t{200});
  QVERIFY(!tap.takeNewer(0).has_value());
  const uint64_t seq = frame->seq;
  tap.offer("s2", 50, 300, 1200, w, h, pixels.data(), stride, t0 + 3 * AnalysisTap::kMinCopyIntervalNs);
  QVERIFY(!tap.takeNewer(seq + 1).has_value());
  frame = tap.takeNewer(seq);
  QVERIFY(frame.has_value());
  QCOMPARE(frame->sessionId, QString("s2"));
  QVERIFY(frame->seq > seq);
}

void TestAnalysis::tapDropsFramesWhenDisabled() {
  AnalysisTap tap;
  std::vector<uint8_t> pixels(16, 1);
  QVERIFY(!tap.wants(1'000'000'000));
  tap.offer("s1", 1, 1, 1, 2, 2, pixels.data(), 8, 1'000'000'000);
  QVERIFY(!tap.takeNewer(0).has_value());
  tap.setEnabled(true);
  tap.offer("s1", 1, 1, 1, 2, 2, pixels.data(), 8, 1'000'000'000);
  tap.setEnabled(false);
  QVERIFY(!tap.takeNewer(0).has_value());
  tap.setEnabled(true);
  tap.offer("s1", 2, 2, 2, 2, 2, pixels.data(), 4, 3'000'000'000);
  QVERIFY(!tap.takeNewer(0).has_value());
}

void TestAnalysis::discoversWorkerCommand() {
  const auto env = discoverWorker(QStringLiteral("/nowhere"), QStringLiteral("\"/opt/my venv/bin/python\" -m fovea_worker.cli"));
  QVERIFY(env.has_value());
  QCOMPARE(env->program, QString("/opt/my venv/bin/python"));
  QCOMPARE(env->arguments, QStringList({"-m", "fovea_worker.cli"}));
  QCOMPARE(env->source, QString("env"));

  QTemporaryDir root;
  QVERIFY(!discoverWorker(root.path() + "/build/src/core", QString()).has_value());
#ifdef _WIN32
  const QString python = root.path() + "/worker/.venv/Scripts/python.exe";
#else
  const QString python = root.path() + "/worker/.venv/bin/python";
#endif
  QVERIFY(QDir().mkpath(QFileInfo(python).path()) && QDir().mkpath(root.path() + "/worker/fovea_worker") &&
          QDir().mkpath(root.path() + "/build/src/core"));
  for (const QString& path : {python, root.path() + "/worker/fovea_worker/cli.py"}) {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
  }
  QFile::setPermissions(python, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  const auto dev = discoverWorker(root.path() + "/build/src/core", QString());
  QVERIFY(dev.has_value());
  QCOMPARE(dev->source, QString("dev"));
  QCOMPARE(QFileInfo(dev->program).canonicalFilePath(), QFileInfo(python).canonicalFilePath());
  QCOMPARE(dev->arguments, QStringList({"-m", "fovea_worker.cli"}));
  QCOMPARE(QFileInfo(dev->workingDirectory).canonicalFilePath(), QFileInfo(root.path() + "/worker").canonicalFilePath());
}

QTEST_GUILESS_MAIN(TestAnalysis)
#include "test_analysis.moc"
