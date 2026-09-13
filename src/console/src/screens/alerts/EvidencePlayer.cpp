#include "screens/alerts/EvidencePlayer.h"
#include "core/CoreClient.h"
#include "core/ThumbnailCache.h"
#include "theme/Tokens.h"
#include "video/FrameSurface.h"
#include "video/PlaybackControls.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QResizeEvent>
#include <algorithm>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int64_t kNsPerMs = 1'000'000;
// Within this of the window end a play request restarts from the window start.
constexpr int64_t kEndSlackMs = 250;
}

EvidencePlayer::EvidencePlayer(CoreClient& client, ThumbnailCache& thumbnails, QWidget* parent)
    : QWidget(parent), client_(client), thumbnails_(thumbnails) {
  setObjectName(QStringLiteral("EvidencePlayer"));
  setFixedHeight(kHeight);
  surface_ = new FrameSurface(this);
  controls_ = new PlaybackControls(this);
  controls_->raise();
  controls_->hide();
  poll_.setInterval(kPollIntervalMs);
  connect(&poll_, &QTimer::timeout, this, &EvidencePlayer::poll);
  connect(controls_, &PlaybackControls::toggleRequested, this, &EvidencePlayer::toggle);
  connect(controls_, &PlaybackControls::seekRequested, this, &EvidencePlayer::seek);
  connect(&thumbnails_, &ThumbnailCache::thumbnailReady, this, [this](const QString& id) {
    if (!evidence_ || evidence_->id != id || evidence_->state == QLatin1StringView("deleted")) return;
    surface_->setStillImage(thumbnails_.thumbnail(id));
    if (!surface_->hasFrame() && evidence_->state == QLatin1StringView("pending")) surface_->setCaption(QString());
  });
  setEvidence(QString(), std::nullopt);
}

EvidencePlayer::~EvidencePlayer() { closeChannel(); }

bool EvidencePlayer::playable() const {
  return evidence_ && (evidence_->state == QLatin1StringView("available") || evidence_->state == QLatin1StringView("partial"));
}

void EvidencePlayer::setEvidence(const QString& cameraId, const std::optional<EvidenceInfo>& evidence) {
  const bool same = evidence_.has_value() == evidence.has_value() && cameraId == cameraId_ &&
                    (!evidence || (evidence->id == evidence_->id && evidence->state == evidence_->state &&
                                   evidence->fromUtcMs == evidence_->fromUtcMs && evidence->toUtcMs == evidence_->toUtcMs));
  if (same && generation_ > 0) return;
  closeChannel();
  state_ = fovea::PlaybackState{};
  ++generation_;
  opening_ = false;
  cameraId_ = cameraId;
  evidence_ = evidence;
  spans_.clear();
  pausedAtEnd_ = false;
  playRequested_ = false;
  surface_->bind(std::nullopt);
  surface_->clearFrame();
  const bool deleted = evidence_ && evidence_->state == QLatin1StringView("deleted");
  const QImage still = evidence_ && !deleted ? thumbnails_.thumbnail(evidence_->id) : QImage();
  surface_->setStillImage(still);
  controls_->setVisible(playable());
  controls_->setState(false, 0, clipDurationMs() * kNsPerMs, false);
  if (!evidence_) surface_->setCaption(QStringLiteral("NO EVIDENCE"));
  else if (deleted) surface_->setCaption(QStringLiteral("EVIDENCE DELETED"));
  else if (evidence_->state == QLatin1StringView("pending")) surface_->setCaption(still.isNull() ? QStringLiteral("EVIDENCE PENDING") : QString());
  else surface_->setCaption(still.isNull() ? QStringLiteral("OPENING CLIP") : QString());
  if (playable() && isVisible()) open();
}

int64_t EvidencePlayer::clipStartUtcMs() const { return evidence_ ? evidence_->fromUtcMs : 0; }

int64_t EvidencePlayer::clipDurationMs() const {
  return evidence_ && evidence_->toUtcMs > evidence_->fromUtcMs ? evidence_->toUtcMs - evidence_->fromUtcMs : 0;
}

int64_t EvidencePlayer::clipPositionMs() const {
  const int64_t atUtc = state_.startUtcMs + state_.positionNs / kNsPerMs;
  return std::clamp<int64_t>(atUtc - clipStartUtcMs(), 0, clipDurationMs());
}

void EvidencePlayer::open() {
  if (opening_ || channelOpen_ || !playable()) return;
  opening_ = true;
  const int generation = generation_;
  client_.listSegments(cameraId_, clipStartUtcMs(), clipStartUtcMs() + clipDurationMs(),
                       [this, generation](bool ok, const QJsonDocument& doc, const QString& error) {
    if (generation != generation_) return;
    opening_ = false;
    if (!isVisible()) return;
    if (!ok) return unavailable(error);
    spans_.clear();
    const int64_t from = clipStartUtcMs();
    const int64_t to = from + clipDurationMs();
    for (const QJsonValue& v : doc.array()) {
      const fovea::RecordingSegment seg = fovea::RecordingSegment::fromJson(v.toObject());
      const bool playableState = seg.state == QLatin1StringView("finalized") || seg.state == QLatin1StringView("damaged");
      if (!playableState || seg.startUtcMs <= 0 || seg.endUtcMs <= seg.startUtcMs || seg.endUtcMs < from || seg.startUtcMs > to) continue;
      spans_.push_back({seg.startUtcMs, seg.endUtcMs, seg.id});
    }
    std::sort(spans_.begin(), spans_.end(), [](const Span& a, const Span& b) { return a.startUtcMs < b.startUtcMs; });
    const Span* first = spanAt(from);
    if (!first) return unavailable(QStringLiteral("no finished recording covers the evidence window"));
    openChannelAt(std::max(from, first->startUtcMs), true);
  }, this);
}

void EvidencePlayer::unavailable(const QString& reason) {
  surface_->setCaption(QStringLiteral("CLIP UNAVAILABLE"));
  controls_->setState(false, clipPositionMs() * kNsPerMs, clipDurationMs() * kNsPerMs, false);
  setToolTip(reason);
}

int64_t EvidencePlayer::currentSegmentEndUtcMs() const {
  int64_t end = state_.startUtcMs + state_.durationNs / kNsPerMs;
  for (const Span& span : spans_)
    if (span.segmentId == state_.segmentId) end = std::max(end, span.endUtcMs);
  return end;
}

const EvidencePlayer::Span* EvidencePlayer::spanAt(int64_t atUtcMs) const {
  const int64_t to = clipStartUtcMs() + clipDurationMs();
  for (const Span& span : spans_) {
    if (span.startUtcMs > to) break;
    if (span.endUtcMs > atUtcMs) return &span;
  }
  return nullptr;
}

// The reply is bound to the client: a player that moved on still learns the channel id and closes it.
void EvidencePlayer::openChannelAt(int64_t atUtcMs, bool play) {
  opening_ = true;
  playRequested_ = play;
  const int generation = generation_;
  QPointer<EvidencePlayer> self(this);
  CoreClient& client = client_;
  const QJsonObject request{{"camera_id", cameraId_}, {"at_utc_ms", static_cast<double>(atUtcMs)}};
  client_.openPlayback(request, [self, &client, generation, play](bool ok, const QJsonDocument& doc, const QString& error) {
    const fovea::PlaybackState state = fovea::PlaybackState::fromJson(doc.object());
    if (!self || self->generation_ != generation || !self->isVisible() || self->channelOpen_) {
      if (self && self->generation_ == generation) self->opening_ = false;
      if (ok && !state.id.isEmpty()) client.closePlayback(state.id, [](bool, const QJsonDocument&, const QString&) {});
      return;
    }
    self->onOpened(ok, state, error, play);
  }, &client_);
}

void EvidencePlayer::onOpened(bool ok, const fovea::PlaybackState& state, const QString& error, bool play) {
  opening_ = false;
  if (!ok) return unavailable(error);
  setToolTip(QString());
  channelOpen_ = true;
  apply(state);
  if (play && !state_.playing) control(QStringLiteral("play"));
  poll_.start();
}

void EvidencePlayer::closeChannel() {
  poll_.stop();
  pollInFlight_ = false;
  if (!channelOpen_) return;
  channelOpen_ = false;
  client_.closePlayback(state_.id, [](bool, const QJsonDocument&, const QString&) {});
}

void EvidencePlayer::poll() {
  if (pollInFlight_ || !channelOpen_) return;
  pollInFlight_ = true;
  const int generation = generation_;
  const QString channel = state_.id;
  client_.playbackState(channel, [this, generation, channel](bool ok, const QJsonDocument& doc, const QString&) {
    if (generation != generation_ || channel != state_.id || !channelOpen_) return;
    pollInFlight_ = false;
    if (!ok) {
      poll_.stop();
      channelOpen_ = false;
      unavailable(QStringLiteral("the playback channel closed"));
      return;
    }
    apply(fovea::PlaybackState::fromJson(doc.object()));
  }, this);
}

// Playback stops at the end of the evidence window even when the segment
// continues, and moves on to the next segment when the current one ends first.
void EvidencePlayer::apply(const fovea::PlaybackState& state) {
  state_ = state;
  surface_->bind(state_.frameRing);
  const bool failed = state_.state == QLatin1StringView("error") || !state_.lastError.isEmpty();
  if (failed) surface_->setCaption(QStringLiteral("PLAYBACK ERROR"));
  else if (surface_->hasFrame()) surface_->setCaption(QString());
  const int64_t position = clipPositionMs();
  const bool clipEnded = clipDurationMs() > 0 && position >= clipDurationMs();
  if (state_.playing && clipEnded && !pausedAtEnd_) {
    pausedAtEnd_ = true;
    playRequested_ = false;
    control(QStringLiteral("pause"));
  }
  controls_->setState(state_.playing, position * kNsPerMs, clipDurationMs() * kNsPerMs, !failed);
  if (state_.state == QLatin1StringView("ended") && playRequested_ && !clipEnded) {
    const Span* next = spanAt(currentSegmentEndUtcMs());
    if (next && next->segmentId != state_.segmentId) seekToUtc(next->startUtcMs, true);
    else playRequested_ = false;
  }
}

void EvidencePlayer::control(const QString& action, const QJsonObject& body) {
  const int generation = generation_;
  const QString channel = state_.id;
  client_.playbackControl(channel, action, body, [this, generation, channel](bool ok, const QJsonDocument& doc, const QString&) {
    if (ok && generation == generation_ && channelOpen_ && channel == state_.id) apply(fovea::PlaybackState::fromJson(doc.object()));
  }, this);
}

void EvidencePlayer::toggle() {
  if (!channelOpen_) return;
  if (state_.playing) {
    playRequested_ = false;
    control(QStringLiteral("pause"));
    return;
  }
  pausedAtEnd_ = false;
  if (clipPositionMs() >= clipDurationMs() - kEndSlackMs) {
    seekToUtc(clipStartUtcMs(), true);
    return;
  }
  if (state_.state == QLatin1StringView("ended")) {
    seekToUtc(currentSegmentEndUtcMs(), true);
    return;
  }
  playRequested_ = true;
  control(QStringLiteral("play"));
}

void EvidencePlayer::seek(double fraction) {
  if (!channelOpen_) return;
  pausedAtEnd_ = false;
  seekToUtc(clipStartUtcMs() + static_cast<int64_t>(std::clamp(fraction, 0.0, 1.0) * static_cast<double>(clipDurationMs())),
            state_.playing);
}

void EvidencePlayer::seekToUtc(int64_t atUtcMs, bool play) {
  const Span* span = spanAt(atUtcMs);
  if (!span) return;
  const int64_t target = std::max(atUtcMs, span->startUtcMs);
  if (channelOpen_ && span->segmentId == state_.segmentId) {
    playRequested_ = play;
    const int64_t pts = std::clamp<int64_t>((target - state_.startUtcMs) * kNsPerMs, 0, state_.durationNs);
    control(QStringLiteral("seek"), QJsonObject{{"pts_ns", static_cast<double>(pts)}});
    if (play) control(QStringLiteral("play"));
    return;
  }
  closeChannel();
  openChannelAt(target, play);
}

void EvidencePlayer::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  surface_->setGeometry(rect());
  controls_->setGeometry(0, height() - PlaybackControls::kHeight, width(), PlaybackControls::kHeight);
}

void EvidencePlayer::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (playable()) open();
}

void EvidencePlayer::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  closeChannel();
}

}
