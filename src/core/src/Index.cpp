#include "fovea/core/Index.h"
#include <cmath>

namespace fovea::core {
namespace {

double num(int64_t v) { return static_cast<double>(v); }
int64_t i64(const QJsonObject& o, const char* key) { return static_cast<int64_t>(o.value(QLatin1String(key)).toDouble()); }
double rounded(double v) { return std::round(v * 1e4) / 1e4; }

}

QJsonObject IndexVersion::toJson() const {
  return QJsonObject{{"hash", hash},
                     {"name", name},
                     {"model_id", modelId},
                     {"model_revision", modelRevision},
                     {"dims", dims},
                     {"dtype", dtype},
                     {"sample_interval_ms", sampleIntervalMs},
                     {"descriptor", descriptor},
                     {"created_utc_ms", num(createdUtcMs)}};
}

QJsonObject IndexJob::toJson() const {
  return QJsonObject{{"id", num(id)},
                     {"segment_id", segmentId},
                     {"camera_id", cameraId},
                     {"index_version", indexVersion},
                     {"state", state},
                     {"reason", reason},
                     {"attempts", attempts},
                     {"generation", num(generation)},
                     {"sample_interval_ms", sampleIntervalMs},
                     {"frames_expected", framesExpected},
                     {"frames_indexed", framesIndexed},
                     {"next_attempt_utc_ms", num(nextAttemptUtcMs)},
                     {"compute_ms", num(computeMs)},
                     {"footage_ms", num(footageMs)},
                     {"updated_utc_ms", num(updatedUtcMs)}};
}

QJsonObject ImportRecord::toJson() const {
  return QJsonObject{{"id", id},
                     {"camera_id", cameraId},
                     {"session_id", sessionId},
                     {"source_path", sourcePath},
                     {"state", state},
                     {"error", error},
                     {"codec", codec},
                     {"start_utc_ms", num(startUtcMs)},
                     {"duration_ns", num(durationNs)},
                     {"progress", rounded(progress)},
                     {"segments", segments},
                     {"bytes", num(bytes)},
                     {"created_utc_ms", num(createdUtcMs)},
                     {"started_utc_ms", num(startedUtcMs)},
                     {"finished_utc_ms", num(finishedUtcMs)}};
}

QJsonObject SearchSample::toJson() const {
  return QJsonObject{{"record_id", num(recordId)},
                     {"camera_id", cameraId},
                     {"segment_id", segmentId},
                     {"utc_ms", num(utcMs)},
                     {"relevance", rounded(static_cast<double>(score))}};
}

SearchSample SearchSample::fromJson(const QJsonObject& o) {
  SearchSample s;
  s.recordId = i64(o, "record_id");
  s.cameraId = o.value("camera_id").toString();
  s.segmentId = o.value("segment_id").toString();
  s.utcMs = i64(o, "utc_ms");
  s.score = static_cast<float>(o.value("relevance").toDouble());
  return s;
}

QJsonObject SearchRange::toJson() const {
  QJsonArray sampleArray;
  for (const SearchSample& s : samples) sampleArray.push_back(s.toJson());
  QJsonObject rep = representative.toJson();
  rep.insert("thumbnail", QStringLiteral("/v1/search/thumbnails/%1").arg(representative.recordId));
  return QJsonObject{{"camera_id", cameraId},
                     {"start_utc_ms", num(startUtcMs)},
                     {"end_utc_ms", num(endUtcMs)},
                     {"relevance", rounded(relevance)},
                     {"representative", rep},
                     {"samples", sampleArray},
                     {"evidence_state", evidenceState}};
}

SearchRange SearchRange::fromJson(const QJsonObject& o) {
  SearchRange r;
  r.cameraId = o.value("camera_id").toString();
  r.startUtcMs = i64(o, "start_utc_ms");
  r.endUtcMs = i64(o, "end_utc_ms");
  r.relevance = o.value("relevance").toDouble();
  r.representative = SearchSample::fromJson(o.value("representative").toObject());
  for (const QJsonValue& v : o.value("samples").toArray()) r.samples.push_back(SearchSample::fromJson(v.toObject()));
  r.evidenceState = o.value("evidence_state").toString(QStringLiteral("available"));
  return r;
}

QVector<SearchRange> mergeSamples(QVector<SearchSample> samples, int64_t minGapMs, int64_t sampleIntervalMs) {
  std::sort(samples.begin(), samples.end(), [](const SearchSample& a, const SearchSample& b) {
    return a.cameraId != b.cameraId ? a.cameraId < b.cameraId : a.utcMs < b.utcMs;
  });
  QVector<SearchRange> ranges;
  for (const SearchSample& s : samples) {
    SearchRange* open = ranges.isEmpty() ? nullptr : &ranges.last();
    if (open && open->cameraId == s.cameraId && s.utcMs - open->samples.last().utcMs < minGapMs) {
      open->samples.push_back(s);
      open->endUtcMs = s.utcMs + sampleIntervalMs;
      if (s.score > open->representative.score) {
        open->representative = s;
        open->relevance = s.score;
      }
      continue;
    }
    SearchRange r;
    r.cameraId = s.cameraId;
    r.startUtcMs = s.utcMs;
    r.endUtcMs = s.utcMs + sampleIntervalMs;
    r.relevance = s.score;
    r.representative = s;
    r.samples.push_back(s);
    ranges.push_back(r);
  }
  std::stable_sort(ranges.begin(), ranges.end(), [](const SearchRange& a, const SearchRange& b) {
    return a.relevance != b.relevance ? a.relevance > b.relevance : a.startUtcMs < b.startUtcMs;
  });
  return ranges;
}

int64_t gridPoints(int64_t fromUtcMs, int64_t toUtcMs, int64_t intervalMs) {
  if (intervalMs <= 0 || fromUtcMs <= 0 || toUtcMs < fromUtcMs) return 0;
  return toUtcMs / intervalMs - (fromUtcMs - 1) / intervalMs;
}

int64_t expectedSamples(int64_t startUtcMs, int64_t endUtcMs, int64_t intervalMs) {
  return gridPoints(startUtcMs, endUtcMs - 1, intervalMs);
}

bool normalizeVector(QVector<float>& v) {
  double sum = 0;
  for (const float x : v) {
    if (!std::isfinite(x)) return false;
    sum += static_cast<double>(x) * static_cast<double>(x);
  }
  if (!(sum > 0)) return false;
  const double scale = 1.0 / std::sqrt(sum);
  for (float& x : v) x = static_cast<float>(static_cast<double>(x) * scale);
  return true;
}

}
