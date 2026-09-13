#include <QCoreApplication>
#include <QNativeIpcKey>
#include <QSharedMemory>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr uint32_t kMagic = 0x464f5645;
constexpr uint32_t kSlots = 3;
constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr uint32_t kStride = kWidth * 4;
constexpr size_t kFrameBytes = size_t(kStride) * kHeight;
constexpr size_t kSegmentBytes = size_t(12) * 1024 * 1024;
constexpr const char *kPosixName = "/fovea-spike-ring";

struct alignas(64) RingHeader {
    uint32_t magic;
    uint32_t slotCount;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t slotBytes;
    uint32_t totalFrames;
    uint32_t writerPid;
    uint32_t latest;
    uint32_t heartbeat;
    uint32_t writerDone;
};

struct alignas(64) SlotHeader {
    uint32_t seq;
    uint32_t frameId;
    uint64_t monoNs;
};

constexpr size_t kSlotBytes = ((sizeof(SlotHeader) + kFrameBytes + 63) / 64) * 64;
static_assert(sizeof(RingHeader) + kSlots * kSlotBytes <= kSegmentBytes);

uint64_t monoNs()
{
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

SlotHeader *slotAt(void *base, uint32_t idx)
{
    auto *p = static_cast<unsigned char *>(base) + sizeof(RingHeader) + size_t(idx) * kSlotBytes;
    return reinterpret_cast<SlotHeader *>(p);
}

unsigned char *payloadOf(SlotHeader *s)
{
    return reinterpret_cast<unsigned char *>(s) + sizeof(SlotHeader);
}

QNativeIpcKey ringKey()
{
    return QNativeIpcKey(QString::fromLatin1(kPosixName), QNativeIpcKey::Type::PosixRealtime);
}

bool posixObjectExists()
{
    int fd = ::shm_open(kPosixName, O_RDONLY, 0);
    if (fd < 0)
        return false;
    ::close(fd);
    return true;
}

const char *typeName(QNativeIpcKey::Type t)
{
    switch (t) {
    case QNativeIpcKey::Type::SystemV: return "SystemV";
    case QNativeIpcKey::Type::PosixRealtime: return "PosixRealtime";
    case QNativeIpcKey::Type::Windows: return "Windows";
    }
    return "?";
}

int probe()
{
    for (auto t : { QNativeIpcKey::Type::PosixRealtime, QNativeIpcKey::Type::SystemV, QNativeIpcKey::Type::Windows })
        std::printf("isKeyTypeSupported(%s) = %d\n", typeName(t), QSharedMemory::isKeyTypeSupported(t));
    std::printf("DefaultTypeForOs = %s, legacyDefaultTypeForOs = %s\n",
                typeName(QNativeIpcKey::DefaultTypeForOs), typeName(QNativeIpcKey::legacyDefaultTypeForOs()));
    std::printf("atomic<uint32_t>::is_always_lock_free = %d, atomic_ref<uint32_t>::is_always_lock_free = %d, required_alignment = %zu\n",
                std::atomic<uint32_t>::is_always_lock_free, std::atomic_ref<uint32_t>::is_always_lock_free,
                std::atomic_ref<uint32_t>::required_alignment);
    std::printf("atomic_ref<uint64_t>::is_always_lock_free = %d\n", std::atomic_ref<uint64_t>::is_always_lock_free);

    for (qsizetype mb : { 12, 3 }) {
        QSharedMemory sysv(QSharedMemory::platformSafeKey(QStringLiteral("fovea_spike_sysv"), QNativeIpcKey::Type::SystemV));
        bool ok = sysv.create(mb * 1024 * 1024);
        std::printf("SystemV create(%lld MB) = %d error=%d \"%s\"\n", (long long)mb, ok, int(sysv.error()),
                    qPrintable(sysv.errorString()));
        if (ok)
            sysv.detach();
    }

    ::shm_unlink(kPosixName);
    QSharedMemory posix(ringKey());
    bool ok = posix.create(kSegmentBytes);
    std::printf("PosixRealtime create(12 MB) = %d size=%lld error=%d \"%s\" nativeKey=%s\n", ok, (long long)posix.size(),
                int(posix.error()), qPrintable(posix.errorString()), qPrintable(posix.nativeKey()));
    if (ok) {
        std::memset(posix.data(), 0xAB, kSegmentBytes);
        std::atomic_ref<uint32_t> a(static_cast<RingHeader *>(posix.data())->magic);
        std::printf("atomic_ref in mapped memory: is_lock_free=%d fetch_add ok=%d\n", a.is_lock_free(),
                    a.fetch_add(1) + 1 == a.load());
        posix.detach();
        std::printf("after creator detach: posix object exists = %d\n", posixObjectExists());
    }
    ::shm_unlink(kPosixName);
    return ok ? 0 : 1;
}

int writer(uint32_t frames, int holdMs)
{
    QSharedMemory shm(ringKey());
    if (!shm.create(kSegmentBytes)) {
        std::printf("writer: create failed error=%d \"%s\"\n", int(shm.error()), qPrintable(shm.errorString()));
        if (shm.error() == QSharedMemory::AlreadyExists) {
            std::printf("writer: stale object, shm_unlink and retry\n");
            ::shm_unlink(kPosixName);
            if (!shm.create(kSegmentBytes))
                return 1;
        } else {
            return 1;
        }
    }
    void *base = shm.data();
    std::memset(base, 0, kSegmentBytes);
    auto *hdr = static_cast<RingHeader *>(base);
    hdr->magic = kMagic;
    hdr->slotCount = kSlots;
    hdr->width = kWidth;
    hdr->height = kHeight;
    hdr->stride = kStride;
    hdr->slotBytes = uint32_t(kSlotBytes);
    hdr->totalFrames = frames;
    hdr->writerPid = uint32_t(::getpid());
    hdr->latest = UINT32_MAX;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::printf("writer: pid=%d segment=%lld bytes, %u frames at 25 fps\n", ::getpid(), (long long)shm.size(), frames);
    std::fflush(stdout);

    const auto period = std::chrono::milliseconds(40);
    auto next = std::chrono::steady_clock::now();
    for (uint32_t id = 1; id <= frames; ++id) {
        uint32_t idx = id % kSlots;
        SlotHeader *s = slotAt(base, idx);
        std::atomic_ref<uint32_t> seq(s->seq);
        uint32_t s0 = seq.load(std::memory_order_relaxed);
        seq.store(s0 + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);

        unsigned char *p = payloadOf(s);
        std::memset(p, int(id & 0xff), kFrameBytes);
        for (size_t off = 0; off < kFrameBytes; off += 4096)
            std::memcpy(p + off, &id, sizeof(id));
        s->frameId = id;
        s->monoNs = monoNs();

        seq.store(s0 + 2, std::memory_order_release);
        std::atomic_ref<uint32_t>(hdr->latest).store(idx, std::memory_order_release);
        std::atomic_ref<uint32_t>(hdr->heartbeat).fetch_add(1, std::memory_order_release);

        next += period;
        std::this_thread::sleep_until(next);
    }
    std::atomic_ref<uint32_t>(hdr->writerDone).store(1, std::memory_order_release);
    std::printf("writer: done, holding %d ms then exiting (QSharedMemory destructor detaches)\n", holdMs);
    std::fflush(stdout);
    QThread::msleep(holdMs);
    return 0;
}

int reader()
{
    QSharedMemory shm(ringKey());
    int attempts = 0;
    while (!shm.attach()) {
        if (++attempts > 200) {
            std::printf("reader: attach failed error=%d \"%s\"\n", int(shm.error()), qPrintable(shm.errorString()));
            return 1;
        }
        QThread::msleep(25);
    }
    void *base = shm.data();
    auto *hdr = static_cast<RingHeader *>(base);
    while (std::atomic_ref<uint32_t>(hdr->magic).load(std::memory_order_acquire) != kMagic)
        QThread::msleep(1);
    std::printf("reader: attached after %d attempts, size=%lld, writerPid=%u, slots=%u %ux%u\n", attempts + 1,
                (long long)shm.size(), hdr->writerPid, hdr->slotCount, hdr->width, hdr->height);

    std::vector<unsigned char> copy(kFrameBytes);
    uint32_t lastId = 0, framesRead = 0, torn = 0, payloadBad = 0, skipped = 0;
    uint64_t latMin = UINT64_MAX, latMax = 0, latSum = 0;
    uint32_t lastHeartbeat = 0;
    auto lastProgress = std::chrono::steady_clock::now();
    const char *exitReason = "?";

    for (;;) {
        uint32_t idx = std::atomic_ref<uint32_t>(hdr->latest).load(std::memory_order_acquire);
        if (idx != UINT32_MAX) {
            SlotHeader *s = slotAt(base, idx);
            std::atomic_ref<uint32_t> seq(s->seq);
            uint32_t frameId = 0;
            uint64_t stamp = 0;
            bool got = false;
            for (int retry = 0; retry < 64 && !got; ++retry) {
                uint32_t s1 = seq.load(std::memory_order_acquire);
                if (s1 & 1u) {
                    ++torn;
                    continue;
                }
                frameId = s->frameId;
                stamp = s->monoNs;
                std::memcpy(copy.data(), payloadOf(s), kFrameBytes);
                std::atomic_thread_fence(std::memory_order_acquire);
                uint32_t s2 = seq.load(std::memory_order_relaxed);
                if (s1 == s2)
                    got = true;
                else
                    ++torn;
            }
            if (got && frameId != lastId) {
                uint64_t lat = monoNs() - stamp;
                latMin = std::min(latMin, lat);
                latMax = std::max(latMax, lat);
                latSum += lat;
                for (size_t off = 0; off < kFrameBytes; off += 4096) {
                    uint32_t v;
                    std::memcpy(&v, copy.data() + off, sizeof(v));
                    if (v != frameId) {
                        ++payloadBad;
                        break;
                    }
                }
                if (lastId && frameId != lastId + 1)
                    skipped += frameId - lastId - 1;
                lastId = frameId;
                ++framesRead;
                lastProgress = std::chrono::steady_clock::now();
            }
        }
        uint32_t hb = std::atomic_ref<uint32_t>(hdr->heartbeat).load(std::memory_order_acquire);
        if (hb != lastHeartbeat) {
            lastHeartbeat = hb;
            lastProgress = std::chrono::steady_clock::now();
        }
        if (std::atomic_ref<uint32_t>(hdr->writerDone).load(std::memory_order_acquire) && lastId == hdr->totalFrames) {
            exitReason = "writerDone flag";
            break;
        }
        if (::kill(pid_t(hdr->writerPid), 0) == -1 && errno == ESRCH) {
            exitReason = "writer pid gone (kill(pid,0)==ESRCH)";
            break;
        }
        if (std::chrono::steady_clock::now() - lastProgress > std::chrono::seconds(2)) {
            exitReason = "heartbeat stalled 2 s";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::printf("reader: frames_read=%u torn_reads=%u payload_mismatch=%u skipped=%u exit=%s\n", framesRead, torn,
                payloadBad, skipped, exitReason);
    if (framesRead)
        std::printf("reader: latency us min=%.1f avg=%.1f max=%.1f\n", latMin / 1000.0,
                    latSum / 1000.0 / framesRead, latMax / 1000.0);

    pid_t wpid = pid_t(hdr->writerPid);
    for (int i = 0; i < 400 && !(::kill(wpid, 0) == -1 && errno == ESRCH); ++i)
        QThread::msleep(10);
    bool writerAlive = !(::kill(wpid, 0) == -1 && errno == ESRCH);
    std::printf("post: writer alive=%d, mapped data still readable magic=0x%x latest=%u\n", writerAlive, hdr->magic,
                hdr->latest);
    std::printf("post: posix object exists after writer exit = %d\n", posixObjectExists());
    QSharedMemory second(ringKey());
    bool secondAttach = second.attach();
    std::printf("post: fresh QSharedMemory::attach after writer exit = %d error=%d \"%s\"\n", secondAttach,
                int(second.error()), qPrintable(second.errorString()));
    if (secondAttach)
        second.detach();
    QSharedMemory creator(ringKey());
    bool created = creator.create(kSegmentBytes);
    std::printf("post: fresh QSharedMemory::create after writer exit = %d error=%d \"%s\"\n", created, int(creator.error()),
                qPrintable(creator.errorString()));
    if (created)
        creator.detach();
    shm.detach();
    std::printf("post: posix object exists after reader detach = %d\n", posixObjectExists());
    ::shm_unlink(kPosixName);
    std::printf("post: posix object exists after shm_unlink = %d\n", posixObjectExists());
    return (framesRead > 0 && payloadBad == 0) ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.contains(QStringLiteral("--probe")))
        return probe();
    if (args.contains(QStringLiteral("--writer"))) {
        int hold = 500;
        int i = args.indexOf(QStringLiteral("--hold-ms"));
        if (i > 0 && i + 1 < args.size())
            hold = args[i + 1].toInt();
        return writer(100, hold);
    }
    if (args.contains(QStringLiteral("--reader")))
        return reader();
    if (args.contains(QStringLiteral("--cleanup")))
        return ::shm_unlink(kPosixName) == 0 ? 0 : 1;
    std::printf("usage: shm_ring --probe | --writer [--hold-ms N] | --reader | --cleanup\n");
    return 2;
}
