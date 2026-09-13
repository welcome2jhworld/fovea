#pragma once
#include "core/CoreLauncher.h"
#include "fovea/Api.h"
#include <QVector>
#include <QWidget>

namespace fovea::ui {

class CameraRail;
class CoreClient;
class RecordingsPanel;
class ServiceStatusLine;
class VideoWall;
class WallToolbar;

class MonitorScreen : public QWidget {
  Q_OBJECT
public:
  MonitorScreen(CoreClient& client, QWidget* parent = nullptr);

  void setSnapshot(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses);
  // The last snapshot can no longer be vouched for (poll failed, service not ready): nothing stays LIVE.
  void markStatusesStale();
  void setServiceState(CoreLauncher::State state, const QString& message);
  CameraRail* rail() const { return rail_; }

signals:
  void retryServiceRequested();
  void addCameraRequested();
  void cameraSettingsRequested(const QString& cameraId);
  void playbackRequested(const fovea::RecordingSegment& segment, const QString& cameraLabel);

private:
  void bindRecordings(const QString& cameraId);

  CameraRail* rail_ = nullptr;
  RecordingsPanel* recordings_ = nullptr;
  QVector<fovea::Camera> cameras_;
  QVector<fovea::CameraStatus> statuses_;
  ServiceStatusLine* status_ = nullptr;
  WallToolbar* toolbar_ = nullptr;
  VideoWall* wall_ = nullptr;
};

}
