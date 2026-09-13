#pragma once
#include "fovea/Api.h"
#include <QSqlDatabase>
#include <QString>
#include <QVector>
#include <functional>
#include <optional>

namespace fovea::core {

struct SegmentProbe {
  bool readable = false;
  int64_t durationNs = 0;
  int64_t bytes = 0;
};

struct RecoveryReport {
  int sessionsClosed = 0;
  int segmentsFinalized = 0;
  int segmentsDamaged = 0;
  int segmentsMissing = 0;
};

class Store {
public:
  Store();
  ~Store();
  bool open(const QString& path);
  void close();
  bool isOpen() const;
  QString lastError() const { return lastError_; }

  RecoveryReport recoverOnStartup(const std::function<SegmentProbe(const QString& path)>& probe, int64_t nowUtcMs);

  QVector<Camera> listCameras(bool includeDeleted = false);
  std::optional<Camera> getCamera(const QString& id);
  bool insertCamera(const Camera& c);
  bool updateCamera(const Camera& c);
  bool softDeleteCamera(const QString& id, int64_t nowUtcMs);

  bool insertSession(const StreamSession& s);
  bool setSessionFirstPts(const QString& id, int64_t firstPtsNs);
  bool setSessionMedia(const QString& id, const QString& codec, int width, int height, double fps, const QString& captureClock);
  bool endSession(const QString& id, const QString& reason, int64_t endedUtcMs);
  QVector<StreamSession> listSessions(const QString& cameraId, int limit = 100);
  std::optional<StreamSession> getSession(const QString& id);

  bool insertSegment(const RecordingSegment& s);
  bool finalizeSegment(const QString& id, int64_t endPtsNs, int64_t endUtcMs, int64_t bytes, int64_t finalizedUtcMs);
  bool setSegmentStart(const QString& id, int64_t startPtsNs, int64_t startUtcMs);
  bool setSegmentState(const QString& id, const QString& state);
  QVector<RecordingSegment> listSegments(const QString& cameraId, int64_t fromUtcMs, int64_t toUtcMs, int limit = 500);
  QVector<RecordingSegment> listSegmentsByState(const QString& state);
  std::optional<RecordingSegment> getSegment(const QString& id);
  std::optional<RecordingSegment> getSegmentByPath(const QString& path);
  int64_t totalSegmentBytes(const QString& cameraId);

  bool insertGap(const ReceiveGap& g);
  bool closeGap(const QString& id, int64_t toUtcMs);
  QVector<ReceiveGap> listGaps(const QString& cameraId, int64_t fromUtcMs, int64_t toUtcMs, int limit = 500);

  std::optional<QString> getSetting(const QString& key);
  bool setSetting(const QString& key, const QString& valueJson);

  bool appendAudit(const QString& actor, const QString& action, const QString& target, const QString& detail, int64_t utcMs);

private:
  bool migrate();
  bool exec(const QString& sql);
  QSqlDatabase db_;
  QString connectionName_;
  QString lastError_;
};

}
