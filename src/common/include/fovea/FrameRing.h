#pragma once
#include <QString>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace fovea {

enum class PixelFormat : uint32_t { BGRA = 1 };

struct FrameHeader {
  uint64_t seq = 0;
  uint64_t ptsNs = 0;
  uint64_t recvMonoNs = 0;
  uint64_t captureUtcMs = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  uint32_t format = static_cast<uint32_t>(PixelFormat::BGRA);
  std::array<uint8_t, 16> sessionId{};
  uint32_t flags = 0;
  uint32_t reserved = 0;
};
static_assert(sizeof(FrameHeader) == 72);

struct RingInfo {
  QString name;
  uint32_t slotCount = 0;
  uint32_t maxWidth = 0;
  uint32_t maxHeight = 0;
  uint32_t slotBytes = 0;
  uint64_t totalBytes = 0;
};

class FrameRingWriter {
public:
  static std::unique_ptr<FrameRingWriter> create(const QString& name, uint32_t slotCount,
                                                 uint32_t maxWidth, uint32_t maxHeight);
  ~FrameRingWriter();
  bool write(const FrameHeader& header, const uint8_t* pixels, size_t srcStride);
  const RingInfo& info() const { return info_; }
  uint64_t lastSeq() const { return seq_; }

private:
  FrameRingWriter() = default;
  RingInfo info_;
  void* base_ = nullptr;
  size_t mapped_ = 0;
  uint64_t seq_ = 0;
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

class FrameRingReader {
public:
  static std::unique_ptr<FrameRingReader> open(const QString& name);
  ~FrameRingReader();
  std::optional<FrameHeader> latest() const;
  bool copyLatest(uint64_t& lastSeenSeq, FrameHeader& header, uint8_t* dst, size_t dstCapacity);
  const RingInfo& info() const { return info_; }
  uint64_t tornReads() const { return torn_; }
  bool writerAlive() const;

private:
  FrameRingReader() = default;
  RingInfo info_;
  void* base_ = nullptr;
  size_t mapped_ = 0;
  uint64_t torn_ = 0;
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

QString makeRingName(const QString& channelId);
std::array<uint8_t, 16> sessionIdBytes(const QString& uuid);
QString sessionIdString(const std::array<uint8_t, 16>& bytes);

}
