#include "fovea/core/SessionClock.h"

namespace fovea::core {

void SessionClock::start(int64_t firstPtsNs, int64_t startedUtcMs) {
  started_ = true;
  firstPtsNs_ = firstPtsNs;
  startedUtcMs_ = startedUtcMs;
  lastPtsNs_ = firstPtsNs;
}

int64_t SessionClock::utcForPts(int64_t ptsNs) const {
  if (!started_) return 0;
  const int64_t delta = ptsNs - firstPtsNs_;
  return startedUtcMs_ + (delta >= 0 ? delta / 1'000'000 : -((-delta) / 1'000'000));
}

bool SessionClock::observe(int64_t ptsNs) {
  if (!started_) return true;
  if (ptsNs < lastPtsNs_ - kBackwardsToleranceNs) return false;
  if (ptsNs > lastPtsNs_) lastPtsNs_ = ptsNs;
  return true;
}

}
