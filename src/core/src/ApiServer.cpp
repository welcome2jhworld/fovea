#include "fovea/core/ApiServer.h"
#include "ApiHttp.h"
#include "fovea/Clock.h"
#include "fovea/Ids.h"
#include "fovea/Redact.h"
#include "fovea/Token.h"
#include "fovea/core/CameraManager.h"
#include "fovea/core/AnalysisScheduler.h"
#include "fovea/core/PlaybackManager.h"
#include "fovea/core/RetentionManager.h"
#include "fovea/core/RuleEngine.h"
#include "fovea/core/Store.h"
#include "fovea/core/WorkerSupervisor.h"
#include <QFuture>
#include <QHostAddress>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPromise>
#include <QTimer>
#include <QUrlQuery>

namespace fovea::core {
namespace {

using namespace http;

std::optional<Credentials> credentialsFrom(const QJsonObject& o) {
  if (!o.contains("username") && !o.contains("password")) return std::nullopt;
  return Credentials{o.value("username").toString(), o.value("password").toString()};
}

QJsonObject cameraWithStatus(const Camera& c, const std::optional<CameraStatus>& s) {
  QJsonObject o = c.toJson();
  o.insert("status", s ? s->toJson() : QJsonObject{});
  return o;
}

}

ApiServer::ApiServer(CameraManager& cameras, PlaybackManager& playback, RetentionManager& retention, Store& store,
                     AnalyticsServices analytics, QString token, QObject* parent)
    : QObject(parent), cameras_(cameras), playback_(playback), retention_(retention), store_(store), analytics_(analytics),
      token_(std::move(token)) {
  cpu_.sample(monoNowNs(), processCpuTimeNs());
  registerRoutes();
  registerAnalyticsRoutes();
}

bool ApiServer::listen(quint16 port) {
  tcp_ = new QTcpServer(this);
  if (!tcp_->listen(QHostAddress::LocalHost, port)) return false;
  if (!server_.bind(tcp_)) return false;
  port_ = tcp_->serverPort();
  return true;
}

bool ApiServer::authorized(const QHttpServerRequest& request) const {
  const QByteArray auth = request.value("Authorization");
  if (!auth.startsWith("Bearer ")) return false;
  return tokenEquals(QString::fromLatin1(auth.mid(7)).trimmed(), token_);
}

void ApiServer::registerRoutes() {
  using Method = QHttpServerRequest::Method;

  server_.route("/v1/health", Method::Get, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    return json(QJsonObject{{"status", "ok"}, {"version", "0.1.0"},
                            {"uptime_ms", static_cast<double>(utcNowMs() - startedUtcMs_)},
                            {"data_dir", cameras_.config().dataDir}});
  });

  server_.route("/v1/cameras", Method::Get, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    QJsonArray arr;
    for (const Camera& c : cameras_.cameras()) arr.push_back(cameraWithStatus(c, cameras_.status(c.id)));
    return json(arr);
  });

  server_.route("/v1/cameras", Method::Post, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto b = body(req);
    if (!b) return fail(Status::BadRequest, "bad_json", "body must be a JSON object");
    Camera c = Camera::fromJson(*b);
    QString error;
    const auto created = cameras_.createCamera(c, credentialsFrom(*b), &error);
    if (!created) return fail(Status::BadRequest, "invalid_camera", error);
    store_.appendAudit("api", "camera.create", created->id, redactUrl(created->mainUrl), utcNowMs());
    return json(cameraWithStatus(*created, cameras_.status(created->id)), Status::Created);
  });

  server_.route("/v1/cameras/test", Method::Post, [this](const QHttpServerRequest& req) -> QFuture<QHttpServerResponse> {
    QPromise<QHttpServerResponse> promise;
    QFuture<QHttpServerResponse> future = promise.future();
    if (!authorized(req)) {
      promise.start();
      promise.addResult(unauthorized());
      promise.finish();
      return future;
    }
    const auto b = body(req);
    if (!b) {
      promise.start();
      promise.addResult(fail(Status::BadRequest, "bad_json", "body must be a JSON object"));
      promise.finish();
      return future;
    }
    auto shared = std::make_shared<QPromise<QHttpServerResponse>>(std::move(promise));
    shared->start();
    cameras_.testConnection(Camera::fromJson(*b), credentialsFrom(*b), [shared](ConnectionTest result) {
      shared->addResult(json(result.toJson()));
      shared->finish();
    });
    return future;
  });

  server_.route("/v1/cameras/<arg>", Method::Get, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto c = cameras_.camera(id);
    if (!c) return fail(Status::NotFound, "not_found", "no such camera");
    return json(cameraWithStatus(*c, cameras_.status(id)));
  });

  server_.route("/v1/cameras/<arg>", Method::Put, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto existing = cameras_.camera(id);
    if (!existing) return fail(Status::NotFound, "not_found", "no such camera");
    const auto b = body(req);
    if (!b) return fail(Status::BadRequest, "bad_json", "body must be a JSON object");
    QJsonObject merged = existing->toJson();
    for (auto it = b->begin(); it != b->end(); ++it) merged.insert(it.key(), it.value());
    Camera c = Camera::fromJson(merged);
    c.id = id;
    QString error;
    if (!cameras_.updateCamera(c, credentialsFrom(*b), &error)) return fail(Status::BadRequest, "invalid_camera", error);
    store_.appendAudit("api", "camera.update", id, redactUrl(c.mainUrl), utcNowMs());
    if (b->contains("retention_days") || b->contains("max_bytes")) retention_.requestRun();
    return json(cameraWithStatus(*cameras_.camera(id), cameras_.status(id)));
  });

  server_.route("/v1/cameras/<arg>", Method::Delete, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    QString error;
    if (!cameras_.deleteCamera(id, &error)) return fail(Status::NotFound, "not_found", error);
    store_.appendAudit("api", "camera.delete", id, QString(), utcNowMs());
    analytics_.rules.onCameraDeleted(id);
    return QHttpServerResponse(Status::NoContent);
  });

  server_.route("/v1/cameras/<arg>/enable", Method::Post, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    QString error;
    if (!cameras_.setEnabled(id, true, &error)) return fail(Status::NotFound, "not_found", error);
    return json(cameras_.status(id).value_or(CameraStatus{}).toJson());
  });

  server_.route("/v1/cameras/<arg>/disable", Method::Post, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    QString error;
    if (!cameras_.setEnabled(id, false, &error)) return fail(Status::NotFound, "not_found", error);
    return json(cameras_.status(id).value_or(CameraStatus{}).toJson());
  });

  server_.route("/v1/cameras/<arg>/status", Method::Get, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto s = cameras_.status(id);
    if (!s) return fail(Status::NotFound, "not_found", "no such camera");
    return json(s->toJson());
  });

  server_.route("/v1/cameras/<arg>/sessions", Method::Get, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    if (!cameras_.camera(id)) return fail(Status::NotFound, "not_found", "no such camera");
    QJsonArray arr;
    for (const StreamSession& s : store_.listSessions(id, static_cast<int>(queryInt(req, "limit", 100)))) arr.push_back(s.toJson());
    return json(arr);
  });

  server_.route("/v1/cameras/<arg>/segments", Method::Get, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    if (!cameras_.camera(id)) return fail(Status::NotFound, "not_found", "no such camera");
    QJsonArray arr;
    for (const RecordingSegment& s : store_.listSegments(id, queryInt(req, "from_utc_ms", 0), queryInt(req, "to_utc_ms", 0),
                                                         static_cast<int>(queryInt(req, "limit", 500))))
      arr.push_back(s.toJson());
    return json(arr);
  });

  server_.route("/v1/cameras/<arg>/gaps", Method::Get, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    if (!cameras_.camera(id)) return fail(Status::NotFound, "not_found", "no such camera");
    QJsonArray arr;
    for (const ReceiveGap& g : store_.listGaps(id, queryInt(req, "from_utc_ms", 0), queryInt(req, "to_utc_ms", 0),
                                               static_cast<int>(queryInt(req, "limit", 500))))
      arr.push_back(g.toJson());
    return json(arr);
  });

  server_.route("/v1/segments/<arg>", Method::Get, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto s = store_.getSegment(id);
    if (!s) return fail(Status::NotFound, "not_found", "no such segment");
    return json(s->toJson());
  });

  server_.route("/v1/playback", Method::Post, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto b = body(req);
    if (!b) return fail(Status::BadRequest, "bad_json", "body must be a JSON object");
    QString error;
    std::optional<PlaybackState> state;
    if (b->contains("segment_id"))
      state = playback_.open(b->value("segment_id").toString(), &error);
    else if (b->contains("camera_id"))
      state = playback_.openAt(b->value("camera_id").toString(), static_cast<int64_t>(b->value("at_utc_ms").toDouble()), &error);
    else
      return fail(Status::BadRequest, "bad_request", "segment_id or camera_id required");
    if (!state) return fail(Status::NotFound, "playback_failed", error);
    store_.appendAudit("api", "playback.open", state->segmentId, QString(), utcNowMs());
    return json(state->toJson(), Status::Created);
  });

  server_.route("/v1/playback/<arg>", Method::Get, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto s = playback_.state(id);
    if (!s) return fail(Status::NotFound, "not_found", "no such playback channel");
    return json(s->toJson());
  });

  server_.route("/v1/playback/<arg>", Method::Delete, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    if (!playback_.close(id)) return fail(Status::NotFound, "not_found", "no such playback channel");
    return QHttpServerResponse(Status::NoContent);
  });

  server_.route("/v1/playback/<arg>/play", Method::Post, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    if (!playback_.play(id)) return fail(Status::NotFound, "not_found", "no such playback channel");
    return json(playback_.state(id).value_or(PlaybackState{}).toJson());
  });

  server_.route("/v1/playback/<arg>/pause", Method::Post, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    if (!playback_.pause(id)) return fail(Status::NotFound, "not_found", "no such playback channel");
    return json(playback_.state(id).value_or(PlaybackState{}).toJson());
  });

  server_.route("/v1/playback/<arg>/seek", Method::Post, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto b = body(req);
    if (!b || !b->contains("pts_ns")) return fail(Status::BadRequest, "bad_request", "pts_ns required");
    if (!playback_.seek(id, static_cast<int64_t>(b->value("pts_ns").toDouble()))) return fail(Status::NotFound, "not_found", "no such playback channel");
    return json(playback_.state(id).value_or(PlaybackState{}).toJson());
  });

  server_.route("/v1/playback/<arg>/rate", Method::Post, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto b = body(req);
    if (!b || !b->contains("rate")) return fail(Status::BadRequest, "bad_request", "rate required");
    if (!playback_.setRate(id, b->value("rate").toDouble(1.0))) return fail(Status::NotFound, "not_found", "no such playback channel");
    return json(playback_.state(id).value_or(PlaybackState{}).toJson());
  });

  server_.route("/v1/metrics", Method::Get, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    QJsonObject m = cameras_.metrics();
    QJsonArray withAnalysis;
    for (const QJsonValue& v : m.value("cameras").toArray()) {
      QJsonObject cam = v.toObject();
      if (analytics_.scheduler.analyzing(cam.value("camera_id").toString()))
        cam.insert("analysis", analytics_.scheduler.cameraJson(cam.value("camera_id").toString()));
      withAnalysis.push_back(cam);
    }
    m.insert("cameras", withAnalysis);
    m.insert("worker", analytics_.worker.toJson());
    QJsonObject process = m.value("process").toObject();
    const int64_t cpuNs = processCpuTimeNs();
    process.insert("rss_bytes", static_cast<double>(processRssBytes()));
    process.insert("cpu_percent", cpu_.sample(monoNowNs(), cpuNs));
    process.insert("cpu_time_ms", static_cast<double>(cpuNs / 1'000'000));
    m.insert("process", process);
    m.insert("playback_channels", playback_.openCount());
    m.insert("uptime_ms", static_cast<double>(utcNowMs() - startedUtcMs_));
    return json(m);
  });

  server_.route("/v1/storage", Method::Get, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    return json(retention_.storageJson());
  });

  server_.route("/v1/service/shutdown", Method::Post, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    store_.appendAudit("api", "service.shutdown", QString(), QString(), utcNowMs());
    if (shutdown_) QTimer::singleShot(50, this, [this] { shutdown_(); });
    return json(QJsonObject{{"status", "stopping"}});
  });
}

}
