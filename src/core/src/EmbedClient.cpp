#include "fovea/core/EmbedClient.h"
#include "fovea/Clock.h"
#include "fovea/core/WorkerSupervisor.h"
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace fovea::core {
namespace {

constexpr int kQueryTimeoutMarginMs = 2000;
constexpr int kIndexTimeoutMarginMs = 60'000;
constexpr double kUnitNormTolerance = 1e-3;

double num(int64_t v) { return static_cast<double>(v); }

EmbedReply failure(const QString& error, bool transient = false) {
  EmbedReply r;
  r.error = error;
  r.transient = transient;
  return r;
}

}

QString descriptorHash(const QJsonObject& descriptor) {
  const QByteArray canonical = QJsonDocument(descriptor).toJson(QJsonDocument::Compact);
  return QString::fromLatin1(QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex().left(12));
}

EmbedReply parseEmbedReply(const QJsonObject& body, const EmbedRequest& request) {
  const QString status = body.value("status").toString();
  if (status == QLatin1String("error"))
    return failure(QStringLiteral("worker_error: ") + body.value("error").toString());
  if (status == QLatin1String("dry_run")) return failure(QStringLiteral("contract_violation: dry_run vectors are not search evidence"));
  if (status != QLatin1String("ok")) return failure(QStringLiteral("contract_violation: status %1").arg(status));
  const QJsonArray violations = body.value("contract_violations").toArray();
  if (!violations.isEmpty()) return failure(QStringLiteral("contract_violation: ") + violations.first().toString());
  if (body.value("job_id").toString() != request.jobId) return failure(QStringLiteral("contract_violation: job_id mismatch"));
  if (body.value("generation").toDouble(-1) != num(request.generation))
    return failure(QStringLiteral("contract_violation: generation mismatch"));
  const qsizetype expected = request.kind == QLatin1String("embed_frames") ? request.frames.size() : 1;
  if (request.kind == QLatin1String("embed_frames")) {
    const QJsonArray ids = body.value("frame_ids").toArray();
    if (ids.size() != expected) return failure(QStringLiteral("contract_violation: %1 frame ids for %2 frames").arg(ids.size()).arg(expected));
    for (qsizetype i = 0; i < expected; ++i)
      if (ids[i].toString() != request.frames[i].frameId) return failure(QStringLiteral("contract_violation: frame ids differ from the job"));
  }
  if (body.value("count").toInt(-1) != expected) return failure(QStringLiteral("contract_violation: count differs from the inputs"));

  const QJsonObject descriptor = body.value("descriptor").toObject();
  EmbedReply reply;
  reply.version.hash = body.value("index_version").toString();
  reply.version.name = descriptor.value("name").toString();
  reply.version.modelId = descriptor.value("model_id").toString();
  reply.version.modelRevision = descriptor.value("model_revision").toString();
  reply.version.dims = descriptor.value("dims").toInt();
  reply.version.dtype = descriptor.value("dtype").toString();
  reply.version.sampleIntervalMs = descriptor.value("sample_interval_ms").toInt();
  reply.version.descriptor = descriptor;
  if (reply.version.name != request.versionName) return failure(QStringLiteral("contract_violation: descriptor names another index version"));
  if (reply.version.sampleIntervalMs != request.sampleIntervalMs)
    return failure(QStringLiteral("contract_violation: descriptor sample interval differs from the job"));
  if (reply.version.modelId.isEmpty()) return failure(QStringLiteral("contract_violation: descriptor without model_id"));
  if (reply.version.hash.isEmpty() || reply.version.hash != descriptorHash(descriptor))
    return failure(QStringLiteral("contract_violation: index_version is not the descriptor hash"));
  const int dims = body.value("dims").toInt();
  if (dims <= 0 || dims != reply.version.dims) return failure(QStringLiteral("contract_violation: dims differ from the descriptor"));

  const QByteArray raw = QByteArray::fromBase64(body.value("vectors").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
  const qsizetype rowBytes = static_cast<qsizetype>(dims) * 4;
  if (raw.size() != rowBytes * expected)
    return failure(QStringLiteral("contract_violation: %1 vector bytes for %2 x %3 dims").arg(raw.size()).arg(expected).arg(dims));
  reply.vectors.reserve(expected);
  for (qsizetype i = 0; i < expected; ++i) {
    QVector<float> v(dims);
    std::memcpy(v.data(), raw.constData() + i * rowBytes, static_cast<size_t>(rowBytes));
    double norm = 0;
    for (const float x : v) norm += static_cast<double>(x) * static_cast<double>(x);
    if (!std::isfinite(norm) || std::abs(std::sqrt(norm) - 1.0) > kUnitNormTolerance || !normalizeVector(v))
      return failure(QStringLiteral("contract_violation: vector %1 is not unit length").arg(i));
    reply.vectors.push_back(std::move(v));
  }
  reply.ok = true;
  return reply;
}

EmbedClient::EmbedClient(WorkerSupervisor& worker, QObject* parent)
    : QObject(parent), worker_(worker), network_(new QNetworkAccessManager(this)) {
  connect(&worker_, &WorkerSupervisor::stateChanged, this, [this] { pump(); });
  connect(&worker_.gate(), &InferenceGate::released, this, &EmbedClient::pump, Qt::QueuedConnection);
}

int EmbedClient::indexBatchFrames() const {
  const InferenceGate& gate = worker_.gate();
  if (!gate.shared() || !gate.detectRecently(monoNowNs())) return kEmbedMaxFrames;
  return std::clamp(static_cast<int>(kSharedGpuBudgetMs / msPerIndexFrame_), 1, kEmbedMaxFrames);
}

bool EmbedClient::workerReady() const { return worker_.embedAvailable(); }

QString EmbedClient::unavailableReason() const {
  const QString state = worker_.state();
  if (state == QLatin1String("disabled")) return QStringLiteral("worker_unavailable");
  if (state != QLatin1String("ready")) return QStringLiteral("worker_") + state;
  if (!worker_.embedAvailable()) return QStringLiteral("detector_warming_up");
  return {};
}

void EmbedClient::embed(EmbedRequest request, Callback done) {
  Pending pending{std::move(request), std::move(done)};
  if (!worker_.embedAvailable()) {
    pending.done(failure(unavailableReason(), true));
    return;
  }
  (pending.request.kind == QLatin1String("embed_text") ? waitingQueries_ : waitingIndex_).push_back(std::move(pending));
  pump();
}

void EmbedClient::pump() {
  InferenceGate& gate = worker_.gate();
  while (!waitingQueries_.empty() && gate.mayPost(InferenceGate::Lane::Embed, monoNowNs())) {
    Pending next = std::move(waitingQueries_.front());
    waitingQueries_.pop_front();
    post(std::move(next));
  }
  if (waitingQueries_.empty() && openQueries_ == 0 && !indexOpen_ && !waitingIndex_.empty() &&
      gate.mayPost(InferenceGate::Lane::Embed, monoNowNs())) {
    Pending next = std::move(waitingIndex_.front());
    waitingIndex_.pop_front();
    post(std::move(next));
  }
  if (waitingQueries_.empty() && (waitingIndex_.empty() || indexOpen_ || openQueries_ > 0)) waitingSinceNs_ = 0;
  else if (waitingSinceNs_ == 0) waitingSinceNs_ = monoNowNs();
  gate.setEmbedWaitingSince(waitingSinceNs_);
}

QJsonObject EmbedClient::jobJson(const EmbedRequest& r) const {
  const bool frames = r.kind == QLatin1String("embed_frames");
  QJsonObject job{{"job_id", r.jobId},
                  {"kind", r.kind},
                  {"camera_id", r.cameraId},
                  {"session_id", r.sessionId},
                  {"generation", num(r.generation)},
                  {"index_version_name", r.versionName},
                  {"sample_interval_ms", r.sampleIntervalMs},
                  {"priority", frames ? QStringLiteral("index") : QStringLiteral("query")},
                  {"clip", QJsonValue::Null},
                  {"gaps", QJsonArray{}},
                  {"limits", QJsonObject{{"max_frames", std::max<int>(1, static_cast<int>(r.frames.size()))}, {"deadline_ms", r.deadlineMs}}}};
  if (frames) {
    QJsonArray list;
    for (qsizetype i = 0; i < r.frames.size(); ++i) {
      const EmbedFrame& f = r.frames[i];
      list.push_back(QJsonObject{{"frame_id", f.frameId},
                                 {"pts_ns", num(f.ptsNs)},
                                 {"recv_mono_ns", 0},
                                 {"capture_utc_ms", num(f.utcMs)},
                                 {"path", QDir::toNativeSeparators(f.path)},
                                 {"index", static_cast<int>(i)}});
    }
    job.insert("frames", list);
  } else {
    job.insert("frames", QJsonArray{});
    job.insert("texts", QJsonArray{r.text});
  }
  return job;
}

void EmbedClient::post(Pending pending) {
  const bool query = pending.request.kind == QLatin1String("embed_text");
  if (!worker_.embedAvailable()) {
    const Callback done = std::move(pending.done);
    done(failure(unavailableReason(), true));
    return;
  }
  QNetworkRequest req(QUrl(worker_.baseUrl() + QStringLiteral("/v1/jobs")));
  req.setRawHeader("Authorization", "Bearer " + worker_.token().toLatin1());
  req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  req.setTransferTimeout(pending.request.deadlineMs + (query ? kQueryTimeoutMarginMs : kIndexTimeoutMarginMs));
  if (query) ++openQueries_;
  else indexOpen_ = true;
  const int64_t postedNs = monoNowNs();
  worker_.gate().opened(InferenceGate::Lane::Embed, postedNs);
  QNetworkReply* reply = network_->post(req, QJsonDocument(jobJson(pending.request)).toJson(QJsonDocument::Compact));
  connect(reply, &QNetworkReply::finished, this, [this, reply, query, postedNs, pending = std::move(pending)] {
    reply->deleteLater();
    if (query) --openQueries_;
    else indexOpen_ = false;
    worker_.gate().closed(InferenceGate::Lane::Embed, reply->error() == QNetworkReply::OperationCanceledError);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject body = status > 0 ? QJsonDocument::fromJson(reply->readAll()).object() : QJsonObject();
    EmbedReply result;
    if (reply->error() == QNetworkReply::OperationCanceledError) {
      result = failure(QStringLiteral("timeout"), true);
    } else if (status == 0) {
      result = failure(QStringLiteral("worker_error: ") + reply->errorString(), true);
    } else if (status != 200) {
      const QJsonObject error = body.value("error").toObject();
      const QString code = error.value("code").toString();
      result = failure(QStringLiteral("worker_http_%1%2").arg(status).arg(code.isEmpty() ? QString() : QStringLiteral(": ") + code +
                                                                                                          QStringLiteral(" ") +
                                                                                                          error.value("message").toString()),
                       status == 503 && code == QLatin1String("deadline_exceeded"));
    } else {
      result = parseEmbedReply(body, pending.request);
    }
    result.roundTripMs = (monoNowNs() - postedNs) / 1'000'000;
    if (!query && result.ok && !pending.request.frames.isEmpty())
      msPerIndexFrame_ = 0.7 * msPerIndexFrame_ + 0.3 * static_cast<double>(result.roundTripMs) / static_cast<double>(pending.request.frames.size());
    pending.done(result);
    pump();
  });
}

}
