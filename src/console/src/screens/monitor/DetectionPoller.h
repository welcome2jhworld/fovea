#pragma once
#include "core/AlertTypes.h"
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

namespace fovea::ui {

class CoreClient;

// GET /v1/cameras/{id}/detections/latest at 2 Hz for the cameras it is given
// (visible, analytics enabled, overlays on); one request per camera at a time.
class DetectionPoller : public QObject {
  Q_OBJECT
public:
  static constexpr int kIntervalMs = 500;

  explicit DetectionPoller(CoreClient& client, QObject* parent = nullptr);
  void setCameras(const QStringList& cameraIds);

signals:
  void detectionsArrived(const QString& cameraId, const fovea::ui::DetectionFrame& frame);
  void detectionsUnavailable(const QString& cameraId);

private:
  void poll();

  CoreClient& client_;
  QTimer timer_;
  QStringList cameras_;
  QSet<QString> inFlight_;
};

}
