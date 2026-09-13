#include "ApiHttp.h"
#include "fovea/Clock.h"
#include "fovea/core/AnalysisScheduler.h"
#include "fovea/core/ApiServer.h"
#include "fovea/core/CameraManager.h"
#include "fovea/core/EventService.h"
#include "fovea/core/RuleEngine.h"
#include "fovea/core/Store.h"
#include "fovea/core/WorkerSupervisor.h"
#include <QFile>
#include <algorithm>

namespace fovea::core {
namespace {

using namespace http;

constexpr int64_t kMaxEventLimit = 1000;

QString operatorName(const QJsonObject& body) {
  const QString name = body.value(QStringLiteral("operator")).toString().trimmed();
  return name.isEmpty() ? QStringLiteral("operator") : name.left(64);
}

QHttpServerResponse badJson() { return fail(Status::BadRequest, "bad_json", "body must be a JSON object"); }

}

void ApiServer::registerAnalyticsRoutes() {
  using Method = QHttpServerRequest::Method;

  server_.route("/v1/zones", Method::Get, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    QJsonArray arr;
    for (const ZoneRecord& z : store_.listZones(req.query().queryItemValue(QStringLiteral("camera_id")))) arr.push_back(z.toJson());
    return json(arr);
  });

  server_.route("/v1/zones", Method::Post, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto b = body(req);
    if (!b) return badJson();
    ServiceError error;
    const auto zone = analytics_.rules.createZone(*b, &error);
    return zone ? json(zone->toJson(), Status::Created) : fail(error);
  });

  server_.route("/v1/zones/<arg>", Method::Put, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto b = body(req);
    if (!b) return badJson();
    ServiceError error;
    const auto zone = analytics_.rules.updateZone(id, *b, &error);
    return zone ? json(zone->toJson()) : fail(error);
  });

  server_.route("/v1/zones/<arg>", Method::Delete, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    ServiceError error;
    return analytics_.rules.deleteZone(id, &error) ? QHttpServerResponse(Status::NoContent) : fail(error);
  });

  server_.route("/v1/rules", Method::Get, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    QJsonArray arr;
    for (const RuleRecord& r : store_.listRules()) arr.push_back(analytics_.rules.ruleJson(r));
    return json(arr);
  });

  server_.route("/v1/rules", Method::Post, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto b = body(req);
    if (!b) return badJson();
    ServiceError error;
    const auto rule = analytics_.rules.createRule(*b, &error);
    return rule ? json(analytics_.rules.ruleJson(*rule), Status::Created) : fail(error);
  });

  server_.route("/v1/rules/<arg>", Method::Put, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto b = body(req);
    if (!b) return badJson();
    ServiceError error;
    const auto rule = analytics_.rules.updateRule(id, *b, &error);
    return rule ? json(analytics_.rules.ruleJson(*rule)) : fail(error);
  });

  server_.route("/v1/rules/<arg>", Method::Delete, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    ServiceError error;
    return analytics_.rules.deleteRule(id, &error) ? QHttpServerResponse(Status::NoContent) : fail(error);
  });

  for (const bool enable : {true, false}) {
    server_.route(enable ? "/v1/rules/<arg>/enable" : "/v1/rules/<arg>/disable", Method::Post,
                  [this, enable](const QString& id, const QHttpServerRequest& req) {
                    if (!authorized(req)) return unauthorized();
                    ServiceError error;
                    const auto rule = analytics_.rules.updateRule(id, QJsonObject{{"enabled", enable}}, &error);
                    return rule ? json(analytics_.rules.ruleJson(*rule)) : fail(error);
                  });
  }

  server_.route("/v1/events", Method::Get, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    EventQuery q;
    q.state = req.query().queryItemValue(QStringLiteral("state"));
    q.cameraId = req.query().queryItemValue(QStringLiteral("camera_id"));
    q.fromUtcMs = queryInt(req, "from_utc_ms", 0);
    q.toUtcMs = queryInt(req, "to_utc_ms", 0);
    q.limit = static_cast<int>(std::clamp<int64_t>(queryInt(req, "limit", 100), 1, kMaxEventLimit));
    QJsonArray arr;
    for (const EventRecord& e : store_.listEvents(q)) arr.push_back(analytics_.events.eventJson(e));
    return json(arr);
  });

  server_.route("/v1/events/counts", Method::Get, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const EventCounts c = store_.eventCounts();
    return json(QJsonObject{{"unresolved", c.unresolved}, {"acknowledged", c.acknowledged}, {"dismissed", c.dismissed}});
  });

  server_.route("/v1/events/<arg>", Method::Get, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto detail = analytics_.events.eventDetailJson(id);
    return detail ? json(*detail) : fail(Status::NotFound, "not_found", "no such event");
  });

  server_.route("/v1/events/<arg>/acknowledge", Method::Post, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto b = optionalBody(req);
    if (!b) return badJson();
    ServiceError error;
    const auto event = analytics_.events.acknowledge(id, operatorName(*b), &error);
    return event ? json(analytics_.events.eventJson(*event)) : fail(error);
  });

  server_.route("/v1/events/<arg>/resolve", Method::Post, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto b = optionalBody(req);
    if (!b) return badJson();
    ServiceError error;
    const auto event = analytics_.events.resolve(id, operatorName(*b), &error);
    return event ? json(analytics_.events.eventJson(*event)) : fail(error);
  });

  server_.route("/v1/events/<arg>/review", Method::Post, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto b = body(req);
    if (!b) return badJson();
    QJsonObject reviewBody = *b;
    reviewBody.insert("operator", operatorName(*b));
    ServiceError error;
    const auto event = analytics_.events.review(id, reviewBody, &error);
    return event ? json(analytics_.events.eventJson(*event)) : fail(error);
  });

  server_.route("/v1/alerts/pending", Method::Get, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    return json(analytics_.events.pendingAlertsJson(req.query().queryItemValue(QStringLiteral("console_id")), utcNowMs()));
  });

  server_.route("/v1/alerts/<arg>/delivered", Method::Post, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto b = optionalBody(req);
    if (!b) return badJson();
    ServiceError error;
    const auto delivery = analytics_.events.confirmDelivery(id, b->value(QStringLiteral("console_id")).toString(), &error);
    return delivery ? json(delivery->toJson()) : fail(error);
  });

  server_.route("/v1/cameras/<arg>/detections/latest", Method::Get, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    if (!cameras_.camera(id)) return fail(Status::NotFound, "not_found", "no such camera");
    if (!analytics_.scheduler.analyzing(id)) return fail(Status::NotFound, "analytics_off", "analytics is off for this camera");
    const auto detections = analytics_.scheduler.latestDetections(id);
    return detections ? json(*detections) : fail(Status::NotFound, "no_detections", "no detection frame yet");
  });

  server_.route("/v1/analysis", Method::Get, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    return json(QJsonObject{{"worker", analytics_.worker.toJson()}, {"cameras", analytics_.scheduler.camerasJson()}});
  });

  server_.route("/v1/evidence/<arg>/thumbnail", Method::Get, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto ref = store_.getEvidence(id);
    if (ref && ref->state == QLatin1String("deleted")) return fail(Status::NotFound, "evidence_deleted", "that evidence was deleted");
    if (!ref || ref->thumbnailPath.isEmpty()) return fail(Status::NotFound, "not_found", "no thumbnail for that evidence");
    QFile f(ref->thumbnailPath);
    if (!f.open(QIODevice::ReadOnly)) return fail(Status::NotFound, "not_found", "thumbnail file is missing");
    return QHttpServerResponse(QByteArrayLiteral("image/jpeg"), f.readAll());
  });
}

}
