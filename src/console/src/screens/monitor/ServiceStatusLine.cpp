#include "screens/monitor/ServiceStatusLine.h"
#include "theme/Tokens.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>

namespace fovea::ui {
namespace tk = tokens;

namespace {
void restyle(QWidget* w) {
  w->style()->unpolish(w);
  w->style()->polish(w);
}
}

ServiceStatusLine::ServiceStatusLine(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("ServiceStatusLine"));
  setAttribute(Qt::WA_StyledBackground, true);
  setFixedHeight(kHeight);
  text_ = new QLabel(this);
  retry_ = new QPushButton(QStringLiteral("Retry"), this);
  retry_->setProperty("role", QStringLiteral("link"));
  retry_->setCursor(Qt::PointingHandCursor);
  retry_->setFocusPolicy(Qt::NoFocus);
  retry_->hide();
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(tk::size::wallToolbarPaddingX, 0, tk::size::wallToolbarPaddingX, 0);
  row->setSpacing(12);
  row->addWidget(text_);
  row->addStretch(1);
  row->addWidget(retry_);
  connect(retry_, &QPushButton::clicked, this, &ServiceStatusLine::retryRequested);
  setState(CoreLauncher::State::Unknown, {});
}

void ServiceStatusLine::setState(CoreLauncher::State state, const QString& message) {
  QString tone;
  QString line;
  switch (state) {
    case CoreLauncher::State::Unknown: line = QStringLiteral("Service status unknown"); break;
    case CoreLauncher::State::Starting: line = QStringLiteral("Starting service · %1").arg(message); break;
    case CoreLauncher::State::Ready: line = QStringLiteral("Service ready"); tone = QStringLiteral("positive"); break;
    case CoreLauncher::State::Failed:
      line = QStringLiteral("Service unavailable · %1").arg(message);
      tone = QStringLiteral("critical");
      break;
  }
  text_->setText(line);
  text_->setProperty("tone", tone);
  restyle(text_);
  retry_->setVisible(state == CoreLauncher::State::Failed);
  setVisible(state != CoreLauncher::State::Ready);
}

}
