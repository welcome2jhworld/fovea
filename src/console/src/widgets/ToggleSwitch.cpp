#include "widgets/ToggleSwitch.h"
#include "theme/Tokens.h"
#include <QPainter>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kPadding = 2;
constexpr int kSlideMs = 140;
}

ToggleSwitch::ToggleSwitch(Size size, QWidget* parent) : QAbstractButton(parent), size_(size) {
  setCheckable(true);
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::TabFocus);
  setFixedSize(sizeHint());
  slide_.setDuration(kSlideMs);
  slide_.setEasingCurve(QEasingCurve::OutCubic);
  connect(&slide_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
    knob_ = v.toDouble();
    update();
  });
}

QSize ToggleSwitch::sizeHint() const {
  return size_ == Size::Md ? QSize(tk::size::toggleMdWidth, tk::size::toggleMdHeight)
                           : QSize(tk::size::toggleSmWidth, tk::size::toggleSmHeight);
}

void ToggleSwitch::animateTo(bool on) {
  slide_.stop();
  slide_.setStartValue(knob_);
  slide_.setEndValue(on ? 1.0 : 0.0);
  slide_.start();
}

void ToggleSwitch::checkStateSet() {
  slide_.stop();
  knob_ = isChecked() ? 1.0 : 0.0;
  update();
}

void ToggleSwitch::nextCheckState() {
  QAbstractButton::nextCheckState();
  animateTo(isChecked());
}

void ToggleSwitch::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  const bool on = isChecked();
  const int knobSize = height() - 2 * kPadding;
  const double radius = height() / 2.0;
  p.setPen(Qt::NoPen);
  p.setBrush(tk::color::q(on ? tk::color::accent : tk::color::lineStrong));
  p.drawRoundedRect(QRectF(rect()), radius, radius);
  const double travel = width() - 2 * kPadding - knobSize;
  const QRectF knob(kPadding + travel * knob_, kPadding, knobSize, knobSize);
  p.setBrush(tk::color::q(on ? tk::color::accentInk : tk::color::textSecondary));
  p.drawEllipse(knob);
  if (hasFocus()) {
    p.setPen(QPen(tk::color::q(tk::color::accent), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
  }
}

}
