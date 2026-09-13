#include "dialogs/PlaybackDialog.h"
#include "core/CoreClient.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include "util/Format.h"
#include "video/FrameSurface.h"
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <algorithm>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kBodyPadding = 12;
constexpr int kPlayerWidth = tk::size::dialog - 2 - 2 * kBodyPadding;
constexpr int kPlayerHeight = kPlayerWidth * 9 / 16;
}

PlayerView::PlayerView(QWidget* parent) : QWidget(parent) {
  setFixedSize(kPlayerWidth, kPlayerHeight);
  surface_ = new FrameSurface(this);
  surface_->setCornerRadius(tk::radius::card);
  controls_ = new PlaybackControls(this);
  controls_->raise();
}

void PlayerView::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  surface_->setGeometry(rect());
  controls_->setGeometry(0, height() - PlaybackControls::kHeight, width(), PlaybackControls::kHeight);
}

PlaybackDialog::PlaybackDialog(CoreClient& client, const fovea::RecordingSegment& segment, const QString& cameraLabel,
                               QWidget* parent)
    : DialogFrame(parent), client_(client), segment_(segment) {
  setObjectName(QStringLiteral("PlaybackDialog"));
  setTitle(QStringLiteral("Playback"));
  const int64_t lengthMs = segment_.endUtcMs > segment_.startUtcMs ? segment_.endUtcMs - segment_.startUtcMs : -1;
  setSubtitle(QStringLiteral("%1 · %2 · %3").arg(cameraLabel, localTimeLabel(segment_.startUtcMs), durationLabel(lengthMs)));

  player_ = new PlayerView(body());
  player_->surface()->setCaption(QStringLiteral("OPENING"));
  status_ = new QLabel(body());
  status_->setFont(Theme::mono(tk::font::label));
  status_->setProperty("tone", QStringLiteral("muted"));
  error_ = new QLabel(body());
  error_->setProperty("tone", QStringLiteral("critical"));
  error_->setWordWrap(true);
  error_->hide();

  auto* column = new QVBoxLayout();
  column->setContentsMargins(kBodyPadding, kBodyPadding, kBodyPadding, kBodyPadding);
  column->setSpacing(8);
  column->addWidget(player_, 0, Qt::AlignHCenter);
  auto* statusRow = new QHBoxLayout();
  statusRow->setContentsMargins(0, 0, 0, 0);
  statusRow->setSpacing(12);
  statusRow->addWidget(status_);
  statusRow->addWidget(error_, 1);
  column->addLayout(statusRow);
  bodyLayout()->addLayout(column);

  poll_.setInterval(kPollIntervalMs);
  connect(&poll_, &QTimer::timeout, this, &PlaybackDialog::poll);
  connect(player_->controls(), &PlaybackControls::toggleRequested, this, &PlaybackDialog::toggle);
  connect(player_->controls(), &PlaybackControls::seekRequested, this, &PlaybackDialog::seek);
  player_->controls()->setState(false, 0, 0, false);
  status_->setText(QStringLiteral("opening"));
}

PlaybackDialog::~PlaybackDialog() { closeChannel(); }

// The reply is bound to the client, not the dialog: a dialog closed before the channel opened
// still learns the id and closes the channel.
void PlaybackDialog::start() {
  QPointer<PlaybackDialog> self(this);
  CoreClient& client = client_;
  client_.openPlayback(QJsonObject{{"segment_id", segment_.id}}, [self, &client](bool ok, const QJsonDocument& doc, const QString& error) {
    if (self) {
      self->onOpened(ok, doc, error);
    } else if (ok) {
      client.closePlayback(fovea::PlaybackState::fromJson(doc.object()).id, [](bool, const QJsonDocument&, const QString&) {});
    }
  }, &client_);
}

void PlaybackDialog::onOpened(bool ok, const QJsonDocument& doc, const QString& error) {
  if (!ok) {
    player_->surface()->setCaption(QStringLiteral("PLAYBACK ERROR"));
    status_->setText(QStringLiteral("failed"));
    showError(QStringLiteral("Could not open playback · %1").arg(error));
    return;
  }
  channelOpen_ = true;
  applyState(fovea::PlaybackState::fromJson(doc.object()));
  if (!state_.playing && state_.state != QLatin1StringView("error")) {
    client_.playbackControl(state_.id, QStringLiteral("play"), {}, [this](bool playOk, const QJsonDocument& d, const QString& e) {
      if (playOk) applyState(fovea::PlaybackState::fromJson(d.object()));
      else showError(QStringLiteral("Play failed · %1").arg(e));
    }, this);
  }
  poll_.start();
}

void PlaybackDialog::poll() {
  if (pollInFlight_ || !channelOpen_) return;
  pollInFlight_ = true;
  client_.playbackState(state_.id, [this](bool ok, const QJsonDocument& doc, const QString& error) {
    pollInFlight_ = false;
    if (!ok) {
      showError(QStringLiteral("Lost the playback channel · %1").arg(error));
      status_->setText(QStringLiteral("disconnected"));
      player_->controls()->setState(false, state_.positionNs, state_.durationNs, false);
      poll_.stop();
      return;
    }
    applyState(fovea::PlaybackState::fromJson(doc.object()));
  }, this);
}

void PlaybackDialog::applyState(const fovea::PlaybackState& state) {
  state_ = state;
  player_->surface()->bind(state_.frameRing);
  const bool failed = state_.state == QLatin1StringView("error") || !state_.lastError.isEmpty();
  if (failed) {
    showError(state_.lastError.isEmpty() ? QStringLiteral("Playback reported an error.") : state_.lastError);
    player_->surface()->setCaption(QStringLiteral("PLAYBACK ERROR"));
  } else {
    showError({});
    player_->surface()->setCaption(player_->surface()->hasFrame() ? QString() : QStringLiteral("DECODING"));
  }
  QString line = state_.state;
  if (state_.playing) line = QStringLiteral("playing · %1×").arg(QString::number(state_.rate, 'g', 3));
  else if (state_.state == QLatin1StringView("ended")) line = QStringLiteral("ended");
  else if (state_.state == QLatin1StringView("ready") || state_.state == QLatin1StringView("paused")) line = QStringLiteral("paused");
  status_->setText(line);
  player_->controls()->setState(state_.playing, state_.positionNs, state_.durationNs, !failed);
}

void PlaybackDialog::toggle() {
  if (!channelOpen_) return;
  const QString action = state_.playing ? QStringLiteral("pause") : QStringLiteral("play");
  client_.playbackControl(state_.id, action, {}, [this](bool ok, const QJsonDocument& doc, const QString& error) {
    if (ok) applyState(fovea::PlaybackState::fromJson(doc.object()));
    else showError(QStringLiteral("Control failed · %1").arg(error));
  }, this);
}

void PlaybackDialog::seek(double fraction) {
  if (!channelOpen_ || state_.durationNs <= 0) return;
  const int64_t target = static_cast<int64_t>(std::clamp(fraction, 0.0, 1.0) * static_cast<double>(state_.durationNs));
  state_.positionNs = target;
  player_->controls()->setState(state_.playing, target, state_.durationNs, true);
  client_.playbackControl(state_.id, QStringLiteral("seek"), QJsonObject{{"pts_ns", static_cast<double>(target)}},
                          [this](bool ok, const QJsonDocument& doc, const QString& error) {
    if (ok) applyState(fovea::PlaybackState::fromJson(doc.object()));
    else showError(QStringLiteral("Seek failed · %1").arg(error));
  }, this);
}

void PlaybackDialog::showError(const QString& message) {
  error_->setText(message);
  error_->setVisible(!message.isEmpty());
}

void PlaybackDialog::closeChannel() {
  poll_.stop();
  if (!channelOpen_) return;
  channelOpen_ = false;
  client_.closePlayback(state_.id, [](bool, const QJsonDocument&, const QString&) {});
}

}
