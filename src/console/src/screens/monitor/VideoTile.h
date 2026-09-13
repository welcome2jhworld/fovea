#pragma once
#include "core/AlertTypes.h"
#include "fovea/Api.h"
#include <QColor>
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QVariantAnimation>
#include <QVector>
#include <QWidget>
#include <optional>

namespace fovea::ui {

class FrameSurface;

struct TileChrome {
  QString id;
  QString name;
  QString state;
  QColor stateColor;
  bool pulsing = false;
  QString footerLeft;
  QColor footerLeftColor;
  QString footerRight;
  QString caption;

  bool operator==(const TileChrome& o) const {
    return id == o.id && name == o.name && state == o.state && stateColor == o.stateColor && pulsing == o.pulsing &&
           footerLeft == o.footerLeft && footerLeftColor == o.footerLeftColor && footerRight == o.footerRight &&
           caption == o.caption;
  }
};

struct OverlayBox {
  QRectF rect;
  QString label;
  bool operator==(const OverlayBox& o) const { return rect == o.rect && label == o.label; }
};

// Transparent layer over the frame: detection boxes, chips, gradient footer, border.
// Repaints on the status and analytics ticks, never on the frame rate (only the LIVE dot animates).
class TileOverlay : public QWidget {
  Q_OBJECT
public:
  explicit TileOverlay(QWidget* parent = nullptr);
  void setChrome(const TileChrome& chrome);
  void setBoxes(const QVector<OverlayBox>& boxes);
  void setDotOpacity(double opacity);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  void paintBoxes(QPainter& p) const;

  TileChrome chrome_;
  QVector<OverlayBox> boxes_;
  double dotOpacity_ = 1.0;
  QRect stateChipRect_;
};

class VideoTile : public QWidget {
  Q_OBJECT
public:
  // Drag payload carrying the camera id when the operator reorders the wall.
  static constexpr const char* kDragMimeType = "application/x-fovea-camera-id";

  explicit VideoTile(QWidget* parent = nullptr);

  void setStatus(const fovea::Camera& camera, const fovea::CameraStatus& status);
  QString cameraId() const { return camera_.id; }
  bool analyticsEnabled() const { return camera_.analyticsEnabled; }
  void setOverlaysEnabled(bool enabled);
  void setDetections(const std::optional<DetectionFrame>& frame);
  FrameSurface* surface() const { return surface_; }
  void setDragEnabled(bool enabled) { dragEnabled_ = enabled; }

signals:
  void maximizeToggled(VideoTile* tile);

protected:
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
  void refreshChrome();
  void refreshBoxes();
  void updatePulse();
  int64_t lastFrameAgeMs() const;
  TileChrome computeChrome() const;

  FrameSurface* surface_ = nullptr;
  TileOverlay* overlay_ = nullptr;
  QVariantAnimation pulse_;
  fovea::Camera camera_;
  fovea::CameraStatus status_;
  TileChrome chrome_;
  QPoint pressPos_;
  bool dragEnabled_ = true;
  bool overlays_ = false;
  std::optional<DetectionFrame> detections_;
  int ticks_ = 0;
  int boxTicks_ = 0;
};

}
