#include "fovea/Clock.h"
#include <QDateTime>
#include <chrono>

namespace fovea {
int64_t monoNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}
int64_t utcNowMs() { return QDateTime::currentMSecsSinceEpoch(); }
}
