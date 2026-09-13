#pragma once
#include "fovea/Api.h"
#include <QJsonDocument>
#include <QObject>
#include <QTimer>
#include <QVector>

namespace fovea::ui {

class CoreClient;

class StatusPoller : public QObject {
  Q_OBJECT
public:
  static constexpr int kIntervalMs = 1000;
  static constexpr int kFailuresBeforeUnreachable = 3;

  StatusPoller(CoreClient& client, QObject* parent = nullptr);

  void setActive(bool active);
  bool isActive() const { return timer_.isActive(); }
  // Refresh ahead of the next tick (after the operator saved or removed a camera).
  void pollNow();

signals:
  void snapshot(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses);
  void pollFailed(const QString& error);
  void unreachable(const QString& error);

private:
  void poll();
  static bool parse(const QJsonDocument& doc, QVector<fovea::Camera>& cameras, QVector<fovea::CameraStatus>& statuses);

  CoreClient& client_;
  QTimer timer_;
  bool inFlight_ = false;
  int failures_ = 0;
  // Bumped when polling stops so a reply from before the stop is ignored.
  int generation_ = 0;
};

}
