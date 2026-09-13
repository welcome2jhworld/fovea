#include "screens/monitor/LayoutPopup.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QVBoxLayout>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr QMargins kShadowMargins(24, 8, 24, 40);
}

PresetRow::PresetRow(WallLayout layout, const QString& label, const QString& hint, QWidget* parent)
    : QWidget(parent), layout_(layout), label_(label), hint_(hint) {
  setFixedHeight(tk::size::popupRow);
  setCursor(Qt::PointingHandCursor);
}

void PresetRow::setSelected(bool selected) {
  if (selected_ == selected) return;
  selected_ = selected;
  update();
}

void PresetRow::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  if (selected_) {
    p.setPen(Qt::NoPen);
    p.setBrush(tk::color::q(tk::color::bgRaised));
    p.drawRoundedRect(rect(), tk::radius::control, tk::radius::control);
  }
  const QRect inner = rect().adjusted(tk::size::popupRowPaddingX, 0, -tk::size::popupRowPaddingX, 0);
  p.setFont(Theme::body());
  p.setPen(tk::color::q(selected_ ? tk::color::textPrimary : tk::color::textSecondary));
  p.drawText(inner, Qt::AlignVCenter | Qt::AlignLeft, label_);
  p.setFont(Theme::mono(tk::font::label));
  p.setPen(tk::color::q(selected_ ? tk::color::accent : tk::color::textDisabled));
  p.drawText(inner, Qt::AlignVCenter | Qt::AlignRight, hint_);
}

void PresetRow::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) emit chosen(layout_);
}

LayoutPopup::LayoutPopup(QWidget* parent) : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
  setObjectName(QStringLiteral("LayoutPopup"));
  setAttribute(Qt::WA_TranslucentBackground, true);

  frame_ = new QWidget(this);
  frame_->setObjectName(QStringLiteral("LayoutPopupFrame"));
  frame_->setAttribute(Qt::WA_StyledBackground, true);
  frame_->setFixedWidth(tk::size::layoutPopup);
  auto* shadow = new QGraphicsDropShadowEffect(frame_);
  shadow->setBlurRadius(40);
  shadow->setOffset(0, 16);
  shadow->setColor(tk::color::shadow());
  frame_->setGraphicsEffect(shadow);

  auto* section = new QLabel(QStringLiteral("GRID"), frame_);
  section->setObjectName(QStringLiteral("PopupSectionLabel"));
  QFont sectionFont = Theme::monoMicro();
  sectionFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
  section->setFont(sectionFont);
  section->setContentsMargins(12, 8, 12, 6);

  const struct { WallLayout layout; const char* label; const char* hint; } presets[] = {
      {WallLayout::OneByOne, "1 × 1", "1 tile"},
      {WallLayout::TwoByTwo, "2 × 2", "4 tiles"},
      {WallLayout::ThreeByThree, "3 × 3", "9 tiles"}};

  auto* divider = new QFrame(frame_);
  divider->setObjectName(QStringLiteral("PopupDivider"));
  divider->setFrameShape(QFrame::NoFrame);
  divider->setAttribute(Qt::WA_StyledBackground, true);
  auto* dividerWrap = new QWidget(frame_);
  auto* dividerRow = new QVBoxLayout(dividerWrap);
  dividerRow->setContentsMargins(4, 6, 4, 6);
  dividerRow->setSpacing(0);
  dividerRow->addWidget(divider);

  auto* saveRow = new QWidget(frame_);
  auto* saveLabel = new QLabel(QStringLiteral("Save as default"), saveRow);
  saveLabel->setObjectName(QStringLiteral("PopupRowLabel"));
  auto* save = new QPushButton(QStringLiteral("Save"), saveRow);
  save->setProperty("role", QStringLiteral("link"));
  save->setCursor(Qt::PointingHandCursor);
  save->setFocusPolicy(Qt::NoFocus);
  auto* saveLayout = new QHBoxLayout(saveRow);
  saveLayout->setContentsMargins(12, 2, 12, 8);
  saveLayout->setSpacing(0);
  saveLayout->addWidget(saveLabel);
  saveLayout->addStretch(1);
  saveLayout->addWidget(save);

  auto* column = new QVBoxLayout(frame_);
  column->setContentsMargins(tk::size::popupPadding, tk::size::popupPadding, tk::size::popupPadding, tk::size::popupPadding);
  column->setSpacing(tk::size::popupGap);
  column->addWidget(section);
  for (const auto& preset : presets) {
    auto* row = new PresetRow(preset.layout, QString::fromUtf8(preset.label), QString::fromUtf8(preset.hint), frame_);
    connect(row, &PresetRow::chosen, this, [this](WallLayout layout) {
      setCurrent(layout);
      emit layoutChosen(layout);
      hide();
    });
    rows_.push_back(row);
    column->addWidget(row);
  }
  column->addWidget(dividerWrap);
  column->addWidget(saveRow);

  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(kShadowMargins);
  outer->setSpacing(0);
  outer->addWidget(frame_);

  connect(save, &QPushButton::clicked, this, [this] {
    emit saveDefaultRequested();
    hide();
  });
}

void LayoutPopup::setCurrent(WallLayout layout) {
  for (PresetRow* row : rows_) row->setSelected(row->layout() == layout);
}

void LayoutPopup::showBelow(QWidget* anchor) {
  adjustSize();
  const QPoint anchorBottomRight = anchor->mapToGlobal(anchor->rect().bottomRight());
  const int x = anchorBottomRight.x() + 1 - tk::size::layoutPopup - kShadowMargins.left();
  const int y = anchorBottomRight.y() + 1 + tk::size::popupOffset - kShadowMargins.top();
  move(x, y);
  setWindowOpacity(0.0);
  show();
  auto* fade = new QPropertyAnimation(this, "windowOpacity", this);
  fade->setDuration(tk::motion::popupFadeMs);
  fade->setStartValue(0.0);
  fade->setEndValue(1.0);
  fade->setEasingCurve(QEasingCurve::OutCubic);
  fade->start(QAbstractAnimation::DeleteWhenStopped);
}

void LayoutPopup::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  emit closed();
}

}
