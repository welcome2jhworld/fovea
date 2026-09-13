#pragma once
#include "dialogs/DialogFrame.h"
#include "video/PlaybackControls.h"
#include "fovea/Api.h"
#include <QJsonDocument>
#include <QTimer>
#include <cstdint>

class QLabel;

namespace fovea::ui {

class CoreClient;
class FrameSurface;

class PlayerView : public QWidget {
public:
  explicit PlayerView(QWidget* parent = nullptr);
  FrameSurface* surface() const { return surface_; }
  PlaybackControls* controls() const { return controls_; }

protected:
  void resizeEvent(QResizeEvent* event) override;

private:
  FrameSurface* surface_ = nullptr;
  PlaybackControls* controls_ = nullptr;
};

class PlaybackDialog : public DialogFrame {
  Q_OBJECT
public:
  static constexpr int kPollIntervalMs = 250;
  PlaybackDialog(CoreClient& client, const fovea::RecordingSegment& segment, const QString& cameraLabel,
                 QWidget* parent = nullptr);
  ~PlaybackDialog() override;

  void start();

private:
  void onOpened(bool ok, const QJsonDocument& doc, const QString& error);
  void poll();
  void applyState(const fovea::PlaybackState& state);
  void toggle();
  void seek(double fraction);
  void showError(const QString& message);
  void closeChannel();

  CoreClient& client_;
  fovea::RecordingSegment segment_;
  PlayerView* player_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* error_ = nullptr;
  QTimer poll_;
  fovea::PlaybackState state_;
  bool pollInFlight_ = false;
  bool channelOpen_ = false;
};

}
