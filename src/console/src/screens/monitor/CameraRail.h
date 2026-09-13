#pragma once
#include "fovea/Api.h"
#include <QSet>
#include <QSortFilterProxyModel>
#include <QTreeView>
#include <QVector>
#include <QWidget>

class QLineEdit;

namespace fovea::ui {

class CameraTreeModel;

class CameraTree : public QTreeView {
  Q_OBJECT
public:
  explicit CameraTree(QWidget* parent = nullptr);
};

class CameraFilterProxy : public QSortFilterProxyModel {
  Q_OBJECT
public:
  explicit CameraFilterProxy(QObject* parent = nullptr);
  void setNeedle(const QString& needle);

protected:
  bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
  QString needle_;
};

class CameraRail : public QWidget {
  Q_OBJECT
public:
  explicit CameraRail(QWidget* parent = nullptr);

  void setSnapshot(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses);
  QString selectedCameraId() const { return selectedId_; }

signals:
  void addCameraRequested();
  void cameraSettingsRequested(const QString& cameraId);
  void selectedCameraChanged(const QString& cameraId);

private:
  void restoreViewState();
  void selectFirstCamera();
  void onSelectionChanged();

  CameraTreeModel* model_ = nullptr;
  CameraFilterProxy* proxy_ = nullptr;
  CameraTree* tree_ = nullptr;
  QLineEdit* filter_ = nullptr;
  QSet<QString> collapsedGroups_;
  QString selectedId_;
  bool restoring_ = false;
};

}
