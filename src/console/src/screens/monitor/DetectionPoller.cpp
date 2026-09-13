#include "screens/monitor/DetectionPoller.h"
#include "core/CoreClient.h"

namespace fovea::ui {

DetectionPoller::DetectionPoller(CoreClient& client, QObject* parent) : QObject(parent), client_(client) {
  timer_.setInterval(kIntervalMs);
  connect(&timer_, &QTimer::timeout, this, &DetectionPoller::poll);
}

void DetectionPoller::setCameras(const QStringList& cameraIds) {
  if (cameraIds == cameras_) return;
  for (const QString& id : std::as_const(cameras_))
    if (!cameraIds.contains(id)) emit detectionsUnavailable(id);
  cameras_ = cameraIds;
  if (cameras_.isEmpty()) {
    timer_.stop();
    return;
  }
  if (!timer_.isActive()) timer_.start();
  poll();
}

void DetectionPoller::poll() {
  for (const QString& id : std::as_const(cameras_)) {
    if (inFlight_.contains(id)) continue;
    inFlight_.insert(id);
    client_.latestDetections(id, [this, id](bool ok, const QJsonDocument& doc, const QString&) {
      inFlight_.remove(id);
      if (!cameras_.contains(id)) return;
      if (ok && doc.isObject()) emit detectionsArrived(id, DetectionFrame::fromJson(doc.object()));
      else emit detectionsUnavailable(id);
    }, this);
  }
}

}
