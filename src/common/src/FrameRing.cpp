#include "fovea/FrameRing.h"
#include "fovea/Clock.h"
#include <QCryptographicHash>
#include <QUuid>
#include <atomic>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fovea {
namespace {

constexpr uint32_t kMagic = 0x46564652;
constexpr uint32_t kVersion = 1;
constexpr size_t kRingHeaderBytes = 4096;

struct RingHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t slotCount;
  uint32_t slotBytes;
  uint32_t maxWidth;
  uint32_t maxHeight;
  uint32_t reserved0;
  uint32_t reserved1;
  uint64_t latestSeq;
  uint64_t writerPid;
  uint64_t createdMonoNs;
};
static_assert(sizeof(RingHeader) <= kRingHeaderBytes);

size_t slotStride(uint32_t slotBytes) { return sizeof(FrameHeader) + slotBytes; }

uint8_t* slotPtr(void* base, uint32_t slotBytes, uint32_t index) {
  return static_cast<uint8_t*>(base) + kRingHeaderBytes + slotStride(slotBytes) * index;
}

RingHeader* ringHeader(void* base) { return static_cast<RingHeader*>(base); }

size_t totalBytes(uint32_t slotCount, uint32_t slotBytes) {
  return kRingHeaderBytes + slotStride(slotBytes) * slotCount;
}

std::atomic_ref<uint64_t> seqRef(uint8_t* slot) {
  return std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(slot));
}

#ifdef _WIN32
void* mapCreate(const QString& name, size_t bytes, void*& handle) {
  const std::wstring w = (QStringLiteral("Local\\") + name).toStdWString();
  HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes & 0xffffffffu), w.c_str());
  if (!h) return nullptr;
  void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
  if (!p) { CloseHandle(h); return nullptr; }
  handle = h;
  return p;
}
void* mapOpen(const QString& name, size_t& bytes, void*& handle) {
  const std::wstring w = (QStringLiteral("Local\\") + name).toStdWString();
  HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, w.c_str());
  if (!h) return nullptr;
  void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, kRingHeaderBytes);
  if (!p) { CloseHandle(h); return nullptr; }
  const RingHeader* hdr = static_cast<const RingHeader*>(p);
  if (hdr->magic != kMagic || hdr->version != kVersion) { UnmapViewOfFile(p); CloseHandle(h); return nullptr; }
  bytes = totalBytes(hdr->slotCount, hdr->slotBytes);
  UnmapViewOfFile(p);
  p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
  if (!p) { CloseHandle(h); return nullptr; }
  handle = h;
  return p;
}
void mapClose(void* base, size_t, void* handle, const QString&, bool) {
  if (base) UnmapViewOfFile(base);
  if (handle) CloseHandle(static_cast<HANDLE>(handle));
}
bool pidAlive(uint64_t pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (!h) return false;
  DWORD code = 0;
  const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  CloseHandle(h);
  return alive;
}
uint64_t currentPid() { return GetCurrentProcessId(); }
#else
std::string shmName(const QString& name) { return "/" + name.toStdString(); }

void* mapCreate(const QString& name, size_t bytes, int& fd) {
  const std::string n = shmName(name);
  shm_unlink(n.c_str());
  fd = shm_open(n.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) return nullptr;
  if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) { close(fd); shm_unlink(n.c_str()); fd = -1; return nullptr; }
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) { close(fd); shm_unlink(n.c_str()); fd = -1; return nullptr; }
  return p;
}
void* mapOpen(const QString& name, size_t& bytes, int& fd) {
  const std::string n = shmName(name);
  fd = shm_open(n.c_str(), O_RDWR, 0600);
  if (fd < 0) return nullptr;
  void* p = mmap(nullptr, kRingHeaderBytes, PROT_READ, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) { close(fd); fd = -1; return nullptr; }
  const RingHeader* hdr = static_cast<const RingHeader*>(p);
  const bool ok = hdr->magic == kMagic && hdr->version == kVersion;
  const size_t total = ok ? totalBytes(hdr->slotCount, hdr->slotBytes) : 0;
  munmap(p, kRingHeaderBytes);
  if (!ok) { close(fd); fd = -1; return nullptr; }
  p = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) { close(fd); fd = -1; return nullptr; }
  bytes = total;
  return p;
}
void mapClose(void* base, size_t bytes, int fd, const QString& name, bool unlink) {
  if (base) munmap(base, bytes);
  if (fd >= 0) close(fd);
  if (unlink) shm_unlink(shmName(name).c_str());
}
bool pidAlive(uint64_t pid) { return kill(static_cast<pid_t>(pid), 0) == 0; }
uint64_t currentPid() { return static_cast<uint64_t>(getpid()); }
#endif

}

QString makeRingName(const QString& channelId) {
  const QByteArray digest = QCryptographicHash::hash(channelId.toUtf8(), QCryptographicHash::Sha1).toHex();
  return QStringLiteral("fv-") + QString::fromLatin1(digest.left(16));
}

std::array<uint8_t, 16> sessionIdBytes(const QString& uuid) {
  std::array<uint8_t, 16> out{};
  const QByteArray raw = QUuid::fromString(uuid).toRfc4122();
  if (raw.size() == 16) std::memcpy(out.data(), raw.constData(), 16);
  return out;
}

QString sessionIdString(const std::array<uint8_t, 16>& bytes) {
  return QUuid::fromRfc4122(QByteArray(reinterpret_cast<const char*>(bytes.data()), 16))
      .toString(QUuid::WithoutBraces);
}

std::unique_ptr<FrameRingWriter> FrameRingWriter::create(const QString& name, uint32_t slotCount,
                                                         uint32_t maxWidth, uint32_t maxHeight) {
  if (slotCount == 0 || maxWidth == 0 || maxHeight == 0) return nullptr;
  const uint32_t slotBytes = maxWidth * 4 * maxHeight;
  const size_t bytes = totalBytes(slotCount, slotBytes);
  std::unique_ptr<FrameRingWriter> w(new FrameRingWriter());
#ifdef _WIN32
  w->base_ = mapCreate(name, bytes, w->handle_);
#else
  w->base_ = mapCreate(name, bytes, w->fd_);
#endif
  if (!w->base_) return nullptr;
  w->mapped_ = bytes;
  std::memset(w->base_, 0, kRingHeaderBytes);
  RingHeader* hdr = ringHeader(w->base_);
  hdr->magic = kMagic;
  hdr->version = kVersion;
  hdr->slotCount = slotCount;
  hdr->slotBytes = slotBytes;
  hdr->maxWidth = maxWidth;
  hdr->maxHeight = maxHeight;
  hdr->latestSeq = 0;
  hdr->writerPid = currentPid();
  hdr->createdMonoNs = static_cast<uint64_t>(monoNowNs());
  for (uint32_t i = 0; i < slotCount; ++i) std::memset(slotPtr(w->base_, slotBytes, i), 0, sizeof(FrameHeader));
  w->info_ = RingInfo{name, slotCount, maxWidth, maxHeight, slotBytes, bytes};
  return w;
}

FrameRingWriter::~FrameRingWriter() {
#ifdef _WIN32
  mapClose(base_, mapped_, handle_, info_.name, true);
#else
  mapClose(base_, mapped_, fd_, info_.name, true);
#endif
}

bool FrameRingWriter::write(const FrameHeader& header, const uint8_t* pixels, size_t srcStride) {
  if (!base_ || header.width == 0 || header.height == 0) return false;
  if (header.width > info_.maxWidth || header.height > info_.maxHeight) return false;
  const uint32_t rowBytes = header.width * 4;
  if (srcStride < rowBytes) return false;
  const uint64_t seq = seq_ + 2;
  uint8_t* slot = slotPtr(base_, info_.slotBytes, static_cast<uint32_t>((seq / 2) % info_.slotCount));
  auto seqAtomic = seqRef(slot);
  seqAtomic.store(seq - 1, std::memory_order_release);
  std::atomic_thread_fence(std::memory_order_release);
  FrameHeader h = header;
  h.seq = seq;
  h.stride = rowBytes;
  uint8_t* dst = slot + sizeof(FrameHeader);
  for (uint32_t y = 0; y < header.height; ++y)
    std::memcpy(dst + static_cast<size_t>(y) * rowBytes, pixels + y * srcStride, rowBytes);
  std::memcpy(slot + sizeof(uint64_t), reinterpret_cast<const uint8_t*>(&h) + sizeof(uint64_t),
              sizeof(FrameHeader) - sizeof(uint64_t));
  std::atomic_thread_fence(std::memory_order_release);
  seqAtomic.store(seq, std::memory_order_release);
  std::atomic_ref<uint64_t>(ringHeader(base_)->latestSeq).store(seq, std::memory_order_release);
  seq_ = seq;
  return true;
}

std::unique_ptr<FrameRingReader> FrameRingReader::open(const QString& name) {
  std::unique_ptr<FrameRingReader> r(new FrameRingReader());
  size_t bytes = 0;
#ifdef _WIN32
  r->base_ = mapOpen(name, bytes, r->handle_);
#else
  r->base_ = mapOpen(name, bytes, r->fd_);
#endif
  if (!r->base_) return nullptr;
  r->mapped_ = bytes;
  const RingHeader* hdr = ringHeader(r->base_);
  r->info_ = RingInfo{name, hdr->slotCount, hdr->maxWidth, hdr->maxHeight, hdr->slotBytes, bytes};
  return r;
}

FrameRingReader::~FrameRingReader() {
#ifdef _WIN32
  mapClose(base_, mapped_, handle_, info_.name, false);
#else
  mapClose(base_, mapped_, fd_, info_.name, false);
#endif
}

std::optional<FrameHeader> FrameRingReader::latest() const {
  if (!base_) return std::nullopt;
  const uint64_t seq = std::atomic_ref<uint64_t>(ringHeader(base_)->latestSeq).load(std::memory_order_acquire);
  if (seq == 0) return std::nullopt;
  const uint8_t* slot = slotPtr(base_, info_.slotBytes, static_cast<uint32_t>((seq / 2) % info_.slotCount));
  FrameHeader h;
  std::memcpy(&h, slot, sizeof(FrameHeader));
  if (h.seq != seq) return std::nullopt;
  return h;
}

bool FrameRingReader::copyLatest(uint64_t& lastSeenSeq, FrameHeader& header, uint8_t* dst, size_t dstCapacity) {
  if (!base_) return false;
  for (int attempt = 0; attempt < 4; ++attempt) {
    const uint64_t seq = std::atomic_ref<uint64_t>(ringHeader(base_)->latestSeq).load(std::memory_order_acquire);
    if (seq == 0 || seq == lastSeenSeq) return false;
    uint8_t* slot = slotPtr(base_, info_.slotBytes, static_cast<uint32_t>((seq / 2) % info_.slotCount));
    auto seqAtomic = seqRef(slot);
    const uint64_t s1 = seqAtomic.load(std::memory_order_acquire);
    if (s1 & 1u) { ++torn_; continue; }
    std::atomic_thread_fence(std::memory_order_acquire);
    FrameHeader h;
    std::memcpy(&h, slot, sizeof(FrameHeader));
    const size_t bytes = static_cast<size_t>(h.stride) * h.height;
    if (h.width == 0 || h.height == 0 || bytes > dstCapacity || bytes > info_.slotBytes) return false;
    std::memcpy(dst, slot + sizeof(FrameHeader), bytes);
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t s2 = seqAtomic.load(std::memory_order_acquire);
    if (s1 != s2 || h.seq != s1) { ++torn_; continue; }
    header = h;
    lastSeenSeq = seq;
    return true;
  }
  return false;
}

bool FrameRingReader::writerAlive() const {
  return base_ && pidAlive(ringHeader(base_)->writerPid);
}

}
