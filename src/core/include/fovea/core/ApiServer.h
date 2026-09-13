#pragma once
#include "fovea/core/SystemStats.h"
#include <QHttpServer>
#include <QObject>
#include <QTcpServer>
#include <functional>

namespace fovea::core {

class AnalysisScheduler;
class CameraManager;
class EventService;
class PlaybackManager;
class RetentionManager;
class RuleEngine;
class Store;
class WorkerSupervisor;

struct AnalyticsServices {
  RuleEngine& rules;
  EventService& events;
  AnalysisScheduler& scheduler;
  WorkerSupervisor& worker;
};

class ApiServer : public QObject {
  Q_OBJECT
public:
  ApiServer(CameraManager& cameras, PlaybackManager& playback, RetentionManager& retention, Store& store,
            AnalyticsServices analytics, QString token, QObject* parent = nullptr);
  bool listen(quint16 port = 0);
  quint16 port() const { return port_; }
  void setShutdownHandler(std::function<void()> handler) { shutdown_ = std::move(handler); }
  void setStartedUtcMs(int64_t ms) { startedUtcMs_ = ms; }

private:
  void registerRoutes();
  void registerAnalyticsRoutes();
  bool authorized(const QHttpServerRequest& request) const;

  CameraManager& cameras_;
  PlaybackManager& playback_;
  RetentionManager& retention_;
  Store& store_;
  AnalyticsServices analytics_;
  QString token_;
  QHttpServer server_;
  QTcpServer* tcp_ = nullptr;
  quint16 port_ = 0;
  int64_t startedUtcMs_ = 0;
  std::function<void()> shutdown_;
  CpuSampler cpu_;
};

}
