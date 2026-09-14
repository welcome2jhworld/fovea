#pragma once
#include "fovea/core/Index.h"
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>
#include <deque>
#include <functional>

class QNetworkAccessManager;

namespace fovea::core {

class WorkerSupervisor;

inline constexpr int kEmbedMaxFrames = 32;
inline constexpr int kQueryDeadlineMs = 3000;
inline constexpr int kIndexDeadlineMs = 120'000;
inline constexpr int kSharedGpuBudgetMs = 200;
inline constexpr double kInitialMsPerFrame = 50.0;

struct EmbedFrame {
  QString frameId;
  int64_t ptsNs = 0;
  int64_t utcMs = 0;
  QString path;
};

struct EmbedRequest {
  QString kind;
  QString jobId;
  QString versionName;
  int sampleIntervalMs = kDefaultSampleIntervalMs;
  int64_t generation = 0;
  QString cameraId;
  QString sessionId;
  QVector<EmbedFrame> frames;
  QString text;
  int deadlineMs = kQueryDeadlineMs;
};

struct EmbedReply {
  bool ok = false;
  // Worker outage, lane busy past the deadline or timeout: nothing wrong with the input.
  bool transient = false;
  QString error;
  IndexVersion version;
  QVector<QVector<float>> vectors;
  int64_t roundTripMs = 0;
};

// The descriptor hash the worker must report: the first 12 hex digits of the
// SHA-256 of the descriptor's compact JSON (sorted keys).
QString descriptorHash(const QJsonObject& descriptor);

// Checks a 200 reply of an embed job against its request: job id,
// generation, frame ids, count, dims, unit vectors, and the descriptor (its
// name, sample interval, dims and hash). dry_run results are refused.
EmbedReply parseEmbedReply(const QJsonObject& body, const EmbedRequest& request);

// Posts embed_frames and embed_text jobs to the worker. Text (query) jobs go
// out as soon as the worker's InferenceGate allows embedding; at most one
// frame (index) job is open, and it is not posted while a query is open or
// waiting, so index work yields to queries (the worker also runs a waiting
// query between its batches).
class EmbedClient : public QObject {
  Q_OBJECT
public:
  using Callback = std::function<void(const EmbedReply& reply)>;

  EmbedClient(WorkerSupervisor& worker, QObject* parent = nullptr);

  bool workerReady() const;
  // Why embedding cannot run now, empty when the worker is ready.
  QString unavailableReason() const;
  void embed(EmbedRequest request, Callback done);
  // Frames per index request: kEmbedMaxFrames, or while detection shares the
  // GPU as many as fit kSharedGpuBudgetMs at the measured time per frame, so
  // a ready detection frame never waits long behind an index request.
  int indexBatchFrames() const;

private:
  struct Pending {
    EmbedRequest request;
    Callback done;
  };
  void post(Pending pending);
  void pump();
  QJsonObject jobJson(const EmbedRequest& request) const;

  WorkerSupervisor& worker_;
  QNetworkAccessManager* network_ = nullptr;
  std::deque<Pending> waitingQueries_;
  std::deque<Pending> waitingIndex_;
  int64_t waitingSinceNs_ = 0;
  double msPerIndexFrame_ = kInitialMsPerFrame;
  int openQueries_ = 0;
  bool indexOpen_ = false;
};

}
