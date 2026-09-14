#include "screens/search/CustomRangePopup.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include "widgets/FormField.h"
#include <QDateTimeEdit>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr QMargins kShadowMargins(24, 8, 24, 40);
constexpr int kPadding = 12;

QDateTimeEdit* dateTimeField(QWidget* parent) {
  auto* edit = new QDateTimeEdit(parent);
  edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
  edit->setButtonSymbols(QAbstractSpinBox::NoButtons);
  edit->setProperty("mono", true);
  return edit;
}
}

CustomRangePopup::CustomRangePopup(QWidget* parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
  setObjectName(QStringLiteral("CustomRangePopup"));
  setAttribute(Qt::WA_TranslucentBackground, true);

  auto* frame = new QWidget(this);
  frame->setObjectName(QStringLiteral("LayoutPopupFrame"));
  frame->setAttribute(Qt::WA_StyledBackground, true);
  frame->setFixedWidth(kWidth);
  auto* shadow = new QGraphicsDropShadowEffect(frame);
  shadow->setBlurRadius(40);
  shadow->setOffset(0, 16);
  shadow->setColor(tk::color::shadow());
  frame->setGraphicsEffect(shadow);

  auto* section = new QLabel(QStringLiteral("CUSTOM RANGE"), frame);
  section->setObjectName(QStringLiteral("PopupSectionLabel"));
  section->setFont(Theme::monoLabel());
  from_ = dateTimeField(frame);
  to_ = dateTimeField(frame);
  error_ = new QLabel(frame);
  error_->setProperty("tone", QStringLiteral("critical"));
  error_->setObjectName(QStringLiteral("DialogError"));
  error_->setWordWrap(true);
  error_->hide();

  auto* cancel = new QPushButton(QStringLiteral("Cancel"), frame);
  cancel->setProperty("role", QStringLiteral("ghost"));
  auto* apply = new QPushButton(QStringLiteral("Apply"), frame);
  apply->setProperty("role", QStringLiteral("primary"));
  for (QPushButton* b : {cancel, apply}) {
    Theme::setVariant(b, "sm");
    b->setCursor(Qt::PointingHandCursor);
  }
  apply->setDefault(true);
  auto* buttons = new QHBoxLayout();
  buttons->setContentsMargins(0, 4, 0, 0);
  buttons->setSpacing(8);
  buttons->addStretch(1);
  buttons->addWidget(cancel);
  buttons->addWidget(apply);

  auto* column = new QVBoxLayout(frame);
  column->setContentsMargins(kPadding, kPadding, kPadding, kPadding);
  column->setSpacing(10);
  column->addWidget(section);
  column->addWidget(formField(QStringLiteral("From"), from_, frame));
  column->addWidget(formField(QStringLiteral("To"), to_, frame));
  column->addWidget(error_);
  column->addLayout(buttons);

  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(kShadowMargins);
  outer->setSpacing(0);
  outer->addWidget(frame);

  connect(cancel, &QPushButton::clicked, this, &QWidget::hide);
  connect(apply, &QPushButton::clicked, this, &CustomRangePopup::apply);
  connect(from_, &QDateTimeEdit::editingFinished, error_, &QWidget::hide);
  connect(to_, &QDateTimeEdit::editingFinished, error_, &QWidget::hide);
}

void CustomRangePopup::showBelow(QWidget* anchor, const TimeRange& initial) {
  from_->setDateTime(QDateTime::fromMSecsSinceEpoch(initial.fromUtcMs).toLocalTime());
  to_->setDateTime(QDateTime::fromMSecsSinceEpoch(initial.toUtcMs).toLocalTime());
  error_->hide();
  adjustSize();
  const QPoint anchorBottomLeft = anchor->mapToGlobal(anchor->rect().bottomLeft());
  move(anchorBottomLeft.x() - kShadowMargins.left(), anchorBottomLeft.y() + 1 + tk::size::popupOffset - kShadowMargins.top());
  show();
  from_->setFocus();
}

void CustomRangePopup::apply() {
  const int64_t from = from_->dateTime().toMSecsSinceEpoch();
  const int64_t to = to_->dateTime().toMSecsSinceEpoch();
  if (to <= from) {
    error_->setText(QStringLiteral("The end must be after the start."));
    error_->show();
    adjustSize();
    return;
  }
  emit applied({from, to});
  hide();
}

void CustomRangePopup::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  emit closed();
}

void CustomRangePopup::keyPressEvent(QKeyEvent* event) {
  switch (event->key()) {
    case Qt::Key_Escape: hide(); return;
    case Qt::Key_Return:
    case Qt::Key_Enter: apply(); return;
    default: QWidget::keyPressEvent(event);
  }
}

}
