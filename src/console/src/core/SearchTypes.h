#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <cstdint>
#include <optional>

// Console views of the M4 search and index payloads (docs/M4_DESIGN.md "Query path").
namespace fovea::ui {

struct SearchResultInfo {
  QString cameraId;
  int64_t startUtcMs = 0;
  int64_t endUtcMs = 0;
  // Ranking score only; never a probability.
  double relevance = 0;
  // Representative sample: thumbnail at GET /v1/search/thumbnails/{recordId}.
  QString recordId;
  int64_t representativeUtcMs = 0;
  int samples = 0;
  QString evidenceState;

  static SearchResultInfo fromJson(const QJsonObject& o);
};

struct SearchStatsInfo {
  int64_t samplesScanned = 0;
  double hoursScanned = 0;
  // Indexed samples over expected samples in the filtered range; absent when the service did not report it.
  std::optional<double> coverageRatio;
  int64_t embedMs = 0;
  int64_t scanMs = 0;
  int64_t totalMs = 0;

  static SearchStatsInfo fromJson(const QJsonObject& o);
};

struct SearchResponseInfo {
  QString sessionId;
  QVector<SearchResultInfo> results;
  SearchStatsInfo stats;
  // The version hash that answered, and its readable name when the service reports one.
  QString indexVersion;
  QString indexVersionName;
  QString model;

  static SearchResponseInfo fromJson(const QJsonObject& o);
};

struct IndexCameraInfo {
  QString cameraId;
  bool indexEnabled = true;
  std::optional<double> coverageRatio;
  int64_t framesIndexed = 0;
  int64_t framesExpected = 0;
};

// GET /v1/index: active version, scheduler state, indexed share per camera and the job queue.
struct IndexStatusInfo {
  QString indexVersion;
  QString model;
  // idle, indexing, describing, paused, waiting_worker or disabled.
  QString state;
  QString lastError;
  QVector<IndexCameraInfo> cameras;
  int queued = 0;
  int running = 0;
  int failed = 0;

  static IndexStatusInfo fromJson(const QJsonObject& o);
};

}
