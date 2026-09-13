#pragma once
#include "fovea/Api.h"
#include "screens/monitor/WallLayout.h"
#include <QHash>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QGridLayout;

namespace fovea::ui {

class VideoTile;

class EmptySlot : public QWidget {
  Q_OBJECT
public:
  explicit EmptySlot(QWidget* parent = nullptr);

protected:
  void paintEvent(QPaintEvent* event) override;
};

class VideoWall : public QWidget {
  Q_OBJECT
public:
  explicit VideoWall(QWidget* parent = nullptr);

  void setWallLayout(WallLayout layout);
  WallLayout wallLayout() const { return layout_; }
  bool isTileMaximized() const { return !maximizedId_.isEmpty(); }
  void setSnapshot(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses);

protected:
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dragMoveEvent(QDragMoveEvent* event) override;
  void dropEvent(QDropEvent* event) override;

private:
  void arrange();
  void toggleMaximize(VideoTile* tile);

  QGridLayout* grid_ = nullptr;
  QHash<QString, VideoTile*> tiles_;
  QStringList order_;
  // Operator's tile order (QSettings); cameras it does not name follow in API order.
  QStringList preferredOrder_;
  QVector<EmptySlot*> empties_;
  WallLayout layout_ = WallLayout::ThreeByThree;
  QString maximizedId_;
};

}
