#include "screens/monitor/WallToolbar.h"
#include "screens/monitor/LayoutPopup.h"
#include "theme/Icons.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QMenu>
#include <QToolButton>
#include <algorithm>
#include <QPainter>
#include <QSignalBlocker>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kSelectPaddingX = 12;
constexpr int kSelectGap = 8;
}

RulesButton::RulesButton(QWidget* parent)
    : QAbstractButton(parent), caret_(themedIcon(Icon::CaretDown, tk::color::q(tk::color::textMuted))) {
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::NoFocus);
  setFixedHeight(tk::size::buttonMd);
  setApplied(0);
}

void RulesButton::setApplied(int count) {
  value_ = QStringLiteral("%1 applied").arg(count);
  updateGeometry();
  update();
}

QSize RulesButton::sizeHint() const {
  const QFontMetrics fm(Theme::body());
  const int text = fm.horizontalAdvance(QStringLiteral("Rules: ")) + fm.horizontalAdvance(value_);
  return QSize(kSelectPaddingX * 2 + text + kSelectGap + tk::size::glyphCaret, tk::size::buttonMd);
}

void RulesButton::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setPen(QPen(tk::color::q(tk::color::line), 1.0));
  p.setBrush(tk::color::q(tk::color::bgPanel));
  p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), tk::radius::input, tk::radius::input);
  p.setFont(Theme::body());
  const QFontMetrics fm(p.font());
  const QString prefix = QStringLiteral("Rules: ");
  QRect text = rect().adjusted(kSelectPaddingX, 0, -kSelectPaddingX, 0);
  p.setPen(tk::color::q(tk::color::textPrimary));
  p.drawText(text, Qt::AlignVCenter | Qt::AlignLeft, prefix);
  text.setLeft(text.left() + fm.horizontalAdvance(prefix));
  p.setPen(tk::color::q(tk::color::accent));
  p.drawText(text, Qt::AlignVCenter | Qt::AlignLeft, value_);
  const QRect caret(width() - kSelectPaddingX - tk::size::glyphCaret, (height() - tk::size::glyphCaret) / 2,
                    tk::size::glyphCaret, tk::size::glyphCaret);
  caret_.paint(&p, caret);
}

WallToolbar::WallToolbar(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("WallToolbar"));
  setAttribute(Qt::WA_StyledBackground, true);
  setFixedHeight(tk::size::toolbar);

  rulesButton_ = new RulesButton(this);
  rulesButton_->setObjectName(QStringLiteral("RulesButton"));
  rulesButton_->setToolTip(QStringLiteral("Enabled rules for the cameras on the wall"));
  auto toggleButton = [this](const QString& name, const QString& text) {
    auto* button = new QToolButton(this);
    button->setObjectName(name);
    button->setText(text);
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
  };
  overlaysButton_ = toggleButton(QStringLiteral("OverlaysButton"), QStringLiteral("Overlays"));
  overlaysButton_->setToolTip(QStringLiteral("Detection boxes on analytics cameras"));
  recordingsButton_ = toggleButton(QStringLiteral("RecordingsButton"), QStringLiteral("Recordings"));
  recordingsButton_->setToolTip(QStringLiteral("Segments and receive gaps of the selected camera"));
  layoutButton_ = new SelectButton(this);
  layoutButton_->setObjectName(QStringLiteral("LayoutButton"));
  popup_ = new LayoutPopup(this);

  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(tk::size::wallToolbarPaddingX, 0, tk::size::wallToolbarPaddingX, 0);
  row->setSpacing(tk::size::wallToolbarGap);
  row->addStretch(1);
  row->addWidget(rulesButton_);
  row->addWidget(overlaysButton_);
  row->addWidget(recordingsButton_);
  row->addWidget(layoutButton_);

  connect(rulesButton_, &QAbstractButton::clicked, this, &WallToolbar::showRulesMenu);
  connect(overlaysButton_, &QToolButton::toggled, this, &WallToolbar::overlaysToggled);
  connect(recordingsButton_, &QToolButton::toggled, this, &WallToolbar::recordingsToggled);

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

void WallToolbar::setWallRules(const QVector<WallRule>& rules) {
  rules_ = rules;
  rulesButton_->setApplied(static_cast<int>(std::count_if(rules_.begin(), rules_.end(), [](const WallRule& r) { return r.enabled; })));
}

void WallToolbar::setOverlaysChecked(bool checked) {
  const QSignalBlocker block(overlaysButton_);
  overlaysButton_->setChecked(checked);
}

void WallToolbar::setRecordingsChecked(bool checked) {
  const QSignalBlocker block(recordingsButton_);
  recordingsButton_->setChecked(checked);
}

void WallToolbar::showRulesMenu() {
  auto* menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose, true);
  if (rules_.isEmpty()) {
    menu->addAction(QStringLiteral("No rules for the cameras on the wall"))->setEnabled(false);
  }
  for (const WallRule& rule : std::as_const(rules_)) {
    QAction* action = menu->addAction(rule.label);
    action->setCheckable(true);
    action->setChecked(rule.enabled);
    connect(action, &QAction::toggled, this, [this, id = rule.id](bool on) { emit ruleEnableRequested(id, on); });
  }
  menu->addSeparator();
  connect(menu->addAction(QStringLiteral("Manage rules…")), &QAction::triggered, this, &WallToolbar::manageRulesRequested);
  const QPoint anchor = rulesButton_->mapToGlobal(QPoint(0, rulesButton_->height() + tk::size::popupOffset));
  menu->popup(anchor);
}

void WallToolbar::setWallLayout(WallLayout layout) {
  layout_ = layout;
  layoutButton_->setLabel(QStringLiteral("Layout: %1").arg(wallLayoutLabel(layout)));
  popup_->setCurrent(layout);
}

}
