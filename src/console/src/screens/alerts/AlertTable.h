#pragma once
#include "alerts/AlertLogic.h"
#include "core/AlertTypes.h"
#include "fovea/Api.h"
#include <QAbstractListModel>
#include <QHash>
#include <QStyledItemDelegate>
#include <QVector>
#include <QWidget>
#include <array>

class QButtonGroup;
class QComboBox;
class QLabel;
class QListView;
class QToolButton;

namespace fovea::ui {

class EventStore;
class ThumbnailCache;

struct AlertRow {
  EventInfo event;
  QString ruleName;
  QString cameraLabel;
};

class AlertTableModel : public QAbstractListModel {
  Q_OBJECT
public:
  explicit AlertTableModel(QObject* parent = nullptr);
  void setRows(QVector<AlertRow> rows);
  const AlertRow& rowAt(int row) const { return rows_[row]; }
  int indexOf(const QString& eventId) const;

  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;

private:
  QVector<AlertRow> rows_;
};

// Row 76: preview thumbnail, alert title and detail, rule, camera, time, status chip.
class AlertRowDelegate : public QStyledItemDelegate {
  Q_OBJECT
public:
  AlertRowDelegate(ThumbnailCache& thumbnails, QObject* parent = nullptr);
  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
  ThumbnailCache& thumbnails_;
};

class AlertTableHeader : public QWidget {
  Q_OBJECT
public:
  explicit AlertTableHeader(QWidget* parent = nullptr);

protected:
  void paintEvent(QPaintEvent* event) override;
};

// Alert log centre column: status tabs with counts, severity and range filters, table.
class AlertTable : public QWidget {
  Q_OBJECT
public:
  AlertTable(EventStore& store, ThumbnailCache& thumbnails, QWidget* parent = nullptr);
  void setCameras(const QVector<fovea::Camera>& cameras);
  // Shows the event's tab (clearing filters that hide it) and selects its row.
  void selectEvent(const QString& eventId);

signals:
  void selectedEventChanged(const QString& eventId);

private:
  void rebuild();
  bool passesFilters(const EventInfo& event) const;

  EventStore& store_;
  QHash<QString, QString> cameraLabels_;
  AlertTableModel* model_ = nullptr;
  QListView* list_ = nullptr;
  QLabel* empty_ = nullptr;
  QButtonGroup* tabGroup_ = nullptr;
  std::array<QToolButton*, 3> tabs_{};
  QComboBox* severity_ = nullptr;
  QComboBox* range_ = nullptr;
  AlertBucket bucket_ = AlertBucket::Unresolved;
  QString selectedId_;
  // Set while the model is reset so the selection model's churn is not taken as an operator choice.
  bool rebuilding_ = false;
};

}
