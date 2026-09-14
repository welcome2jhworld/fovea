#include "fovea/core/IndexScheduler.h"
#include "fovea/Clock.h"
#include "fovea/Ids.h"
#include "fovea/core/EmbedClient.h"
#include "fovea/core/Store.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace fovea::core {
namespace {

constexpr int kTickMs = 250;
constexpr int kMaintainMs = 5000;
constexpr int64_t kQueueIntervalNs = 5'000'000'000;
constexpr int64_t kCompactionIntervalNs = 60'000'000'000;
constexpr int64_t kMinCompactRecords = 16;
constexpr int64_t kRetryBaseMs = 10'000;
constexpr int64_t kRetryMaxMs = 600'000;
constexpr int kRecentFailures = 5;
const QString kProbeText = QStringLiteral("index version probe");

double num(int64_t v) { return static_cast<double>(v); }

int64_t retryDelayMs(int attempts) {
  double delay = static_cast<double>(kRetryBaseMs);
  for (int i = 1; i < attempts; ++i) delay *= 3.0;
  return std::min<int64_t>(kRetryMaxMs, static_cast<int64_t>(delay));
}

QString settingText(Store& store, const char* key, const QString& fallback) {
  const std::optional<QString> raw = store.getSetting(QLatin1String(key));
  if (!raw) return fallback;
  const QJsonValue v = QJsonDocument::fromJson(QByteArray("[") + raw->toUtf8() + "]").array().at(0);
  return v.isString() && !v.toString().isEmpty() ? v.toString() : fallback;
}

QString jsonString(const QString& value) {
  const QByteArray array = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
  return QString::fromUtf8(array.mid(1, array.size() - 2));
}

int64_t directoryBytes(const QString& dir, const QStringList& filters) {
  int64_t total = 0;
  QDirIterator it(dir, filters, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) total += it.nextFileInfo().size();
  return total;
}

}

QStringList knownIndexVersionNames() { return {QStringLiteral("siglip2-b16-224"), QStringLiteral("qwen3vl-emb-2b-1024")}; }

struct IndexScheduler::Running {
  IndexJob job;
  IndexVersion version;
  RecordingSegment segment;
  std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
  QVector<SampledFrame> frames;
  QString spool;
  QString thumbnails;
  int next = 0;
  int inserted = 0;
  int64_t computeMs = 0;
  bool sampling = true;
};

IndexScheduler::IndexScheduler(Store& store, EmbedClient& embed, QString dataDir, QObject* parent)
    : QObject(parent), store_(store), embed_(embed), dataDir_(std::move(dataDir)), indexDir_(dataDir_ + QStringLiteral("/index")),
      vectors_(indexDir_), workerBackoff_(1000, 60'000, 2.0) {
  samplers_.setMaxThreadCount(1);
  tickTimer_.setInterval(kTickMs);
  maintainTimer_.setInterval(kMaintainMs);
  connect(&tickTimer_, &QTimer::timeout, this, &IndexScheduler::tick);
  connect(&maintainTimer_, &QTimer::timeout, this, &IndexScheduler::maintain);
}

IndexScheduler::~IndexScheduler() {
  if (running_) running_->cancel->store(true);
  samplers_.waitForDone();
}

QString IndexScheduler::spoolDir() const { return dataDir_ + QStringLiteral("/spool/index"); }

void IndexScheduler::start() {
  QDir(spoolDir()).removeRecursively();
  QDir().mkpath(indexDir_);
  MarkedEvidence stale;
  const int requeued = store_.recoverIndexJobs(utcNowMs(), kIndexMaxAttempts, &stale);
  Store::removeIndexFiles(stale.indexThumbnails, indexDir_);
  if (requeued != 0 || stale.embeddings > 0)
    qInfo("index: %d interrupted jobs requeued, %d rows of unfinished jobs deleted", requeued, stale.embeddings);
  for (const QString& hash : store_.deletedIndexVersions())
    if (!vectors_.removeVersion(hash)) qWarning("index: cannot remove the files of deleted version %s", qPrintable(hash));
  checkVectorFiles();
  const int repaired = store_.requeueJobsMissingRecords(utcNowMs());
  if (repaired > 0) qInfo("index: %d segments whose records were repaired are indexed again", repaired);
  started_ = true;
  tickTimer_.start();
  maintainTimer_.start();
  qInfo("index: active version %s every %d ms", qPrintable(activeName()), sampleIntervalMs());
}

void IndexScheduler::stop() {
  started_ = false;
  tickTimer_.stop();
  maintainTimer_.stop();
  if (!running_) return;
  running_->cancel->store(true);
  samplers_.waitForDone();
  endJob(QStringLiteral("queued"), QStringLiteral("core_stopped"), false);
}

void IndexScheduler::checkVectorFiles() {
  MarkedEvidence marked;
  int truncated = 0, dropped = 0, removed = 0;
  const QStringList onDisk = vectors_.listFiles();
  for (const QString& file : onDisk) {
    const std::optional<IndexVersion> version = store_.getIndexVersion(VectorStore::versionOf(file));
    const VectorStore::Check check = vectors_.repair(file);
    if (!version || !check.valid || check.dims != version->dims) {
      qWarning("index: vector file %s unusable (%s)", qPrintable(file),
               qPrintable(!version ? QStringLiteral("unknown index version") : !check.valid ? check.error : QStringLiteral("dims mismatch")));
      dropped += std::max(0, store_.dropRecordsPastEnd(file, -1, 1, &marked));
      if (vectors_.remove(file)) ++removed;
      continue;
    }
    if (check.truncatedBytes > 0) ++truncated;
    dropped += std::max(0, store_.dropRecordsPastEnd(
                               file, VectorStore::kHeaderBytes + check.records * VectorStore::recordBytes(check.dims), check.dims, &marked));
  }
  const QSet<QString> present(onDisk.begin(), onDisk.end());
  for (const QString& file : store_.referencedVectorFiles())
    if (!present.contains(file)) dropped += std::max(0, store_.dropRecordsPastEnd(file, -1, 1, &marked));
  const QStringList referenced = store_.referencedVectorFiles();
  const QSet<QString> used(referenced.begin(), referenced.end());
  for (const QString& file : vectors_.listFiles())
    if (!used.contains(file) && vectors_.remove(file)) ++removed;
  Store::removeIndexFiles(marked.indexThumbnails, indexDir_);
  if (truncated + dropped + removed > 0)
    qInfo("index: startup check truncated %d partial records, deleted %d rows past their file end, removed %d files", truncated, dropped,
          removed);
}

QString IndexScheduler::activeName() { return settingText(store_, kActiveIndexVersionKey, QString::fromLatin1(kDefaultIndexVersionName)); }

QString IndexScheduler::previousName() { return settingText(store_, kPreviousIndexVersionKey, QString()); }

int IndexScheduler::sampleIntervalMs() {
  const std::optional<QString> raw = store_.getSetting(QLatin1String(kSampleIntervalKey));
  const int value = raw ? raw->toInt() : kDefaultSampleIntervalMs;
  return std::clamp(value > 0 ? value : kDefaultSampleIntervalMs, kMinSampleIntervalMs, kMaxSampleIntervalMs);
}

std::optional<IndexVersion> IndexScheduler::versionNamed(const QString& name) {
  const int interval = sampleIntervalMs();
  if (const auto it = resolved_.constFind(name); it != resolved_.constEnd() && it->sampleIntervalMs == interval) return *it;
  return store_.findIndexVersion(name, interval);
}

std::optional<IndexVersion> IndexScheduler::activeVersion() { return versionNamed(activeName()); }

void IndexScheduler::noteVersion(const IndexVersion& reported) {
  IndexVersion v = reported;
  const std::optional<IndexVersion> stored = store_.getIndexVersion(v.hash);
  v.createdUtcMs = stored ? stored->createdUtcMs : utcNowMs();
  if (!store_.upsertIndexVersion(v)) {
    qWarning("index: cannot store version %s: %s", qPrintable(v.hash), qPrintable(store_.lastError()));
    return;
  }
  if (v.sampleIntervalMs != sampleIntervalMs()) return;
  const auto previous = resolved_.constFind(v.name);
  const bool changed = previous == resolved_.constEnd() || previous->hash != v.hash;
  resolved_.insert(v.name, v);
  if (!changed) return;
  qInfo("index: %s is version %s (%s@%s, %d dims)", qPrintable(v.name), qPrintable(v.hash), qPrintable(v.modelId),
        qPrintable(v.modelRevision.left(12)), v.dims);
  if (v.name == activeName()) {
    const int queued = store_.queueIndexJobs(v, utcNowMs());
    lastQueueMonoNs_ = monoNowNs();
    if (queued > 0) qInfo("index: %d segments queued for %s", queued, qPrintable(v.hash));
  }
}

void IndexScheduler::describeActive() {
  if (!describing_.isEmpty()) return;
  const QString name = activeName();
  const int interval = sampleIntervalMs();
  describing_ = name;
  EmbedRequest request;
  request.kind = QStringLiteral("embed_text");
  request.jobId = newId();
  request.versionName = name;
  request.sampleIntervalMs = interval;
  request.text = kProbeText;
  request.deadlineMs = kIndexDeadlineMs;
  const QPointer<IndexScheduler> self(this);
  embed_.embed(request, [self, name, interval](const EmbedReply& reply) {
    if (!self) return;
    self->describing_.clear();
    if (!reply.ok) {
      self->lastError_ = QStringLiteral("describe %1: %2").arg(name, reply.error);
      const int64_t delayMs = self->workerBackoff_.nextDelayMs();
      self->pausedUntilMonoNs_ = monoNowNs() + delayMs * 1'000'000;
      qWarning("index: cannot describe %s: %s; retry in %lld ms", qPrintable(name), qPrintable(reply.error),
               static_cast<long long>(delayMs));
      return;
    }
    self->workerBackoff_.reset();
    if (name != self->activeName() || interval != self->sampleIntervalMs()) return;
    self->lastError_.clear();
    self->noteVersion(reply.version);
  });
}

void IndexScheduler::tick() {
  if (!started_ || running_ || !embed_.workerReady() || monoNowNs() < pausedUntilMonoNs_) return;
  const QString name = activeName();
  const auto resolved = resolved_.constFind(name);
  if (resolved == resolved_.constEnd() || resolved->sampleIntervalMs != sampleIntervalMs()) {
    describeActive();
    return;
  }
  const IndexVersion version = *resolved;
  const int64_t now = utcNowMs();
  if (monoNowNs() - lastQueueMonoNs_ > kQueueIntervalNs) {
    lastQueueMonoNs_ = monoNowNs();
    if (const int queued = store_.queueIndexJobs(version, now); queued > 0)
      qInfo("index: %d segments queued for %s", queued, qPrintable(version.hash));
  }
  const std::optional<IndexJob> job = store_.nextIndexJob(version.hash, now, kIndexMaxAttempts);
  if (job) startJob(*job);
}

void IndexScheduler::startJob(const IndexJob& queued) {
  MarkedEvidence stale;
  const std::optional<IndexJob> job = store_.beginIndexJob(queued.id, utcNowMs(), &stale);
  Store::removeIndexFiles(stale.indexThumbnails, indexDir_);
  if (!job) {
    qWarning("index: cannot start job %lld: %s", static_cast<long long>(queued.id), qPrintable(store_.lastError()));
    return;
  }
  running_ = std::make_unique<Running>();
  running_->job = *job;
  running_->version = resolved_.value(activeName());
  const std::optional<RecordingSegment> segment = store_.getSegment(job->segmentId);
  if (!segment || segment->state != QLatin1String("finalized")) {
    running_->sampling = false;
    endJob(QStringLiteral("skipped"), QStringLiteral("segment_deleted"), false);
    return;
  }
  running_->segment = *segment;
  running_->spool = QStringLiteral("%1/%2").arg(spoolDir()).arg(job->id);
  running_->thumbnails = QStringLiteral("%1/%2/thumbs/%3/%4").arg(indexDir_, running_->version.hash, segment->cameraId, segment->id);
  QDir(running_->thumbnails).removeRecursively();
  QDir(running_->spool).removeRecursively();

  SampleRequest request;
  request.segmentPath = segment->path;
  request.startUtcMs = segment->startUtcMs;
  request.endUtcMs = segment->endUtcMs;
  request.startPtsNs = segment->startPtsNs;
  request.intervalMs = job->sampleIntervalMs;
  request.spoolDir = running_->spool;
  request.thumbnailDir = running_->thumbnails;
  request.thumbnailPrefix = QStringLiteral("g%1").arg(job->generation);
  const std::shared_ptr<std::atomic<bool>> cancel = running_->cancel;
  const int64_t jobId = job->id;
  samplers_.start([this, request, cancel, jobId] {
    const SampleResult result = sampleSegment(request, cancel.get());
    QMetaObject::invokeMethod(this, [this, jobId, result] { onSampled(jobId, result); }, Qt::QueuedConnection);
  });
}

void IndexScheduler::onSampled(int64_t jobId, const SampleResult& result) {
  if (!running_ || running_->job.id != jobId || !running_->sampling) return;
  Running& r = *running_;
  r.sampling = false;
  r.computeMs += result.elapsedMs;
  r.job.framesExpected = static_cast<int>(result.expected);
  if (!result.error.isEmpty()) {
    const std::optional<RecordingSegment> segment = store_.getSegment(r.job.segmentId);
    if (!segment || segment->state != QLatin1String("finalized")) endJob(QStringLiteral("skipped"), QStringLiteral("segment_deleted"), false);
    else endJob(QStringLiteral("failed"), QStringLiteral("sample: ") + result.error, false);
    return;
  }
  r.frames = result.frames;
  if (r.frames.isEmpty() && result.expected > 0) {
    endJob(QStringLiteral("failed"), QStringLiteral("sample: no frame at any sample instant"), false);
    return;
  }
  postBatch();
}

void IndexScheduler::postBatch() {
  Running& r = *running_;
  if (r.next >= r.frames.size()) {
    endJob(QStringLiteral("done"), QString(), false);
    return;
  }
  const int first = r.next;
  const int count = std::min<int>(embed_.indexBatchFrames(), static_cast<int>(r.frames.size()) - first);
  EmbedRequest request;
  request.kind = QStringLiteral("embed_frames");
  request.jobId = newId();
  request.versionName = r.version.name;
  request.sampleIntervalMs = r.job.sampleIntervalMs;
  request.generation = r.job.generation;
  request.cameraId = r.segment.cameraId;
  request.sessionId = r.segment.sessionId;
  request.deadlineMs = kIndexDeadlineMs;
  for (int i = first; i < first + count; ++i) {
    const SampledFrame& f = r.frames[i];
    request.frames.push_back({QStringLiteral("%1-%2").arg(r.job.id).arg(f.index), f.ptsNs, f.utcMs, f.jpegPath});
  }
  r.next += count;
  const QPointer<IndexScheduler> self(this);
  const int64_t jobId = r.job.id;
  embed_.embed(request, [self, jobId, first, count](const EmbedReply& reply) {
    if (self) self->onBatch(jobId, first, count, reply);
  });
}

void IndexScheduler::onBatch(int64_t jobId, int first, int count, const EmbedReply& reply) {
  if (!running_ || running_->job.id != jobId) return;
  Running& r = *running_;
  r.computeMs += reply.roundTripMs;
  if (!reply.ok) {
    endJob(reply.transient ? QStringLiteral("queued") : QStringLiteral("failed"), QStringLiteral("embed: ") + reply.error, reply.transient);
    return;
  }
  if (reply.version.hash != r.version.hash) {
    noteVersion(reply.version);
    endJob(QStringLiteral("skipped"), QStringLiteral("index_version_changed"), false);
    return;
  }
  QVector<EmbeddingRecord> records;
  records.reserve(count);
  for (int i = first; i < first + count; ++i) {
    EmbeddingRecord rec;
    rec.sessionId = r.segment.sessionId;
    rec.ptsNs = r.frames[i].ptsNs;
    rec.utcMs = r.frames[i].utcMs;
    rec.thumbnailPath = r.frames[i].thumbnailPath;
    records.push_back(rec);
  }
  QString writeError;
  const int dims = r.version.dims;
  const bool stored = store_.appendEmbeddings(r.job, records, [&](QVector<EmbeddingRecord>& rows) {
    QHash<QString, QVector<int>> byFile;
    QStringList order;
    for (int i = 0; i < rows.size(); ++i) {
      const QString file = vectors_.appendFile(r.version.hash, r.segment.cameraId, rows[i].utcMs);
      if (!byFile.contains(file)) order.push_back(file);
      byFile[file].push_back(i);
    }
    for (const QString& file : order) {
      QVector<VectorStore::Record> batch;
      for (const int i : byFile[file]) batch.push_back({rows[i].id, rows[i].utcMs, reply.vectors[i]});
      QVector<int64_t> offsets;
      if (!vectors_.append(file, dims, batch, &offsets, &writeError)) return false;
      const QVector<int>& indices = byFile[file];
      for (qsizetype k = 0; k < indices.size(); ++k) {
        rows[indices[k]].vectorFile = file;
        rows[indices[k]].vectorOffset = offsets[k];
      }
    }
    return true;
  });
  for (int i = first; i < first + count; ++i) QFile::remove(r.frames[i].jpegPath);
  if (!stored) {
    const std::optional<RecordingSegment> segment = store_.getSegment(r.job.segmentId);
    if (!segment || segment->state != QLatin1String("finalized")) endJob(QStringLiteral("skipped"), QStringLiteral("segment_deleted"), false);
    else endJob(QStringLiteral("failed"), QStringLiteral("store: ") + (writeError.isEmpty() ? store_.lastError() : writeError), false);
    return;
  }
  r.inserted += count;
  postBatch();
}

void IndexScheduler::endJob(const QString& state, const QString& reason, bool transient) {
  std::unique_ptr<Running> r = std::move(running_);
  IndexJob job = r->job;
  const int64_t now = utcNowMs();
  job.computeMs = r->computeMs;
  job.footageMs = std::max<int64_t>(0, r->segment.endUtcMs - r->segment.startUtcMs);
  if (state == QLatin1String("failed")) {
    ++job.attempts;
    job.nextAttemptUtcMs = now + retryDelayMs(job.attempts);
  } else {
    job.nextAttemptUtcMs = 0;
  }
  MarkedEvidence stale;
  if (!store_.finishIndexJob(job, state, reason, now, &stale))
    qWarning("index: cannot finish job %lld: %s", static_cast<long long>(job.id), qPrintable(store_.lastError()));
  Store::removeIndexFiles(stale.indexThumbnails, indexDir_);
  if (state != QLatin1String("done") && !r->thumbnails.isEmpty()) QDir(r->thumbnails).removeRecursively();
  if (!r->spool.isEmpty()) QDir(r->spool).removeRecursively();
  if (transient) {
    const int64_t delayMs = workerBackoff_.nextDelayMs();
    pausedUntilMonoNs_ = monoNowNs() + delayMs * 1'000'000;
    lastError_ = reason;
    qWarning("index: job %lld requeued (%s); indexing paused %lld ms", static_cast<long long>(job.id), qPrintable(reason),
             static_cast<long long>(delayMs));
  } else if (state == QLatin1String("done")) {
    workerBackoff_.reset();
    lastError_.clear();
    qInfo("index: segment %s indexed, %d of %d samples, compute %lld ms for %lld ms of footage", qPrintable(job.segmentId), r->inserted,
          job.framesExpected, static_cast<long long>(job.computeMs), static_cast<long long>(job.footageMs));
  } else {
    if (state == QLatin1String("failed")) lastError_ = reason;
    qWarning("index: job %lld %s: %s", static_cast<long long>(job.id), qPrintable(state), qPrintable(reason));
  }
  if (started_) QMetaObject::invokeMethod(this, &IndexScheduler::tick, Qt::QueuedConnection);
}

void IndexScheduler::maintain() {
  removeFilesWhenIdle();
  if (compacting_ || scans_ > 0 || monoNowNs() - lastCompactionMonoNs_ < kCompactionIntervalNs) return;
  lastCompactionMonoNs_ = monoNowNs();
  compactNext();
}

void IndexScheduler::compactNext() {
  for (const VectorFileUsage& usage : store_.vectorFileUsage()) {
    const std::optional<IndexVersion> version = store_.getIndexVersion(usage.indexVersion);
    if (!version || version->dims <= 0) continue;
    const int64_t size = QFileInfo(vectors_.absolutePath(usage.file)).size();
    const int64_t records = std::max<int64_t>(0, (size - VectorStore::kHeaderBytes) / VectorStore::recordBytes(version->dims));
    if (usage.liveRows == 0) {
      if (store_.moveFileRecords(usage.file, QString(), {}, {})) {
        vectors_.seal(usage.file);
        pendingRemovals_.push_back(usage.file);
      }
      continue;
    }
    if (records < kMinCompactRecords || usage.liveRows * 2 >= records) continue;
    vectors_.seal(usage.file);
    const QString target = vectors_.freshName(usage.file);
    const QVector<std::pair<int64_t, int64_t>> moved = store_.liveFileRecords(usage.file);
    QVector<int64_t> offsets;
    offsets.reserve(moved.size());
    for (const auto& [id, offset] : moved) offsets.push_back(offset);
    compacting_ = true;
    const QString root = vectors_.root();
    const QString from = usage.file;
    const int dims = version->dims;
    const QPointer<IndexScheduler> self(this);
    QThreadPool::globalInstance()->start([self, root, from, target, dims, offsets, moved, records] {
      QVector<int64_t> newOffsets;
      QString error;
      const bool copied = VectorStore::copyRecords(root, from, target, dims, offsets, &newOffsets, &error);
      QMetaObject::invokeMethod(
          qApp,
          [self, root, from, target, moved, newOffsets, copied, error, records] {
            if (!self) return;
            self->compacting_ = false;
            if (!copied || !self->store_.moveFileRecords(from, target, moved, newOffsets)) {
              qWarning("index: compaction of %s failed: %s", qPrintable(from), qPrintable(copied ? self->store_.lastError() : error));
              QFile::remove(root + QLatin1Char('/') + target);
              return;
            }
            qInfo("index: compacted %s into %s, %lld of %lld records kept", qPrintable(from), qPrintable(target),
                  static_cast<long long>(moved.size()), static_cast<long long>(records));
            self->pendingRemovals_.push_back(from);
            self->removeFilesWhenIdle();
          },
          Qt::QueuedConnection);
    });
    break;
  }
  removeFilesWhenIdle();
}

void IndexScheduler::endScan() {
  --scans_;
  if (scans_ == 0) removeFilesWhenIdle();
}

void IndexScheduler::removeFilesWhenIdle() {
  if (scans_ > 0) return;
  for (const QString& file : std::as_const(pendingRemovals_))
    if (!vectors_.remove(file)) qWarning("index: cannot remove %s", qPrintable(file));
  pendingRemovals_.clear();
  for (const QString& version : std::as_const(pendingVersionRemovals_))
    if (!vectors_.removeVersion(version)) qWarning("index: cannot remove the files of version %s", qPrintable(version));
  pendingVersionRemovals_.clear();
}

bool IndexScheduler::setActive(const QString& name, ServiceError* error) {
  if (!knownIndexVersionNames().contains(name)) {
    *error = {400, QStringLiteral("unknown_index_version"),
              QStringLiteral("index_version_name must be one of %1").arg(knownIndexVersionNames().join(QStringLiteral(", ")))};
    return false;
  }
  const QString current = activeName();
  if (current != name) {
    if (!store_.setSetting(QLatin1String(kPreviousIndexVersionKey), jsonString(current)) ||
        !store_.setSetting(QLatin1String(kActiveIndexVersionKey), jsonString(name))) {
      *error = {500, QStringLiteral("store_failed"), store_.lastError()};
      return false;
    }
    qInfo("index: active version %s (was %s)", qPrintable(name), qPrintable(current));
  }
  if (const auto it = resolved_.constFind(name); it != resolved_.constEnd() && it->sampleIntervalMs == sampleIntervalMs()) {
    store_.queueIndexJobs(*it, utcNowMs());
    lastQueueMonoNs_ = monoNowNs();
  }
  pausedUntilMonoNs_ = 0;
  QMetaObject::invokeMethod(this, &IndexScheduler::tick, Qt::QueuedConnection);
  return true;
}

bool IndexScheduler::deleteVersion(const QString& hash, ServiceError* error) {
  const std::optional<IndexVersion> version = store_.getIndexVersion(hash);
  if (!version) {
    *error = {404, QStringLiteral("not_found"), QStringLiteral("no such index version")};
    return false;
  }
  const std::optional<IndexVersion> active = activeVersion();
  if (active && active->hash == hash) {
    *error = {409, QStringLiteral("active_index_version"), QStringLiteral("the active index version cannot be deleted")};
    return false;
  }
  if (running_ && running_->version.hash == hash) {
    *error = {409, QStringLiteral("index_version_busy"), QStringLiteral("a job of that version is running")};
    return false;
  }
  // Removing the files under a running scan would leave it reading a mapping
  // of a file that is gone, and a version reactivated meanwhile would lose the
  // records it wrote in between.
  if (scans_ > 0) {
    *error = {409, QStringLiteral("index_version_busy"), QStringLiteral("a search is scanning the index")};
    return false;
  }
  if (!store_.deleteIndexVersion(hash, utcNowMs())) {
    *error = {500, QStringLiteral("store_failed"), store_.lastError()};
    return false;
  }
  for (auto it = resolved_.begin(); it != resolved_.end();) {
    if (it->hash == hash) it = resolved_.erase(it);
    else ++it;
  }
  if (previousName() == version->name && !store_.findIndexVersion(version->name, sampleIntervalMs()))
    store_.setSetting(QLatin1String(kPreviousIndexVersionKey), jsonString(QString()));
  pendingVersionRemovals_.push_back(hash);
  removeFilesWhenIdle();
  qInfo("index: version %s (%s) deleted", qPrintable(hash), qPrintable(version->name));
  return true;
}

QJsonObject IndexScheduler::statusJson(bool withStorage) {
  const QString name = activeName();
  const std::optional<IndexVersion> active = activeVersion();
  const std::optional<IndexVersion> previous = previousName().isEmpty() ? std::nullopt : versionNamed(previousName());
  QString state;
  if (!embed_.workerReady()) state = embed_.unavailableReason() == QLatin1String("worker_unavailable") ? QStringLiteral("disabled")
                                                                                                          : QStringLiteral("waiting_worker");
  else if (running_) state = QStringLiteral("indexing");
  else if (!describing_.isEmpty()) state = QStringLiteral("describing");
  else if (monoNowNs() < pausedUntilMonoNs_) state = QStringLiteral("paused");
  else state = QStringLiteral("idle");

  QJsonArray versions;
  for (const IndexVersion& v : store_.listIndexVersions()) {
    QJsonObject o = v.toJson();
    const IndexQueueStats q = store_.indexQueueStats(v.hash);
    o.insert("active", active && active->hash == v.hash);
    o.insert("previous", previous && previous->hash == v.hash);
    o.insert("queue", QJsonObject{{"queued", q.queued}, {"running", q.running}, {"done", q.done}, {"failed", q.failed}, {"skipped", q.skipped}});
    o.insert("frames_indexed", num(q.framesIndexed));
    o.insert("footage_ms", num(q.footageMs));
    o.insert("compute_ms", num(q.computeMs));
    CoverageCount total;
    for (const CameraCoverage& c : store_.coverageByCamera(v)) {
      total.expected += c.count.expected;
      total.indexed += c.count.indexed;
    }
    o.insert("coverage_ratio", std::round(total.ratio() * 1e4) / 1e4);
    if (withStorage) {
      const auto [rows, payload] = store_.embeddingRowStats(v.hash);
      const QString dir = indexDir_ + QLatin1Char('/') + v.hash;
      o.insert("storage", QJsonObject{{"vector_bytes", num(directoryBytes(dir, {QStringLiteral("*.vec")}))},
                                      {"thumbnail_bytes", num(directoryBytes(dir + QStringLiteral("/thumbs"), {QStringLiteral("*.jpg")}))},
                                      {"rows", num(rows)},
                                      {"row_payload_bytes", num(payload)}});
    }
    versions.push_back(o);
  }

  QJsonObject out{{"active_index_version", name},
                  {"previous_index_version", previousName()},
                  {"index_version", active ? active->hash : QString()},
                  {"model", active ? active->modelId : QString()},
                  {"sample_interval_ms", sampleIntervalMs()},
                  {"state", state},
                  {"last_error", lastError_},
                  {"versions", versions},
                  {"known_index_version_names", QJsonArray::fromStringList(knownIndexVersionNames())}};
  QJsonArray cameras;
  QJsonArray failures;
  if (active) {
    const QVector<Camera> cams = store_.listCameras();
    for (const CameraCoverage& c : store_.coverageByCamera(*active)) {
      const auto cam = std::find_if(cams.begin(), cams.end(), [&c](const Camera& x) { return x.id == c.cameraId; });
      cameras.push_back(QJsonObject{{"camera_id", c.cameraId},
                                    {"index_enabled", cam != cams.end() && cam->indexEnabled},
                                    {"frames_expected", num(c.count.expected)},
                                    {"frames_indexed", num(c.count.indexed)},
                                    {"coverage_ratio", std::round(c.count.ratio() * 1e4) / 1e4}});
    }
    const IndexQueueStats q = store_.indexQueueStats(active->hash);
    out.insert("queue", QJsonObject{{"queued", q.queued}, {"running", q.running}, {"done", q.done}, {"failed", q.failed}, {"skipped", q.skipped}});
    const double computeS = static_cast<double>(q.computeMs) / 1000.0;
    const double footageH = static_cast<double>(q.footageMs) / 3.6e6;
    out.insert("throughput", QJsonObject{{"footage_ms", num(q.footageMs)},
                                         {"compute_ms", num(q.computeMs)},
                                         {"frames_indexed", num(q.framesIndexed)},
                                         {"compute_s_per_footage_hour", footageH > 0 ? std::round(computeS / footageH * 10) / 10 : 0.0},
                                         {"realtime_factor", q.computeMs > 0 ? std::round(static_cast<double>(q.footageMs) / static_cast<double>(q.computeMs) * 100) / 100 : 0.0}});
    for (const IndexJob& j : store_.listIndexJobs(active->hash, QStringLiteral("failed"), kRecentFailures)) failures.push_back(j.toJson());
  } else {
    out.insert("queue", QJsonObject{{"queued", 0}, {"running", 0}, {"done", 0}, {"failed", 0}, {"skipped", 0}});
  }
  out.insert("cameras", cameras);
  out.insert("recent_failures", failures);
  if (running_) {
    QJsonObject job = running_->job.toJson();
    job.insert("stage", running_->sampling ? QStringLiteral("sampling") : QStringLiteral("embedding"));
    job.insert("frames_sampled", static_cast<int>(running_->frames.size()));
    job.insert("frames_stored", running_->inserted);
    out.insert("running_job", job);
  } else {
    out.insert("running_job", QJsonValue::Null);
  }
  return out;
}

}
