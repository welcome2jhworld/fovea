#include "core/SearchTypes.h"
#include <QJsonArray>
#include <QJsonValue>
#include <algorithm>

namespace fovea::ui {

namespace {
constexpr QLatin1StringView kThumbnailPrefix{"/v1/search/thumbnails/"};

int64_t i64(const QJsonObject& o, const char* k) {
  const QJsonValue v = o.value(QLatin1StringView(k));
  return v.isDouble() ? static_cast<int64_t>(v.toDouble()) : 0;
}
QString str(const QJsonObject& o, const char* k) { return o.value(QLatin1StringView(k)).toString(); }

std::optional<double> ratio(const QJsonObject& o, const char* k) {
  const QJsonValue v = o.value(QLatin1StringView(k));
  if (!v.isDouble()) return std::nullopt;
  return std::clamp(v.toDouble(), 0.0, 1.0);
}

// Record ids are uint64 in the vector store; the API may send them as numbers or strings.
QString idString(const QJsonValue& v) {
  if (v.isString()) return v.toString();
  if (v.isDouble()) return QString::number(static_cast<qint64>(v.toDouble()));
  return {};
}

// A count, or an array whose length is the count.
int countOf(const QJsonValue& v) {
  if (v.isArray()) return static_cast<int>(v.toArray().size());
  return v.toInt();
}
}

SearchResultInfo SearchResultInfo::fromJson(const QJsonObject& o) {
  SearchResultInfo r;
  r.cameraId = str(o, "camera_id");
  r.startUtcMs = i64(o, "start_utc_ms");
  r.endUtcMs = std::max(r.startUtcMs, i64(o, "end_utc_ms"));
  const QJsonValue relevance = o.value(QLatin1StringView("relevance"));
  r.relevance = relevance.isDouble() ? relevance.toDouble() : o.value(QLatin1StringView("score")).toDouble();
  const QJsonObject rep = o.value(QLatin1StringView("representative")).toObject();
  r.representativeUtcMs = i64(rep, "utc_ms");
  r.recordId = idString(rep.value(QLatin1StringView("record_id")));
  if (r.recordId.isEmpty()) {
    const QString thumbnail = idString(rep.value(QLatin1StringView("thumbnail")));
    r.recordId = thumbnail.startsWith(kThumbnailPrefix) ? thumbnail.mid(kThumbnailPrefix.size()) : thumbnail;
  }
  r.samples = countOf(o.value(QLatin1StringView("samples")));
  r.evidenceState = str(o, "evidence_state");
  return r;
}

SearchStatsInfo SearchStatsInfo::fromJson(const QJsonObject& o) {
  SearchStatsInfo s;
  s.samplesScanned = i64(o, "samples_scanned");
  s.hoursScanned = o.value(QLatin1StringView("hours_scanned")).toDouble();
  s.coverageRatio = ratio(o, "coverage_ratio");
  s.embedMs = i64(o, "embed_ms");
  s.scanMs = i64(o, "scan_ms");
  s.totalMs = i64(o, "total_ms");
  return s;
}

SearchResponseInfo SearchResponseInfo::fromJson(const QJsonObject& o) {
  SearchResponseInfo r;
  r.sessionId = idString(o.value(QLatin1StringView("session_id")));
  for (const QJsonValue& v : o.value(QLatin1StringView("results")).toArray()) r.results.push_back(SearchResultInfo::fromJson(v.toObject()));
  r.stats = SearchStatsInfo::fromJson(o.value(QLatin1StringView("stats")).toObject());
  r.indexVersion = str(o, "index_version");
  r.indexVersionName = str(o, "index_version_name");
  r.model = str(o, "model");
  return r;
}

IndexStatusInfo IndexStatusInfo::fromJson(const QJsonObject& o) {
  IndexStatusInfo s;
  s.indexVersion = str(o, "active_index_version");
  if (s.indexVersion.isEmpty()) s.indexVersion = str(o, "index_version");
  s.model = str(o, "model");
  s.state = str(o, "state");
  s.lastError = str(o, "last_error");
  for (const QJsonValue& v : o.value(QLatin1StringView("cameras")).toArray()) {
    const QJsonObject c = v.toObject();
    IndexCameraInfo camera;
    camera.cameraId = str(c, "camera_id");
    camera.indexEnabled = c.value(QLatin1StringView("index_enabled")).toBool(true);
    camera.framesIndexed = i64(c, "frames_indexed");
    camera.framesExpected = i64(c, "frames_expected");
    camera.coverageRatio = ratio(c, "coverage_ratio");
    if (!camera.coverageRatio && camera.framesExpected > 0)
      camera.coverageRatio = std::clamp(static_cast<double>(camera.framesIndexed) / static_cast<double>(camera.framesExpected), 0.0, 1.0);
    if (!camera.cameraId.isEmpty()) s.cameras.push_back(camera);
  }
  const QJsonValue queue = o.value(QLatin1StringView("queue"));
  if (queue.isObject()) {
    const QJsonObject q = queue.toObject();
    s.queued = q.value(QLatin1StringView("queued")).toInt();
    s.running = q.value(QLatin1StringView("running")).toInt();
    s.failed = q.value(QLatin1StringView("failed")).toInt();
  } else {
    s.queued = queue.toInt();
  }
  return s;
}

}
