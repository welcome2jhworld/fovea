#pragma once
#include "fovea/Backoff.h"
#include "fovea/core/Analytics.h"
#include "fovea/core/Index.h"
#include "fovea/core/SegmentSampler.h"
#include "fovea/core/VectorStore.h"
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <memory>
#include <optional>

namespace fovea::core {

class EmbedClient;
class Store;
struct EmbedReply;

inline constexpr int kIndexMaxAttempts = 5;
// Index versions the worker can describe (docs/M4_DESIGN.md "Index versions").
QStringList knownIndexVersionNames();

// Builds and keeps the embedding index of the active index version on the core
// thread (docs/M4_DESIGN.md "Pipeline"). The worker describes the active
// version name at the configured sample interval (its hash identifies the
// version); every finalized segment of an index-enabled camera then gets a
// job, run one at a time: SegmentSampler on a pool thread, frames posted in
// batches of up to 32 through EmbedClient (which lets queries go first), each
// batch's vectors appended to the VectorStore and its rows inserted in one
// store transaction. A worker outage requeues the job without counting an
// attempt and pauses indexing with backoff (1..60 s); other failures count
// attempts with backoff (10 s, 30 s, 90 s, ... at most 10 min, 5 attempts).
// At start, jobs left running are requeued and rows of unfinished jobs are
// deleted, vector files lose a trailing partial record, rows pointing past
// the end of their file are deleted and files no row references are removed.
// Files more than half dead are compacted while no search scan runs.
class IndexScheduler : public QObject {
  Q_OBJECT
public:
  IndexScheduler(Store& store, EmbedClient& embed, QString dataDir, QObject* parent = nullptr);
  ~IndexScheduler() override;

  void start();
  // Stops starting jobs; a running sampler is cancelled and its job requeued.
  void stop();

  QString activeName();
  QString previousName();
  int sampleIntervalMs();
  // The version the active name resolved to (this run, or the newest stored one).
  std::optional<IndexVersion> activeVersion();
  // The newest stored version of a name at the current sample interval.
  std::optional<IndexVersion> versionNamed(const QString& name);
  // Makes name active (the previous active name keeps answering until the new
  // version covers a query's range) and queues its re-index.
  bool setActive(const QString& name, ServiceError* error);
  bool deleteVersion(const QString& hash, ServiceError* error);
  // A search reply named a version: registered, and made the resolution of its name.
  void noteVersion(const IndexVersion& v);

  VectorStore& vectors() { return vectors_; }
  QString indexDir() const { return indexDir_; }
  // Search scans in flight; files are compacted or removed only while none is.
  void beginScan() { ++scans_; }
  void endScan();

  QJsonObject statusJson(bool withStorage);

private:
  struct Running;

  void tick();
  void maintain();
  void checkVectorFiles();
  void describeActive();
  void startJob(const IndexJob& job);
  void onSampled(int64_t jobId, const SampleResult& result);
  void postBatch();
  void onBatch(int64_t jobId, int first, int count, const EmbedReply& reply);
  void endJob(const QString& state, const QString& reason, bool transient);
  void compactNext();
  void removeFilesWhenIdle();
  QString spoolDir() const;

  Store& store_;
  EmbedClient& embed_;
  QString dataDir_;
  QString indexDir_;
  VectorStore vectors_;
  QTimer tickTimer_;
  QTimer maintainTimer_;
  QThreadPool samplers_;
  std::unique_ptr<Running> running_;
  QHash<QString, IndexVersion> resolved_;
  QString describing_;
  Backoff workerBackoff_;
  int64_t pausedUntilMonoNs_ = 0;
  int64_t lastQueueMonoNs_ = 0;
  int64_t lastCompactionMonoNs_ = 0;
  QString lastError_;
  QStringList pendingRemovals_;
  QStringList pendingVersionRemovals_;
  bool compacting_ = false;
  bool started_ = false;
  int scans_ = 0;
};

}
