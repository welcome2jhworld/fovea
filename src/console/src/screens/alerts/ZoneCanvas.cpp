#include "screens/alerts/ZoneCanvas.h"
#include "theme/Tokens.h"
#include "video/FrameSurface.h"
#include "video/WallTicker.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <algorithm>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr double kStroke = 1.5;
constexpr double kHandle = 7.0;
constexpr double kHitRadius = 8.0;
}

class ZoneOverlay : public QWidget {
public:
  explicit ZoneOverlay(ZoneCanvas* canvas) : QWidget(canvas), canvas_(canvas) {
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
  }

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPolygonF polygon;
    for (const QPointF& pt : canvas_->points_) polygon << canvas_->toWidget(pt);
    const QColor accent = tk::color::q(tk::color::accent);
    p.setPen(QPen(accent, kStroke));
    if (polygon.size() >= 3) {
      p.setBrush(tk::color::tintAccent());
      p.drawPolygon(polygon);
    } else if (polygon.size() == 2) {
      p.drawPolyline(polygon);
    }
    p.setPen(QPen(accent, 1.0));
    p.setBrush(tk::color::q(tk::color::textPrimary));
    for (const QPointF& pt : std::as_const(polygon))
      p.drawRoundedRect(QRectF(pt.x() - kHandle / 2, pt.y() - kHandle / 2, kHandle, kHandle), 1, 1);
    p.setPen(QPen(tk::color::q(tk::color::line), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), tk::radius::input, tk::radius::input);
  }

private:
  ZoneCanvas* canvas_;
};

ZoneCanvas::ZoneCanvas(QWidget* parent) : QWidget(parent) {
  setFixedHeight(kHeight);
  setCursor(Qt::CrossCursor);
  surface_ = new FrameSurface(this);
  surface_->setCornerRadius(tk::radius::input);
  surface_->setCaption(QStringLiteral("NO LIVE FRAME"));
  overlay_ = new ZoneOverlay(this);
  overlay_->raise();
  connect(&WallTicker::instance(), &WallTicker::tick, this, [this] {
    const bool has = surface_->hasFrame();
    if (has == hadFrame_) return;
    hadFrame_ = has;
    surface_->setCaption(has ? QString() : QStringLiteral("WAITING FOR FRAME"));
    overlay_->update();
    emit changed();
  });
}

void ZoneCanvas::bind(const std::optional<fovea::RingRef>& ring, const QString& session) {
  surface_->bind(ring, session);
  surface_->setCaption(surface_->hasFrame() ? QString() : ring ? QStringLiteral("WAITING FOR FRAME") : QStringLiteral("NO LIVE FRAME"));
}

void ZoneCanvas::setZone(const QVector<QPointF>& points, const QSize& referenceSize) {
  points_ = points;
  loadedReference_ = referenceSize;
  dragging_ = -1;
  overlay_->update();
  emit changed();
}

void ZoneCanvas::clearPoints() {
  points_.clear();
  dragging_ = -1;
  overlay_->update();
  emit changed();
}

bool ZoneCanvas::hasFrame() const { return surface_->hasFrame(); }

QSize ZoneCanvas::referenceSize() const { return surface_->hasFrame() ? surface_->frameSize() : loadedReference_; }

QRectF ZoneCanvas::frameArea() const {
  if (surface_->hasFrame()) return surface_->imageRect();
  if (loadedReference_.isEmpty()) return QRectF(rect());
  const QSizeF fitted = QSizeF(loadedReference_).scaled(QSizeF(size()), Qt::KeepAspectRatio);
  return QRectF(QPointF((width() - fitted.width()) / 2.0, (height() - fitted.height()) / 2.0), fitted);
}

QPointF ZoneCanvas::toWidget(const QPointF& normalized) const {
  const QRectF area = frameArea();
  return QPointF(area.left() + normalized.x() * area.width(), area.top() + normalized.y() * area.height());
}

QPointF ZoneCanvas::toNormalized(const QPointF& widgetPos) const {
  const QRectF area = frameArea();
  return QPointF(std::clamp((widgetPos.x() - area.left()) / area.width(), 0.0, 1.0),
                 std::clamp((widgetPos.y() - area.top()) / area.height(), 0.0, 1.0));
}

int ZoneCanvas::vertexAt(const QPointF& widgetPos) const {
  for (int i = 0; i < points_.size(); ++i) {
    const QPointF d = toWidget(points_[i]) - widgetPos;
    if (d.x() * d.x() + d.y() * d.y() <= kHitRadius * kHitRadius) return i;
  }
  return -1;
}

void ZoneCanvas::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  surface_->setGeometry(rect());
  overlay_->setGeometry(rect());
}

// Editing needs the live frame: its size becomes the zone's reference size.
void ZoneCanvas::mousePressEvent(QMouseEvent* event) {
  if (!surface_->hasFrame()) return;
  const QPointF pos = event->position();
  const int hit = vertexAt(pos);
  if (event->button() == Qt::RightButton) {
    if (hit >= 0) {
      points_.removeAt(hit);
      overlay_->update();
      emit changed();
    }
    return;
  }
  if (event->button() != Qt::LeftButton) return;
  if (hit >= 0) {
    dragging_ = hit;
    return;
  }
  if (!surface_->imageRect().contains(pos) || points_.size() >= kMaxPoints) return;
  points_.push_back(toNormalized(pos));
  overlay_->update();
  emit changed();
}

void ZoneCanvas::mouseMoveEvent(QMouseEvent* event) {
  if (dragging_ < 0 || dragging_ >= points_.size()) return;
  points_[dragging_] = toNormalized(event->position());
  overlay_->update();
}

void ZoneCanvas::mouseReleaseEvent(QMouseEvent*) {
  if (dragging_ < 0) return;
  dragging_ = -1;
  emit changed();
}

void ZoneCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
  const int hit = vertexAt(event->position());
  if (hit < 0 || !surface_->hasFrame()) return;
  points_.removeAt(hit);
  overlay_->update();
  emit changed();
}

}
