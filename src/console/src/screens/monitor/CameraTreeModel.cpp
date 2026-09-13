#include "screens/monitor/CameraTreeModel.h"
#include <QHash>

namespace fovea::ui {

CameraTreeModel::CameraTreeModel(QObject* parent) : QAbstractItemModel(parent) {}

QString CameraTreeModel::structureKey(const QVector<Entry>& entries, const QVector<Group>& groups,
                                      const QVector<RootItem>& roots) {
  QString key;
  for (const RootItem& r : roots) {
    if (r.isGroup) {
      key += QLatin1Char('G') + groups[r.index].name + QLatin1Char('{');
      for (int c : groups[r.index].cameras) key += entries[c].camera.id + QLatin1Char(',');
      key += QLatin1Char('}');
    } else {
      key += QLatin1Char('C') + entries[r.index].camera.id + QLatin1Char(';');
    }
  }
  return key;
}

void CameraTreeModel::setSnapshot(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses) {
  QHash<QString, const fovea::CameraStatus*> statusById;
  for (const fovea::CameraStatus& s : statuses) statusById.insert(s.cameraId, &s);

  QVector<Entry> entries;
  QVector<Group> groups;
  QVector<RootItem> roots;
  QHash<QString, int> groupIndex;
  entries.reserve(cameras.size());
  for (const fovea::Camera& c : cameras) {
    Entry e;
    e.camera = c;
    const fovea::CameraStatus* s = statusById.value(c.id, nullptr);
    e.online = s && s->state == QLatin1StringView("online") && !s->stale;
    const int entryIdx = static_cast<int>(entries.size());
    entries.push_back(e);
    if (c.groupName.isEmpty()) {
      roots.push_back({false, entryIdx});
      continue;
    }
    auto it = groupIndex.find(c.groupName);
    if (it == groupIndex.end()) {
      it = groupIndex.insert(c.groupName, static_cast<int>(groups.size()));
      groups.push_back({c.groupName, {}});
      roots.push_back({true, it.value()});
    }
    groups[it.value()].cameras.push_back(entryIdx);
  }

  const bool sameStructure = structureKey(entries_, groups_, roots_) == structureKey(entries, groups, roots);
  if (sameStructure) {
    entries_ = entries;
    groups_ = groups;
    roots_ = roots;
    for (int r = 0; r < roots_.size(); ++r) {
      const QModelIndex rootIdx = index(r, 0);
      if (roots_[r].isGroup) {
        const int n = static_cast<int>(groups_[roots_[r].index].cameras.size());
        if (n > 0) emit dataChanged(index(0, 0, rootIdx), index(n - 1, 0, rootIdx));
      } else {
        emit dataChanged(rootIdx, rootIdx);
      }
    }
    return;
  }
  beginResetModel();
  entries_ = entries;
  groups_ = groups;
  roots_ = roots;
  endResetModel();
  emit structureChanged();
}

QModelIndex CameraTreeModel::indexForCamera(const QString& cameraId) const {
  for (int r = 0; r < roots_.size(); ++r) {
    if (roots_[r].isGroup) {
      const Group& g = groups_[roots_[r].index];
      for (int i = 0; i < g.cameras.size(); ++i)
        if (entries_[g.cameras[i]].camera.id == cameraId) return index(i, 0, index(r, 0));
    } else if (entries_[roots_[r].index].camera.id == cameraId) {
      return index(r, 0);
    }
  }
  return {};
}

QStringList CameraTreeModel::groupNames() const {
  QStringList names;
  for (const Group& g : groups_) names.push_back(g.name);
  return names;
}

QModelIndex CameraTreeModel::index(int row, int column, const QModelIndex& parent) const {
  if (column != 0 || row < 0) return {};
  if (!parent.isValid()) return row < roots_.size() ? createIndex(row, 0, kRootId) : QModelIndex();
  if (parent.internalId() != kRootId) return {};
  const RootItem& r = roots_[parent.row()];
  if (!r.isGroup || row >= groups_[r.index].cameras.size()) return {};
  return createIndex(row, 0, static_cast<quintptr>(parent.row() + 1));
}

QModelIndex CameraTreeModel::parent(const QModelIndex& child) const {
  if (!child.isValid() || child.internalId() == kRootId) return {};
  return createIndex(static_cast<int>(child.internalId() - 1), 0, kRootId);
}

int CameraTreeModel::rowCount(const QModelIndex& parent) const {
  if (!parent.isValid()) return static_cast<int>(roots_.size());
  if (parent.internalId() != kRootId) return 0;
  const RootItem& r = roots_[parent.row()];
  return r.isGroup ? static_cast<int>(groups_[r.index].cameras.size()) : 0;
}

int CameraTreeModel::columnCount(const QModelIndex&) const { return 1; }

const CameraTreeModel::Entry* CameraTreeModel::entryAt(const QModelIndex& index) const {
  if (!index.isValid()) return nullptr;
  if (index.internalId() == kRootId) {
    const RootItem& r = roots_[index.row()];
    return r.isGroup ? nullptr : &entries_[r.index];
  }
  const RootItem& r = roots_[static_cast<int>(index.internalId() - 1)];
  return &entries_[groups_[r.index].cameras[index.row()]];
}

QVariant CameraTreeModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid()) return {};
  const Entry* e = entryAt(index);
  if (!e) {
    const Group& g = groups_[roots_[index.row()].index];
    switch (role) {
      case Qt::DisplayRole:
      case GroupNameRole:
      case SearchTextRole: return g.name;
      case IsGroupRole: return true;
      default: return {};
    }
  }
  const fovea::Camera& c = e->camera;
  switch (role) {
    case Qt::DisplayRole: return c.name.isEmpty() ? c.code : c.name;
    case IsGroupRole: return false;
    case CameraIdRole: return c.id;
    case CodeRole: return c.code;
    case NameRole: return c.name;
    case GroupNameRole: return c.groupName;
    case OnlineRole: return e->online;
    case SearchTextRole: return c.code + QLatin1Char(' ') + c.name + QLatin1Char(' ') + c.groupName;
    case Qt::ToolTipRole: return c.code.isEmpty() ? c.name : c.code + QStringLiteral(" · ") + c.name;
    default: return {};
  }
}

Qt::ItemFlags CameraTreeModel::flags(const QModelIndex& index) const {
  if (!index.isValid()) return Qt::NoItemFlags;
  return entryAt(index) ? (Qt::ItemIsEnabled | Qt::ItemIsSelectable) : Qt::ItemIsEnabled;
}

}
