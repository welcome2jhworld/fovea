#pragma once
#include "fovea/Api.h"
#include <QPointF>
#include <QSize>
#include <QVector>
#include <QWidget>
#include <optional>

namespace fovea::ui {

class FrameSurface;

// Zone polygon drawn over a camera's live frame. Click adds a point, drag moves
// one, right-click or double-click removes one. Points are normalized to the
// frame; the reference size is the size of the frame the zone was drawn on.
class ZoneCanvas : public QWidget {
  Q_OBJECT
public:
  static constexpr int kHeight = 200;

  explicit ZoneCanvas(QWidget* parent = nullptr);

  void bind(const std::optional<fovea::RingRef>& ring, const QString& session);
  void setZone(const QVector<QPointF>& points, const QSize& referenceSize);
  void clearPoints();
  // The rule API accepts zones of 3 to kMaxPoints vertices.
  static constexpr int kMaxPoints = 32;
  QVector<QPointF> points() const { return points_; }
  // The frame on screen when there is one, otherwise the size the loaded zone was drawn at.
  QSize referenceSize() const;
  bool hasFrame() const;

signals:
  // Points edited or the live frame appeared or went away.
  void changed();

protected:
  void resizeEvent(QResizeEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
  friend class ZoneOverlay;
  QRectF frameArea() const;
  QPointF toWidget(const QPointF& normalized) const;
  QPointF toNormalized(const QPointF& widgetPos) const;
  int vertexAt(const QPointF& widgetPos) const;

  FrameSurface* surface_ = nullptr;
  QWidget* overlay_ = nullptr;
  QVector<QPointF> points_;
  QSize loadedReference_;
  int dragging_ = -1;
  bool hadFrame_ = false;
};

}
