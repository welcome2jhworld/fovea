#include "fovea/core/SystemStats.h"
#include <QTemporaryDir>
#include <QTest>
#include <chrono>

using namespace fovea::core;

class TestSystemStats : public QObject {
  Q_OBJECT
private slots:
  void processCounters() {
    QVERIFY(processRssBytes() > 0);
    const int64_t before = processCpuTimeNs();
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    uint64_t spins = 0;
    while (std::chrono::steady_clock::now() < until) spins += 1;
    QVERIFY(spins > 0);
    QVERIFY(processCpuTimeNs() - before >= 50'000'000);
  }
  void freeDisk() {
    QTemporaryDir dir;
    QVERIFY(freeDiskBytes(dir.path()) > 0);
    QVERIFY(freeDiskBytes(dir.path() + "/not/created/yet") > 0);
  }
  void cpuSamplerIntervals() {
    CpuSampler sampler;
    QCOMPARE(sampler.sample(1'000'000'000, 0), 0.0);
    QCOMPARE(sampler.sample(1'500'000'000, 400'000'000), 0.0);
    QCOMPARE(sampler.sample(3'000'000'000, 1'000'000'000), 50.0);
    QCOMPARE(sampler.sample(3'100'000'000, 9'000'000'000), 50.0);
    QCOMPARE(sampler.sample(4'000'000'000, 3'000'000'000), 200.0);
  }
};

QTEST_GUILESS_MAIN(TestSystemStats)
#include "test_system_stats.moc"
