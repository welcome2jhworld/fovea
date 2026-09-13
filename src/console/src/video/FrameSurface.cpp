#include "video/FrameSurface.h"
#include "fovea/Clock.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include "video/WallTicker.h"
#include "widgets/Painting.h"
#include <QPainter>
#include <QPainterPath>

namespace fovea::ui {
namespace tk = tokens;

FrameSurface::FrameSurface(QWidget* parent) : QWidget(parent) {
  setAttribute(Qt::WA_OpaquePaintEvent, false);
  connect(&WallTicker::instance(), &WallTicker::tick, this, &FrameSurface::onTick);
}

FrameSurface::~FrameSurface() = default;

void FrameSurface::bind(const std::optional<fovea::RingRef>& ring, const QString& session) {
  const QString name = ring ? ring->name : QString();
  if (name == ringName_ && session == session_) return;
  if (name != ringName_) clearFrame();
  ringName_ = name;
  session_ = session;
  reader_.reset();
  writerLost_ = false;
  lastOpenAttemptNs_ = 0;
}

void FrameSurface::setCaption(const QString& caption) {
  if (caption == caption_) return;
  caption_ = caption;
  update();
}

void FrameSurface::setStillImage(const QImage& image) {
  still_ = image;
  update();
}

void FrameSurface::clearFrame() {
  image_ = QImage();
  frameHeader_ = fovea::FrameHeader{};
  lastSeenSeq_ = 0;
  update();
}

void FrameSurface::setCornerRadius(int radius) {
  cornerRadius_ = radius;
  update();
}

void FrameSurface::setStripeOpacity(double opacity) {
  stripeOpacity_ = opacity;
  update();
}

bool FrameSurface::openReader(int64_t nowMonoNs) {
  lastOpenAttemptNs_ = nowMonoNs;
  auto fresh = fovea::FrameRingReader::open(ringName_);
  if (!fresh) return false;
  const fovea::RingInfo& info = fresh->info();
  // The writer publishes the magic before the geometry, so a header can be mapped half written.
  if (info.slotCount == 0 || info.slotBytes == 0 || info.maxWidth == 0 || info.maxHeight == 0) return false;
  if (!fresh->writerAlive()) {
    writerLost_ = true;
    return false;
  }
  if (staging_.size() != info.slotBytes) {
    if (!image_.isNull()) image_ = image_.copy();
    staging_.assign(info.slotBytes, 0);
    display_.assign(info.slotBytes, 0);
  }
  reader_ = std::move(fresh);
  writerLost_ = false;
  lastSeenSeq_ = 0;
  lastObservedSeq_ = 0;
  lastWriterCheckNs_ = nowMonoNs;
  return true;
}

void FrameSurface::onTick() {
  if (ringName_.isEmpty()) return;
  const int64_t now = fovea::monoNowNs();
  if (!reader_ && (now - lastOpenAttemptNs_ < kOpenRetryNs || !openReader(now))) return;

  // Hidden surfaces only track that frames arrive; the pixels are copied once they are shown.
  if (!isVisible()) {
    const std::optional<fovea::FrameHeader> latest = reader_->latest();
    if (latest && latest->seq != lastObservedSeq_) {
      lastObservedSeq_ = latest->seq;
      lastFrameMonoNs_ = now;
      return;
    }
  } else {
    fovea::FrameHeader h;
    if (reader_->copyLatest(lastSeenSeq_, h, staging_.data(), staging_.size())) {
      staging_.swap(display_);
      frameHeader_ = h;
      image_ = QImage(display_.data(), static_cast<int>(h.width), static_cast<int>(h.height),
                      static_cast<qsizetype>(h.stride), QImage::Format_ARGB32);
      lastObservedSeq_ = lastSeenSeq_;
      lastFrameMonoNs_ = now;
      update();
      return;
    }
  }
  // No new frame: if the writer has exited, keep the last frame and wait for a new ring.
  if (now - lastWriterCheckNs_ < kWriterCheckNs) return;
  lastWriterCheckNs_ = now;
  if (reader_->writerAlive()) return;
  reader_.reset();
  writerLost_ = true;
  lastOpenAttemptNs_ = now;
}

QRectF FrameSurface::imageRect() const {
  const QImage& shown = image_.isNull() ? still_ : image_;
  if (shown.isNull()) return {};
  const QSizeF fitted = QSizeF(shown.size()).scaled(QSizeF(size()), Qt::KeepAspectRatio);
  return QRectF(QPointF((width() - fitted.width()) / 2.0, (height() - fitted.height()) / 2.0), fitted);
}

void FrameSurface::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  if (cornerRadius_ > 0) {
    QPainterPath clip;
    clip.addRoundedRect(rect(), cornerRadius_, cornerRadius_);
    painter.setClipPath(clip);
  }
  painter.fillRect(rect(), tk::color::q(tk::color::bgVideo));
  const QImage& shown = image_.isNull() ? still_ : image_;
  if (!shown.isNull()) {
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(imageRect(), shown);
  } else {
    paintPlaceholderStripes(painter, QRectF(rect()), stripeOpacity_);
  }
  if (!caption_.isEmpty()) {
    painter.setFont(Theme::placeholderCaption(height()));
    painter.setPen(tk::color::q(tk::color::textPlaceholderCaption));
    painter.drawText(rect(), Qt::AlignCenter, caption_);
  }
}

}
