#pragma once
#include "core/AlertTypes.h"
#include "fovea/Api.h"
#include <QTimer>
#include <QWidget>
#include <optional>

namespace fovea::ui {

class CoreClient;
class FrameSurface;
class PlaybackControls;
class ThumbnailCache;

// 190 px clip player for an event's evidence window. Available and partial
// evidence lists the recordings of the window and opens a playback channel
// (POST /v1/playback {camera_id, at_utc_ms}) at the first recorded time at or
// after the window start. A channel plays one segment: when it ends before the
// window does, the player reopens at the next segment, and a seek into another
// segment reopens there. Times on the control bar are relative to the window.
// Pending evidence shows the thumbnail; deleted evidence never offers a clip.
class EvidencePlayer : public QWidget {
  Q_OBJECT
public:
  static constexpr int kHeight = 190;
  static constexpr int kPollIntervalMs = 250;

  EvidencePlayer(CoreClient& client, ThumbnailCache& thumbnails, QWidget* parent = nullptr);
  ~EvidencePlayer() override;

  void setEvidence(const QString& cameraId, const std::optional<EvidenceInfo>& evidence);

protected:
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

private:
  struct Span {
    int64_t startUtcMs = 0;
    int64_t endUtcMs = 0;
    QString segmentId;
  };

  bool playable() const;
  void open();
  void openChannelAt(int64_t atUtcMs, bool play);
  void onOpened(bool ok, const fovea::PlaybackState& state, const QString& error, bool play);
  void unavailable(const QString& reason);
  // The span holding atUtcMs, else the first one starting after it.
  const Span* spanAt(int64_t atUtcMs) const;
  int64_t currentSegmentEndUtcMs() const;
  void seekToUtc(int64_t atUtcMs, bool play);
  void closeChannel();
  void poll();
  void apply(const fovea::PlaybackState& state);
  void toggle();
  void seek(double fraction);
  void control(const QString& action, const QJsonObject& body = {});
  int64_t clipStartUtcMs() const;
  int64_t clipDurationMs() const;
  int64_t clipPositionMs() const;

  CoreClient& client_;
  ThumbnailCache& thumbnails_;
  FrameSurface* surface_ = nullptr;
  PlaybackControls* controls_ = nullptr;
  QTimer poll_;
  QString cameraId_;
  std::optional<EvidenceInfo> evidence_;
  fovea::PlaybackState state_;
  QVector<Span> spans_;
  int generation_ = 0;
  bool opening_ = false;
  bool channelOpen_ = false;
  bool pollInFlight_ = false;
  bool pausedAtEnd_ = false;
  bool playRequested_ = false;
};

}
