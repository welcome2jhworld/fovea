#include "fovea/FrameRing.h"
#include <QProcess>
#include <QTest>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#ifndef Q_OS_WIN
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace fovea;

class TestFrameRing : public QObject {
  Q_OBJECT
private slots:
  void createAndRead();
  void rejectsOversizedFrames();
  void detectsTornReadsUnderContention();
  void openFailsWithoutWriter();
  void removesOnlyOrphanRings();
  void sessionIdRoundTrip();
};

void TestFrameRing::createAndRead() {
  const QString name = makeRingName("test-create-" + QString::number(QCoreApplication::applicationPid()));
  auto writer = FrameRingWriter::create(name, 3, 64, 48);
  QVERIFY(writer);
  auto reader = FrameRingReader::open(name);
  QVERIFY(reader);
  QCOMPARE(reader->info().slotCount, 3u);
  QVERIFY(!reader->latest().has_value());

  std::vector<uint8_t> pixels(64 * 4 * 48);
  for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<uint8_t>(i);
  FrameHeader h;
  h.width = 32;
  h.height = 20;
  h.ptsNs = 1234;
  h.recvMonoNs = 5678;
  QVERIFY(writer->write(h, pixels.data(), 64 * 4));

  uint64_t seen = 0;
  FrameHeader out;
  std::vector<uint8_t> dst(64 * 4 * 48);
  QVERIFY(reader->copyLatest(seen, out, dst.data(), dst.size()));
  QCOMPARE(out.width, 32u);
  QCOMPARE(out.height, 20u);
  QCOMPARE(out.stride, 128u);
  QCOMPARE(out.ptsNs, 1234u);
  QCOMPARE(seen, out.seq);
  QCOMPARE(dst[0], pixels[0]);
  QCOMPARE(dst[128 + 5], pixels[256 + 5]);
  QVERIFY(!reader->copyLatest(seen, out, dst.data(), dst.size()));
  QVERIFY(reader->writerAlive());
}

void TestFrameRing::rejectsOversizedFrames() {
  const QString name = makeRingName("test-oversize-" + QString::number(QCoreApplication::applicationPid()));
  auto writer = FrameRingWriter::create(name, 2, 16, 16);
  QVERIFY(writer);
  std::vector<uint8_t> pixels(32 * 4 * 32);
  FrameHeader h;
  h.width = 32;
  h.height = 16;
  QVERIFY(!writer->write(h, pixels.data(), 32 * 4));
  h.width = 16;
  QVERIFY(writer->write(h, pixels.data(), 32 * 4));
}

void TestFrameRing::detectsTornReadsUnderContention() {
  const QString name = makeRingName("test-torn-" + QString::number(QCoreApplication::applicationPid()));
  const uint32_t w = 320, hgt = 240;
  auto writer = FrameRingWriter::create(name, 2, w, hgt);
  QVERIFY(writer);
  auto reader = FrameRingReader::open(name);
  QVERIFY(reader);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> written{0};
  std::thread producer([&] {
    std::vector<uint8_t> px(w * 4 * hgt);
    uint8_t v = 0;
    while (!stop.load()) {
      std::fill(px.begin(), px.end(), v++);
      FrameHeader h;
      h.width = w;
      h.height = hgt;
      h.ptsNs = v;
      writer->write(h, px.data(), w * 4);
      written.fetch_add(1);
    }
  });

  uint64_t seen = 0;
  uint64_t consistent = 0;
  std::vector<uint8_t> dst(w * 4 * hgt);
  FrameHeader out;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  while (std::chrono::steady_clock::now() < deadline) {
    if (reader->copyLatest(seen, out, dst.data(), dst.size())) {
      const uint8_t first = dst.front();
      bool uniform = true;
      for (size_t i = 0; i < dst.size(); i += 4096) uniform = uniform && dst[i] == first;
      QVERIFY2(uniform, "a frame accepted by the reader must never be torn");
      QVERIFY(dst.back() == first);
      ++consistent;
    }
  }
  stop.store(true);
  producer.join();
  QVERIFY(written.load() > 10);
  QVERIFY(consistent > 0);
}

void TestFrameRing::openFailsWithoutWriter() {
  QVERIFY(!FrameRingReader::open(makeRingName("does-not-exist")));
}

void TestFrameRing::removesOnlyOrphanRings() {
  const QString name = makeRingName("test-orphan-" + QString::number(QCoreApplication::applicationPid()));
  auto writer = FrameRingWriter::create(name, 2, 16, 16);
  QVERIFY(writer);
  QVERIFY(!removeOrphanRing(name));
  QVERIFY(FrameRingReader::open(name));
  QVERIFY(!removeOrphanRing(makeRingName("does-not-exist")));
#ifdef Q_OS_WIN
  QSKIP("Windows releases a mapping with its last handle");
#else
  QProcess exited;
  exited.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("sleep 0.2")});
  QVERIFY(exited.waitForStarted());
  const auto deadPid = static_cast<uint64_t>(exited.processId());
  QVERIFY(deadPid > 0);
  QVERIFY(exited.waitForFinished());
  const int fd = shm_open(("/" + name.toStdString()).c_str(), O_RDWR, 0600);
  QVERIFY(fd >= 0);
  void* base = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  QVERIFY(base != MAP_FAILED);
  const uint64_t writerPidOffset = 40;
  std::memcpy(static_cast<char*>(base) + writerPidOffset, &deadPid, sizeof(deadPid));
  munmap(base, 4096);
  QVERIFY(removeOrphanRing(name));
  QVERIFY(!FrameRingReader::open(name));
#endif
}

void TestFrameRing::sessionIdRoundTrip() {
  const QString id = QStringLiteral("123e4567-e89b-12d3-a456-426614174000");
  QCOMPARE(sessionIdString(sessionIdBytes(id)), id);
}

QTEST_GUILESS_MAIN(TestFrameRing)
#include "test_frame_ring.moc"
