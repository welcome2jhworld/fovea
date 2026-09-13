#pragma once
#include "theme/Tone.h"
#include <QString>
#include <QWidget>

namespace fovea::ui {

// Mono 10 text on its tone tint (panel header severity, evidence state).
class ToneChip : public QWidget {
  Q_OBJECT
public:
  ToneChip(int height, int paddingX, QWidget* parent = nullptr);
  void set(const QString& text, Tone tone);
  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  int height_;
  int paddingX_;
  QString text_;
  Tone tone_ = Tone::Neutral;
};

}
