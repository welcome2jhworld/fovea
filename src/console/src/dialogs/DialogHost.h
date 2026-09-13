#pragma once
#include <QPointer>
#include <QWidget>

namespace fovea::ui {

class DialogFrame;

// Backdrop scrim covering the main window; shows one DialogFrame at a time,
// centred, with the design's drop shadow painted around it.
class DialogHost : public QWidget {
  Q_OBJECT
public:
  explicit DialogHost(QWidget* parent = nullptr);

  void open(DialogFrame* dialog);
  void closeDialog();
  // Deletes the open dialog now; called while the window's services still exist.
  void shutdown();
  bool isOpen() const { return dialog_ != nullptr; }
  DialogFrame* dialog() const { return dialog_; }

signals:
  void opened();
  void closed();

protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  void place();
  QPointer<DialogFrame> dialog_;
};

}
