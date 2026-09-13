#pragma once
#include "fovea/Api.h"
#include "fovea/Backoff.h"
#include "fovea/core/Config.h"
#include "fovea/core/SecretStore.h"
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

typedef struct _GstMessage GstMessage;
typedef struct _GstStructure GstStructure;

namespace fovea::core {

class Store;

// One camera's media path across reconnects: each connection attempt is a
// fresh GStreamer pipeline and a fresh StreamSession. Lives on the manager
// thread; GStreamer streaming threads only touch the per-run statistics
// (mutex) and the frame ring, and marshal everything else here with queued
// invocations.
class CameraPipeline : public QObject {
  Q_OBJECT
public:
  CameraPipeline(Camera camera, std::optional<Credentials> credentials, Store& store, const CoreConfig& config,
                 QObject* parent = nullptr);
  ~CameraPipeline() override;

  const Camera& camera() const { return camera_; }
  void start();
  // Synchronous: sends EOS so the open recording finalizes, waits up to
  // three seconds for it, tears the pipeline down and ends the session.
  void stop(const QString& reason);
  void reconfigure(Camera camera, std::optional<Credentials> credentials);
  void setCamera(const Camera& camera);

  bool running() const { return run_ != nullptr; }
  CameraStatus status() const;
  QJsonObject metrics() const;
  uint64_t ringBytes() const;

signals:
  void statusChanged(const QString& cameraId);

private:
  struct Run;
  void startRun();
  void endRun(const QString& reason);
  void failRun(const QString& reason, const QString& error);
  void scheduleReconnect();
  void tick();
  void drainMessages(uint64_t runId);
  void drainNow();
  void handleMessage(GstMessage* msg);
  void onFirstPacket(uint64_t runId);
  void onCapsChanged(uint64_t runId);
  void onPtsBackwards(uint64_t runId);
  void onNoMorePads(uint64_t runId);
  void onFragmentOpened(const GstStructure* s);
  void onFragmentClosed(const GstStructure* s);
  void checkDiskFloor();
  void finalizeLeftoverSegment(const QString& segmentId, const QString& path);
  int64_t utcForPts(int64_t ptsNs) const;

  Camera camera_;
  std::optional<Credentials> credentials_;
  Store& store_;
  CoreConfig config_;
  std::unique_ptr<Run> run_;
  uint64_t nextRunId_ = 1;
  QTimer tickTimer_;
  QTimer reconnectTimer_;
  Backoff backoff_;
  bool wantRunning_ = false;
  QString state_ = QStringLiteral("disabled");
  int64_t sinceUtcMs_ = 0;
  int reconnects_ = 0;
  QString lastError_;
  QString openGapId_;
};

// Short-lived probe for POST /v1/cameras/test: connects, measures handshake
// time, reads the parsed caps, counts bytes for the bitrate and encodes one
// frame as a JPEG preview. Deletes itself after reporting.
class ConnectionProbe : public QObject {
  Q_OBJECT
public:
  ConnectionProbe(Camera camera, std::optional<Credentials> credentials, QObject* parent = nullptr);
  ~ConnectionProbe() override;
  void run(std::function<void(ConnectionTest)> done);

private:
  struct Impl;
  void drain();
  void finish(const QString& error);
  std::unique_ptr<Impl> impl_;
};

}
