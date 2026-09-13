#pragma once
#include "fovea/Api.h"
#include "fovea/Backoff.h"
#include "fovea/core/Config.h"
#include "fovea/core/SecretStore.h"
#include "fovea/core/Store.h"
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

typedef struct _GstMessage GstMessage;
typedef struct _GstStructure GstStructure;

namespace fovea::core {

// One camera's media path across reconnects: each connection attempt is a
// fresh GStreamer pipeline and a fresh StreamSession. Lives on the manager
// thread; GStreamer streaming threads only touch the per-run statistics
// (mutex) and the frame ring, and marshal everything else here with queued
// invocations. Ending a run never blocks this thread: the EOS that finalizes
// the open recording is sent from a GStreamer pool thread, and the pipeline is
// torn down and its unfinished segments probed off this thread as well.
class CameraPipeline : public QObject {
  Q_OBJECT
public:
  CameraPipeline(Camera camera, std::optional<Credentials> credentials, Store& store, const CoreConfig& config,
                 QObject* parent = nullptr);
  // Abandons runs that are still finalizing; segments they leave in
  // "recording" are resolved by startup recovery.
  ~CameraPipeline() override;

  const Camera& camera() const { return camera_; }
  void start();
  // Ends the session at once. The open recording finalizes in the background
  // (EOS, at most three seconds); idle() turns true and idleReached is
  // emitted once the pipeline is torn down and every segment probe is done.
  void stop(const QString& reason);
  void reconfigure(Camera camera, std::optional<Credentials> credentials);
  void setCamera(const Camera& camera);

  bool idle() const;
  CameraStatus status() const;
  QJsonObject metrics() const;
  uint64_t ringBytes() const;

signals:
  void statusChanged(const QString& cameraId);
  void idleReached(const QString& cameraId);

private:
  struct Run;
  struct Mailbox;
  void startRun();
  void requestStart();
  void retireRun(const QString& reason);
  void closeSession(Run& run, const QString& reason);
  void finishRetire(uint64_t runId);
  void disposeRun(std::unique_ptr<Run> run);
  void failRun(const QString& reason, const QString& error);
  void restartRun(const QString& reason);
  void scheduleReconnect();
  void tick();
  void checkDiskFloor();
  bool refreshDiskPaused();
  Run* findRun(uint64_t runId) const;
  void drainMessages(uint64_t runId);
  void drain(Run& run);
  void handleMessage(Run& run, GstMessage* msg);
  void onFirstPacket(uint64_t runId);
  void onCapsChanged(uint64_t runId);
  void onPtsBackwards(uint64_t runId);
  void onNoMorePads(uint64_t runId);
  void onFragmentOpened(Run& run, const GstStructure* s);
  void onFragmentClosed(Run& run, const GstStructure* s);
  void onTornDown();
  void onLeftoverProbed(const QString& segmentId, const SegmentProbe& probe);
  void closeOpenGap(int64_t toUtcMs);
  void emitIdleIfDone();
  int64_t utcForPts(const Run& run, int64_t ptsNs) const;

  Camera camera_;
  std::optional<Credentials> credentials_;
  Store& store_;
  CoreConfig config_;
  std::shared_ptr<Mailbox> mailbox_;
  std::unique_ptr<Run> run_;
  std::vector<std::unique_ptr<Run>> retiring_;
  uint64_t nextRunId_ = 1;
  int pendingTeardowns_ = 0;
  int pendingProbes_ = 0;
  bool startPending_ = false;
  QTimer tickTimer_;
  QTimer reconnectTimer_;
  Backoff backoff_;
  bool wantRunning_ = false;
  QString state_ = QStringLiteral("disabled");
  int64_t sinceUtcMs_ = 0;
  int reconnects_ = 0;
  QString lastError_;
  QString openGapId_;
  int64_t lastFrameRecvMonoNs_ = 0;
  QString recordingState_ = QStringLiteral("recording");
  bool diskPaused_ = false;
  int64_t nextDiskCheckNs_ = 0;
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
