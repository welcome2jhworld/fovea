#include "video/PlaybackControls.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include "util/Format.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kControlPaddingX = 12;
constexpr int kControlGap = 10;
constexpr int kGlyph = 12;
constexpr int kTrackHeight = 3;
constexpr int kTimeWidth = 92;

QString timeLabel(int64_t positionNs, int64_t durationNs) {
  return QStringLiteral("%1 / %2").arg(clockLabel(positionNs / 1'000'000), clockLabel(durationNs / 1'000'000));
}
}

PlaybackControls::PlaybackControls(QWidget* parent) : QWidget(parent) {
  setAttribute(Qt::WA_NoSystemBackground, true);
  setCursor(Qt::PointingHandCursor);
  setFixedHeight(kHeight);
}

void PlaybackControls::setState(bool playing, int64_t positionNs, int64_t durationNs, bool interactive) {
  playing_ = playing;
  positionNs_ = positionNs;
  durationNs_ = durationNs;
  interactive_ = interactive;
  update();
}

QRect PlaybackControls::glyphRect() const {
  return QRect(kControlPaddingX, (height() - kGlyph) / 2, kGlyph, kGlyph);
}

QRect PlaybackControls::trackRect() const {
  const int left = kControlPaddingX + kGlyph + kControlGap;
  const int right = width() - kControlPaddingX - kTimeWidth - kControlGap;
  return QRect(left, (height() - kTrackHeight) / 2, std::max(0, right - left), kTrackHeight);
}

void PlaybackControls::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.fillRect(rect(), tk::color::scrimControls());
  const QRect glyph = glyphRect();
  p.setPen(Qt::NoPen);
  p.setBrush(tk::color::q(interactive_ ? tk::color::textPrimary : tk::color::textDisabled));
  if (playing_) {
    const int barW = 4;
    p.drawRect(QRect(glyph.left(), glyph.top(), barW, glyph.height()));
    p.drawRect(QRect(glyph.right() - barW + 1, glyph.top(), barW, glyph.height()));
  } else {
    QPainterPath tri;
    tri.moveTo(glyph.left() + 1, glyph.top());
    tri.lineTo(glyph.right() + 1, glyph.center().y() + 0.5);
    tri.lineTo(glyph.left() + 1, glyph.bottom() + 1);
    tri.closeSubpath();
    p.drawPath(tri);
  }
  const QRect track = trackRect();
  p.setBrush(tk::color::q(tk::color::line));
  p.drawRoundedRect(track, 2, 2);
  if (durationNs_ > 0) {
    const double fraction = std::clamp(static_cast<double>(positionNs_) / static_cast<double>(durationNs_), 0.0, 1.0);
    const int fillW = static_cast<int>(std::lround(track.width() * fraction));
    if (fillW > 0) {
      p.setBrush(tk::color::q(tk::color::accent));
      p.drawRoundedRect(QRect(track.left(), track.top(), fillW, track.height()), 2, 2);
    }
  }
  p.setFont(Theme::mono(tk::font::label));
  p.setPen(tk::color::q(tk::color::textSecondary));
  p.drawText(QRect(width() - kControlPaddingX - kTimeWidth, 0, kTimeWidth, height()), Qt::AlignVCenter | Qt::AlignRight,
             timeLabel(positionNs_, durationNs_));
}

void PlaybackControls::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !interactive_) return;
  const QPoint pos = event->position().toPoint();
  if (glyphRect().adjusted(-6, -8, 6, 8).contains(pos)) {
    emit toggleRequested();
    return;
  }
  const QRect track = trackRect();
  if (track.width() > 0 && pos.x() >= track.left() && pos.x() <= track.right() &&
      track.adjusted(0, -8, 0, 8).contains(pos)) {
    emit seekRequested(static_cast<double>(pos.x() - track.left()) / track.width());
  }
}

}
