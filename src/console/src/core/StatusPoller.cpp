#include "core/StatusPoller.h"
#include "core/CoreClient.h"
#include <QJsonArray>
#include <QJsonObject>

namespace fovea::ui {

StatusPoller::StatusPoller(CoreClient& client, QObject* parent) : QObject(parent), client_(client) {
  timer_.setInterval(kIntervalMs);
  connect(&timer_, &QTimer::timeout, this, &StatusPoller::poll);
}

void StatusPoller::setActive(bool active) {
  if (active == timer_.isActive()) return;
  if (active) {
    failures_ = 0;
    timer_.start();
    poll();
  } else {
    timer_.stop();
    ++generation_;
    inFlight_ = false;
  }
}

void StatusPoller::pollNow() {
  if (timer_.isActive()) poll();
}

void StatusPoller::poll() {
  if (inFlight_) return;
  inFlight_ = true;
  const int generation = generation_;
  client_.listCameras([this, generation](bool ok, const QJsonDocument& doc, const QString& error) {
    if (generation != generation_) return;
    inFlight_ = false;
    if (ok) {
      QVector<fovea::Camera> cameras;
      QVector<fovea::CameraStatus> statuses;
      if (parse(doc, cameras, statuses)) {
        failures_ = 0;
        emit snapshot(cameras, statuses);
        return;
      }
    }
    ++failures_;
    const QString reason = ok ? QStringLiteral("GET /v1/cameras returned an unexpected payload") : error;
    emit pollFailed(reason);
    if (failures_ >= kFailuresBeforeUnreachable) {
      failures_ = 0;
      emit unreachable(reason);
    }
  }, this);
}

// GET /v1/cameras is a top-level array of Camera objects, each carrying its CameraStatus under "status".
bool StatusPoller::parse(const QJsonDocument& doc, QVector<fovea::Camera>& cameras,
                         QVector<fovea::CameraStatus>& statuses) {
  if (!doc.isArray()) return false;
  const QJsonArray entries = doc.array();
  cameras.reserve(entries.size());
  statuses.reserve(entries.size());
  for (const QJsonValue& v : entries) {
    if (!v.isObject()) return false;
    const QJsonObject entry = v.toObject();
    const QJsonValue status = entry.value(QLatin1StringView("status"));
    if (!status.isObject()) return false;
    fovea::Camera camera = fovea::Camera::fromJson(entry);
    if (camera.id.isEmpty()) return false;
    fovea::CameraStatus cameraStatus = fovea::CameraStatus::fromJson(status.toObject());
    if (cameraStatus.cameraId.isEmpty()) cameraStatus.cameraId = camera.id;
    cameras.push_back(std::move(camera));
    statuses.push_back(std::move(cameraStatus));
  }
  return true;
}

}
