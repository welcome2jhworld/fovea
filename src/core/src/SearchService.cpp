#include "fovea/core/SearchService.h"
#include "fovea/Clock.h"
#include "fovea/Ids.h"
#include "fovea/core/EmbedClient.h"
#include "fovea/core/IndexScheduler.h"
#include "fovea/core/Store.h"
#include "fovea/core/VectorStore.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QHash>
#include <QThreadPool>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <queue>
#include <vector>

namespace fovea::core {
namespace {

constexpr int kMaxQueryChars = 1000;
constexpr int kOpenVectorFiles = 4;
constexpr int64_t kMaxMinGapMs = 600'000;
const QString kScoring = QStringLiteral("embedding_similarity");
const QString kScoringNote = QStringLiteral("similarity only, not verified");

double num(int64_t v) { return static_cast<double>(v); }

ServiceError invalid(const QString& message) { return {400, QStringLiteral("invalid_search"), message}; }

std::optional<int64_t> optionalTime(const QJsonObject& body, const char* key, QString* problem) {
  const QJsonValue v = body.value(QLatin1String(key));
  if (v.isUndefined() || v.isNull()) return std::nullopt;
  if (!v.isDouble() || v.toDouble() < 0 || v.toDouble() != std::floor(v.toDouble())) {
    *problem = QStringLiteral("%1 must be a non-negative integer").arg(QLatin1String(key));
    return std::nullopt;
  }
  return static_cast<int64_t>(v.toDouble());
}

QJsonObject coverageJson(const CoverageCount& c) {
  return QJsonObject{{"frames_expected", num(c.expected)}, {"frames_indexed", num(c.indexed)}, {"coverage_ratio", std::round(c.ratio() * 1e4) / 1e4}};
}

}

ScanResult scanIndex(const ScanRequest& request) {
  ScanResult result;
  const int64_t started = monoNowNs();
  const int dims = request.version.dims;
  if (request.query.size() != dims) {
    result.error = QStringLiteral("query vector has %1 dims, the version %2").arg(request.query.size()).arg(dims);
    return result;
  }
  Store store;
  if (!store.open(request.databasePath)) {
    result.error = QStringLiteral("cannot open the database: ") + store.lastError();
    return result;
  }
  using Scored = std::pair<float, int64_t>;
  std::priority_queue<Scored, std::vector<Scored>, std::greater<>> best;
  const size_t keep = static_cast<size_t>(std::max(1, request.candidates));
  const float* query = request.query.constData();
  // Rows arrive camera by camera in time order, so at most a handful of files
  // are in play at once; the reader holds a read-only mapping, never a copy.
  // Rows arrive camera by camera in time order; a camera's day can hold a
  // sealed and a fresh file, so a few readers stay open.
  QList<QString> open;
  QHash<QString, std::shared_ptr<VectorStore::Reader>> readers;
  const auto readerFor = [&](const QString& file) -> const VectorStore::Reader* {
    const auto known = readers.constFind(file);
    if (known != readers.constEnd()) return known->get();
    auto reader = std::make_shared<VectorStore::Reader>();
    if (!reader->open(request.indexRoot + QLatin1Char('/') + file, dims)) reader.reset();
    readers.insert(file, reader);
    open.push_back(file);
    if (open.size() > kOpenVectorFiles) readers.remove(open.takeFirst());
    return reader.get();
  };
  const bool scanned = store.forEachSearchRow(
      request.version.hash, request.cameraIds, request.fromUtcMs, request.toUtcMs,
      [&](const QString& file, int64_t id, int64_t, int64_t offset) {
        const VectorStore::Reader* reader = readerFor(file);
        if (!reader) {
          ++result.unreadable;
          return;
        }
        const float* v = reader->vector(offset, id);
        if (!v) {
          ++result.unreadable;
          if (result.unreadableIds.size() < kMaxUnreadableReported) result.unreadableIds.push_back(id);
          return;
        }
        float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        int d = 0;
        for (; d + 3 < dims; d += 4) {
          s0 += query[d] * v[d];
          s1 += query[d + 1] * v[d + 1];
          s2 += query[d + 2] * v[d + 2];
          s3 += query[d + 3] * v[d + 3];
        }
        float score = s0 + s1 + s2 + s3;
        for (; d < dims; ++d) score += query[d] * v[d];
        ++result.scanned;
        if (best.size() < keep) best.emplace(score, id);
        else if (score > best.top().first) {
          best.pop();
          best.emplace(score, id);
        }
      });
  if (!scanned) {
    result.error = QStringLiteral("cannot read the index: ") + store.lastError();
    return result;
  }
  const size_t share = static_cast<size_t>(std::ceil(static_cast<double>(result.scanned) * request.candidateFraction));
  while (best.size() > std::max<size_t>(1, share)) best.pop();
  QVector<Scored> ranked(static_cast<qsizetype>(best.size()));
  for (qsizetype k = ranked.size() - 1; k >= 0; --k) {
    ranked[k] = best.top();
    best.pop();
  }
  QVector<int64_t> ids;
  ids.reserve(ranked.size());
  for (const Scored& candidate : std::as_const(ranked)) ids.push_back(candidate.second);
  QHash<int64_t, VectorRow> byId;
  for (const VectorRow& row : store.embeddingRowsByIds(ids)) byId.insert(row.id, row);
  result.samples.reserve(ranked.size());
  for (const auto& [score, id] : std::as_const(ranked)) {
    const auto row = byId.constFind(id);
    if (row == byId.constEnd()) continue;
    result.samples.push_back(SearchSample{id, row->cameraId, row->segmentId, row->utcMs, score});
  }
  result.coverage = store.indexCoverage(request.version, request.cameraIds, request.fromUtcMs, request.toUtcMs);
  store.close();
  result.scanMs = (monoNowNs() - started) / 1'000'000;
  return result;
}

SearchService::SearchService(Store& store, EmbedClient& embed, IndexScheduler& index, QObject* parent)
    : QObject(parent), store_(store), embed_(embed), index_(index) {}

QVector<SearchRange> SearchService::revalidate(const QVector<SearchRange>& ranges, int64_t sampleIntervalMs) {
  QVector<int64_t> ids;
  for (const SearchRange& r : ranges)
    for (const SearchSample& s : r.samples) ids.push_back(s.recordId);
  const QSet<int64_t> live = store_.liveEmbeddingIds(ids);
  QVector<SearchRange> out;
  for (const SearchRange& original : ranges) {
    SearchRange r = original;
    r.samples.clear();
    for (const SearchSample& s : original.samples)
      if (live.contains(s.recordId)) r.samples.push_back(s);
    if (r.samples.isEmpty()) continue;
    const bool lostSamples = r.samples.size() != original.samples.size();
    r.startUtcMs = std::numeric_limits<int64_t>::max();
    r.endUtcMs = 0;
    r.representative = r.samples.first();
    for (const SearchSample& s : std::as_const(r.samples)) {
      r.startUtcMs = std::min(r.startUtcMs, s.utcMs);
      r.endUtcMs = std::max(r.endUtcMs, s.utcMs + sampleIntervalMs);
      if (s.score > r.representative.score) r.representative = s;
    }
    r.relevance = r.representative.score;
    bool deletedFootage = lostSamples;
    int64_t footageEnd = 0;
    // Sample instants lie in [segment start, segment end), so a segment ending
    // exactly at the first instant of the range holds none of it.
    for (const RecordingSegment& seg : store_.segmentsOverlapping(r.cameraId, original.startUtcMs + 1, original.endUtcMs - 1)) {
      if (seg.state == QLatin1String("deleted")) deletedFootage = true;
      else footageEnd = std::max(footageEnd, seg.endUtcMs);
    }
    if (footageEnd > r.startUtcMs) r.endUtcMs = std::min(r.endUtcMs, footageEnd);
    r.evidenceState = deletedFootage ? QStringLiteral("partial") : QStringLiteral("available");
    out.push_back(r);
  }
  return out;
}

void SearchService::search(const QJsonObject& body, Done done) {
  QueryContext context;
  context.startedNs = monoNowNs();
  context.query = body.value("query").toString().trimmed();
  if (context.query.isEmpty()) return done({}, invalid(QStringLiteral("query is required")));
  if (context.query.size() > kMaxQueryChars)
    return done({}, invalid(QStringLiteral("query is longer than %1 characters").arg(kMaxQueryChars)));
  QString problem;
  const std::optional<int64_t> from = optionalTime(body, "from_utc_ms", &problem);
  const std::optional<int64_t> to = optionalTime(body, "to_utc_ms", &problem);
  if (!problem.isEmpty()) return done({}, invalid(problem));
  if (from && to && *to < *from) return done({}, invalid(QStringLiteral("to_utc_ms is before from_utc_ms")));
  const QJsonValue limitValue = body.value("limit");
  context.limit = limitValue.isUndefined() ? kDefaultSearchLimit : limitValue.toInt(-1);
  if (context.limit < 1 || context.limit > kMaxSearchLimit)
    return done({}, invalid(QStringLiteral("limit must be between 1 and %1").arg(kMaxSearchLimit)));
  const QJsonValue gapValue = body.value("min_gap_ms");
  context.minGapMs = gapValue.isUndefined() ? kDefaultMinGapMs : static_cast<int64_t>(gapValue.toDouble(-1));
  if (context.minGapMs < 0 || context.minGapMs > kMaxMinGapMs)
    return done({}, invalid(QStringLiteral("min_gap_ms must be between 0 and %1").arg(kMaxMinGapMs)));
  const QJsonValue fractionValue = body.value("candidate_fraction");
  context.candidateFraction = fractionValue.isUndefined() ? kDefaultCandidateFraction : fractionValue.toDouble(-1);
  if (!(context.candidateFraction > 0 && context.candidateFraction <= 1))
    return done({}, invalid(QStringLiteral("candidate_fraction must be in (0, 1]")));
  if (body.contains("camera_ids")) {
    if (!body.value("camera_ids").isArray()) return done({}, invalid(QStringLiteral("camera_ids must be an array of camera ids")));
    for (const QJsonValue& v : body.value("camera_ids").toArray()) {
      const QString id = v.toString();
      if (!store_.getCamera(id)) return done({}, ServiceError{400, QStringLiteral("unknown_camera"), QStringLiteral("no camera %1").arg(id)});
      if (!context.cameraIds.contains(id)) context.cameraIds.push_back(id);
    }
  }

  const auto [oldest, newest] = store_.footageRange(context.cameraIds);
  context.fromUtcMs = std::max(from.value_or(0), oldest);
  context.toUtcMs = std::min(to.value_or(std::numeric_limits<int64_t>::max()), newest);
  // Nothing of the selected cameras lies in the window: answering at once
  // keeps an empty query off the worker's embed lane.
  context.emptyRange = newest <= 0 || context.fromUtcMs > context.toUtcMs;
  if (context.emptyRange) context.fromUtcMs = context.toUtcMs = std::max<int64_t>(0, newest);

  const QString requested = body.value("index_version").toString();
  context.filters = QJsonObject{{"from_utc_ms", from ? QJsonValue(num(*from)) : QJsonValue(QJsonValue::Null)},
                                {"to_utc_ms", to ? QJsonValue(num(*to)) : QJsonValue(QJsonValue::Null)},
                                {"effective_from_utc_ms", num(context.fromUtcMs)},
                                {"effective_to_utc_ms", num(context.toUtcMs)},
                                {"camera_ids", QJsonArray::fromStringList(context.cameraIds)},
                                {"limit", context.limit},
                                {"min_gap_ms", num(context.minGapMs)},
                                {"candidate_fraction", context.candidateFraction},
                                {"index_version", requested}};

  if (!requested.isEmpty()) {
    std::optional<IndexVersion> version = store_.getIndexVersion(requested);
    if (!version && knownIndexVersionNames().contains(requested)) version = index_.versionNamed(requested);
    if (!version) return done({}, ServiceError{404, QStringLiteral("unknown_index_version"), QStringLiteral("no index version %1").arg(requested)});
    return runQuery(context, *version, done);
  }
  const std::optional<IndexVersion> active = index_.activeVersion();
  std::optional<IndexVersion> previous;
  const QString previousName = index_.previousName();
  if (!previousName.isEmpty() && previousName != index_.activeName()) previous = index_.versionNamed(previousName);
  if (!previous) {
    if (!active) return done({}, ServiceError{409, QStringLiteral("index_not_ready"), QStringLiteral("no index version has been built yet")});
    return runQuery(context, *active, done);
  }
  if (!active) return runQuery(context, *previous, done);
  if (context.emptyRange) return runQuery(context, *active, done);

  // Counting the rows a version covers walks its whole index, so the choice
  // between the active and the previous version runs on a pool thread.
  const QPointer<SearchService> self(this);
  const QString databasePath = store_.path();
  const IndexVersion activeVersion = *active;
  const IndexVersion previousVersion = *previous;
  QThreadPool::globalInstance()->start([self, databasePath, context, activeVersion, previousVersion, done] {
    Store store;
    IndexVersion chosen = activeVersion;
    if (store.open(databasePath)) {
      const double activeRatio = store.indexCoverage(activeVersion, context.cameraIds, context.fromUtcMs, context.toUtcMs).ratio();
      const double previousRatio = store.indexCoverage(previousVersion, context.cameraIds, context.fromUtcMs, context.toUtcMs).ratio();
      if (previousRatio > activeRatio) chosen = previousVersion;
      store.close();
    }
    QMetaObject::invokeMethod(
        qApp,
        [self, context, chosen, done] {
          if (!self) return;
          self->runQuery(context, chosen, done);
        },
        Qt::QueuedConnection);
  });
}

QJsonObject SearchService::buildResponse(const QueryContext& context, const IndexVersion& version, const QJsonArray& results,
                                         const QJsonObject& stats) {
  SearchSessionRecord session;
  session.id = newId();
  session.query = context.query;
  session.filters = context.filters;
  session.indexVersion = version.hash;
  session.model = version.modelId;
  session.createdUtcMs = utcNowMs();
  session.stats = stats;
  session.results = results;
  if (!store_.insertSearchSession(session)) qWarning("search: cannot store session: %s", qPrintable(store_.lastError()));
  return QJsonObject{{"session_id", session.id},
                     {"query", context.query},
                     {"results", results},
                     {"stats", stats},
                     {"filters", context.filters},
                     {"index_version", version.hash},
                     {"index_version_name", version.name},
                     {"model", version.modelId},
                     {"model_revision", version.modelRevision},
                     {"created_utc_ms", num(session.createdUtcMs)},
                     {"scoring", kScoring},
                     {"note", kScoringNote}};
}

void SearchService::startScan(std::function<void()> job) {
  if (runningScans_ >= kMaxRunningScans) {
    queuedScans_.push_back(std::move(job));
    return;
  }
  ++runningScans_;
  job();
}

void SearchService::scanFinished() {
  --runningScans_;
  if (queuedScans_.empty()) return;
  const std::function<void()> next = queuedScans_.front();
  queuedScans_.pop_front();
  ++runningScans_;
  next();
}

void SearchService::runQuery(const QueryContext& context, const IndexVersion& version, const Done& done) {
  store_.appendAudit(QStringLiteral("api"), QStringLiteral("search.query"), QString(),
                     QString::fromUtf8(QJsonDocument(QJsonObject{{"query", context.query}, {"filters", context.filters}, {"index_version", version.hash}})
                                           .toJson(QJsonDocument::Compact)),
                     utcNowMs());
  if (context.emptyRange) {
    QJsonObject stats = coverageJson(CoverageCount{});
    stats.insert("samples_scanned", 0);
    stats.insert("samples_unreadable", 0);
    stats.insert("candidates", 0);
    stats.insert("hours_scanned", 0);
    stats.insert("sample_interval_ms", version.sampleIntervalMs);
    stats.insert("embed_ms", 0);
    stats.insert("scan_ms", 0);
    stats.insert("total_ms", num((monoNowNs() - context.startedNs) / 1'000'000));
    done(buildResponse(context, version, QJsonArray{}, stats), ServiceError{});
    return;
  }
  if (!embed_.workerReady())
    return done({}, ServiceError{503, QStringLiteral("embed_unavailable"), QStringLiteral("model worker is not ready (%1)").arg(embed_.unavailableReason())});
  if (runningScans_ >= kMaxRunningScans && static_cast<int>(queuedScans_.size()) >= kMaxQueuedScans)
    return done({}, ServiceError{503, QStringLiteral("search_busy"),
                                 QStringLiteral("%1 searches are already running").arg(runningScans_ + static_cast<int>(queuedScans_.size()))});

  EmbedRequest request;
  request.kind = QStringLiteral("embed_text");
  request.jobId = newId();
  request.versionName = version.name;
  request.sampleIntervalMs = version.sampleIntervalMs;
  request.text = context.query;
  request.deadlineMs = kQueryDeadlineMs;
  const QPointer<SearchService> self(this);
  const IndexVersion chosen = version;
  embed_.embed(request, [self, done, context, chosen](const EmbedReply& reply) {
    if (!self) return;
    if (!reply.ok) {
      done({}, ServiceError{reply.transient ? 503 : 502, QStringLiteral("embed_failed"), reply.error});
      return;
    }
    IndexVersion version = chosen;
    if (reply.version.hash != chosen.hash) {
      self->index_.noteVersion(reply.version);
      version = reply.version;
    }
    ScanRequest scan;
    scan.databasePath = self->store_.path();
    scan.indexRoot = self->index_.indexDir();
    scan.version = version;
    scan.cameraIds = context.cameraIds;
    scan.fromUtcMs = context.fromUtcMs;
    scan.toUtcMs = context.toUtcMs;
    scan.query = reply.vectors.value(0);
    scan.candidates = context.limit * kCandidatesPerResult;
    scan.candidateFraction = context.candidateFraction;
    const int64_t embedMs = reply.roundTripMs;
    self->startScan([self, scan, done, context, version, embedMs] {
      self->index_.beginScan();
      QThreadPool::globalInstance()->start([self, scan, done, context, version, embedMs] {
        const ScanResult result = scanIndex(scan);
        QMetaObject::invokeMethod(
            qApp,
            [self, result, done, context, version, embedMs] {
              if (!self) return;
              self->index_.endScan();
              self->scanFinished();
              if (!result.error.isEmpty()) {
                done({}, ServiceError{500, QStringLiteral("scan_failed"), result.error});
                return;
              }
              if (!result.unreadableIds.isEmpty()) {
                MarkedEvidence marked;
                const int dropped = self->store_.dropUnreadableRecords(result.unreadableIds, &marked);
                if (dropped > 0) {
                  Store::removeIndexFiles(marked.indexThumbnails, self->index_.indexDir());
                  const int requeued = self->store_.requeueJobsMissingRecords(utcNowMs());
                  qWarning("search: %d records were not at their offset; dropped them and requeued %d jobs", dropped, requeued);
                }
              }
              QVector<SearchRange> ranges = mergeSamples(result.samples, context.minGapMs, version.sampleIntervalMs);
              ranges = self->revalidate(ranges, version.sampleIntervalMs);
              std::stable_sort(ranges.begin(), ranges.end(), [](const SearchRange& a, const SearchRange& b) {
                return a.relevance != b.relevance ? a.relevance > b.relevance : a.startUtcMs < b.startUtcMs;
              });
              if (ranges.size() > context.limit) ranges.resize(context.limit);
              QJsonArray results;
              for (const SearchRange& r : std::as_const(ranges)) results.push_back(r.toJson());
              QJsonObject stats = coverageJson(result.coverage);
              stats.insert("samples_scanned", num(result.scanned));
              stats.insert("samples_unreadable", num(result.unreadable));
              stats.insert("candidates", static_cast<int>(result.samples.size()));
              stats.insert("hours_scanned",
                           std::round(static_cast<double>(result.scanned) * version.sampleIntervalMs / 3.6e6 * 1e4) / 1e4);
              stats.insert("sample_interval_ms", version.sampleIntervalMs);
              stats.insert("embed_ms", num(embedMs));
              stats.insert("scan_ms", num(result.scanMs));
              stats.insert("total_ms", num((monoNowNs() - context.startedNs) / 1'000'000));
              done(self->buildResponse(context, version, results, stats), ServiceError{});
            },
            Qt::QueuedConnection);
      });
    });
  });
}

std::optional<QJsonObject> SearchService::session(const QString& id) {
  const std::optional<SearchSessionRecord> s = store_.getSearchSession(id);
  if (!s) return std::nullopt;
  const std::optional<IndexVersion> version = store_.getIndexVersion(s->indexVersion);
  const int interval = s->stats.value("sample_interval_ms").toInt(version ? version->sampleIntervalMs : kDefaultSampleIntervalMs);
  QVector<SearchRange> ranges;
  for (const QJsonValue& v : s->results) ranges.push_back(SearchRange::fromJson(v.toObject()));
  QJsonArray results;
  for (const SearchRange& r : revalidate(ranges, interval)) results.push_back(r.toJson());
  return QJsonObject{{"session_id", s->id},
                     {"query", s->query},
                     {"results", results},
                     {"stats", s->stats},
                     {"filters", s->filters},
                     {"index_version", s->indexVersion},
                     {"index_version_name", version ? version->name : QString()},
                     {"model", s->model},
                     {"created_utc_ms", num(s->createdUtcMs)},
                     {"scoring", kScoring},
                     {"note", kScoringNote}};
}

std::optional<QByteArray> SearchService::thumbnail(int64_t recordId, ServiceError* error) {
  const std::optional<EmbeddingRecord> record = store_.getEmbeddingRecord(recordId);
  if (!record) {
    *error = {404, QStringLiteral("not_found"), QStringLiteral("no such record")};
    return std::nullopt;
  }
  if (record->deleted) {
    *error = {404, QStringLiteral("record_deleted"), QStringLiteral("the footage of that record was deleted")};
    return std::nullopt;
  }
  QFile f(record->thumbnailPath);
  if (record->thumbnailPath.isEmpty() || !f.open(QIODevice::ReadOnly)) {
    *error = {404, QStringLiteral("not_found"), QStringLiteral("thumbnail file is missing")};
    return std::nullopt;
  }
  return f.readAll();
}

}
