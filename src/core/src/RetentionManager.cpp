#include "fovea/core/RetentionManager.h"
#include "fovea/Clock.h"
#include "fovea/core/Store.h"
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <utility>

namespace fovea::core {
namespace {

constexpr int kRunIntervalMs = 60'000;
constexpr int kFreeCheckIntervalMs = 5'000;
constexpr int64_t kLowSpaceMinGapMs = 10'000;
constexpr int kContinueDelayMs = 500;
// Bounds how long one pass holds the main thread; the rest follows shortly.
constexpr int kMaxDeletionsPerPass = 200;
constexpr int kPageSize = 64;
constexpr int64_t kDayMs = 86'400'000;

const QString kReasonAge = QStringLiteral("age");
const QString kReasonMaxBytes = QStringLiteral("max_bytes");
const QString kReasonDiskFloor = QStringLiteral("disk_floor");

double num(int64_t v) { return static_cast<double>(v); }

}

RetentionManager::RetentionManager(Store& store, CoreConfig config, CameraList cameras, FreeSpace freeSpace, QObject* parent)
    : QObject(parent), store_(store), config_(std::move(config)), cameras_(std::move(cameras)), freeSpace_(std::move(freeSpace)) {
  periodic_.setInterval(kRunIntervalMs);
  freeCheck_.setInterval(kFreeCheckIntervalMs);
  soon_.setSingleShot(true);
  connect(&periodic_, &QTimer::timeout, this, &RetentionManager::run);
  connect(&freeCheck_, &QTimer::timeout, this, &RetentionManager::checkFreeSpace);
  connect(&soon_, &QTimer::timeout, this, &RetentionManager::run);
}

void RetentionManager::start() {
  periodic_.start();
  freeCheck_.start();
  requestRun();
}

void RetentionManager::requestRun() {
  if (!soon_.isActive()) soon_.start(0);
}

int64_t RetentionManager::ageLimitMs(const Camera& camera) const {
  if (config_.retentionSecondsOverride > 0) return config_.retentionSecondsOverride * 1000;
  return static_cast<int64_t>(camera.retentionDays) * kDayMs;
}

void RetentionManager::checkFreeSpace() {
  const int64_t freeBytes = freeSpace_();
  if (freeBytes < 0 || freeBytes >= config_.minFreeBytes + config_.diskHeadroomBytes()) return;
  if (utcNowMs() - last_.runUtcMs >= kLowSpaceMinGapMs) requestRun();
}

void RetentionManager::run() {
  last_ = runPass(utcNowMs());
  const RetentionReport& r = last_;
  if (r.deleted > 0 || r.purged > 0)
    qInfo("retention: deleted %d segments (%lld MB), %d held, %d files purged, free %lld MB", r.deleted,
          static_cast<long long>(r.deletedBytes / (1024 * 1024)), r.held, r.purged,
          static_cast<long long>(freeSpace_() / (1024 * 1024)));
  if (r.floorUnreachable && !floorWarned_)
    qWarning("retention: free space is below the floor of %lld MB and deleting recordings cannot free enough",
             static_cast<long long>(config_.minFreeBytes / (1024 * 1024)));
  floorWarned_ = r.floorUnreachable;
  if (r.incomplete) soon_.start(kContinueDelayMs);
}

RetentionReport RetentionManager::runPass(int64_t nowUtcMs) {
  Pass pass;
  pass.nowUtcMs = nowUtcMs;
  pass.budget = kMaxDeletionsPerPass;
  pass.report.runUtcMs = nowUtcMs;
  pass.report.purged = store_.purgeDeletedSegmentFiles(config_.recordingsDir, nowUtcMs, kMaxDeletionsPerPass);

  QHash<QString, int64_t> bytesByCamera;
  for (const CameraStorage& s : store_.storageByCamera()) bytesByCamera.insert(s.cameraId, s.bytes);
  const auto always = [] { return true; };
  const auto account = [&bytesByCamera](const RecordingSegment& seg, int64_t) { bytesByCamera[seg.cameraId] -= seg.bytes; };

  for (const Camera& camera : cameras_()) {
    sweep(pass, camera.id, nowUtcMs - ageLimitMs(camera), kReasonAge, always, account);
    if (camera.maxBytes > 0) {
      const auto overLimit = [&] { return bytesByCamera.value(camera.id) > camera.maxBytes; };
      sweep(pass, camera.id, std::nullopt, kReasonMaxBytes, overLimit, account);
    }
  }

  const int64_t freeBytes = freeSpace_();
  const int64_t target = config_.minFreeBytes + config_.diskHeadroomBytes();
  if (!pass.stopped && freeBytes >= 0 && freeBytes < target) {
    const int64_t deficit = target - freeBytes;
    if (store_.reclaimableBytes(nowUtcMs) < deficit) {
      pass.report.floorUnreachable = true;
    } else {
      int64_t freed = 0;
      sweep(pass, QString(), std::nullopt, kReasonDiskFloor, [&] { return freed < deficit; },
            [&freed](const RecordingSegment&, int64_t fileBytes) { freed += fileBytes; });
    }
  }
  pass.report.held = static_cast<int>(pass.held.size());
  return pass.report;
}

void RetentionManager::sweep(Pass& pass, const QString& cameraId, std::optional<int64_t> endedBeforeUtcMs, const QString& reason,
                             const std::function<bool()>& wanted, const OnDeleted& onDeleted) {
  SegmentCursor cursor;
  while (!pass.stopped && wanted()) {
    const QVector<RecordingSegment> page = store_.listRetentionCandidates(cameraId, endedBeforeUtcMs, cursor, kPageSize);
    if (page.isEmpty()) return;
    for (const RecordingSegment& seg : page) {
      if (!wanted()) return;
      if (pass.budget == 0) {
        pass.stopped = true;
        pass.report.incomplete = true;
        return;
      }
      cursor = {seg.startUtcMs, seg.id};
      const int64_t fileBytes = QFileInfo(seg.path).size();
      MarkedEvidence marked;
      switch (store_.deleteSegmentUnlessHeld(seg.id, reason, pass.nowUtcMs, &marked)) {
        case SegmentDeletion::Deleted:
          --pass.budget;
          ++pass.report.deleted;
          pass.report.deletedBytes += seg.bytes;
          if (marked.refs > 0) {
            Store::removeEvidenceFiles(marked.thumbnails, config_.dataDir + QStringLiteral("/evidence"));
            qInfo("retention: segment %s deleted, %d evidence refs marked deleted", qPrintable(seg.id), marked.refs);
          }
          if (marked.embeddings > 0) {
            Store::removeIndexFiles(marked.indexThumbnails, config_.dataDir + QStringLiteral("/index"));
            qInfo("retention: segment %s deleted, %d search index records marked deleted", qPrintable(seg.id), marked.embeddings);
          }
          if (!store_.removeDeletedSegmentFile(seg.id, seg.path, config_.recordingsDir, pass.nowUtcMs))
            qWarning("retention: segment %s deleted, file removal pending: %s", qPrintable(seg.id), qPrintable(store_.lastError()));
          onDeleted(seg, fileBytes);
          break;
        case SegmentDeletion::Held:
          pass.held.insert(seg.id);
          break;
        case SegmentDeletion::NotDeletable:
          break;
        case SegmentDeletion::Failed:
          qWarning("retention: cannot delete segment %s: %s", qPrintable(seg.id), qPrintable(store_.lastError()));
          pass.stopped = true;
          return;
      }
    }
  }
}

QJsonObject RetentionManager::storageJson() {
  QHash<QString, CameraStorage> byCamera;
  for (const CameraStorage& s : store_.storageByCamera()) byCamera.insert(s.cameraId, s);
  QJsonArray perCamera;
  for (const Camera& camera : cameras_()) {
    const CameraStorage s = byCamera.value(camera.id);
    perCamera.push_back(QJsonObject{{"camera_id", camera.id},
                                    {"bytes", num(s.bytes)},
                                    {"segments", s.segments},
                                    {"oldest_utc_ms", num(s.oldestUtcMs)},
                                    {"retention_days", camera.retentionDays},
                                    {"max_bytes", num(camera.maxBytes)},
                                    {"age_limit_ms", num(ageLimitMs(camera))}});
  }
  return QJsonObject{{"free_bytes", num(freeSpace_())},
                     {"floor_bytes", num(config_.minFreeBytes)},
                     {"headroom_bytes", num(config_.diskHeadroomBytes())},
                     {"per_camera", perCamera},
                     {"last_run_utc_ms", num(last_.runUtcMs)},
                     {"deleted_last_run", last_.deleted},
                     {"deleted_bytes_last_run", num(last_.deletedBytes)},
                     {"held_last_run", last_.held},
                     {"floor_unreachable", last_.floorUnreachable},
                     {"retention_seconds_override", num(config_.retentionSecondsOverride)}};
}

}
