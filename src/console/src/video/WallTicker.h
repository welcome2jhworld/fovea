#pragma once
#include <QObject>
#include <QTimer>

namespace fovea::ui {

// One 30 Hz timer shared by every FrameSurface so all tiles poll their ring
// and repaint on the same cadence.
class WallTicker : public QObject {
  Q_OBJECT
public:
  static constexpr int kIntervalMs = 33;
  static WallTicker& instance();

signals:
  void tick();

private:
  WallTicker();
  QTimer timer_;
};

}
