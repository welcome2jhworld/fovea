#pragma once
#include <cstdint>

namespace fovea::core {

// Maps media running time (pts_ns) of one StreamSession to UTC. The first
// observed buffer anchors the mapping; later buffers are checked for backwards
// jumps, which end the session (docs/ARCHITECTURE.md, "Time model").
class SessionClock {
public:
  static constexpr int64_t kBackwardsToleranceNs = 500'000'000;
  static constexpr const char* kBackwardsReason = "pts_backwards";

  void start(int64_t firstPtsNs, int64_t startedUtcMs);
  bool started() const { return started_; }
  int64_t firstPtsNs() const { return firstPtsNs_; }
  int64_t startedUtcMs() const { return startedUtcMs_; }
  int64_t lastPtsNs() const { return lastPtsNs_; }

  int64_t utcForPts(int64_t ptsNs) const;

  // Records ptsNs as the latest observed timestamp. Returns false when the
  // stream jumped backwards by more than the tolerance; the clock keeps the
  // previous high-water mark in that case.
  bool observe(int64_t ptsNs);

private:
  bool started_ = false;
  int64_t firstPtsNs_ = 0;
  int64_t startedUtcMs_ = 0;
  int64_t lastPtsNs_ = 0;
};

}
