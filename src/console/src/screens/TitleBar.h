#pragma once
#include <QWidget>

class QAction;
class QMenu;
class QToolButton;

namespace fovea::ui {

class TitleBar : public QWidget {
  Q_OBJECT
public:
  explicit TitleBar(QWidget* parent = nullptr);
  QAction* stopServiceAction() const { return stopService_; }

signals:
  void minimizeRequested();
  void maximizeRequested();
  void closeRequested();

protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
  QToolButton* brandName_ = nullptr;
  QMenu* menu_ = nullptr;
  QAction* stopService_ = nullptr;
};

}
