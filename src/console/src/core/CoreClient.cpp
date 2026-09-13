#include "core/CoreClient.h"
#include "fovea/Api.h"
#include "fovea/Paths.h"
#include "fovea/Token.h"
#include <QEventLoop>
#include <QFile>
#include <QJsonParseError>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace fovea::ui {

namespace {
CoreClient::Failure classify(QNetworkReply::NetworkError error) {
  switch (error) {
    case QNetworkReply::NoError: return CoreClient::Failure::None;
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError: return CoreClient::Failure::Refused;
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError: return CoreClient::Failure::Timeout;
    default: return CoreClient::Failure::Other;
  }
}
}

CoreClient::CoreClient(QObject* parent) : QObject(parent) {
  nam_.setAutoDeleteReplies(false);
}

void CoreClient::resetEndpoint() {
  port_ = 0;
  token_.clear();
}

QString CoreClient::baseUrl() const {
  return port_ ? QStringLiteral("http://127.0.0.1:%1").arg(port_) : QString();
}

void CoreClient::drain(int timeoutMs) {
  if (pending_ == 0) return;
  QEventLoop loop;
  QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
  connect(this, &CoreClient::drained, &loop, &QEventLoop::quit);
  loop.exec();
}

bool CoreClient::ensureEndpoint(QString& error) {
  if (port_ != 0) return true;
  const QString infoPath = fovea::coreInfoPath();
  QFile f(infoPath);
  if (!f.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("service discovery file not found: %1").arg(infoPath);
    return false;
  }
  QJsonParseError parseError{};
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
  const int port = doc.object().value(QLatin1StringView("port")).toInt();
  if (parseError.error != QJsonParseError::NoError || port <= 0 || port > 65535) {
    error = QStringLiteral("service discovery file is invalid: %1").arg(infoPath);
    return false;
  }
  const QString token = fovea::loadToken(fovea::tokenPath());
  if (token.isEmpty()) {
    error = QStringLiteral("service token not readable: %1").arg(fovea::tokenPath());
    return false;
  }
  port_ = static_cast<quint16>(port);
  token_ = token;
  return true;
}

void CoreClient::request(const QByteArray& verb, const QString& path, const QJsonDocument& body, int timeoutMs,
                         ResponseCallback cb, QObject* context) {
  QObject* ctx = context ? context : this;
  QString discoveryError;
  if (!ensureEndpoint(discoveryError)) {
    QPointer<QObject> guard(ctx);
    QTimer::singleShot(0, this, [cb = std::move(cb), guard, discoveryError] {
      if (guard) cb(Response{Failure::NoDiscovery, QJsonDocument(), discoveryError, QByteArray()});
    });
    return;
  }
  QNetworkRequest req(QUrl(baseUrl() + path));
  req.setRawHeader("Authorization", "Bearer " + token_.toLatin1());
  req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  req.setTransferTimeout(timeoutMs);
  const QByteArray payload = body.isNull() ? QByteArray() : body.toJson(QJsonDocument::Compact);
  QNetworkReply* reply = nam_.sendCustomRequest(req, verb, payload);
  reply->setParent(this);
  ++pending_;
  connect(reply, &QNetworkReply::finished, ctx, [this, reply, cb = std::move(cb)] {
    const QNetworkReply::NetworkError netError = reply->error();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QJsonParseError parseError{};
    Response response;
    response.body = reply->readAll();
    response.doc = QJsonDocument::fromJson(response.body, &parseError);
    if (parseError.error != QJsonParseError::NoError) response.doc = QJsonDocument();
    response.failure = classify(netError);
    if (response.failure != Failure::None) {
      const QString apiMessage = response.doc.object().value(QLatin1StringView("error")).toObject()
                                     .value(QLatin1StringView("message")).toString();
      response.error = apiMessage.isEmpty() ? reply->errorString() : apiMessage;
      if (status > 0) response.error = QStringLiteral("HTTP %1: %2").arg(status).arg(response.error);
      if (response.failure == Failure::Refused) resetEndpoint();
    } else if (status < 200 || status >= 300) {
      response.failure = Failure::Other;
      response.error = QStringLiteral("HTTP %1").arg(status);
    }
    cb(response);
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    reply->deleteLater();
    if (--pending_ == 0) emit drained();
  });
}

void CoreClient::send(const QByteArray& verb, const QString& path, const QJsonDocument& body, Callback cb,
                      QObject* context, int timeoutMs) {
  request(verb, path, body, timeoutMs, [cb = std::move(cb)](const Response& r) {
    cb(r.failure == Failure::None, r.doc, r.error);
  }, context);
}

void CoreClient::probeHealth(ProbeCallback cb, QObject* context) {
  request("GET", QStringLiteral("/v1/health"), {}, kTimeoutMs, [cb = std::move(cb)](const Response& r) {
    cb(ProbeResult{r.failure == Failure::None, r.failure, r.error});
  }, context);
}

void CoreClient::listCameras(Callback cb, QObject* context) {
  send("GET", QStringLiteral("/v1/cameras"), {}, std::move(cb), context);
}

void CoreClient::createCamera(const QJsonObject& camera, Callback cb, QObject* context) {
  send("POST", QStringLiteral("/v1/cameras"), QJsonDocument(camera), std::move(cb), context);
}

void CoreClient::updateCamera(const QString& id, const QJsonObject& camera, Callback cb, QObject* context) {
  send("PUT", QStringLiteral("/v1/cameras/%1").arg(id), QJsonDocument(camera), std::move(cb), context);
}

void CoreClient::deleteCamera(const QString& id, Callback cb, QObject* context) {
  send("DELETE", QStringLiteral("/v1/cameras/%1").arg(id), {}, std::move(cb), context);
}

void CoreClient::enableCamera(const QString& id, bool enabled, Callback cb, QObject* context) {
  const QString action = enabled ? QStringLiteral("enable") : QStringLiteral("disable");
  send("POST", QStringLiteral("/v1/cameras/%1/%2").arg(id, action), {}, std::move(cb), context);
}

void CoreClient::testConnection(const QJsonObject& camera, Callback cb, QObject* context) {
  const int probeMs = camera.value(QLatin1StringView("timeout_ms")).toInt(fovea::Camera{}.timeoutMs);
  send("POST", QStringLiteral("/v1/cameras/test"), QJsonDocument(camera), std::move(cb), context,
       probeMs + kTestTimeoutMarginMs);
}

void CoreClient::listSegments(const QString& id, int64_t fromUtcMs, int64_t toUtcMs, Callback cb, QObject* context) {
  QUrlQuery q;
  q.addQueryItem(QStringLiteral("from_utc_ms"), QString::number(fromUtcMs));
  q.addQueryItem(QStringLiteral("to_utc_ms"), QString::number(toUtcMs));
  send("GET", QStringLiteral("/v1/cameras/%1/segments?%2").arg(id, q.query()), {}, std::move(cb), context);
}

void CoreClient::listGaps(const QString& id, int64_t fromUtcMs, int64_t toUtcMs, Callback cb, QObject* context) {
  QUrlQuery q;
  q.addQueryItem(QStringLiteral("from_utc_ms"), QString::number(fromUtcMs));
  q.addQueryItem(QStringLiteral("to_utc_ms"), QString::number(toUtcMs));
  send("GET", QStringLiteral("/v1/cameras/%1/gaps?%2").arg(id, q.query()), {}, std::move(cb), context);
}

void CoreClient::listSessions(const QString& id, Callback cb, QObject* context) {
  send("GET", QStringLiteral("/v1/cameras/%1/sessions").arg(id), {}, std::move(cb), context);
}

void CoreClient::openPlayback(const QJsonObject& request, Callback cb, QObject* context) {
  send("POST", QStringLiteral("/v1/playback"), QJsonDocument(request), std::move(cb), context);
}

void CoreClient::playbackState(const QString& id, Callback cb, QObject* context) {
  send("GET", QStringLiteral("/v1/playback/%1").arg(id), {}, std::move(cb), context);
}

void CoreClient::playbackControl(const QString& id, const QString& action, const QJsonObject& body, Callback cb,
                                 QObject* context) {
  send("POST", QStringLiteral("/v1/playback/%1/%2").arg(id, action),
       body.isEmpty() ? QJsonDocument() : QJsonDocument(body), std::move(cb), context);
}

void CoreClient::closePlayback(const QString& id, Callback cb, QObject* context) {
  send("DELETE", QStringLiteral("/v1/playback/%1").arg(id), {}, std::move(cb), context);
}

void CoreClient::metrics(Callback cb, QObject* context) {
  send("GET", QStringLiteral("/v1/metrics"), {}, std::move(cb), context);
}

void CoreClient::listZones(Callback cb, QObject* context) {
  send("GET", QStringLiteral("/v1/zones"), {}, std::move(cb), context);
}

void CoreClient::createZone(const QJsonObject& zone, Callback cb, QObject* context) {
  send("POST", QStringLiteral("/v1/zones"), QJsonDocument(zone), std::move(cb), context);
}

void CoreClient::updateZone(const QString& id, const QJsonObject& zone, Callback cb, QObject* context) {
  send("PUT", QStringLiteral("/v1/zones/%1").arg(id), QJsonDocument(zone), std::move(cb), context);
}

void CoreClient::listRules(Callback cb, QObject* context) {
  send("GET", QStringLiteral("/v1/rules"), {}, std::move(cb), context);
}

void CoreClient::createRule(const QJsonObject& revision, Callback cb, QObject* context) {
  send("POST", QStringLiteral("/v1/rules"), QJsonDocument(revision), std::move(cb), context);
}

void CoreClient::updateRule(const QString& id, const QJsonObject& revision, Callback cb, QObject* context) {
  send("PUT", QStringLiteral("/v1/rules/%1").arg(id), QJsonDocument(revision), std::move(cb), context);
}

void CoreClient::setRuleEnabled(const QString& id, bool enabled, Callback cb, QObject* context) {
  const QString action = enabled ? QStringLiteral("enable") : QStringLiteral("disable");
  send("POST", QStringLiteral("/v1/rules/%1/%2").arg(id, action), {}, std::move(cb), context);
}

void CoreClient::listEvents(int64_t fromUtcMs, int limit, Callback cb, QObject* context) {
  QUrlQuery q;
  q.addQueryItem(QStringLiteral("from_utc_ms"), QString::number(fromUtcMs));
  q.addQueryItem(QStringLiteral("limit"), QString::number(limit));
  send("GET", QStringLiteral("/v1/events?%1").arg(q.query()), {}, std::move(cb), context);
}

void CoreClient::getEvent(const QString& id, Callback cb, QObject* context) {
  send("GET", QStringLiteral("/v1/events/%1").arg(id), {}, std::move(cb), context);
}

void CoreClient::eventCounts(Callback cb, QObject* context) {
  send("GET", QStringLiteral("/v1/events/counts"), {}, std::move(cb), context);
}

void CoreClient::eventAction(const QString& id, const QString& action, const QJsonObject& body, Callback cb,
                             QObject* context) {
  send("POST", QStringLiteral("/v1/events/%1/%2").arg(id, action),
       body.isEmpty() ? QJsonDocument() : QJsonDocument(body), std::move(cb), context);
}

void CoreClient::pendingAlerts(const QString& consoleId, Callback cb, QObject* context) {
  QUrlQuery q;
  q.addQueryItem(QStringLiteral("console_id"), consoleId);
  send("GET", QStringLiteral("/v1/alerts/pending?%1").arg(q.query()), {}, std::move(cb), context);
}

void CoreClient::confirmAlert(const QString& deliveryId, const QString& consoleId, Callback cb, QObject* context) {
  send("POST", QStringLiteral("/v1/alerts/%1/delivered").arg(deliveryId), QJsonDocument(QJsonObject{{"console_id", consoleId}}),
       std::move(cb), context);
}

void CoreClient::latestDetections(const QString& cameraId, Callback cb, QObject* context) {
  send("GET", QStringLiteral("/v1/cameras/%1/detections/latest").arg(cameraId), {}, std::move(cb), context);
}

void CoreClient::evidenceThumbnail(const QString& evidenceId, BytesCallback cb, QObject* context) {
  request("GET", QStringLiteral("/v1/evidence/%1/thumbnail").arg(evidenceId), {}, kTimeoutMs,
          [cb = std::move(cb)](const Response& r) { cb(r.failure == Failure::None, r.body, r.error); }, context);
}

void CoreClient::shutdownService(Callback cb, QObject* context) {
  send("POST", QStringLiteral("/v1/service/shutdown"), {}, std::move(cb), context);
}

}
