#include "screens/TitleBar.h"
#include "theme/Icons.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include <QAction>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QToolButton>
#include <QWindow>

namespace fovea::ui {
namespace tk = tokens;

namespace {
QToolButton* windowButton(const char* name, QWidget* parent) {
  auto* b = new QToolButton(parent);
  b->setObjectName(QLatin1StringView(name));
  b->setFont(Theme::mono(tk::font::small));
  b->setIconSize(QSize(tk::size::glyphTitleBar, tk::size::glyphTitleBar));
  b->setCursor(Qt::ArrowCursor);
  b->setFocusPolicy(Qt::NoFocus);
  b->setFixedHeight(tk::size::titleBar - 2);
  return b;
}
}

TitleBar::TitleBar(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("TitleBar"));
  setAttribute(Qt::WA_StyledBackground, true);
  setFixedHeight(tk::size::titleBar);

  auto* mark = new QWidget(this);
  mark->setObjectName(QStringLiteral("BrandMark"));
  mark->setAttribute(Qt::WA_StyledBackground, true);
  mark->setFixedSize(tk::size::brandMark, tk::size::brandMark);

  menu_ = new QMenu(this);
  stopService_ = menu_->addAction(QStringLiteral("Stop service…"));

  brandName_ = new QToolButton(this);
  brandName_->setObjectName(QStringLiteral("BrandName"));
  brandName_->setText(QStringLiteral("Fovea"));
  QFont brandFont = Theme::sans(tk::font::body, QFont::DemiBold);
  brandFont.setLetterSpacing(QFont::AbsoluteSpacing, tk::font::brandSpacingPx);
  brandName_->setFont(brandFont);
  brandName_->setMenu(menu_);
  brandName_->setPopupMode(QToolButton::InstantPopup);
  brandName_->setFocusPolicy(Qt::NoFocus);
  brandName_->setToolTip(QStringLiteral("Service menu"));

  const QColor glyphColor = tk::color::q(tk::color::textMuted);
  auto* minimize = windowButton("WinMinimize", this);
  minimize->setText(QStringLiteral("—"));
  auto* maximize = windowButton("WinMaximize", this);
  maximize->setIcon(themedIcon(Icon::Maximize, glyphColor));
  auto* close = windowButton("WinClose", this);
  close->setIcon(themedIcon(Icon::Close, glyphColor));
  connect(minimize, &QToolButton::clicked, this, &TitleBar::minimizeRequested);
  connect(maximize, &QToolButton::clicked, this, &TitleBar::maximizeRequested);
  connect(close, &QToolButton::clicked, this, &TitleBar::closeRequested);

  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(tk::size::titleBarPaddingX, 0, tk::size::titleBarPaddingX, 0);
  row->setSpacing(0);
  row->addWidget(mark);
  row->addSpacing(tk::size::titleBrandGap);
  row->addWidget(brandName_);
  row->addStretch(1);
  row->addWidget(minimize);
  row->addSpacing(tk::size::titleButtonGap);
  row->addWidget(maximize);
  row->addSpacing(tk::size::titleButtonGap);
  row->addWidget(close);
}

void TitleBar::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && window()->windowHandle()) {
    window()->windowHandle()->startSystemMove();
    event->accept();
    return;
  }
  QWidget::mousePressEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    emit maximizeRequested();
    event->accept();
    return;
  }
  QWidget::mouseDoubleClickEvent(event);
}

}
