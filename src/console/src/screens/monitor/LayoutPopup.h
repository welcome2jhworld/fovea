#pragma once
#include "screens/monitor/WallLayout.h"
#include <QVector>
#include <QWidget>

namespace fovea::ui {

class PresetRow : public QWidget {
  Q_OBJECT
public:
  PresetRow(WallLayout layout, const QString& label, const QString& hint, QWidget* parent = nullptr);
  WallLayout layout() const { return layout_; }
  void setSelected(bool selected);

signals:
  void chosen(WallLayout layout);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;

private:
  WallLayout layout_;
  QString label_;
  QString hint_;
  bool selected_ = false;
};

class LayoutPopup : public QWidget {
  Q_OBJECT
public:
  explicit LayoutPopup(QWidget* parent = nullptr);
  void setCurrent(WallLayout layout);
  void showBelow(QWidget* anchor);

signals:
  void layoutChosen(WallLayout layout);
  void saveDefaultRequested();
  void closed();

protected:
  void hideEvent(QHideEvent* event) override;

private:
  QWidget* frame_ = nullptr;
  QVector<PresetRow*> rows_;
};

}
