#pragma once
#include "fovea/core/Analytics.h"
#include "fovea/core/Index.h"
#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <deque>
#include <functional>
#include <optional>

namespace fovea::core {

class EmbedClient;
class IndexScheduler;
class Store;

inline constexpr int kDefaultSearchLimit = 20;
inline constexpr int kMaxSearchLimit = 200;
inline constexpr int kCandidatesPerResult = 8;
// Candidates are also capped at this share of the samples scanned, so a short
// range of footage does not merge into one range per camera.
inline constexpr double kDefaultCandidateFraction = 0.3;
// Scans are bounded: more than this many at once would hold one mapping and
// one candidate heap each while the client waits for only the newest answer.
inline constexpr int kMaxRunningScans = 2;
inline constexpr int kMaxQueuedScans = 4;
// Records whose vector is not where the database says are reported back so the
// core can drop them and index that footage again.
inline constexpr int kMaxUnreadableReported = 4096;

struct ScanRequest {
  QString databasePath;
  QString indexRoot;
  IndexVersion version;
  QStringList cameraIds;
  int64_t fromUtcMs = 0;
  int64_t toUtcMs = 0;
  QVector<float> query;
  int candidates = kDefaultSearchLimit * kCandidatesPerResult;
  double candidateFraction = kDefaultCandidateFraction;
};

struct ScanResult {
  QString error;
  QVector<SearchSample> samples;
  int64_t scanned = 0;
  int64_t unreadable = 0;
  // Ids whose record was not at its offset (at most kMaxUnreadableReported).
  QVector<int64_t> unreadableIds;
  CoverageCount coverage;
  int64_t scanMs = 0;
};

// Scores every live vector of the version in the camera and time filter
// against the unit query vector (dot product = cosine) and keeps the best
// candidates (at most candidateFraction of the samples scanned, at least one),
// best first. Opens its own database connection, so it runs on any thread.
ScanResult scanIndex(const ScanRequest& request);

// The query path of docs/M4_DESIGN.md: validate the filters, embed the text
// with the chosen index version (the active one, or the previous one while it
// covers more of the range), scan on a pool thread, merge the best samples
// into ranges, drop deleted samples, mark ranges over deleted footage partial,
// store the SearchSession and audit the query.
class SearchService : public QObject {
  Q_OBJECT
public:
  using Done = std::function<void(const QJsonObject& response, const ServiceError& error)>;

  SearchService(Store& store, EmbedClient& embed, IndexScheduler& index, QObject* parent = nullptr);

  // What a query needs once its filters are validated.
  struct QueryContext {
    QString query;
    QJsonObject filters;
    QStringList cameraIds;
    int64_t fromUtcMs = 0;
    int64_t toUtcMs = 0;
    bool emptyRange = false;
    int limit = kDefaultSearchLimit;
    int64_t minGapMs = kDefaultMinGapMs;
    double candidateFraction = kDefaultCandidateFraction;
    int64_t startedNs = 0;
  };

  // done runs on the core thread; error.code is empty on success.
  void search(const QJsonObject& body, Done done);
  // A stored session, its results checked against deletions since.
  std::optional<QJsonObject> session(const QString& id);
  std::optional<QByteArray> thumbnail(int64_t recordId, ServiceError* error);

private:
  // Drops samples whose rows were deleted, rebuilds each range from what is
  // left (ending no later than the footage it lies in) and marks ranges over
  // deleted footage partial.
  QVector<SearchRange> revalidate(const QVector<SearchRange>& ranges, int64_t sampleIntervalMs);
  // Audits the query, embeds it and scans; answers at once when the filter
  // leaves no footage.
  void runQuery(const QueryContext& context, const IndexVersion& version, const Done& done);
  QJsonObject buildResponse(const QueryContext& context, const IndexVersion& version, const QJsonArray& results,
                            const QJsonObject& stats);
  // Runs job now or queues it behind the scans already running.
  void startScan(std::function<void()> job);
  void scanFinished();

  Store& store_;
  EmbedClient& embed_;
  IndexScheduler& index_;
  int runningScans_ = 0;
  std::deque<std::function<void()>> queuedScans_;
};

}
