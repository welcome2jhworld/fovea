#pragma once
#include "fovea/core/Analytics.h"
#include "fovea/rules/Types.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

namespace fovea::core {

class AnalysisTap;
class CameraManager;
class Store;
class WorkerSupervisor;

struct DetectedObject {
  QString trackId;
  QString cls;
  double confidence = 0;
  QVector<double> bbox;
  QPointF foot;
  QPointF center;
};

// One analysed frame. frame.tracks is empty: the rule engine builds tracks
// from detections with each zone's anchor. reason says why quality is unknown.
struct AnalysisResult {
  rules::ObservationFrame frame;
  QString frameId;
  QString spoolPath;
  QString reason;
  QVector<DetectedObject> detections;
};

// Parses one detect_frames reply for a single-frame job. Returns the reason the
// result is unusable, empty when detections holds the frame's detections.
QString parseDetectReply(const QJsonObject& body, const QString& jobId, uint64_t generation, const QString& frameId,
                         int64_t ptsNs, QVector<DetectedObject>* detections);

// Samples analytics-enabled cameras for the detector on the core thread: every
// 1/detect_fps per camera, round robin, the newest frame from the camera's
// AnalysisTap is JPEG-encoded on a small thread pool into
// <spool>/<camera>/<frame_id>.jpg and posted to the worker (3 s timeout). One
// request per camera and one posted request overall are open at a time; a due
// camera whose request is still open counts a skip. Every taken frame produces
// exactly one AnalysisResult: known with detections, or unknown when the worker
// is down, the detector is loading, the request failed or timed out, or the
// reply broke the contract. A per-camera generation increases when analysis of
// the camera starts, on session change and on bumpGeneration (rule-set change).
// Jobs carry the camera's DetectHints (detector threshold and the longest gap
// its rules tolerate, over which the worker keeps track ids). Latency samples
// cover every posted request that finished, failed or timed out.
class AnalysisScheduler : public QObject {
  Q_OBJECT
public:
  using Sink = std::function<void(const AnalysisResult&)>;
  using Hints = std::function<DetectHints(const QString& cameraId)>;

  AnalysisScheduler(CameraManager& cameras, WorkerSupervisor& worker, Store& store, QString spoolDir, QObject* parent = nullptr);
  ~AnalysisScheduler() override;

  void setSink(Sink sink) { sink_ = std::move(sink); }
  void setHints(Hints hints) { hints_ = std::move(hints); }
  void start();
  uint64_t generation(const QString& cameraId) const;
  void bumpGeneration(const QString& cameraId);
  bool analyzing(const QString& cameraId) const;
  std::optional<QJsonObject> latestDetections(const QString& cameraId) const;
  QJsonObject cameraJson(const QString& cameraId) const;
  QJsonArray camerasJson() const;

private:
  struct Pending {
    QString frameId;
    QString sessionId;
    int64_t ptsNs = 0;
    int64_t recvMonoNs = 0;
    int64_t utcMs = 0;
    int width = 0;
    int height = 0;
    uint64_t generation = 0;
    QString spoolPath;
    QString jobId;
    int64_t takenMonoNs = 0;
    int64_t postedMonoNs = 0;
    bool excludeFromStats = false;
    QNetworkReply* reply = nullptr;
  };
  struct Camera {
    QString id;
    std::shared_ptr<AnalysisTap> tap;
    double fpsTarget = 2.0;
    int64_t periodNs = 500'000'000;
    int64_t nextDueMonoNs = 0;
    uint64_t lastSeq = 0;
    QString sessionId;
    std::optional<Pending> pending;
    bool readyToPost = false;
    std::optional<QJsonObject> detections;
    int64_t framesSent = 0;
    int64_t framesKnown = 0;
    int64_t framesUnknown = 0;
    int64_t skips = 0;
    QString lastReason;
    QString lastQuality;
    int64_t lastResultUtcMs = 0;
    std::deque<int64_t> takenTimesNs;
    std::deque<int64_t> knownTimesNs;
    std::deque<int64_t> turnaroundNs;
    std::deque<int64_t> requestNs;
    int64_t firstKnownMonoNs = 0;
    int64_t lastSlowWarnNs = 0;
    int64_t coverageFromUtcMs = 0;
    QString coverageSessionId;
    int coverageSent = 0;
    int coverageKnown = 0;
    int coverageUnknown = 0;
  };
  struct Relay;

  void tick();
  void refreshCameras();
  void observeSession(Camera& cam, const QString& sessionId);
  void dispatch(Camera& cam, int64_t nowMonoNs);
  void onEncoded(const QString& cameraId, const QString& frameId, const QString& error);
  void postReady();
  QString unavailableReason() const;
  void post(Camera& cam);
  void onReply(const QString& cameraId, const QString& jobId, QNetworkReply* reply);
  void finish(Camera& cam, AnalysisResult result);
  void unknown(Camera& cam, const QString& reason);
  void flushCoverage(Camera& cam, int64_t nowUtcMs);
  void sweepSpool();
  void dropCamera(Camera& cam);

  CameraManager& cameras_;
  WorkerSupervisor& worker_;
  Store& store_;
  QString spoolDir_;
  Sink sink_;
  Hints hints_;
  QNetworkAccessManager* network_ = nullptr;
  std::shared_ptr<Relay> relay_;
  QThreadPool encoders_;
  QTimer tickTimer_;
  QTimer janitorTimer_;
  std::map<QString, Camera> cams_;
  // Outlives a camera's analysis, so a camera that stops and resumes never reuses a generation.
  std::map<QString, uint64_t> generations_;
  QStringList order_;
  int roundRobin_ = 0;
  int posted_ = 0;
  bool firstJobAfterReady_ = true;
};

}
