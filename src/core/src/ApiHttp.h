#pragma once
#include "fovea/Api.h"
#include "fovea/core/Analytics.h"
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
#include <optional>

namespace fovea::core::http {

using Status = QHttpServerResponse::StatusCode;

inline QHttpServerResponse json(const QJsonObject& o, Status s = Status::Ok) { return QHttpServerResponse(o, s); }
inline QHttpServerResponse json(const QJsonArray& a, Status s = Status::Ok) { return QHttpServerResponse(a, s); }
inline QHttpServerResponse fail(Status s, const QString& code, const QString& message) {
  return QHttpServerResponse(errorJson(code, message), s);
}
inline QHttpServerResponse fail(const ServiceError& e) {
  return QHttpServerResponse(errorJson(e.code, e.message), static_cast<Status>(e.status));
}
inline QHttpServerResponse unauthorized() { return fail(Status::Unauthorized, "unauthorized", "missing or invalid token"); }

inline std::optional<QJsonObject> body(const QHttpServerRequest& req) {
  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(req.body(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
  return doc.object();
}

// An empty body counts as an empty object.
inline std::optional<QJsonObject> optionalBody(const QHttpServerRequest& req) {
  if (req.body().trimmed().isEmpty()) return QJsonObject{};
  return body(req);
}

inline int64_t queryInt(const QHttpServerRequest& req, const char* key, int64_t def) {
  const QString v = req.query().queryItemValue(QLatin1String(key));
  bool ok = false;
  const int64_t n = v.toLongLong(&ok);
  return ok ? n : def;
}

}
