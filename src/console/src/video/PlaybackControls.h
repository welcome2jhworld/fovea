#pragma once
#include <QWidget>
#include <cstdint>

namespace fovea::ui {

// 34 px scrim bar over the player: play/pause, 3 px track with accent fill, mono time.
class PlaybackControls : public QWidget {
  Q_OBJECT
public:
  static constexpr int kHeight = 34;
  explicit PlaybackControls(QWidget* parent = nullptr);
  void setState(bool playing, int64_t positionNs, int64_t durationNs, bool interactive);

signals:
  void toggleRequested();
  void seekRequested(double fraction);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;

private:
  QRect glyphRect() const;
  QRect trackRect() const;
  bool playing_ = false;
  int64_t positionNs_ = 0;
  int64_t durationNs_ = 0;
  bool interactive_ = false;
};

}
