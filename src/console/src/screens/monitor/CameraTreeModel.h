#pragma once
#include "fovea/Api.h"
#include <QAbstractItemModel>
#include <QStringList>
#include <QVector>

namespace fovea::ui {

// Groups (distinct group_name, first-seen order) with camera rows beneath;
// cameras without a group sit at the root.
class CameraTreeModel : public QAbstractItemModel {
  Q_OBJECT
public:
  enum Role {
    IsGroupRole = Qt::UserRole + 1,
    CameraIdRole,
    CodeRole,
    NameRole,
    GroupNameRole,
    OnlineRole,
    SearchTextRole,
  };

  explicit CameraTreeModel(QObject* parent = nullptr);

  void setSnapshot(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses);
  QModelIndex indexForCamera(const QString& cameraId) const;
  QStringList groupNames() const;

  QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
  QModelIndex parent(const QModelIndex& child) const override;
  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;

signals:
  void structureChanged();

private:
  struct Entry {
    fovea::Camera camera;
    bool online = false;
  };
  struct Group {
    QString name;
    QVector<int> cameras;
  };
  struct RootItem {
    bool isGroup = false;
    int index = 0;
  };
  static constexpr quintptr kRootId = 0;

  const Entry* entryAt(const QModelIndex& index) const;
  static QString structureKey(const QVector<Entry>& entries, const QVector<Group>& groups,
                              const QVector<RootItem>& roots);

  QVector<Entry> entries_;
  QVector<Group> groups_;
  QVector<RootItem> roots_;
};

}
