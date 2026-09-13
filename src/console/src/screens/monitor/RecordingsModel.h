#pragma once
#include "fovea/Api.h"
#include <QAbstractListModel>
#include <QStyledItemDelegate>
#include <QVector>

namespace fovea::ui {

struct RecordingRow {
  enum class Kind { Segment, Gap };
  Kind kind = Kind::Segment;
  fovea::RecordingSegment segment;
  fovea::ReceiveGap gap;
  int64_t sortUtcMs = 0;
};

// Segments and receive gaps of one camera, newest first.
class RecordingsModel : public QAbstractListModel {
  Q_OBJECT
public:
  enum Role { KindRole = Qt::UserRole + 1, PlayableRole };
  explicit RecordingsModel(QObject* parent = nullptr);

  void setRows(QVector<fovea::RecordingSegment> segments, QVector<fovea::ReceiveGap> gaps, int64_t nowUtcMs);
  const RecordingRow& rowAt(const QModelIndex& index) const { return rows_[index.row()]; }
  int64_t nowUtcMs() const { return nowUtcMs_; }

  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;

private:
  QVector<RecordingRow> rows_;
  int64_t nowUtcMs_ = 0;
};

class RecordingRowDelegate : public QStyledItemDelegate {
  Q_OBJECT
public:
  static constexpr int kRowHeight = 58;
  explicit RecordingRowDelegate(QObject* parent = nullptr);
  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
  bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option,
                   const QModelIndex& index) override;

signals:
  void playClicked(const QModelIndex& index);

private:
  static QRect playRect(const QRect& row);
};

}
