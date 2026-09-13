#pragma once
#include "core/AlertTypes.h"
#include "fovea/Api.h"
#include <QAbstractListModel>
#include <QHash>
#include <QStyledItemDelegate>
#include <QVector>
#include <QWidget>

class QLabel;
class QListView;
class QPushButton;

namespace fovea::ui {

class EventStore;

struct FeedRow {
  EventInfo event;
  QString cameraLabel;
};

// Events newest first, as the core reports them.
class EventFeedModel : public QAbstractListModel {
  Q_OBJECT
public:
  explicit EventFeedModel(QObject* parent = nullptr);
  void setRows(QVector<FeedRow> rows);
  const FeedRow& rowAt(int row) const { return rows_[row]; }
  int indexOf(const QString& eventId) const;

  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;

private:
  QVector<FeedRow> rows_;
};

class EventRowDelegate : public QStyledItemDelegate {
  Q_OBJECT
public:
  static constexpr int kRowHeight = 86;
  explicit EventRowDelegate(QObject* parent = nullptr);
  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

// Monitor screen right panel (340): LIVE EVENTS, Filter, Acknowledge / Open case.
class LiveEventFeed : public QWidget {
  Q_OBJECT
public:
  LiveEventFeed(EventStore& store, QWidget* parent = nullptr);
  void setCameras(const QVector<fovea::Camera>& cameras);

signals:
  void openCaseRequested(const QString& eventId);

private:
  void rebuild();
  void select(const QString& eventId);
  void updateButtons();

  EventStore& store_;
  QHash<QString, QString> cameraLabels_;
  EventFeedModel* model_ = nullptr;
  QListView* list_ = nullptr;
  QLabel* empty_ = nullptr;
  QPushButton* acknowledge_ = nullptr;
  QPushButton* openCase_ = nullptr;
  QString selectedId_;
  // Until the operator picks a row, the footer acts on the newest unresolved event.
  bool followNewest_ = true;
  bool unresolvedOnly_ = false;
};

}
