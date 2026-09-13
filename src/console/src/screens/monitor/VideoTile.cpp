#include "screens/monitor/VideoTile.h"
#include "alerts/AlertLogic.h"
#include "fovea/Clock.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include "util/Format.h"
#include "video/FrameSurface.h"
#include "video/WallTicker.h"
#include <QApplication>
#include <QDrag>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kChromeRefreshTicks = 30;
// About 4 Hz: boxes disappear promptly once the frame on screen outruns the last detection.
constexpr int kBoxRefreshTicks = 8;
constexpr double kBoxStroke = 1.5;
constexpr int kBoxLabelHeight = 17;
constexpr int kBoxLabelOffset = 19;
constexpr int kBoxLabelPaddingX = 5;
constexpr int kDragPixmapWidth = 160;

QString ageLabel(int64_t ageMs) {
  if (ageMs < 60'000) return QStringLiteral("%1 s").arg(ageMs / 1000);
  if (ageMs < 3'600'000) return QStringLiteral("%1 min").arg(ageMs / 60'000);
  return QStringLiteral("%1 h").arg(ageMs / 3'600'000);
}

int chipWidth(const QFontMetrics& fm, const QString& text, int extra = 0) {
  return tk::size::tileChipPaddingX * 2 + fm.horizontalAdvance(text) + extra;
}

void drawChip(QPainter& p, const QRect& rect, const QFont& font, const QColor& color, const QString& text,
              int textOffset = 0) {
  p.setPen(Qt::NoPen);
  p.setBrush(tk::color::scrimChip());
  p.drawRoundedRect(rect, tk::radius::chip, tk::radius::chip);
  p.setFont(font);
  p.setPen(color);
  p.drawText(rect.adjusted(tk::size::tileChipPaddingX + textOffset, 0, -tk::size::tileChipPaddingX, 0),
             Qt::AlignVCenter | Qt::AlignLeft, text);
}
}

TileOverlay::TileOverlay(QWidget* parent) : QWidget(parent) {
  setAttribute(Qt::WA_TransparentForMouseEvents, true);
  setAttribute(Qt::WA_NoSystemBackground, true);
}

void TileOverlay::setChrome(const TileChrome& chrome) {
  chrome_ = chrome;
  update();
}

void TileOverlay::setBoxes(const QVector<OverlayBox>& boxes) {
  if (boxes == boxes_) return;
  boxes_ = boxes;
  update();
}

void TileOverlay::paintBoxes(QPainter& p) const {
  const QColor critical = tk::color::q(tk::color::critical);
  const QFont font = Theme::monoMicro();
  const QFontMetrics fm(font);
  for (const OverlayBox& box : boxes_) {
    p.setPen(QPen(critical, kBoxStroke));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(box.rect, tk::radius::box, tk::radius::box);
    const double tabWidth = fm.horizontalAdvance(box.label) + 2 * kBoxLabelPaddingX;
    double tabTop = box.rect.top() - kBoxLabelOffset;
    if (tabTop < 0) tabTop = box.rect.top() + kBoxStroke;
    const QRectF tab(box.rect.left() - kBoxStroke / 2.0, tabTop, tabWidth, kBoxLabelHeight);
    p.setPen(Qt::NoPen);
    p.setBrush(critical);
    p.drawRect(tab);
    p.setFont(font);
    p.setPen(tk::color::q(tk::color::criticalInk));
    p.drawText(tab, Qt::AlignCenter, box.label);
  }
}

void TileOverlay::setDotOpacity(double opacity) {
  dotOpacity_ = opacity;
  if (stateChipRect_.isValid()) update(stateChipRect_);
  else update();
}

void TileOverlay::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  paintBoxes(p);
  const int inset = tk::size::tileChipInset;
  const int chipH = tk::size::tileChipHeight;
  const QFont idFont = Theme::monoMicro();
  const QFont nameFont = Theme::sans(tk::font::label);
  const QFontMetrics idMetrics(idFont);
  const QFontMetrics nameMetrics(nameFont);

  const int dotExtra = tk::size::tileStateDot + tk::size::tileStateGap;
  const int stateW = chipWidth(idMetrics, chrome_.state, dotExtra);
  stateChipRect_ = QRect(width() - inset - stateW, inset, stateW, chipH);

  int x = inset;
  if (!chrome_.id.isEmpty()) {
    const QRect idRect(x, inset, chipWidth(idMetrics, chrome_.id), chipH);
    drawChip(p, idRect, idFont, tk::color::q(tk::color::textPrimary), chrome_.id);
    x = idRect.right() + 1 + tk::size::tileChipGap;
  }
  if (!chrome_.name.isEmpty()) {
    const int maxW = stateChipRect_.left() - tk::size::tileChipGap - x;
    if (maxW > tk::size::tileChipPaddingX * 2 + 8) {
      const QString name = nameMetrics.elidedText(chrome_.name, Qt::ElideRight, maxW - tk::size::tileChipPaddingX * 2);
      drawChip(p, QRect(x, inset, chipWidth(nameMetrics, name), chipH), nameFont,
               tk::color::q(tk::color::textSecondary), name);
    }
  }
  if (!chrome_.state.isEmpty()) {
    drawChip(p, stateChipRect_, idFont, chrome_.stateColor, chrome_.state, dotExtra);
    QColor dot = chrome_.stateColor;
    dot.setAlphaF(static_cast<float>(chrome_.pulsing ? dotOpacity_ : 1.0));
    p.setPen(Qt::NoPen);
    p.setBrush(dot);
    const int dotY = stateChipRect_.top() + (chipH - tk::size::tileStateDot) / 2;
    p.drawEllipse(QRect(stateChipRect_.left() + tk::size::tileChipPaddingX, dotY, tk::size::tileStateDot,
                        tk::size::tileStateDot));
  }

  const QRect footer(0, height() - tk::size::tileFooter, width(), tk::size::tileFooter);
  QLinearGradient gradient(footer.topLeft(), footer.bottomLeft());
  gradient.setColorAt(0.0, tk::color::scrimFooterStart());
  gradient.setColorAt(1.0, tk::color::scrimFooterEnd());
  p.fillRect(footer, gradient);
  const QRect footerText = footer.adjusted(tk::size::tileFooterPaddingX, 0, -tk::size::tileFooterPaddingX,
                                           -tk::size::tileFooterPaddingBottom);
  if (!chrome_.footerRight.isEmpty()) {
    p.setFont(Theme::monoMicro());
    p.setPen(tk::color::q(tk::color::textMuted));
    p.drawText(footerText, Qt::AlignBottom | Qt::AlignRight, chrome_.footerRight);
  }
  if (!chrome_.footerLeft.isEmpty()) {
    p.setFont(nameFont);
    p.setPen(chrome_.footerLeftColor);
    const int rightW = chrome_.footerRight.isEmpty() ? 0 : QFontMetrics(Theme::monoMicro()).horizontalAdvance(chrome_.footerRight) + 12;
    const QString left = nameMetrics.elidedText(chrome_.footerLeft, Qt::ElideRight, footerText.width() - rightW);
    p.drawText(footerText, Qt::AlignBottom | Qt::AlignLeft, left);
  }

  p.setPen(QPen(tk::color::q(boxes_.isEmpty() ? tk::color::line : tk::color::lineStrong), 1.0));
  p.setBrush(Qt::NoBrush);
  p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), tk::radius::card, tk::radius::card);
}

VideoTile::VideoTile(QWidget* parent) : QWidget(parent) {
  surface_ = new FrameSurface(this);
  surface_->setCornerRadius(tk::radius::card);
  surface_->setStripeOpacity(tk::stripe::wallOpacity);
  overlay_ = new TileOverlay(this);
  overlay_->raise();

  pulse_.setDuration(tk::motion::livePulseMs);
  pulse_.setLoopCount(-1);
  pulse_.setEasingCurve(QEasingCurve::InOutSine);
  pulse_.setKeyValueAt(0.0, 1.0);
  pulse_.setKeyValueAt(0.5, tk::motion::livePulseMin);
  pulse_.setKeyValueAt(1.0, 1.0);
  connect(&pulse_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
    overlay_->setDotOpacity(v.toDouble());
  });

  connect(&WallTicker::instance(), &WallTicker::tick, this, [this] {
    if (detections_ && ++boxTicks_ >= kBoxRefreshTicks) {
      boxTicks_ = 0;
      refreshBoxes();
    }
    if (++ticks_ < kChromeRefreshTicks) return;
    ticks_ = 0;
    refreshChrome();
  });
}

void VideoTile::setOverlaysEnabled(bool enabled) {
  if (enabled == overlays_) return;
  overlays_ = enabled;
  if (!enabled) detections_.reset();
  refreshBoxes();
}

void VideoTile::setDetections(const std::optional<DetectionFrame>& frame) {
  detections_ = overlays_ ? frame : std::nullopt;
  refreshBoxes();
}

void VideoTile::refreshBoxes() {
  QVector<OverlayBox> boxes;
  if (overlays_ && detections_ && surface_->hasFrame() &&
      detectionMatchesFrame(detections_->sessionId, detections_->ptsNs, surface_->frameSession(),
                            surface_->framePtsNs())) {
    const QRectF area = surface_->imageRect();
    for (const Detection& d : std::as_const(detections_->detections)) {
      const QRectF box(area.left() + d.box.left() * area.width(), area.top() + d.box.top() * area.height(),
                       d.box.width() * area.width(), d.box.height() * area.height());
      boxes.push_back({box, QStringLiteral("%1 %2").arg(d.cls, QString::number(d.confidence, 'f', 2))});
    }
  }
  overlay_->setBoxes(boxes);
}

void VideoTile::setStatus(const fovea::Camera& camera, const fovea::CameraStatus& status) {
  camera_ = camera;
  status_ = status;
  surface_->bind(status.frameRing, status.sessionId);
  refreshChrome();
}

// The freshest evidence of a frame: this console's own copy, or the service's receive time
// (same host monotonic clock). The service resets its figure on reconnect, the console does not.
int64_t VideoTile::lastFrameAgeMs() const {
  const int64_t now = fovea::monoNowNs();
  std::optional<int64_t> ageNs;
  if (surface_->lastFrameMonoNs() > 0) ageNs = now - surface_->lastFrameMonoNs();
  if (status_.lastFrameRecvMonoNs > 0) ageNs = std::min(ageNs.value_or(INT64_MAX), now - status_.lastFrameRecvMonoNs);
  return ageNs ? std::max<int64_t>(0, *ageNs) / 1'000'000 : -1;
}

TileChrome VideoTile::computeChrome() const {
  TileChrome c;
  c.id = camera_.code;
  c.name = camera_.name.isEmpty() ? camera_.code : camera_.name;
  const QString& state = status_.state;
  const bool disabled = state == QLatin1StringView("disabled");
  const bool connecting = state == QLatin1StringView("connecting");
  const bool reconnecting = state == QLatin1StringView("reconnecting");
  const bool live = state == QLatin1StringView("online") && !status_.stale && !surface_->writerLost();
  const int64_t ageMs = lastFrameAgeMs();

  if (disabled) {
    c.state = QStringLiteral("DISABLED");
    c.stateColor = tk::color::q(tk::color::textDisabled);
  } else if (live) {
    c.state = QStringLiteral("LIVE");
    c.stateColor = tk::color::q(tk::color::positive);
    c.pulsing = true;
  } else if (connecting) {
    c.state = QStringLiteral("CONNECTING");
    c.stateColor = tk::color::q(tk::color::textMuted);
  } else if (reconnecting) {
    c.state = QStringLiteral("RECONNECTING");
    c.stateColor = tk::color::q(tk::color::textMuted);
  } else {
    c.state = QStringLiteral("NO SIGNAL");
    c.stateColor = tk::color::q(tk::color::textMuted);
  }

  if (disabled) c.caption = QStringLiteral("DISABLED");
  else if (live && surface_->hasFrame()) c.caption = QString();
  else if (live || connecting || ageMs < 0) c.caption = QStringLiteral("CONNECTING");
  else c.caption = QStringLiteral("SIGNAL LOST");

  c.footerLeftColor = tk::color::q(tk::color::textSecondary);
  if (live) {
    if (status_.recording == QLatin1StringView("paused_disk")) {
      c.footerLeft = QStringLiteral("recording paused · disk full");
      c.footerLeftColor = tk::color::q(tk::color::warning);
    } else if (status_.recording == QLatin1StringView("error")) {
      c.footerLeft = QStringLiteral("recording error");
      c.footerLeftColor = tk::color::q(tk::color::warning);
    }
  } else if (!disabled) {
    c.footerLeft = ageMs >= 0 ? QStringLiteral("last frame %1 ago").arg(ageLabel(ageMs)) : QStringLiteral("no frames yet");
  }

  if (live && surface_->hasFrame()) {
    const QString fps = QString::number(std::lround(status_.fpsNew));
    const QString codec = codecLabel(status_.codec);
    c.footerRight = codec.isEmpty() ? QStringLiteral("%1 fps").arg(fps) : QStringLiteral("%1 fps · %2").arg(fps, codec);
  } else {
    c.footerRight = QStringLiteral("—");
  }
  return c;
}

void VideoTile::refreshChrome() {
  const TileChrome next = computeChrome();
  if (next == chrome_) return;
  chrome_ = next;
  overlay_->setChrome(chrome_);
  surface_->setCaption(chrome_.caption);
  updatePulse();
}

void VideoTile::updatePulse() {
  const bool run = chrome_.pulsing && isVisible();
  if (run && pulse_.state() != QAbstractAnimation::Running) pulse_.start();
  if (!run && pulse_.state() == QAbstractAnimation::Running) {
    pulse_.stop();
    overlay_->setDotOpacity(1.0);
  }
}

void VideoTile::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  updatePulse();
}

void VideoTile::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  updatePulse();
}

void VideoTile::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  surface_->setGeometry(rect());
  overlay_->setGeometry(rect());
  refreshBoxes();
}

void VideoTile::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) pressPos_ = event->position().toPoint();
  QWidget::mousePressEvent(event);
}

void VideoTile::mouseMoveEvent(QMouseEvent* event) {
  if (!dragEnabled_ || !(event->buttons() & Qt::LeftButton) ||
      (event->position().toPoint() - pressPos_).manhattanLength() < QApplication::startDragDistance()) {
    QWidget::mouseMoveEvent(event);
    return;
  }
  auto* mime = new QMimeData;
  mime->setData(QString::fromLatin1(kDragMimeType), camera_.id.toUtf8());
  auto* drag = new QDrag(this);
  drag->setMimeData(mime);
  drag->setPixmap(grab().scaledToWidth(kDragPixmapWidth, Qt::SmoothTransformation));
  drag->exec(Qt::MoveAction);
}

void VideoTile::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    emit maximizeToggled(this);
    event->accept();
    return;
  }
  QWidget::mouseDoubleClickEvent(event);
}

}
