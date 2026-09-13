#pragma once
#include "fovea/Api.h"
#include "fovea/core/Config.h"
#include "fovea/core/SecretStore.h"
#include <QJsonObject>
#include <QObject>
#include <QVector>
#include <functional>
#include <memory>
#include <optional>

namespace fovea::core {

class Store;
class CameraPipeline;
class AnalysisTap;

// Owns one CameraPipeline per enabled camera, the reconnect policy, receive gap
// bookkeeping and the per-camera status snapshot. All public methods are
// called on the thread that owns the manager (the core main thread); pipeline
// threads post back with queued invocations. Methods return quickly: starting
// or stopping a pipeline is asynchronous.
class CameraManager : public QObject {
  Q_OBJECT
public:
  CameraManager(Store& store, SecretStore& secrets, CoreConfig config, QObject* parent = nullptr);
  ~CameraManager() override;

  void start();
  // Stops every pipeline and refuses to start new ones. done runs once all
  // recordings are finalized, or after ten seconds at most.
  void stopAll(std::function<void()> done);

  QVector<Camera> cameras() const;
  std::optional<Camera> camera(const QString& id) const;
  QVector<CameraStatus> statuses() const;
  std::optional<CameraStatus> status(const QString& id) const;
  // Newest decoded frame slot of a camera whose pipeline has run; null otherwise.
  std::shared_ptr<AnalysisTap> analysisTap(const QString& cameraId) const;

  std::optional<Camera> createCamera(Camera c, const std::optional<Credentials>& creds, QString* error);
  bool updateCamera(const Camera& c, const std::optional<Credentials>& creds, QString* error);
  bool deleteCamera(const QString& id, QString* error);
  bool setEnabled(const QString& id, bool enabled, QString* error);

  // Runs a short probe pipeline (up to timeout_ms) without recording and
  // reports codec, size, fps, bitrate and handshake time. done is invoked on
  // the manager's thread exactly once.
  void testConnection(const Camera& c, const std::optional<Credentials>& creds,
                      std::function<void(ConnectionTest)> done);

  QJsonObject metrics() const;
  const CoreConfig& config() const { return config_; }

signals:
  void statusChanged(const QString& cameraId);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  CoreConfig config_;
};

}
