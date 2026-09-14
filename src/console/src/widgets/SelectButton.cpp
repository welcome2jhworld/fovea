#include "widgets/SelectButton.h"
#include "theme/Icons.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include <QFontMetrics>
#include <QPainter>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kSelectPaddingX = 12;
constexpr int kSelectGap = 8;
}

SelectButton::SelectButton(QWidget* parent)
    : QAbstractButton(parent),
      caretClosed_(themedIcon(Icon::CaretDown, tk::color::q(tk::color::textMuted))),
      caretOpen_(themedIcon(Icon::CaretDown, tk::color::q(tk::color::accent))) {
  setCheckable(true);
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::NoFocus);
  setFixedHeight(tk::size::buttonMd);
}

void SelectButton::setLabel(const QString& label) {
  if (label_ == label) return;
  label_ = label;
  updateGeometry();
  update();
}

QSize SelectButton::sizeHint() const {
  const QFontMetrics fm(Theme::body());
  return QSize(kSelectPaddingX * 2 + fm.horizontalAdvance(label_) + kSelectGap + tk::size::glyphCaret,
               tk::size::buttonMd);
}

void SelectButton::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  const bool open = isChecked();
  p.setPen(QPen(tk::color::q(open ? tk::color::accent : tk::color::line), 1.0));
  p.setBrush(tk::color::q(open ? tk::color::bgRaised : tk::color::bgPanel));
  p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), tk::radius::input, tk::radius::input);
  p.setFont(Theme::body());
  const QRect inner = rect().adjusted(kSelectPaddingX, 0, -kSelectPaddingX, 0);
  p.setPen(tk::color::q(tk::color::textPrimary));
  p.drawText(inner, Qt::AlignVCenter | Qt::AlignLeft, label_);
  const QRect caret(inner.right() + 1 - tk::size::glyphCaret, (height() - tk::size::glyphCaret) / 2,
                    tk::size::glyphCaret, tk::size::glyphCaret);
  (open ? caretOpen_ : caretClosed_).paint(&p, caret);
}

}
