#include "screens/MainTabBar.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kBadgeGap = 7;
constexpr int kBadgePaddingX = 5;
constexpr int kBadgeMinWidth = 17;
}

NavTabs::NavTabs(const QStringList& labels, QWidget* parent)
    : QWidget(parent), labels_(labels), badges_(labels.size(), 0) {
  setObjectName(QStringLiteral("NavTabs"));
  setFixedHeight(tk::size::tabBar);
  setCursor(Qt::PointingHandCursor);
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  int width = 0;
  for (int i = 0; i < count(); ++i) width += tabWidth(i) + (i > 0 ? tk::size::tabGap : 0);
  setFixedWidth(width);
}

int NavTabs::badgeWidth(int index) const {
  if (badges_[index] <= 0) return 0;
  const QFontMetrics fm(Theme::monoMicro());
  return kBadgeGap + std::max(kBadgeMinWidth, fm.horizontalAdvance(QString::number(badges_[index])) + 2 * kBadgePaddingX);
}

int NavTabs::tabWidth(int index) const {
  const QFontMetrics fm(Theme::sans(tk::font::body, QFont::DemiBold));
  return fm.horizontalAdvance(labels_[index]) + 2 * tk::size::tabPaddingX + badgeWidth(index);
}

void NavTabs::setCurrentIndex(int index) {
  if (index < 0 || index >= count() || index == current_) return;
  current_ = index;
  update();
  emit currentChanged(current_);
}

void NavTabs::setBadge(int index, int badgeCount) {
  if (index < 0 || index >= count()) return;
  badges_[index] = badgeCount;
  int width = 0;
  for (int i = 0; i < count(); ++i) width += tabWidth(i) + (i > 0 ? tk::size::tabGap : 0);
  setFixedWidth(width);
  update();
}

void NavTabs::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  int x = 0;
  for (int i = 0; i < count(); ++i) {
    const bool active = i == current_;
    const int w = tabWidth(i);
    const QRect tab(x, 0, w, height());
    p.setFont(Theme::sans(tk::font::body, active ? QFont::DemiBold : QFont::Normal));
    p.setPen(tk::color::q(active ? tk::color::textPrimary : tk::color::textSecondary));
    const QFontMetrics fm(p.font());
    const int textWidth = fm.horizontalAdvance(labels_[i]);
    const QRect textRect(x + tk::size::tabPaddingX, 0, textWidth, height());
    p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, labels_[i]);
    if (badges_[i] > 0) {
      const QString text = QString::number(badges_[i]);
      const int bw = badgeWidth(i) - kBadgeGap;
      const QRect badge(textRect.right() + 1 + kBadgeGap, (height() - tk::size::tabBadgeHeight) / 2, bw, tk::size::tabBadgeHeight);
      p.setPen(Qt::NoPen);
      p.setBrush(tk::color::q(tk::color::critical));
      p.drawRoundedRect(badge, tk::radius::toggle, tk::radius::toggle);
      p.setFont(Theme::monoMicro());
      p.setPen(tk::color::q(tk::color::criticalInk));
      p.drawText(badge, Qt::AlignCenter, text);
    }
    if (active) p.fillRect(QRect(x, height() - tk::size::tabIndicator, w, tk::size::tabIndicator), tk::color::q(tk::color::accent));
    x += w + tk::size::tabGap;
  }
}

void NavTabs::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) return;
  int x = 0;
  for (int i = 0; i < count(); ++i) {
    const int w = tabWidth(i);
    if (event->position().x() >= x && event->position().x() < x + w) {
      setCurrentIndex(i);
      return;
    }
    x += w + tk::size::tabGap;
  }
}

MainTabBar::MainTabBar(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("MainTabBar"));
  setAttribute(Qt::WA_StyledBackground, true);
  setFixedHeight(tk::size::tabBar);
  tabs_ = new NavTabs({QStringLiteral("Monitor"), QStringLiteral("Search"), QStringLiteral("Alerts & Analytics"),
                       QStringLiteral("Model Train")}, this);
  right_ = new QWidget(this);
  right_->setObjectName(QStringLiteral("TabBarRight"));
  auto* rightRow = new QHBoxLayout(right_);
  rightRow->setContentsMargins(0, 0, 0, 0);
  rightRow->setSpacing(10);
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(tk::size::tabBarPaddingX, 0, tk::size::tabBarPaddingX, 0);
  row->setSpacing(0);
  row->addWidget(tabs_);
  row->addStretch(1);
  row->addWidget(right_);
}

}
