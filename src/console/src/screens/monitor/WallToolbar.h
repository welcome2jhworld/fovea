#pragma once
#include "screens/monitor/WallLayout.h"
#include <QAbstractButton>
#include <QIcon>
#include <QWidget>

namespace fovea::ui {

class LayoutPopup;

// Select-style trigger with a separately coloured caret (QSS allows one colour per widget).
class SelectButton : public QAbstractButton {
  Q_OBJECT
public:
  explicit SelectButton(QWidget* parent = nullptr);
  void setLabel(const QString& label);
  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  QString label_;
  QIcon caretClosed_;
  QIcon caretOpen_;
};

class WallToolbar : public QWidget {
  Q_OBJECT
public:
  explicit WallToolbar(QWidget* parent = nullptr);
  void setWallLayout(WallLayout layout);

signals:
  void layoutChosen(WallLayout layout);
  void saveDefaultRequested();

private:
  SelectButton* layoutButton_ = nullptr;
  LayoutPopup* popup_ = nullptr;
  WallLayout layout_ = WallLayout::ThreeByThree;
};

}
