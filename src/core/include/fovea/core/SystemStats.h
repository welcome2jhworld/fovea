#pragma once
#include <QString>
#include <cstdint>

namespace fovea::core {

int64_t processRssBytes();
// User plus system CPU time of this process, all threads.
int64_t processCpuTimeNs();
// Bytes available to this user on the volume holding dir, or its nearest existing parent; -1 when unknown.
int64_t freeDiskBytes(const QString& dir);

// CPU use between successive samples, 100 = one core. A sample taken less
// than a second after the previous one repeats the previous value, so
// frequent callers do not see noise from tiny intervals.
class CpuSampler {
public:
  double sample(int64_t monoNs, int64_t cpuNs);

private:
  int64_t lastMonoNs_ = 0;
  int64_t lastCpuNs_ = 0;
  double lastPercent_ = 0;
};

}
