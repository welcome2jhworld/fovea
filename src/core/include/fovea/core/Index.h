#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <cstdint>

namespace fovea::core {

inline constexpr int kDefaultSampleIntervalMs = 1000;
inline constexpr int kMinSampleIntervalMs = 200;
inline constexpr int kMaxSampleIntervalMs = 60'000;
inline constexpr int64_t kDefaultMinGapMs = 3000;
inline constexpr const char* kDefaultIndexVersionName = "siglip2-b16-224";
// Settings (value_json is a JSON string or number).
inline constexpr const char* kActiveIndexVersionKey = "search.active_index_version";
inline constexpr const char* kPreviousIndexVersionKey = "search.previous_index_version";
inline constexpr const char* kSampleIntervalKey = "index.sample_interval_ms";

// What a vector means. The worker describes a version name at a sample
// interval and computes hash (the first 12 hex digits of the SHA-256 of its
// canonical descriptor JSON); the core stores the descriptor as reported and
// never compares vectors of two hashes.
struct IndexVersion {
  QString hash;
  QString name;
  QString modelId;
  QString modelRevision;
  int dims = 0;
  QString dtype;
  int sampleIntervalMs = kDefaultSampleIntervalMs;
  QJsonObject descriptor;
  int64_t createdUtcMs = 0;
  int64_t deletedUtcMs = 0;

  QJsonObject toJson() const;
};

struct IndexJob {
  int64_t id = 0;
  QString segmentId;
  QString cameraId;
  QString indexVersion;
  QString state = QStringLiteral("queued");
  QString reason;
  int attempts = 0;
  int64_t generation = 0;
  int sampleIntervalMs = kDefaultSampleIntervalMs;
  int framesExpected = 0;
  int framesIndexed = 0;
  int64_t nextAttemptUtcMs = 0;
  int64_t computeMs = 0;
  int64_t footageMs = 0;
  int64_t createdUtcMs = 0;
  int64_t updatedUtcMs = 0;

  QJsonObject toJson() const;
};

struct EmbeddingRecord {
  int64_t id = 0;
  QString indexVersion;
  QString cameraId;
  QString sessionId;
  QString segmentId;
  int64_t jobId = 0;
  int64_t generation = 0;
  int64_t ptsNs = 0;
  int64_t utcMs = 0;
  QString vectorFile;
  int64_t vectorOffset = -1;
  QString thumbnailPath;
  bool deleted = false;
};

struct ImportRecord {
  QString id;
  QString cameraId;
  QString sessionId;
  QString sourcePath;
  int64_t startUtcMs = 0;
  QString state = QStringLiteral("queued");
  QString error;
  QString codec;
  int64_t durationNs = 0;
  double progress = 0;
  int segments = 0;
  int64_t bytes = 0;
  int64_t createdUtcMs = 0;
  int64_t startedUtcMs = 0;
  int64_t finishedUtcMs = 0;

  QJsonObject toJson() const;
};

struct SearchSessionRecord {
  QString id;
  QString query;
  QJsonObject filters;
  QString indexVersion;
  QString model;
  int64_t createdUtcMs = 0;
  QJsonObject stats;
  QJsonArray results;
};

// A live vector a search may score.
struct VectorRow {
  int64_t id = 0;
  QString cameraId;
  QString segmentId;
  int64_t utcMs = 0;
  QString vectorFile;
  int64_t vectorOffset = 0;
};

struct IndexQueueStats {
  int queued = 0;
  int running = 0;
  int done = 0;
  int failed = 0;
  int skipped = 0;
  int64_t computeMs = 0;
  int64_t footageMs = 0;
  int64_t framesIndexed = 0;
};

// Sampled instants a range should have against those with a live record.
struct CoverageCount {
  int64_t expected = 0;
  int64_t indexed = 0;
  double ratio() const {
    return expected > 0 ? std::min(1.0, static_cast<double>(indexed) / static_cast<double>(expected)) : 0.0;
  }
};

struct CameraCoverage {
  QString cameraId;
  CoverageCount count;
};

// A vector file and how many live rows point into it.
struct VectorFileUsage {
  QString file;
  QString indexVersion;
  int64_t liveRows = 0;
};

struct SearchSample {
  int64_t recordId = 0;
  QString cameraId;
  QString segmentId;
  int64_t utcMs = 0;
  float score = 0;

  QJsonObject toJson() const;
  static SearchSample fromJson(const QJsonObject& o);
};

struct SearchRange {
  QString cameraId;
  int64_t startUtcMs = 0;
  int64_t endUtcMs = 0;
  double relevance = 0;
  SearchSample representative;
  QVector<SearchSample> samples;
  QString evidenceState = QStringLiteral("available");

  QJsonObject toJson() const;
  static SearchRange fromJson(const QJsonObject& o);
};

// Groups the samples of one camera whose neighbours are less than minGapMs
// apart. A range runs from its first sample to its last sample plus one sample
// interval; relevance is the best sample score and representative that
// sample. Ranges come out best first (ties: earlier start first).
QVector<SearchRange> mergeSamples(QVector<SearchSample> samples, int64_t minGapMs, int64_t sampleIntervalMs);

// Multiples of intervalMs in [fromUtcMs, toUtcMs] (both positive).
int64_t gridPoints(int64_t fromUtcMs, int64_t toUtcMs, int64_t intervalMs);
// Sample instants a segment is expected to have: grid points in [startUtcMs, endUtcMs).
int64_t expectedSamples(int64_t startUtcMs, int64_t endUtcMs, int64_t intervalMs);

// Scales v to unit length; false when it has no direction or a non-finite value.
bool normalizeVector(QVector<float>& v);

}
