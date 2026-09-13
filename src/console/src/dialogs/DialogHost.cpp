#include "dialogs/DialogHost.h"
#include "dialogs/DialogFrame.h"
#include "theme/Tokens.h"
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kShadowSteps = 14;
constexpr int kShadowSpread = 60;
constexpr int kShadowOffsetY = 24;
constexpr double kShadowLayerAlpha = 0.055;
}

DialogHost::DialogHost(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("DialogHost"));
  setAttribute(Qt::WA_NoSystemBackground, true);
  hide();
}

void DialogHost::open(DialogFrame* dialog) {
  if (dialog_) closeDialog();
  dialog_ = dialog;
  dialog_->setParent(this);
  dialog_->installEventFilter(this);
  connect(dialog_, &DialogFrame::closeRequested, this, &DialogHost::closeDialog);
  place();
  show();
  raise();
  dialog_->show();
  dialog_->setFocus();
  emit opened();
}

void DialogHost::closeDialog() {
  if (!dialog_) return;
  DialogFrame* dialog = dialog_;
  dialog_ = nullptr;
  dialog->hide();
  dialog->deleteLater();
  hide();
  emit closed();
}

void DialogHost::shutdown() {
  if (!dialog_) return;
  DialogFrame* dialog = dialog_;
  dialog_ = nullptr;
  delete dialog;
  hide();
}

void DialogHost::place() {
  if (!dialog_) return;
  dialog_->adjustSize();
  const QSize s = dialog_->size();
  dialog_->move((width() - s.width()) / 2, std::max(0, (height() - s.height()) / 2));
  update();
}

bool DialogHost::eventFilter(QObject* watched, QEvent* event) {
  if (watched == dialog_ && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest)) {
    const QSize s = dialog_->size();
    dialog_->move((width() - s.width()) / 2, std::max(0, (height() - s.height()) / 2));
    update();
  }
  return QWidget::eventFilter(watched, event);
}

void DialogHost::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  place();
}

void DialogHost::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), tk::color::scrimBackdrop());
  if (!dialog_) return;
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setPen(Qt::NoPen);
  const QRectF base = QRectF(dialog_->geometry()).translated(0, kShadowOffsetY);
  for (int i = kShadowSteps; i >= 1; --i) {
    const double t = static_cast<double>(i) / kShadowSteps;
    const double spread = kShadowSpread * t;
    QColor layer(0, 0, 0);
    layer.setAlphaF(static_cast<float>(kShadowLayerAlpha));
    p.setBrush(layer);
    p.drawRoundedRect(base.adjusted(-spread, -spread, spread, spread), tk::radius::dialog + spread,
                      tk::radius::dialog + spread);
  }
}

void DialogHost::mousePressEvent(QMouseEvent* event) { event->accept(); }
void DialogHost::mouseReleaseEvent(QMouseEvent* event) { event->accept(); }
void DialogHost::mouseDoubleClickEvent(QMouseEvent* event) { event->accept(); }

}
