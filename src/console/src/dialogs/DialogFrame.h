#pragma once
#include <QWidget>

class QLabel;
class QHBoxLayout;
class QVBoxLayout;

namespace fovea::ui {

// 880-wide sheet (header 52 / body / footer 60) that DialogHost centres over
// the window. Painted, not a QDialog: it lives inside the main window so the
// backdrop scrim and screenshots include it.
class DialogFrame : public QWidget {
  Q_OBJECT
public:
  explicit DialogFrame(QWidget* parent = nullptr);

  void setTitle(const QString& title);
  void setSubtitle(const QString& subtitle);
  QWidget* body() const { return body_; }
  QVBoxLayout* bodyLayout() const { return bodyLayout_; }
  QWidget* footer();
  QHBoxLayout* footerLayout();

signals:
  void closeRequested();

protected:
  void paintEvent(QPaintEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

private:
  QLabel* title_ = nullptr;
  QLabel* subtitle_ = nullptr;
  QWidget* body_ = nullptr;
  QVBoxLayout* bodyLayout_ = nullptr;
  QWidget* footer_ = nullptr;
  QHBoxLayout* footerLayout_ = nullptr;
  QVBoxLayout* column_ = nullptr;
};

}
