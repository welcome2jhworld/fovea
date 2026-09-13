#pragma once
#include <QIcon>
#include <QStyledItemDelegate>

namespace fovea::ui {

class CameraTreeDelegate : public QStyledItemDelegate {
  Q_OBJECT
public:
  explicit CameraTreeDelegate(QObject* parent = nullptr);
  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
  QIcon groupOpen_;
  QIcon groupClosed_;
  QIcon camera_;
};

}
