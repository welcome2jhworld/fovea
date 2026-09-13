#pragma once
#include <cstdint>

namespace fovea {
class Backoff {
public:
  Backoff(int64_t initialMs = 1000, int64_t maxMs = 30000, double factor = 2.0);
  int64_t nextDelayMs();
  void reset();
  int attempts() const { return attempts_; }
private:
  int64_t initialMs_;
  int64_t maxMs_;
  double factor_;
  int attempts_ = 0;
};
}
