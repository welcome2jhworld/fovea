#pragma once
#include "fovea/Api.h"
#include "fovea/core/Analytics.h"
#include "fovea/core/Index.h"
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QVariantMap>
#include <QVector>
#include <functional>
#include <optional>
#include <utility>

namespace fovea::core {

struct SegmentProbe {
  bool readable = false;
  int64_t durationNs = 0;
  int64_t bytes = 0;
};

struct SessionRef {
  QString cameraId;
  QString sessionId;
};

struct RecoveryReport {
  int sessionsClosed = 0;
  QVector<SessionRef> closedSessions;
  int gapsClosed = 0;
  int segmentsFinalized = 0;
  int segmentsDamaged = 0;
  int segmentsMissing = 0;
  int filesAdopted = 0;
  int filesQuarantined = 0;
  int filesPurged = 0;
};

enum class SegmentDeletion { Deleted, Held, NotDeletable, Failed };

// Evidence refs and embedding records a deletion marked deleted, and the
// thumbnail files they no longer reference (the caller removes them once the
// transaction committed).
struct MarkedEvidence {
  int refs = 0;
  QStringList thumbnails;
  int embeddings = 0;
  QStringList indexThumbnails;
};

struct AuditEntry {
  QString actor;
  QString action;
  QString target;
  QString detail;
  int64_t utcMs = 0;
};

struct CameraStorage {
  QString cameraId;
  int64_t bytes = 0;
  int64_t oldestUtcMs = 0;
  int segments = 0;
};

// Keyset position for paging retention candidates oldest first.
struct SegmentCursor {
  int64_t startUtcMs = -1;
  QString id;
};

class Store {
public:
  Store();
  ~Store();
  bool open(const QString& path);
  void close();
  QString path() const { return path_; }
  bool isOpen() const;
  QString lastError() const { return lastError_; }

  // Resolves what a crash left open: segments still "recording" are probed,
  // recording files without a row (the crash hit between splitmuxsink
  // opening the file and the row insert) are adopted into their session or
  // moved to <recordingsDir>/quarantine, open gaps close at nowUtcMs and open
  // sessions end at their last finalized segment. Files of segments that
  // retention deleted but did not get to remove (a crash after the row commit)
  // are removed.
  RecoveryReport recoverOnStartup(const std::function<SegmentProbe(const QString& path)>& probe, int64_t nowUtcMs,
                                  const QString& recordingsDir);

  QVector<Camera> listCameras(bool includeDeleted = false);
  std::optional<Camera> getCamera(const QString& id);
  bool insertCamera(const Camera& c);
  bool updateCamera(const Camera& c);
  // One transaction: the camera, its segments, evidence refs and embedding rows are deleted.
  bool softDeleteCamera(const QString& id, int64_t nowUtcMs, MarkedEvidence* marked = nullptr);

  bool insertSession(const StreamSession& s);
  // Stores the session's pts -> UTC mapping once, as the session clock anchored it.
  bool setSessionAnchor(const QString& id, int64_t firstPtsNs, int64_t startedUtcMs);
  bool setSessionMedia(const QString& id, const QString& codec, int width, int height, double fps, const QString& captureClock);
  bool endSession(const QString& id, const QString& reason, int64_t endedUtcMs);
  QVector<StreamSession> listSessions(const QString& cameraId, int limit = 100);
  std::optional<StreamSession> getSession(const QString& id);

  bool insertSegment(const RecordingSegment& s);
  bool finalizeSegment(const QString& id, int64_t endPtsNs, int64_t endUtcMs, int64_t bytes, int64_t finalizedUtcMs);
  bool markSegmentDamaged(const QString& id, int64_t bytes);
  QVector<RecordingSegment> listSegments(const QString& cameraId, int64_t fromUtcMs, int64_t toUtcMs, int limit = 500);
  QVector<RecordingSegment> listSegmentsByState(const QString& state);
  std::optional<RecordingSegment> getSegment(const QString& id);
  std::optional<RecordingSegment> getSegmentByPath(const QString& path);
  // Finalized, damaged and recording segments per camera; oldest_utc_ms is 0 when unknown.
  QVector<CameraStorage> storageByCamera();

  // A hold is active while its until_utc_ms is 0 (open-ended) or in the future.
  // A failed query counts as held.
  bool hasEvidenceHold(const QString& segmentId, int64_t nowUtcMs);
  bool insertEvidenceHold(const QString& segmentId, int64_t untilUtcMs, const QString& reason);
  // Finalized and damaged segments oldest first (by start_utc_ms, then id),
  // after the cursor; cameraId empty means every camera. With endedBeforeUtcMs
  // only segments whose content ended before it.
  QVector<RecordingSegment> listRetentionCandidates(const QString& cameraId, std::optional<int64_t> endedBeforeUtcMs,
                                                    const SegmentCursor& after, int limit);
  // Bytes of finalized and damaged segments without an active hold.
  int64_t reclaimableBytes(int64_t nowUtcMs);
  // One IMMEDIATE transaction: the hold check, the state change to "deleted",
  // the audit row, the evidence refs listing the segment, its embedding rows
  // and its unfinished index jobs (skipped). The caller removes the file (and
  // the marked thumbnails) only after Deleted.
  SegmentDeletion deleteSegmentUnlessHeld(const QString& id, const QString& reason, int64_t nowUtcMs,
                                          MarkedEvidence* marked = nullptr);
  // Removes files inside evidenceDir; returns how many were removed.
  static int removeEvidenceFiles(const QStringList& paths, const QString& evidenceDir);
  // Removes the file of a segment deleted by retention and records the
  // removal. Paths outside recordingsDir are never touched.
  bool removeDeletedSegmentFile(const QString& id, const QString& path, const QString& recordingsDir, int64_t nowUtcMs);
  int purgeDeletedSegmentFiles(const QString& recordingsDir, int64_t nowUtcMs, int limit);

  bool insertGap(const ReceiveGap& g);
  bool closeGap(const QString& id, int64_t toUtcMs);
  QVector<ReceiveGap> listGaps(const QString& cameraId, int64_t fromUtcMs, int64_t toUtcMs, int limit = 500);

  std::optional<QString> getSetting(const QString& key);
  bool setSetting(const QString& key, const QString& valueJson);

  bool appendAudit(const QString& actor, const QString& action, const QString& target, const QString& detail, int64_t utcMs);

  // Zones and rules keep every revision; list and get return the current one.
  bool insertZone(const ZoneRecord& zone);
  bool insertZoneRevision(const ZoneRecord& zone);
  bool softDeleteZone(const QString& id, int64_t nowUtcMs);
  QVector<ZoneRecord> listZones(const QString& cameraId = QString());
  std::optional<ZoneRecord> getZone(const QString& id, int revision = 0);
  bool insertRule(const RuleRecord& rule);
  bool insertRuleRevision(const RuleRecord& rule);
  bool softDeleteRule(const QString& id, int64_t nowUtcMs);
  QVector<RuleRecord> listRules();
  std::optional<RuleRecord> getRule(const QString& id);

  bool insertEvaluation(const EvaluationRecord& e);
  QVector<EvaluationRecord> listEvaluations(const QString& ruleId, int64_t fromUtcMs, int64_t toUtcMs, int limit);

  // One transaction: the event, its evidence ref, an open-ended hold on every
  // segment already overlapping the evidence window, and its alert deliveries.
  bool openEvent(const EventRecord& event, const EvidenceRef& evidence, const QVector<AlertDelivery>& deliveries);
  bool setEventCondition(const QString& id, const QString& condition, int64_t clearedUtcMs);
  bool setEventOperatorState(const QString& id, const QString& state);
  // One transaction: the operator state, the expiry of the event's evidence
  // holds when holdsUntilUtcMs is set, and the audit row.
  bool applyOperatorAction(const QString& eventId, const QString& state, std::optional<int64_t> holdsUntilUtcMs,
                           const AuditEntry& audit);
  // Events a previous run left active or clearing end at nowUtcMs, each with a
  // cleared evaluation row and an audit row (note core_restart) in the same
  // transaction; returns their ids.
  QStringList clearOpenEvents(int64_t nowUtcMs);
  // When the rule's latest event cleared; 0 when none did.
  int64_t lastClearedUtcMs(const QString& ruleId);
  std::optional<EventRecord> getEvent(const QString& id);
  QVector<EventRecord> listEvents(const EventQuery& query);
  // Events per alert-log tab over all time: unresolved (new) and acknowledged
  // exclude events whose latest review is false_alarm; dismissed holds those
  // and every resolved event.
  EventCounts eventCounts();
  // One transaction: the review and its audit row.
  bool insertReview(const EventReview& review, const AuditEntry& audit);
  std::optional<EventReview> latestReview(const QString& eventId);

  QVector<AlertDelivery> deliveriesForEvent(const QString& eventId);
  std::optional<AlertDelivery> getDelivery(const QString& id);
  // Pending and failed deliveries of events that are not resolved, oldest first.
  QVector<AlertDelivery> openDeliveries();
  bool markDelivered(const QString& id, int64_t nowUtcMs);
  bool recordDeliveryAttempt(const QString& id, int attempts, const QString& state, const QString& error);

  std::optional<EvidenceRef> getEvidence(const QString& id);
  std::optional<EvidenceRef> evidenceForEvent(const QString& eventId);
  // Pending refs, and partial refs not yet re-evaluated after their window ended plus graceMs.
  QVector<EvidenceRef> evidenceToEvaluate(int64_t graceMs);
  // Segments of a camera (any state) whose time range intersects [fromUtcMs, toUtcMs].
  QVector<RecordingSegment> segmentsOverlapping(const QString& cameraId, int64_t fromUtcMs, int64_t toUtcMs);
  // One transaction: the ref's new state and thumbnail path and a hold (reason
  // "event:<id>") per segment, with until_utc_ms 0 or holdUntilUtcMs when that is set.
  bool updateEvidence(const EvidenceRef& ref, int64_t holdUntilUtcMs);
  bool setEventHoldsUntil(const QString& eventId, int64_t untilUtcMs);

  bool insertCoverage(const CoverageRecord& c);

  // Index versions (schema version 4). get, find and list skip deleted versions.
  bool upsertIndexVersion(const IndexVersion& v);
  std::optional<IndexVersion> getIndexVersion(const QString& hash);
  // The newest version with that name and sample interval.
  std::optional<IndexVersion> findIndexVersion(const QString& name, int sampleIntervalMs);
  QVector<IndexVersion> listIndexVersions();
  // One transaction: the version is marked deleted and its jobs and embedding rows are removed.
  bool deleteIndexVersion(const QString& hash, int64_t nowUtcMs);

  // Queues a job per finalized segment of an index-enabled camera that has
  // none for the version yet, oldest footage first; returns how many.
  int queueIndexJobs(const IndexVersion& v, int64_t nowUtcMs);
  // The oldest queued job, or failed job due for a retry below maxAttempts,
  // of a finalized segment of an index-enabled camera.
  std::optional<IndexJob> nextIndexJob(const QString& version, int64_t nowUtcMs, int maxAttempts);
  // One transaction: rows earlier attempts of the job appended are marked
  // deleted, the generation increases and the job is running.
  std::optional<IndexJob> beginIndexJob(int64_t jobId, int64_t nowUtcMs, MarkedEvidence* stale);
  // Moves the running attempt job.generation to state (with job's attempts,
  // next attempt, frames and compute counters); rows of an attempt that ends
  // in any state but done are marked deleted in the same transaction. False
  // when that attempt is no longer running.
  bool finishIndexJob(const IndexJob& job, const QString& state, const QString& reason, int64_t nowUtcMs, MarkedEvidence* stale);
  // Jobs a previous run left running are queued again with one attempt counted
  // (or failed at maxAttempts), jobs of deleted segments are skipped and rows
  // of every job that is not done are marked deleted; returns how many jobs
  // were requeued.
  int recoverIndexJobs(int64_t nowUtcMs, int maxAttempts, MarkedEvidence* stale);
  std::optional<IndexJob> getIndexJob(int64_t id);
  // Newest first; state empty means any.
  QVector<IndexJob> listIndexJobs(const QString& version, const QString& state, int limit);
  IndexQueueStats indexQueueStats(const QString& version);

  // One transaction for an attempt that is still running on a finalized
  // segment: rows are inserted (ids assigned), writeVectors appends the
  // vectors and sets each record's file and offset, the rows get them and the
  // job's frames_indexed grows. False (nothing stored) otherwise.
  bool appendEmbeddings(const IndexJob& job, QVector<EmbeddingRecord>& records,
                        const std::function<bool(QVector<EmbeddingRecord>&)>& writeVectors);
  std::optional<EmbeddingRecord> getEmbeddingRecord(int64_t id);
  // The ids among ids whose rows are not deleted.
  QSet<int64_t> liveEmbeddingIds(const QVector<int64_t>& ids);
  // Calls row(vectorFile, id, utcMs, vectorOffset) for every live row of the
  // version from the cameras (all when empty) with utc_ms in [fromUtcMs,
  // toUtcMs], in index order (camera, then time), which groups them by file.
  // Streams: a search never holds the whole range in memory.
  bool forEachSearchRow(const QString& version, const QStringList& cameraIds, int64_t fromUtcMs, int64_t toUtcMs,
                        const std::function<void(const QString&, int64_t, int64_t, int64_t)>& row);
  // The rows of these ids, id order; missing ids are left out.
  QVector<VectorRow> embeddingRowsByIds(const QVector<int64_t>& ids);
  // Marks rows whose vector could not be read where it should be deleted (the
  // record id at the offset differs), so a later scan skips them; returns how
  // many were live, -1 on failure.
  int dropUnreadableRecords(const QVector<int64_t>& ids, MarkedEvidence* marked);
  // Queues done jobs of finalized segments that have fewer live rows than they
  // indexed (the startup check or a scan dropped some), so the footage is
  // indexed again; returns how many.
  int requeueJobsMissingRecords(int64_t nowUtcMs);
  // Hashes of versions marked deleted, whose files may still be on disk.
  QStringList deletedIndexVersions();
  // Earliest start and latest end of finalized segments of the cameras (all
  // when empty); {0, 0} when there are none.
  std::pair<int64_t, int64_t> footageRange(const QStringList& cameraIds);
  // Live rows of the version and an estimate of their column payload in bytes.
  std::pair<int64_t, int64_t> embeddingRowStats(const QString& version);
  // Expected samples over finalized segments of the cameras (all when empty)
  // clipped to [fromUtcMs, toUtcMs], against live rows of the version there.
  CoverageCount indexCoverage(const IndexVersion& v, const QStringList& cameraIds, int64_t fromUtcMs, int64_t toUtcMs);
  QVector<CameraCoverage> coverageByCamera(const IndexVersion& v);
  QVector<VectorFileUsage> vectorFileUsage();
  // Live rows of a file as (id, offset), in offset order.
  QVector<std::pair<int64_t, int64_t>> liveFileRecords(const QString& vectorFile);
  // One transaction: the listed rows move from fromFile to toFile at
  // newOffsets; every other row still on fromFile loses its vector.
  bool moveFileRecords(const QString& fromFile, const QString& toFile, const QVector<std::pair<int64_t, int64_t>>& moved,
                       const QVector<int64_t>& newOffsets);
  // Rows on vectorFile whose record does not lie within fileBytes (every row
  // when fileBytes < 0) are marked deleted and lose their vector; returns how many were live.
  int dropRecordsPastEnd(const QString& vectorFile, int64_t fileBytes, int dims, MarkedEvidence* marked);
  QStringList referencedVectorFiles();
  // Removes files inside indexDir, and their directories once empty; returns how many files were removed.
  static int removeIndexFiles(const QStringList& paths, const QString& indexDir);

  bool insertSearchSession(const SearchSessionRecord& s);
  std::optional<SearchSessionRecord> getSearchSession(const QString& id);

  bool insertImport(const ImportRecord& r);
  bool updateImport(const ImportRecord& r);
  std::optional<ImportRecord> getImport(const QString& id);
  // Newest first.
  QVector<ImportRecord> listImports(int limit);
  // Whether a segment of the camera that is not deleted overlaps [fromUtcMs, toUtcMs).
  bool cameraHasFootage(const QString& cameraId, int64_t fromUtcMs, int64_t toUtcMs);
  // One transaction: every segment of the session that is not deleted becomes
  // deleted with reason (retention purges the files), with its embedding
  // rows, index jobs and evidence refs.
  bool deleteSessionSegments(const QString& sessionId, const QString& reason, int64_t nowUtcMs, MarkedEvidence* marked);

private:
  bool migrate();
  // Runs body in a transaction; commits when it returns true, else rolls back.
  bool transact(const std::function<bool()>& body);
  // Marks refs matching the condition deleted with reason; collects their thumbnails.
  bool markEvidenceDeleted(const QString& condition, const QVariantMap& binds, const QString& reason, int64_t nowUtcMs,
                           MarkedEvidence* marked);
  // Marks embedding rows matching the condition deleted, collects their
  // thumbnails, and skips queued and failed jobs of deleted segments.
  bool markEmbeddingsDeleted(const QString& condition, const QVariantMap& binds, MarkedEvidence* marked);
  bool skipJobsOfDeletedSegments(const QString& condition, const QVariantMap& binds, int64_t nowUtcMs);
  std::optional<bool> activeHold(const QString& segmentId, int64_t nowUtcMs);
  bool adoptRecordingFile(const std::function<SegmentProbe(const QString& path)>& probe, int64_t nowUtcMs,
                          const QString& cameraId, const QString& path);
  bool exec(const QString& sql);
  QSqlDatabase db_;
  QString connectionName_;
  QString path_;
  QString lastError_;
};

}
