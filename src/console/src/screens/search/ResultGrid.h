#pragma once
#include "core/SearchTypes.h"
#include <QAbstractListModel>
#include <QListView>
#include <QStyledItemDelegate>
#include <QVector>

namespace fovea::ui {

class ThumbnailCache;

struct SearchResultRow {
  SearchResultInfo result;
  QString cameraName;
  QString cameraCode;
};

// Ranked results, or a fixed number of skeleton cards while a query runs.
class SearchResultsModel : public QAbstractListModel {
  Q_OBJECT
public:
  explicit SearchResultsModel(QObject* parent = nullptr);
  void setRows(QVector<SearchResultRow> rows);
  void setSkeleton(int count);
  bool isSkeleton() const { return skeleton_ > 0; }
  const SearchResultRow& rowAt(int row) const { return rows_[row]; }

  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;

private:
  QVector<SearchResultRow> rows_;
  int skeleton_ = 0;
};

// Result card: 158 px thumbnail with the relevance badge top-left and the length badge
// bottom-right, then camera name, camera code and start time. The cell carries the grid gap.
class ResultCardDelegate : public QStyledItemDelegate {
  Q_OBJECT
public:
  static constexpr int kGap = 14;
  static constexpr int kThumbHeight = 158;

  explicit ResultCardDelegate(ThumbnailCache& thumbnails, QObject* parent = nullptr);
  static int cardHeight();
  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
  ThumbnailCache& thumbnails_;
};

// QListView in IconMode with a fixed cell size: four columns while cards stay at least
// kMinCardWidth wide, fewer on narrow windows. Space asks the inspector to play or pause.
class ResultGrid : public QListView {
  Q_OBJECT
public:
  static constexpr int kColumns = 4;
  static constexpr int kMinCardWidth = 240;

  explicit ResultGrid(QWidget* parent = nullptr);

signals:
  void playbackToggleRequested();

protected:
  void keyPressEvent(QKeyEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void updateGeometries() override;

private:
  static constexpr int kWrapSlack = 2;
  void updateGrid();
};

}
