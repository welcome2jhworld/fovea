#pragma once
#include <QHttpServer>
#include <QObject>
#include <QTcpServer>
#include <functional>

namespace fovea::core {

class CameraManager;
class PlaybackManager;
class Store;

class ApiServer : public QObject {
  Q_OBJECT
public:
  ApiServer(CameraManager& cameras, PlaybackManager& playback, Store& store, QString token, QObject* parent = nullptr);
  bool listen(quint16 port = 0);
  quint16 port() const { return port_; }
  void setShutdownHandler(std::function<void()> handler) { shutdown_ = std::move(handler); }
  void setStartedUtcMs(int64_t ms) { startedUtcMs_ = ms; }

private:
  void registerRoutes();
  bool authorized(const QHttpServerRequest& request) const;

  CameraManager& cameras_;
  PlaybackManager& playback_;
  Store& store_;
  QString token_;
  QHttpServer server_;
  QTcpServer* tcp_ = nullptr;
  quint16 port_ = 0;
  int64_t startedUtcMs_ = 0;
  std::function<void()> shutdown_;
};

}
