#include "widgets/StatusChip.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include <QFontMetrics>
#include <QPainter>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kHeight = 22;
constexpr int kPaddingX = 8;
constexpr int kDot = 5;
constexpr int kDotGap = 6;

QColor toneColor(StatusChip::Tone tone) {
  switch (tone) {
    case StatusChip::Tone::Info: return tk::color::q(tk::color::accent);
    case StatusChip::Tone::Positive: return tk::color::q(tk::color::positive);
    case StatusChip::Tone::Warning: return tk::color::q(tk::color::warning);
    case StatusChip::Tone::Critical: return tk::color::q(tk::color::critical);
    case StatusChip::Tone::Neutral: break;
  }
  return tk::color::q(tk::color::textMuted);
}

QColor toneTint(StatusChip::Tone tone) {
  switch (tone) {
    case StatusChip::Tone::Info: return tk::color::tintAccent();
    case StatusChip::Tone::Positive: return tk::color::tintPositive();
    case StatusChip::Tone::Warning: return tk::color::tintWarning();
    case StatusChip::Tone::Critical: return tk::color::tintCritical();
    case StatusChip::Tone::Neutral: break;
  }
  return tk::color::tintNeutral();
}
}

StatusChip::StatusChip(QWidget* parent) : QWidget(parent) {
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void StatusChip::set(const QString& text, Tone tone, bool dot) {
  text_ = text;
  tone_ = tone;
  dot_ = dot;
  updateGeometry();
  update();
}

QSize StatusChip::sizeHint() const {
  const QFontMetrics fm(Theme::monoLabel());
  const int extra = dot_ ? kDot + kDotGap : 0;
  return QSize(2 * kPaddingX + extra + fm.horizontalAdvance(text_) + 2, kHeight);
}

void StatusChip::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  const QColor color = toneColor(tone_);
  p.setPen(Qt::NoPen);
  p.setBrush(toneTint(tone_));
  p.drawRoundedRect(rect(), tk::radius::chip, tk::radius::chip);
  int x = kPaddingX;
  if (dot_) {
    p.setBrush(color);
    p.drawEllipse(QRect(x, (height() - kDot) / 2, kDot, kDot));
    x += kDot + kDotGap;
  }
  p.setFont(Theme::monoLabel());
  p.setPen(color);
  p.drawText(QRect(x, 0, width() - x, height()), Qt::AlignVCenter | Qt::AlignLeft, text_);
}

}
