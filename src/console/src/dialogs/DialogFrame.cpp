#include "dialogs/DialogFrame.h"
#include "theme/Icons.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kHeaderPaddingX = 20;
constexpr int kHeaderGap = 10;
}

DialogFrame::DialogFrame(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("DialogFrame"));
  setFixedWidth(tk::size::dialog);
  setFocusPolicy(Qt::StrongFocus);

  auto* header = new QWidget(this);
  header->setObjectName(QStringLiteral("DialogHeader"));
  header->setAttribute(Qt::WA_StyledBackground, true);
  header->setFixedHeight(tk::size::dialogHeader);
  title_ = new QLabel(header);
  title_->setObjectName(QStringLiteral("DialogTitle"));
  title_->setFont(Theme::subhead());
  subtitle_ = new QLabel(header);
  subtitle_->setObjectName(QStringLiteral("DialogSubtitle"));
  subtitle_->setFont(Theme::mono(tk::font::label));
  auto* close = new QToolButton(header);
  close->setObjectName(QStringLiteral("DialogClose"));
  close->setIcon(themedIcon(Icon::Close, tk::color::q(tk::color::textMuted)));
  close->setIconSize(QSize(tk::size::glyphDialogClose, tk::size::glyphDialogClose));
  close->setCursor(Qt::PointingHandCursor);
  close->setFocusPolicy(Qt::NoFocus);
  close->setToolTip(QStringLiteral("Close (Esc)"));
  auto* headerRow = new QHBoxLayout(header);
  headerRow->setContentsMargins(kHeaderPaddingX, 0, kHeaderPaddingX, 0);
  headerRow->setSpacing(kHeaderGap);
  headerRow->addWidget(title_, 0, Qt::AlignBaseline);
  headerRow->addWidget(subtitle_, 0, Qt::AlignBaseline);
  headerRow->addStretch(1);
  headerRow->addWidget(close);
  connect(close, &QToolButton::clicked, this, &DialogFrame::closeRequested);

  body_ = new QWidget(this);
  body_->setObjectName(QStringLiteral("DialogBody"));
  bodyLayout_ = new QVBoxLayout(body_);
  bodyLayout_->setContentsMargins(0, 0, 0, 0);
  bodyLayout_->setSpacing(0);

  column_ = new QVBoxLayout(this);
  column_->setContentsMargins(1, 1, 1, 1);
  column_->setSpacing(0);
  column_->addWidget(header);
  column_->addWidget(body_, 1);
}

void DialogFrame::setTitle(const QString& title) { title_->setText(title); }

void DialogFrame::setSubtitle(const QString& subtitle) {
  subtitle_->setText(subtitle);
  subtitle_->setVisible(!subtitle.isEmpty());
}

QWidget* DialogFrame::footer() {
  if (!footer_) {
    footer_ = new QWidget(this);
    footer_->setObjectName(QStringLiteral("DialogFooter"));
    footer_->setAttribute(Qt::WA_StyledBackground, true);
    footer_->setFixedHeight(tk::size::dialogFooter);
    footerLayout_ = new QHBoxLayout(footer_);
    footerLayout_->setContentsMargins(kHeaderPaddingX, 0, kHeaderPaddingX, 0);
    footerLayout_->setSpacing(8);
    column_->addWidget(footer_);
  }
  return footer_;
}

QHBoxLayout* DialogFrame::footerLayout() {
  footer();
  return footerLayout_;
}

void DialogFrame::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setPen(QPen(tk::color::q(tk::color::lineStrong), 1.0));
  p.setBrush(tk::color::q(tk::color::bgApp));
  p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), tk::radius::dialog, tk::radius::dialog);
}

void DialogFrame::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape) {
    emit closeRequested();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

}
