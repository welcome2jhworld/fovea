#include "fovea/core/SystemStats.h"
#include <QFileInfo>
#include <QStorageInfo>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#else
#include <QFile>
#include <unistd.h>
#endif
#endif

namespace fovea::core {

int64_t processRssBytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
  return static_cast<int64_t>(pmc.WorkingSetSize);
#elif defined(__APPLE__)
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0;
  return static_cast<int64_t>(info.resident_size);
#else
  QFile statm(QStringLiteral("/proc/self/statm"));
  if (!statm.open(QIODevice::ReadOnly)) return 0;
  const QList<QByteArray> fields = statm.readAll().simplified().split(' ');
  if (fields.size() < 2) return 0;
  return fields[1].toLongLong() * static_cast<int64_t>(sysconf(_SC_PAGESIZE));
#endif
}

int64_t processCpuTimeNs() {
#if defined(_WIN32)
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
  const auto hundredNs = [](const FILETIME& t) {
    return (static_cast<int64_t>(t.dwHighDateTime) << 32) | static_cast<int64_t>(t.dwLowDateTime);
  };
  return (hundredNs(kernel) + hundredNs(user)) * 100;
#else
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
  const auto ns = [](const timeval& t) {
    return static_cast<int64_t>(t.tv_sec) * 1'000'000'000 + static_cast<int64_t>(t.tv_usec) * 1000;
  };
  return ns(usage.ru_utime) + ns(usage.ru_stime);
#endif
}

int64_t freeDiskBytes(const QString& dir) {
  for (QString path = QFileInfo(dir).absoluteFilePath();;) {
    const QStorageInfo info(path);
    if (info.isValid()) return info.bytesAvailable();
    const QString parent = QFileInfo(path).path();
    if (parent == path) return -1;
    path = parent;
  }
}

double CpuSampler::sample(int64_t monoNs, int64_t cpuNs) {
  constexpr int64_t kMinIntervalNs = 1'000'000'000;
  if (lastMonoNs_ == 0) {
    lastMonoNs_ = monoNs;
    lastCpuNs_ = cpuNs;
    return lastPercent_;
  }
  const int64_t wall = monoNs - lastMonoNs_;
  if (wall < kMinIntervalNs) return lastPercent_;
  lastPercent_ = static_cast<double>(cpuNs - lastCpuNs_) * 100.0 / static_cast<double>(wall);
  lastMonoNs_ = monoNs;
  lastCpuNs_ = cpuNs;
  return lastPercent_;
}

}
