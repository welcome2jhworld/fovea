#include "ApiHttp.h"
#include "fovea/Clock.h"
#include "fovea/core/ApiServer.h"
#include "fovea/core/ImportManager.h"
#include "fovea/core/IndexScheduler.h"
#include "fovea/core/SearchService.h"
#include "fovea/core/Store.h"
#include <QFuture>
#include <QPromise>
#include <algorithm>
#include <memory>

namespace fovea::core {
namespace {

using namespace http;

constexpr int64_t kMaxImportList = 1000;

QHttpServerResponse badJson() { return fail(Status::BadRequest, "bad_json", "body must be a JSON object"); }

QFuture<QHttpServerResponse> ready(QHttpServerResponse response) {
  QPromise<QHttpServerResponse> promise;
  QFuture<QHttpServerResponse> future = promise.future();
  promise.start();
  promise.addResult(std::move(response));
  promise.finish();
  return future;
}

QString detailJson(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

}

void ApiServer::registerSearchRoutes() {
  using Method = QHttpServerRequest::Method;

  server_.route("/v1/search", Method::Post, [this](const QHttpServerRequest& req) -> QFuture<QHttpServerResponse> {
    if (!authorized(req)) return ready(unauthorized());
    const auto b = body(req);
    if (!b) return ready(badJson());
    auto promise = std::make_shared<QPromise<QHttpServerResponse>>();
    QFuture<QHttpServerResponse> future = promise->future();
    promise->start();
    indexing_.search.search(*b, [promise](const QJsonObject& response, const ServiceError& error) {
      promise->addResult(error.code.isEmpty() ? json(response) : fail(error));
      promise->finish();
    });
    return future;
  });

  server_.route("/v1/search/thumbnails/<arg>", Method::Get, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    bool ok = false;
    const int64_t recordId = id.toLongLong(&ok);
    if (!ok) return fail(Status::NotFound, "not_found", "no such record");
    ServiceError error;
    const std::optional<QByteArray> jpeg = indexing_.search.thumbnail(recordId, &error);
    return jpeg ? QHttpServerResponse(QByteArrayLiteral("image/jpeg"), *jpeg) : fail(error);
  });

  server_.route("/v1/search/<arg>", Method::Get, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto session = indexing_.search.session(id);
    return session ? json(*session) : fail(Status::NotFound, "not_found", "no such search session");
  });

  server_.route("/v1/index", Method::Get, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    return json(indexing_.index.statusJson(queryInt(req, "storage", 0) != 0));
  });

  server_.route("/v1/index/active", Method::Put, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto b = body(req);
    if (!b) return badJson();
    const QString name = b->value("index_version_name").toString();
    const QString previous = indexing_.index.activeName();
    ServiceError error;
    if (!indexing_.index.setActive(name, &error)) return fail(error);
    store_.appendAudit("api", "index.activate", name, detailJson({{"previous", previous}}), utcNowMs());
    return json(indexing_.index.statusJson(false));
  });

  server_.route("/v1/index/versions/<arg>", Method::Delete, [this](const QString& hash, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    ServiceError error;
    if (!indexing_.index.deleteVersion(hash, &error)) return fail(error);
    store_.appendAudit("api", "index.version.delete", hash, QString(), utcNowMs());
    return QHttpServerResponse(Status::NoContent);
  });

  server_.route("/v1/imports", Method::Post, [this](const QHttpServerRequest& req) -> QFuture<QHttpServerResponse> {
    if (!authorized(req)) return ready(unauthorized());
    const auto b = body(req);
    if (!b) return ready(badJson());
    auto promise = std::make_shared<QPromise<QHttpServerResponse>>();
    QFuture<QHttpServerResponse> future = promise->future();
    promise->start();
    indexing_.imports.submit(b->value("path").toString(), b->value("camera_id").toString(),
                             static_cast<int64_t>(b->value("start_utc_ms").toDouble()),
                             [promise](std::optional<ImportRecord> record, const ServiceError& error) {
                               promise->addResult(record ? json(record->toJson(), Status::Accepted) : fail(error));
                               promise->finish();
                             });
    return future;
  });

  server_.route("/v1/imports", Method::Get, [this](const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    QJsonArray arr;
    for (const ImportRecord& r : store_.listImports(static_cast<int>(std::clamp<int64_t>(queryInt(req, "limit", 100), 1, kMaxImportList))))
      arr.push_back(r.toJson());
    return json(arr);
  });

  server_.route("/v1/imports/<arg>", Method::Get, [this](const QString& id, const QHttpServerRequest& req) {
    if (!authorized(req)) return unauthorized();
    const auto record = store_.getImport(id);
    return record ? json(record->toJson()) : fail(Status::NotFound, "not_found", "no such import");
  });
}

}
