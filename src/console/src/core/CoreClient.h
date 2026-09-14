#pragma once
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <cstdint>
#include <functional>

namespace fovea::ui {

class CoreClient : public QObject {
  Q_OBJECT
public:
  using Callback = std::function<void(bool ok, const QJsonDocument& doc, const QString& error)>;

  // Why a request did not produce a 2xx answer; NoDiscovery and Refused mean nothing is serving.
  enum class Failure { None, NoDiscovery, Refused, Timeout, Other };
  struct ProbeResult {
    bool ok = false;
    Failure failure = Failure::None;
    QString error;
  };
  using ProbeCallback = std::function<void(const ProbeResult& result)>;
  using BytesCallback = std::function<void(bool ok, const QByteArray& body, const QString& error)>;

  static constexpr int kTimeoutMs = 5000;
  // A query waits for the worker's text embedding (3 s deadline) and the vector scan.
  static constexpr int kSearchTimeoutMs = 30000;
  // Headroom over the timeout_ms the core probes a camera for.
  static constexpr int kTestTimeoutMarginMs = 5000;

  explicit CoreClient(QObject* parent = nullptr);

  // Forget the cached port and token so the next request re-reads core.json.
  void resetEndpoint();
  bool endpointKnown() const { return port_ != 0; }
  // Requests sent and not yet finished (an aborted request finishes at once).
  int pendingRequests() const { return pending_; }
  QString baseUrl() const;
  // Runs the event loop until no request is outstanding or timeoutMs elapses; used on exit so
  // requests issued while closing (DELETE /v1/playback/{id}) reach the service.
  void drain(int timeoutMs);

  void probeHealth(ProbeCallback cb, QObject* context = nullptr);
  void listCameras(Callback cb, QObject* context = nullptr);
  void createCamera(const QJsonObject& camera, Callback cb, QObject* context = nullptr);
  void updateCamera(const QString& id, const QJsonObject& camera, Callback cb, QObject* context = nullptr);
  void deleteCamera(const QString& id, Callback cb, QObject* context = nullptr);
  void enableCamera(const QString& id, bool enabled, Callback cb, QObject* context = nullptr);
  void testConnection(const QJsonObject& camera, Callback cb, QObject* context = nullptr);
  void listSegments(const QString& id, int64_t fromUtcMs, int64_t toUtcMs, Callback cb, QObject* context = nullptr);
  void listGaps(const QString& id, int64_t fromUtcMs, int64_t toUtcMs, Callback cb, QObject* context = nullptr);
  void listSessions(const QString& id, Callback cb, QObject* context = nullptr);
  void openPlayback(const QJsonObject& request, Callback cb, QObject* context = nullptr);
  void playbackState(const QString& id, Callback cb, QObject* context = nullptr);
  void playbackControl(const QString& id, const QString& action, const QJsonObject& body, Callback cb,
                       QObject* context = nullptr);
  void closePlayback(const QString& id, Callback cb, QObject* context = nullptr);
  void metrics(Callback cb, QObject* context = nullptr);

  // M3: zones, rules, events, alert delivery, overlays, evidence (docs/M3_DESIGN.md).
  void listZones(Callback cb, QObject* context = nullptr);
  void createZone(const QJsonObject& zone, Callback cb, QObject* context = nullptr);
  void updateZone(const QString& id, const QJsonObject& zone, Callback cb, QObject* context = nullptr);
  void listRules(Callback cb, QObject* context = nullptr);
  void createRule(const QJsonObject& revision, Callback cb, QObject* context = nullptr);
  void updateRule(const QString& id, const QJsonObject& revision, Callback cb, QObject* context = nullptr);
  void setRuleEnabled(const QString& id, bool enabled, Callback cb, QObject* context = nullptr);
  void listEvents(int64_t fromUtcMs, int limit, Callback cb, QObject* context = nullptr);
  void getEvent(const QString& id, Callback cb, QObject* context = nullptr);
  // GET /v1/events/counts: {unresolved, acknowledged, dismissed} over all events.
  void eventCounts(Callback cb, QObject* context = nullptr);
  // action: acknowledge, resolve or review (body {label, note}).
  void eventAction(const QString& id, const QString& action, const QJsonObject& body, Callback cb,
                   QObject* context = nullptr);
  void pendingAlerts(const QString& consoleId, Callback cb, QObject* context = nullptr);
  void confirmAlert(const QString& deliveryId, const QString& consoleId, Callback cb, QObject* context = nullptr);
  void latestDetections(const QString& cameraId, Callback cb, QObject* context = nullptr);
  void evidenceThumbnail(const QString& evidenceId, BytesCallback cb, QObject* context = nullptr);
  void shutdownService(Callback cb, QObject* context = nullptr);

  // M4: search and index status (docs/M4_DESIGN.md "Query path"). The returned reply lets a
  // newer query abort an older one; it is null when the service cannot be discovered.
  QPointer<QNetworkReply> search(const QJsonObject& request, Callback cb, QObject* context = nullptr);
  // The reply, so a caller that no longer needs the image can abort it.
  QPointer<QNetworkReply> searchThumbnail(const QString& recordId, BytesCallback cb, QObject* context = nullptr);
  void indexStatus(Callback cb, QObject* context = nullptr);

signals:
  void drained();

private:
  struct Response {
    Failure failure = Failure::None;
    QJsonDocument doc;
    QString error;
    QByteArray body;
  };
  using ResponseCallback = std::function<void(const Response& response)>;

  bool ensureEndpoint(QString& error);
  QNetworkReply* request(const QByteArray& verb, const QString& path, const QJsonDocument& body, int timeoutMs,
                         ResponseCallback cb, QObject* context);
  QNetworkReply* send(const QByteArray& verb, const QString& path, const QJsonDocument& body, Callback cb,
                      QObject* context, int timeoutMs = kTimeoutMs);

  QNetworkAccessManager nam_;
  quint16 port_ = 0;
  QString token_;
  int pending_ = 0;
};

}
