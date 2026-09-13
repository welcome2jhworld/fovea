#pragma once
#include "fovea/Api.h"
#include "fovea/core/Config.h"
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVector>
#include <functional>
#include <optional>

namespace fovea::core {

class Store;

struct RetentionReport {
  int64_t runUtcMs = 0;
  int deleted = 0;
  int64_t deletedBytes = 0;
  int held = 0;
  int purged = 0;
  bool floorUnreachable = false;
  bool incomplete = false;
};

// Deletes recordings on the core main thread, which owns the store: segments
// older than their camera's retention period, the oldest segments of a camera
// over its byte limit, and, while free space is below the disk floor plus
// headroom, the oldest segments of any camera (only when that can reach the
// target; deleting everything and still pausing helps nobody). Finalized and
// damaged segments only; a segment with an active evidence hold is never
// deleted. Runs at start, every minute, when free space drops below the floor
// plus headroom, and on request.
class RetentionManager : public QObject {
  Q_OBJECT
public:
  using CameraList = std::function<QVector<Camera>()>;
  using FreeSpace = std::function<int64_t()>;

  RetentionManager(Store& store, CoreConfig config, CameraList cameras, FreeSpace freeSpace, QObject* parent = nullptr);

  void start();
  void requestRun();
  RetentionReport runPass(int64_t nowUtcMs);
  const RetentionReport& lastReport() const { return last_; }
  int64_t ageLimitMs(const Camera& camera) const;
  QJsonObject storageJson();

private:
  struct Pass {
    int64_t nowUtcMs = 0;
    int budget = 0;
    bool stopped = false;
    QSet<QString> held;
    RetentionReport report;
  };
  using OnDeleted = std::function<void(const RecordingSegment& segment, int64_t fileBytes)>;

  void run();
  void checkFreeSpace();
  void sweep(Pass& pass, const QString& cameraId, std::optional<int64_t> endedBeforeUtcMs, const QString& reason,
             const std::function<bool()>& wanted, const OnDeleted& onDeleted);

  Store& store_;
  CoreConfig config_;
  CameraList cameras_;
  FreeSpace freeSpace_;
  QTimer periodic_;
  QTimer freeCheck_;
  QTimer soon_;
  RetentionReport last_;
  bool floorWarned_ = false;
};

}
