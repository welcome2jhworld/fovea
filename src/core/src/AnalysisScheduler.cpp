#include "fovea/core/AnalysisScheduler.h"
#include "fovea/Clock.h"
#include "fovea/Ids.h"
#include "fovea/core/AnalysisTap.h"
#include "fovea/core/CameraManager.h"
#include "fovea/core/Store.h"
#include "fovea/core/WorkerSupervisor.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace fovea::core {
namespace {

constexpr int kTickMs = 20;
constexpr int kJanitorMs = 5000;
constexpr int kRequestTimeoutMs = 3000;
constexpr int kJobDeadlineMs = 2500;
constexpr int kMaxPosted = 1;
constexpr int kJpegMaxWidth = 960;
constexpr int kEncoderThreads = 2;
constexpr int64_t kEncodeTimeoutNs = 2'000'000'000;
constexpr int64_t kSpoolMaxAgeMs = 30'000;
constexpr int64_t kCoverageWindowMs = 60'000;
constexpr int64_t kRateWindowNs = 10'000'000'000;
constexpr size_t kStatSamples = 200;
constexpr int64_t kSlowWarnIntervalNs = 60'000'000'000;
constexpr double kDefaultFps = 2.0;
constexpr double kMinFps = 0.2;
constexpr double kMaxFps = 10.0;
// The evaluator's default maxObservationGapNs is 1.5 s.
constexpr double kMinUsefulFps = 1.0 / 1.5;

double num(int64_t v) { return static_cast<double>(v); }

bool unitArray(const QJsonValue& v, qsizetype size, QVector<double>* out) {
  const QJsonArray a = v.toArray();
  if (!v.isArray() || a.size() != size) return false;
  QVector<double> values;
  for (const QJsonValue& x : a) {
    if (!x.isDouble() || !(x.toDouble() >= 0.0 && x.toDouble() <= 1.0)) return false;
    values.push_back(x.toDouble());
  }
  *out = values;
  return true;
}

QJsonObject latencyJson(const std::deque<int64_t>& samples) {
  if (samples.empty()) return QJsonObject{{"p50", QJsonValue::Null}, {"p95", QJsonValue::Null}, {"samples", 0}};
  std::vector<int64_t> sorted(samples.begin(), samples.end());
  std::sort(sorted.begin(), sorted.end());
  const auto at = [&](double q) {
    const size_t idx = std::min(sorted.size() - 1, static_cast<size_t>(q * static_cast<double>(sorted.size())));
    return std::round(static_cast<double>(sorted[idx]) / 1e5) / 10.0;
  };
  return QJsonObject{{"p50", at(0.5)}, {"p95", at(0.95)}, {"samples", static_cast<int>(sorted.size())}};
}

void pushSample(std::deque<int64_t>& samples, int64_t value) {
  samples.push_back(value);
  while (samples.size() > kStatSamples) samples.pop_front();
}

std::pair<int, int> jpegSize(int w, int h) {
  if (w <= kJpegMaxWidth) return {w & ~1, h & ~1};
  const int jh = static_cast<int>(std::lround(static_cast<double>(h) * kJpegMaxWidth / w));
  return {kJpegMaxWidth, std::max(2, jh & ~1)};
}

// gst_video_convert_sample runs its own short pipeline and waits on its bus,
// so it is safe on a pool thread without a main loop.
QString encodeJpeg(const QByteArray& bgra, int width, int height, const QString& path) {
  auto* owned = new QByteArray(bgra);
  const gsize size = static_cast<gsize>(owned->size());
  GstBuffer* buffer = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY, const_cast<char*>(owned->constData()), size, 0,
                                                  size, owned, [](gpointer p) { delete static_cast<QByteArray*>(p); });
  GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRA", "width", G_TYPE_INT, width, "height",
                                      G_TYPE_INT, height, "framerate", GST_TYPE_FRACTION, 0, 1, "pixel-aspect-ratio",
                                      GST_TYPE_FRACTION, 1, 1, nullptr);
  GstSample* raw = gst_sample_new(buffer, caps, nullptr, nullptr);
  gst_buffer_unref(buffer);
  gst_caps_unref(caps);
  const auto [jw, jh] = jpegSize(width, height);
  GstCaps* target = gst_caps_new_simple("image/jpeg", "width", G_TYPE_INT, jw, "height", G_TYPE_INT, jh, "pixel-aspect-ratio",
                                        GST_TYPE_FRACTION, 1, 1, nullptr);
  GError* error = nullptr;
  GstSample* jpeg = gst_video_convert_sample(raw, target, static_cast<GstClockTime>(kEncodeTimeoutNs), &error);
  gst_caps_unref(target);
  gst_sample_unref(raw);
  if (!jpeg) {
    const QString message = error ? QString::fromUtf8(error->message) : QStringLiteral("no output");
    if (error) g_error_free(error);
    return message;
  }
  QString result;
  GstBuffer* out = gst_sample_get_buffer(jpeg);
  GstMapInfo map;
  if (!out || !gst_buffer_map(out, &map, GST_MAP_READ)) {
    result = QStringLiteral("cannot map the JPEG buffer");
  } else {
    QFile f(path);
    const qint64 bytes = static_cast<qint64>(map.size);
    if (!QDir().mkpath(QFileInfo(path).path()) || !f.open(QIODevice::WriteOnly) ||
        f.write(reinterpret_cast<const char*>(map.data), bytes) != bytes)
      result = QStringLiteral("cannot write %1").arg(QFileInfo(path).fileName());
    gst_buffer_unmap(out, &map);
  }
  gst_sample_unref(jpeg);
  return result;
}

}

QString parseDetectReply(const QJsonObject& body, const QString& jobId, uint64_t generation, const QString& frameId,
                         int64_t ptsNs, QVector<DetectedObject>* detections) {
  if (body.contains(QStringLiteral("error")) && body.value(QStringLiteral("error")).isObject())
    return QStringLiteral("worker_error: ") + body.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
  const QString status = body.value(QStringLiteral("status")).toString();
  if (status != QLatin1String("ok")) {
    const QString error = body.value(QStringLiteral("error")).toString();
    return QStringLiteral("worker_status_%1%2").arg(status.isEmpty() ? QStringLiteral("missing") : status,
                                                     error.isEmpty() ? QString() : QStringLiteral(": ") + error);
  }
  const QJsonArray violations = body.value(QStringLiteral("contract_violations")).toArray();
  if (!violations.isEmpty()) return QStringLiteral("contract_violation: ") + violations.first().toString();
  if (body.value(QStringLiteral("job_id")).toString() != jobId) return QStringLiteral("contract_violation: job_id mismatch");
  if (body.value(QStringLiteral("generation")).toDouble(-1) != static_cast<double>(generation))
    return QStringLiteral("contract_violation: generation mismatch");
  const QJsonArray frames = body.value(QStringLiteral("frames")).toArray();
  if (frames.size() != 1) return QStringLiteral("contract_violation: %1 frames for a one-frame job").arg(frames.size());
  const QJsonObject frame = frames.first().toObject();
  if (frame.value(QStringLiteral("frame_id")).toString() != frameId) return QStringLiteral("contract_violation: unknown frame id");
  if (std::llround(frame.value(QStringLiteral("pts_ns")).toDouble(-1)) != ptsNs)
    return QStringLiteral("contract_violation: pts differs from the job");
  if (frame.value(QStringLiteral("width")).toInt() <= 0 || frame.value(QStringLiteral("height")).toInt() <= 0)
    return QStringLiteral("contract_violation: invalid frame size");
  QVector<DetectedObject> out;
  for (const QJsonValue& v : frame.value(QStringLiteral("detections")).toArray()) {
    const QJsonObject d = v.toObject();
    DetectedObject o;
    const QJsonValue track = d.value(QStringLiteral("track_id"));
    if (!track.isNull() && !track.isUndefined() && !track.isString()) return QStringLiteral("contract_violation: track_id type");
    o.trackId = track.toString();
    o.cls = d.value(QStringLiteral("cls")).toString();
    o.confidence = d.value(QStringLiteral("confidence")).toDouble(-1);
    QVector<double> foot;
    QVector<double> center;
    if (o.cls.isEmpty() || !(o.confidence >= 0.0 && o.confidence <= 1.0) || !unitArray(d.value(QStringLiteral("bbox")), 4, &o.bbox) ||
        !unitArray(d.value(QStringLiteral("anchor_foot")), 2, &foot) || !unitArray(d.value(QStringLiteral("anchor_center")), 2, &center))
      return QStringLiteral("contract_violation: malformed detection");
    o.foot = QPointF(foot[0], foot[1]);
    o.center = QPointF(center[0], center[1]);
    out.push_back(o);
  }
  *detections = out;
  return {};
}

struct AnalysisScheduler::Relay {
  std::mutex mutex;
  AnalysisScheduler* target = nullptr;

  void post(std::function<void(AnalysisScheduler&)> fn) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!target) return;
    AnalysisScheduler* t = target;
    QMetaObject::invokeMethod(t, [t, fn = std::move(fn)] { fn(*t); }, Qt::QueuedConnection);
  }
  void detach() {
    std::lock_guard<std::mutex> lock(mutex);
    target = nullptr;
  }
};

AnalysisScheduler::AnalysisScheduler(CameraManager& cameras, WorkerSupervisor& worker, Store& store, QString spoolDir, QObject* parent)
    : QObject(parent), cameras_(cameras), worker_(worker), store_(store), spoolDir_(std::move(spoolDir)),
      network_(new QNetworkAccessManager(this)), relay_(std::make_shared<Relay>()) {
  relay_->target = this;
  encoders_.setMaxThreadCount(kEncoderThreads);
  tickTimer_.setInterval(kTickMs);
  janitorTimer_.setInterval(kJanitorMs);
  connect(&tickTimer_, &QTimer::timeout, this, &AnalysisScheduler::tick);
  connect(&janitorTimer_, &QTimer::timeout, this, &AnalysisScheduler::sweepSpool);
  connect(&cameras_, &CameraManager::statusChanged, this, &AnalysisScheduler::refreshCameras);
  connect(&worker_, &WorkerSupervisor::stateChanged, this, [this](const QString& state) {
    if (state == QLatin1String("ready")) firstJobAfterReady_ = true;
  });
}

AnalysisScheduler::~AnalysisScheduler() {
  relay_->detach();
  encoders_.waitForDone();
  const int64_t nowUtc = utcNowMs();
  for (auto& [id, cam] : cams_) {
    flushCoverage(cam, nowUtc);
    if (cam.pending && cam.pending->reply) {
      disconnect(cam.pending->reply, nullptr, this, nullptr);
      cam.pending->reply->abort();
    }
  }
  delete network_;
}

void AnalysisScheduler::start() {
  QDir(spoolDir_).removeRecursively();
  QDir().mkpath(spoolDir_);
  refreshCameras();
  tickTimer_.start();
  janitorTimer_.start();
}

uint64_t AnalysisScheduler::generation(const QString& cameraId) const {
  const auto it = generations_.find(cameraId);
  return it == generations_.end() ? 0 : it->second;
}

void AnalysisScheduler::bumpGeneration(const QString& cameraId) { ++generations_[cameraId]; }

bool AnalysisScheduler::analyzing(const QString& cameraId) const { return cams_.count(cameraId) > 0; }

std::optional<QJsonObject> AnalysisScheduler::latestDetections(const QString& cameraId) const {
  const auto it = cams_.find(cameraId);
  if (it == cams_.end()) return std::nullopt;
  return it->second.detections;
}

void AnalysisScheduler::refreshCameras() {
  QStringList wanted;
  for (const fovea::Camera& c : cameras_.cameras()) {
    if (!c.enabled || !c.analyticsEnabled) continue;
    wanted.push_back(c.id);
    auto [it, added] = cams_.try_emplace(c.id);
    Camera& cam = it->second;
    if (added) {
      cam.id = c.id;
      bumpGeneration(c.id);
      double fps = kDefaultFps;
      if (const auto setting = store_.getSetting(QStringLiteral("analysis.") + c.id))
        fps = QJsonDocument::fromJson(setting->toUtf8()).object().value(QStringLiteral("detect_fps")).toDouble(kDefaultFps);
      cam.fpsTarget = std::clamp(fps, kMinFps, kMaxFps);
      cam.periodNs = static_cast<int64_t>(1e9 / cam.fpsTarget);
      cam.coverageFromUtcMs = utcNowMs();
      qInfo("analysis: camera %s at %.2g fps", qPrintable(c.id), cam.fpsTarget);
    }
    if (!cam.tap) cam.tap = cameras_.analysisTap(c.id);
    if (const auto status = cameras_.status(c.id)) observeSession(cam, status->sessionId);
  }
  for (auto it = cams_.begin(); it != cams_.end();) {
    if (wanted.contains(it->first)) {
      ++it;
      continue;
    }
    dropCamera(it->second);
    it = cams_.erase(it);
  }
  if (wanted != order_) {
    order_ = wanted;
    roundRobin_ = 0;
  }
}

void AnalysisScheduler::dropCamera(Camera& cam) {
  flushCoverage(cam, utcNowMs());
  if (!cam.pending) return;
  if (cam.pending->reply) {
    disconnect(cam.pending->reply, nullptr, this, nullptr);
    cam.pending->reply->abort();
    cam.pending->reply->deleteLater();
    --posted_;
  }
  if (!cam.pending->spoolPath.isEmpty()) QFile::remove(cam.pending->spoolPath);
  cam.pending.reset();
  qInfo("analysis: camera %s stopped", qPrintable(cam.id));
}

void AnalysisScheduler::observeSession(Camera& cam, const QString& sessionId) {
  if (sessionId.isEmpty() || sessionId == cam.sessionId) return;
  if (!cam.sessionId.isEmpty()) {
    bumpGeneration(cam.id);
    flushCoverage(cam, utcNowMs());
  }
  cam.sessionId = sessionId;
  cam.coverageSessionId = sessionId;
}

void AnalysisScheduler::tick() {
  const int64_t now = monoNowNs();
  const QStringList order = order_;
  const int n = static_cast<int>(order.size());
  const int start = roundRobin_;
  int next = start;
  for (int i = 0; i < n; ++i) {
    const int index = (start + i) % n;
    const auto found = cams_.find(order[index]);
    if (found == cams_.end()) continue;
    Camera& cam = found->second;
    if (now < cam.nextDueMonoNs) continue;
    if (cam.pending) {
      ++cam.skips;
      cam.nextDueMonoNs = std::max(cam.nextDueMonoNs + cam.periodNs, now);
      continue;
    }
    dispatch(cam, now);
    if (cam.pending) next = (index + 1) % n;
  }
  roundRobin_ = next;
  postReady();
}

void AnalysisScheduler::dispatch(Camera& cam, int64_t nowMonoNs) {
  if (!cam.tap) cam.tap = cameras_.analysisTap(cam.id);
  if (!cam.tap) return;
  std::optional<AnalysisFrame> frame = cam.tap->takeNewer(cam.lastSeq);
  if (!frame) return;
  cam.lastSeq = frame->seq;
  cam.nextDueMonoNs = cam.nextDueMonoNs + cam.periodNs < nowMonoNs ? nowMonoNs + cam.periodNs : cam.nextDueMonoNs + cam.periodNs;
  observeSession(cam, frame->sessionId);

  Pending p;
  p.frameId = newId();
  p.sessionId = frame->sessionId;
  p.ptsNs = frame->ptsNs;
  p.recvMonoNs = frame->recvMonoNs;
  p.utcMs = frame->utcMs;
  p.width = frame->width;
  p.height = frame->height;
  p.generation = generation(cam.id);
  p.takenMonoNs = nowMonoNs;
  cam.pending = p;
  cam.takenTimesNs.push_back(nowMonoNs);
  ++cam.framesSent;
  ++cam.coverageSent;

  if (!worker_.detectorAvailable()) {
    unknown(cam, unavailableReason());
    return;
  }
  cam.pending->spoolPath = QStringLiteral("%1/%2/%3.jpg").arg(spoolDir_, cam.id, p.frameId);
  const std::shared_ptr<Relay> relay = relay_;
  encoders_.start([relay, cameraId = cam.id, frameId = p.frameId, path = cam.pending->spoolPath, pixels = std::move(frame->bgra),
                   w = frame->width, h = frame->height] {
    const QString error = encodeJpeg(pixels, w, h, path);
    relay->post([cameraId, frameId, error](AnalysisScheduler& self) { self.onEncoded(cameraId, frameId, error); });
  });
}

void AnalysisScheduler::onEncoded(const QString& cameraId, const QString& frameId, const QString& error) {
  const auto it = cams_.find(cameraId);
  if (it == cams_.end() || !it->second.pending || it->second.pending->frameId != frameId) {
    QFile::remove(QStringLiteral("%1/%2/%3.jpg").arg(spoolDir_, cameraId, frameId));
    return;
  }
  Camera& cam = it->second;
  if (!error.isEmpty()) {
    unknown(cam, QStringLiteral("encode_failed: ") + error);
    return;
  }
  cam.readyToPost = true;
  postReady();
}

void AnalysisScheduler::postReady() {
  while (posted_ < kMaxPosted) {
    Camera* next = nullptr;
    for (auto& [id, cam] : cams_)
      if (cam.readyToPost && cam.pending && (!next || cam.pending->takenMonoNs < next->pending->takenMonoNs)) next = &cam;
    if (!next) return;
    post(*next);
  }
}

QString AnalysisScheduler::unavailableReason() const {
  const QString state = worker_.state();
  if (state == QLatin1String("disabled")) return QStringLiteral("worker_unavailable");
  if (state != QLatin1String("ready")) return QStringLiteral("worker_") + state;
  return QStringLiteral("detector_") + worker_.detectorState();
}

void AnalysisScheduler::post(Camera& cam) {
  cam.readyToPost = false;
  if (!worker_.detectorAvailable()) {
    unknown(cam, unavailableReason());
    return;
  }
  Pending& p = *cam.pending;
  p.jobId = newId();
  QJsonObject job{
      {"job_id", p.jobId},
      {"kind", "detect_frames"},
      {"camera_id", cam.id},
      {"session_id", p.sessionId},
      {"generation", static_cast<double>(p.generation)},
      {"frames", QJsonArray{QJsonObject{{"frame_id", p.frameId},
                                        {"pts_ns", num(p.ptsNs)},
                                        {"recv_mono_ns", num(p.recvMonoNs)},
                                        {"capture_utc_ms", num(p.utcMs)},
                                        {"path", QDir::toNativeSeparators(p.spoolPath)},
                                        {"index", 0}}}},
      {"clip", QJsonValue::Null},
      {"gaps", QJsonArray{}},
      {"limits", QJsonObject{{"max_frames", 1}, {"deadline_ms", kJobDeadlineMs}}}};
  if (hints_) {
    const DetectHints hints = hints_(cam.id);
    if (hints.threshold > 0) job.insert("threshold", hints.threshold);
    if (hints.maxGapNs > 0) job.insert("max_gap_ns", num(hints.maxGapNs));
  }
  QNetworkRequest req(QUrl(worker_.baseUrl() + QStringLiteral("/v1/jobs")));
  req.setRawHeader("Authorization", "Bearer " + worker_.token().toLatin1());
  req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  req.setTransferTimeout(kRequestTimeoutMs);
  p.excludeFromStats = std::exchange(firstJobAfterReady_, false);
  p.postedMonoNs = monoNowNs();
  p.reply = network_->post(req, QJsonDocument(job).toJson(QJsonDocument::Compact));
  ++posted_;
  QNetworkReply* reply = p.reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply, cameraId = cam.id, jobId = p.jobId] { onReply(cameraId, jobId, reply); });
}

void AnalysisScheduler::onReply(const QString& cameraId, const QString& jobId, QNetworkReply* reply) {
  reply->deleteLater();
  --posted_;
  const auto it = cams_.find(cameraId);
  if (it == cams_.end() || !it->second.pending || it->second.pending->jobId != jobId) {
    postReady();
    return;
  }
  Camera& cam = it->second;
  Pending& p = *cam.pending;
  p.reply = nullptr;
  const int64_t repliedMonoNs = monoNowNs();
  if (!p.excludeFromStats) {
    pushSample(cam.turnaroundNs, repliedMonoNs - p.takenMonoNs);
    pushSample(cam.requestNs, repliedMonoNs - p.postedMonoNs);
  }
  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QByteArray raw = reply->readAll();
  const QJsonObject body = QJsonDocument::fromJson(raw).object();
  if (reply->error() == QNetworkReply::OperationCanceledError) {
    unknown(cam, QStringLiteral("timeout"));
  } else if (status != 200) {
    const QString detail = body.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString();
    unknown(cam, status == 0 ? QStringLiteral("worker_error: ") + reply->errorString()
                             : QStringLiteral("worker_http_%1%2").arg(status).arg(detail.isEmpty() ? QString() : QStringLiteral(": ") + detail));
  } else {
    AnalysisResult result;
    const QString problem = parseDetectReply(body, p.jobId, p.generation, p.frameId, p.ptsNs, &result.detections);
    if (!problem.isEmpty()) {
      unknown(cam, problem);
    } else {
      if (cam.framesKnown == 0) cam.firstKnownMonoNs = repliedMonoNs;
      cam.knownTimesNs.push_back(repliedMonoNs);
      ++cam.framesKnown;
      ++cam.coverageKnown;
      cam.lastQuality = QStringLiteral("known");
      QJsonArray dets;
      for (const DetectedObject& d : result.detections)
        dets.push_back(QJsonObject{{"track_id", d.trackId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(d.trackId)},
                                   {"cls", d.cls},
                                   {"confidence", d.confidence},
                                   {"bbox", QJsonArray{d.bbox[0], d.bbox[1], d.bbox[2], d.bbox[3]}},
                                   {"anchor_foot", QJsonArray{d.foot.x(), d.foot.y()}},
                                   {"anchor_center", QJsonArray{d.center.x(), d.center.y()}}});
      cam.detections = QJsonObject{{"camera_id", cam.id},       {"session_id", p.sessionId}, {"frame_id", p.frameId},
                                   {"pts_ns", num(p.ptsNs)},     {"utc_ms", num(p.utcMs)},    {"width", p.width},
                                   {"height", p.height},         {"generation", static_cast<double>(p.generation)},
                                   {"detections", dets}};
      result.frame.quality = rules::Quality::Known;
      finish(cam, std::move(result));
    }
  }
  postReady();
}

void AnalysisScheduler::unknown(Camera& cam, const QString& reason) {
  ++cam.framesUnknown;
  ++cam.coverageUnknown;
  cam.lastQuality = QStringLiteral("unknown");
  if (reason != cam.lastReason) qInfo("analysis: camera %s unknown: %s", qPrintable(cam.id), qPrintable(reason));
  cam.lastReason = reason;
  AnalysisResult result;
  result.frame.quality = rules::Quality::Unknown;
  result.reason = reason;
  finish(cam, std::move(result));
}

void AnalysisScheduler::finish(Camera& cam, AnalysisResult result) {
  const Pending p = *cam.pending;
  cam.pending.reset();
  cam.readyToPost = false;
  if (result.frame.quality == rules::Quality::Known) cam.lastReason.clear();
  cam.lastResultUtcMs = utcNowMs();
  result.frameId = p.frameId;
  result.spoolPath = QFileInfo::exists(p.spoolPath) ? p.spoolPath : QString();
  result.frame.cameraId = cam.id;
  result.frame.sessionId = p.sessionId;
  result.frame.ptsNs = p.ptsNs;
  result.frame.utcMs = p.utcMs;
  result.frame.recvMonoNs = p.recvMonoNs;
  result.frame.generation = p.generation;
  result.frame.frameWidth = p.width;
  result.frame.frameHeight = p.height;
  if (sink_) sink_(result);
  if (!p.spoolPath.isEmpty()) QFile::remove(p.spoolPath);
}

void AnalysisScheduler::flushCoverage(Camera& cam, int64_t nowUtcMs) {
  if (cam.coverageSent > 0 || cam.coverageKnown > 0 || cam.coverageUnknown > 0) {
    CoverageRecord c;
    c.cameraId = cam.id;
    c.sessionId = cam.coverageSessionId;
    c.fromUtcMs = cam.coverageFromUtcMs;
    c.toUtcMs = nowUtcMs;
    c.framesSent = cam.coverageSent;
    c.framesKnown = cam.coverageKnown;
    c.framesUnknown = cam.coverageUnknown;
    c.detectFpsTarget = cam.fpsTarget;
    if (!store_.insertCoverage(c)) qWarning("analysis: cannot store coverage: %s", qPrintable(store_.lastError()));
  }
  cam.coverageSent = cam.coverageKnown = cam.coverageUnknown = 0;
  cam.coverageFromUtcMs = nowUtcMs;
  cam.coverageSessionId = cam.sessionId;
}

void AnalysisScheduler::sweepSpool() {
  const int64_t nowUtc = utcNowMs();
  const int64_t nowMono = monoNowNs();
  QStringList inFlight;
  for (auto& [id, cam] : cams_) {
    if (cam.pending) inFlight.push_back(QFileInfo(cam.pending->spoolPath).absoluteFilePath());
    if (nowUtc - cam.coverageFromUtcMs >= kCoverageWindowMs) flushCoverage(cam, nowUtc);
    for (std::deque<int64_t>* times : {&cam.knownTimesNs, &cam.takenTimesNs})
      while (!times->empty() && nowMono - times->front() > kRateWindowNs) times->pop_front();
    const double windowS = static_cast<double>(kRateWindowNs) / 1e9;
    const double achieved = static_cast<double>(cam.knownTimesNs.size()) / windowS;
    const double taken = static_cast<double>(cam.takenTimesNs.size()) / windowS;
    // Only a detector that cannot keep up is worth a warning; a stalled source or a worker outage reports itself.
    if (worker_.detectorAvailable() && cam.framesKnown > 0 && nowMono - cam.firstKnownMonoNs > kRateWindowNs &&
        taken >= kMinUsefulFps && achieved < kMinUsefulFps && nowMono - cam.lastSlowWarnNs > kSlowWarnIntervalNs) {
      cam.lastSlowWarnNs = nowMono;
      qWarning("analysis: camera %s achieved %.2f detections/s from %.2f frames/s, below %.2f; rules on it will report unknown",
               qPrintable(cam.id), achieved, taken, kMinUsefulFps);
    }
  }
  QDirIterator it(spoolDir_, {QStringLiteral("*.jpg")}, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QFileInfo info(it.next());
    if (inFlight.contains(info.absoluteFilePath())) continue;
    if (nowUtc - info.lastModified().toMSecsSinceEpoch() > kSpoolMaxAgeMs) QFile::remove(info.absoluteFilePath());
  }
}

QJsonObject AnalysisScheduler::cameraJson(const QString& cameraId) const {
  const auto found = cams_.find(cameraId);
  if (found == cams_.end()) return QJsonObject{{"camera_id", cameraId}, {"analyzing", false}};
  const Camera& cam = found->second;
  const int64_t now = monoNowNs();
  const auto recent = std::count_if(cam.knownTimesNs.begin(), cam.knownTimesNs.end(), [now](int64_t t) { return now - t <= kRateWindowNs; });
  const int64_t results = cam.framesKnown + cam.framesUnknown;
  QString stage = QStringLiteral("idle");
  if (cam.pending) stage = cam.pending->reply ? QStringLiteral("posted") : cam.readyToPost ? QStringLiteral("ready") : QStringLiteral("encoding");
  return QJsonObject{{"camera_id", cam.id},
                     {"analyzing", true},
                     {"session_id", cam.sessionId},
                     {"generation", static_cast<double>(generation(cam.id))},
                     {"detect_fps_target", cam.fpsTarget},
                     {"achieved_fps", std::round(static_cast<double>(recent) / (static_cast<double>(kRateWindowNs) / 1e9) * 100) / 100},
                     {"frames_sent", num(cam.framesSent)},
                     {"frames_known", num(cam.framesKnown)},
                     {"frames_unknown", num(cam.framesUnknown)},
                     {"unknown_ratio", results > 0 ? static_cast<double>(cam.framesUnknown) / static_cast<double>(results) : 0.0},
                     {"skips", num(cam.skips)},
                     {"in_flight", stage},
                     {"quality", cam.lastQuality.isEmpty() ? QStringLiteral("unknown") : cam.lastQuality},
                     {"last_unknown_reason", cam.lastReason},
                     {"last_result_utc_ms", num(cam.lastResultUtcMs)},
                     {"turnaround_ms", latencyJson(cam.turnaroundNs)},
                     {"request_ms", latencyJson(cam.requestNs)}};
}

QJsonArray AnalysisScheduler::camerasJson() const {
  QJsonArray out;
  for (const QString& id : order_) out.push_back(cameraJson(id));
  return out;
}

}
