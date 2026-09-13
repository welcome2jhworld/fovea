#include "widgets/ToneChip.h"
#include "theme/Theme.h"
#include "widgets/Painting.h"
#include <QPainter>

namespace fovea::ui {

ToneChip::ToneChip(int height, int paddingX, QWidget* parent) : QWidget(parent), height_(height), paddingX_(paddingX) {
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void ToneChip::set(const QString& text, Tone tone) {
  text_ = text;
  tone_ = tone;
  updateGeometry();
  update();
}

QSize ToneChip::sizeHint() const { return QSize(chipWidth(Theme::monoMicro(), text_, paddingX_), height_); }

void ToneChip::paintEvent(QPaintEvent*) {
  if (text_.isEmpty()) return;
  QPainter p(this);
  paintToneChip(p, QPoint(0, 0), text_, tone_, height_, paddingX_, Theme::monoMicro());
}

}
