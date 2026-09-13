#pragma once
#include <QString>
#include <QWidget>

namespace fovea::ui {

// Mono 11 chip with an optional 5 px dot; tones map to the README chip tints.
class StatusChip : public QWidget {
  Q_OBJECT
public:
  enum class Tone { Neutral, Info, Positive, Warning, Critical };
  explicit StatusChip(QWidget* parent = nullptr);
  void set(const QString& text, Tone tone, bool dot);
  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  QString text_;
  Tone tone_ = Tone::Neutral;
  bool dot_ = false;
};

}
