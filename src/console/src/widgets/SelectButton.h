#pragma once
#include <QAbstractButton>
#include <QIcon>
#include <QString>

namespace fovea::ui {

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

}
