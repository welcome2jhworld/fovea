#include "screens/monitor/VideoWall.h"
#include "screens/monitor/VideoTile.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include <QDropEvent>
#include <QGridLayout>
#include <QMimeData>
#include <QPainter>
#include <QSettings>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kMaxColumns = 3;
const QString kOrderKey = QStringLiteral("wall/order");
}

EmptySlot::EmptySlot(QWidget* parent) : QWidget(parent) {}

void EmptySlot::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setPen(QPen(tk::color::q(tk::color::line), 1.0));
  p.setBrush(tk::color::q(tk::color::bgVideo));
  p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), tk::radius::card, tk::radius::card);
  p.setFont(Theme::placeholderCaption(height()));
  p.setPen(tk::color::q(tk::color::textPlaceholderCaption));
  p.drawText(rect(), Qt::AlignCenter, QStringLiteral("EMPTY SLOT"));
}

VideoWall::VideoWall(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("VideoWall"));
  setAttribute(Qt::WA_StyledBackground, true);
  grid_ = new QGridLayout(this);
  grid_->setContentsMargins(tk::size::wallPadding, tk::size::wallPadding, tk::size::wallPadding, tk::size::wallPadding);
  grid_->setSpacing(tk::size::wallGap);
  setAcceptDrops(true);
  preferredOrder_ = QSettings().value(kOrderKey).toStringList();
  arrange();
}

void VideoWall::setWallLayout(WallLayout layout) {
  if (layout == layout_) return;
  layout_ = layout;
  maximizedId_.clear();
  arrange();
}

void VideoWall::setSnapshot(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses) {
  QHash<QString, const fovea::CameraStatus*> statusById;
  for (const fovea::CameraStatus& s : statuses) statusById.insert(s.cameraId, &s);

  QStringList apiOrder;
  for (const fovea::Camera& c : cameras) {
    apiOrder.push_back(c.id);
    VideoTile* tile = tiles_.value(c.id, nullptr);
    if (!tile) {
      tile = new VideoTile(this);
      tile->setOverlaysEnabled(overlays_);
      tile->hide();
      connect(tile, &VideoTile::maximizeToggled, this, &VideoWall::toggleMaximize);
      tiles_.insert(c.id, tile);
    }
    const fovea::CameraStatus* s = statusById.value(c.id, nullptr);
    tile->setStatus(c, s ? *s : fovea::CameraStatus{});
  }
  QStringList order;
  for (const QString& id : preferredOrder_)
    if (apiOrder.contains(id)) order.push_back(id);
  for (const QString& id : apiOrder)
    if (!order.contains(id)) order.push_back(id);
  for (auto it = tiles_.begin(); it != tiles_.end();) {
    if (order.contains(it.key())) {
      ++it;
      continue;
    }
    it.value()->deleteLater();
    it = tiles_.erase(it);
  }
  if (!maximizedId_.isEmpty() && !tiles_.contains(maximizedId_)) maximizedId_.clear();
  if (order != order_) {
    order_ = order;
    arrange();
  }
}

QStringList VideoWall::visibleCameraIds() const { return visible_; }

void VideoWall::setOverlaysEnabled(bool enabled) {
  overlays_ = enabled;
  for (VideoTile* tile : std::as_const(tiles_)) tile->setOverlaysEnabled(enabled);
}

void VideoWall::popToFirstSlot(const QString& cameraId) {
  const qsizetype from = order_.indexOf(cameraId);
  if (from < 0) return;
  if (!maximizedId_.isEmpty() && maximizedId_ != cameraId) maximizedId_ = cameraId;
  if (from > 0) order_.swapItemsAt(0, from);
  preferredOrder_ = order_;
  arrange();
}

void VideoWall::toggleMaximize(VideoTile* tile) {
  maximizedId_ = maximizedId_ == tile->cameraId() ? QString() : tile->cameraId();
  arrange();
}

void VideoWall::dragEnterEvent(QDragEnterEvent* event) {
  if (event->mimeData()->hasFormat(QString::fromLatin1(VideoTile::kDragMimeType))) event->acceptProposedAction();
}

void VideoWall::dragMoveEvent(QDragMoveEvent* event) {
  if (event->mimeData()->hasFormat(QString::fromLatin1(VideoTile::kDragMimeType))) event->acceptProposedAction();
}

// Dropping on another tile swaps the two; dropping on an empty slot moves the tile last.
void VideoWall::dropEvent(QDropEvent* event) {
  const QString sourceId =
      QString::fromUtf8(event->mimeData()->data(QString::fromLatin1(VideoTile::kDragMimeType)));
  const qsizetype from = order_.indexOf(sourceId);
  if (from < 0) return;
  QWidget* target = childAt(event->position().toPoint());
  while (target && target->parentWidget() != this) target = target->parentWidget();
  if (!target) return;
  if (auto* tile = qobject_cast<VideoTile*>(target)) {
    const qsizetype to = order_.indexOf(tile->cameraId());
    if (to < 0 || to == from) return;
    order_.swapItemsAt(from, to);
  } else if (qobject_cast<EmptySlot*>(target)) {
    order_.move(from, order_.size() - 1);
  } else {
    return;
  }
  event->acceptProposedAction();
  preferredOrder_ = order_;
  QSettings().setValue(kOrderKey, preferredOrder_);
  arrange();
}

void VideoWall::arrange() {
  while (QLayoutItem* item = grid_->takeAt(0)) delete item;
  for (VideoTile* tile : tiles_) tile->hide();
  for (EmptySlot* slot : empties_) slot->hide();

  const int columns = maximizedId_.isEmpty() ? wallColumns(layout_) : 1;
  for (int i = 0; i < kMaxColumns; ++i) {
    grid_->setColumnStretch(i, i < columns ? 1 : 0);
    grid_->setRowStretch(i, i < columns ? 1 : 0);
  }

  QStringList visible;
  for (VideoTile* tile : tiles_) tile->setDragEnabled(maximizedId_.isEmpty());
  if (!maximizedId_.isEmpty()) {
    VideoTile* tile = tiles_.value(maximizedId_);
    grid_->addWidget(tile, 0, 0);
    tile->show();
    visible.push_back(maximizedId_);
  } else {
    const int slotCount = columns * columns;
    int emptyIndex = 0;
    for (int i = 0; i < slotCount; ++i) {
      QWidget* w = nullptr;
      if (i < order_.size()) {
        w = tiles_.value(order_[i]);
        visible.push_back(order_[i]);
      } else {
        if (emptyIndex >= empties_.size()) empties_.push_back(new EmptySlot(this));
        w = empties_[emptyIndex++];
      }
      grid_->addWidget(w, i / columns, i % columns);
      w->show();
    }
  }
  if (visible != visible_) {
    visible_ = visible;
    emit visibleCamerasChanged();
  }
}

}
