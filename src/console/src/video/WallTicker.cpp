#include "video/WallTicker.h"

namespace fovea::ui {

WallTicker& WallTicker::instance() {
  static WallTicker ticker;
  return ticker;
}

WallTicker::WallTicker() {
  timer_.setTimerType(Qt::PreciseTimer);
  timer_.setInterval(kIntervalMs);
  connect(&timer_, &QTimer::timeout, this, &WallTicker::tick);
  timer_.start();
}

}
