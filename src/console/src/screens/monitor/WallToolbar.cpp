#include "screens/monitor/WallToolbar.h"
#include "screens/monitor/LayoutPopup.h"
#include "theme/Icons.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include <QFontMetrics>
#include <QHBoxLayout>
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

WallToolbar::WallToolbar(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("WallToolbar"));
  setAttribute(Qt::WA_StyledBackground, true);
  setFixedHeight(tk::size::toolbar);

  layoutButton_ = new SelectButton(this);
  layoutButton_->setObjectName(QStringLiteral("LayoutButton"));
  popup_ = new LayoutPopup(this);

  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(tk::size::wallToolbarPaddingX, 0, tk::size::wallToolbarPaddingX, 0);
  row->setSpacing(tk::size::wallToolbarGap);
  row->addStretch(1);
  row->addWidget(layoutButton_);

  connect(layoutButton_, &QAbstractButton::toggled, this, [this](bool checked) {
    if (!checked) return;
    popup_->setCurrent(layout_);
    popup_->showBelow(layoutButton_);
  });
  connect(popup_, &LayoutPopup::closed, this, [this] { layoutButton_->setChecked(false); });
  connect(popup_, &LayoutPopup::layoutChosen, this, [this](WallLayout layout) {
    setWallLayout(layout);
    emit layoutChosen(layout);
  });
  connect(popup_, &LayoutPopup::saveDefaultRequested, this, &WallToolbar::saveDefaultRequested);
  setWallLayout(layout_);
}

void WallToolbar::setWallLayout(WallLayout layout) {
  layout_ = layout;
  layoutButton_->setLabel(QStringLiteral("Layout: %1").arg(wallLayoutLabel(layout)));
  popup_->setCurrent(layout);
}

}
