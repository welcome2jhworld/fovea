#include "fovea/Backoff.h"
#include <algorithm>
#include <cmath>

namespace fovea {
Backoff::Backoff(int64_t initialMs, int64_t maxMs, double factor)
    : initialMs_(initialMs), maxMs_(maxMs), factor_(factor) {}

int64_t Backoff::nextDelayMs() {
  const double raw = static_cast<double>(initialMs_) * std::pow(factor_, attempts_);
  ++attempts_;
  return std::min<int64_t>(maxMs_, static_cast<int64_t>(raw));
}

void Backoff::reset() { attempts_ = 0; }
}
