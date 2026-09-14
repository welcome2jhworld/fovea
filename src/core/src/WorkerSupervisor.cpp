#include "fovea/core/WorkerSupervisor.h"
#include "fovea/Clock.h"
#include "fovea/Redact.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSaveFile>
#include <algorithm>
#include <utility>

namespace fovea::core {
namespace {

constexpr int kInfoPollMs = 200;
constexpr int kHealthIntervalMs = 2000;
// Until the detector reports ready, so analysis resumes soon after a model load.
constexpr int kHealthLoadingIntervalMs = 500;
constexpr int kHealthTimeoutMs = 1500;
constexpr int kMaxHealthFailures = 3;
constexpr int64_t kStartTimeoutNs = 60'000'000'000;
constexpr int64_t kStableResetNs = 30'000'000'000;
constexpr int kStopWaitMs = 3000;
constexpr int kDevTreeDepth = 6;

#ifdef _WIN32
const QString kVenvBin = QStringLiteral("Scripts");
const QString kExe = QStringLiteral(".exe");
#else
const QString kVenvBin = QStringLiteral("bin");
const QString kExe;
#endif

QString workerInfoPath(const QString& dataDir) { return dataDir + QStringLiteral("/worker.json"); }
QString workerTokenPath(const QString& dataDir) { return dataDir + QStringLiteral("/worker.token"); }

QString writeFreshToken(const QString& path) {
  QByteArray bytes(32, Qt::Uninitialized);
  QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(bytes.data()), bytes.size() / 4);
  const QString token = QString::fromLatin1(bytes.toHex());
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly)) return {};
  f.write(token.toLatin1());
  if (!f.commit()) return {};
  QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  return token;
}

}

bool InferenceGate::sharesGpu(const QJsonObject& health) {
  const auto mps = [](const QJsonValue& v) { return v.toString() == QLatin1String("mps"); };
  if (mps(health.value(QStringLiteral("detector_device"))) || mps(health.value(QStringLiteral("embed_device")))) return true;
  const QJsonObject versions = health.value(QStringLiteral("embed_versions")).toObject();
  return std::any_of(versions.begin(), versions.end(), [&mps](const QJsonValue& v) { return mps(v.toObject().value(QStringLiteral("device"))); });
}

void InferenceGate::setShared(bool shared) {
  if (shared_ == shared) return;
  shared_ = shared;
  if (!shared) emit released();
}

bool InferenceGate::mayPost(Lane lane, int64_t nowMonoNs) const {
  if (!shared_) return true;
  const int other = lane == Lane::Detect ? index(Lane::Embed) : index(Lane::Detect);
  if (open_[other] > 0 || draining_[other]) return false;
  const bool embedOverdue = embedWaitingSinceNs_ > 0 && nowMonoNs - embedWaitingSinceNs_ > kEmbedMaxWaitNs;
  return lane == Lane::Detect ? !embedOverdue : (!detectWaiting_ || embedOverdue);
}

void InferenceGate::opened(Lane lane, int64_t nowMonoNs) {
  ++open_[index(lane)];
  if (lane == Lane::Detect) lastDetectNs_ = nowMonoNs;
}

void InferenceGate::closed(Lane lane, bool abandoned) {
  const int i = index(lane);
  open_[i] = std::max(0, open_[i] - 1);
  if (abandoned && shared_) draining_[i] = true;
  emit released();
}

void InferenceGate::laneIdle(Lane lane) {
  if (!std::exchange(draining_[index(lane)], false)) return;
  emit released();
}

void InferenceGate::reset() {
  draining_[0] = draining_[1] = false;
  emit released();
}

void InferenceGate::setDetectWaiting(bool waiting) {
  if (std::exchange(detectWaiting_, waiting) && !waiting) emit released();
}

void InferenceGate::setEmbedWaitingSince(int64_t monoNs) { embedWaitingSinceNs_ = monoNs; }

bool InferenceGate::detectRecently(int64_t nowMonoNs) const { return lastDetectNs_ > 0 && nowMonoNs - lastDetectNs_ < kRecentDetectNs; }

QString detectorRestartReason(const QJsonObject& health, int* detectorFailures) {
  const qint64 busyMs = health.value(QStringLiteral("detector_busy_ms")).toInteger();
  if (busyMs > kDetectorStuckMs) return QStringLiteral("detector lane stuck in one job for %1 ms").arg(busyMs);
  if (health.value(QStringLiteral("detector_state")).toString() != QLatin1String("failed")) {
    *detectorFailures = 0;
    return {};
  }
  if (++*detectorFailures < kMaxDetectorFailures) return {};
  const QString error = health.value(QStringLiteral("detector_load_error")).toString();
  return QStringLiteral("detector failed%1").arg(error.isEmpty() ? QString() : QStringLiteral(": ") + error);
}

std::optional<WorkerCommand> discoverWorker(const QString& appDir, const QString& envCommand) {
  if (!envCommand.trimmed().isEmpty()) {
    QStringList parts = QProcess::splitCommand(envCommand);
    if (parts.isEmpty()) return std::nullopt;
    WorkerCommand c;
    c.program = parts.takeFirst();
    c.arguments = parts;
    c.source = QStringLiteral("env");
    return c;
  }
  const QString packaged = appDir + QStringLiteral("/worker/.venv/") + kVenvBin + QStringLiteral("/fovea-worker") + kExe;
  if (QFileInfo(packaged).isExecutable()) return WorkerCommand{packaged, {}, appDir + QStringLiteral("/worker"), QStringLiteral("app")};
  QDir dir(appDir);
  for (int depth = 0; depth < kDevTreeDepth; ++depth) {
    const QString worker = dir.absoluteFilePath(QStringLiteral("worker"));
    const QString python = worker + QStringLiteral("/.venv/") + kVenvBin + QStringLiteral("/python") + kExe;
    if (QFileInfo::exists(worker + QStringLiteral("/fovea_worker/cli.py")) && QFileInfo(python).isExecutable())
      return WorkerCommand{python, {QStringLiteral("-m"), QStringLiteral("fovea_worker.cli")}, worker, QStringLiteral("dev")};
    if (!dir.cdUp()) break;
  }
  return std::nullopt;
}

WorkerSupervisor::WorkerSupervisor(QString dataDir, QString logPath, std::optional<WorkerCommand> command, QObject* parent)
    : QObject(parent), dataDir_(std::move(dataDir)), logPath_(std::move(logPath)), command_(std::move(command)),
      network_(new QNetworkAccessManager(this)), backoff_(1000, 30000, 2.0) {
  infoTimer_.setInterval(kInfoPollMs);
  healthTimer_.setInterval(kHealthIntervalMs);
  restartTimer_.setSingleShot(true);
  connect(&infoTimer_, &QTimer::timeout, this, &WorkerSupervisor::pollInfo);
  connect(&healthTimer_, &QTimer::timeout, this, &WorkerSupervisor::checkHealth);
  connect(&restartTimer_, &QTimer::timeout, this, &WorkerSupervisor::launch);
}

WorkerSupervisor::~WorkerSupervisor() { stop(); }

void WorkerSupervisor::start() {
  stopping_ = false;
  if (!command_) {
    lastError_ = QStringLiteral("no worker found (set FOVEA_WORKER_CMD or install worker/.venv)");
    qWarning("worker: %s; analytics report quality unknown", qPrintable(lastError_));
    setState(QStringLiteral("disabled"));
    return;
  }
  launch();
}

void WorkerSupervisor::setState(const QString& state) {
  if (state_ == state) return;
  state_ = state;
  if (state == QLatin1String("ready")) readySinceMonoNs_ = monoNowNs();
  emit stateChanged(state_);
}

void WorkerSupervisor::launch() {
  if (stopping_ || !command_) return;
  QFile::remove(workerInfoPath(dataDir_));
  token_ = writeFreshToken(workerTokenPath(dataDir_));
  if (token_.isEmpty()) {
    fail(QStringLiteral("cannot write the worker token file"));
    return;
  }
  port_ = 0;
  health_ = {};
  gate_.reset();
  healthFailures_ = 0;
  detectorFailures_ = 0;
  auto* process = new QProcess(this);
  process_ = process;
#ifndef _WIN32
  // Without it the child inherits every descriptor the core has open, such as recording files.
  process->setUnixProcessParameters(QProcess::UnixProcessFlag::CloseFileDescriptors);
#endif
  process->setProgram(command_->program);
  QStringList arguments = command_->arguments;
  arguments << QStringLiteral("serve") << QStringLiteral("--backend") << QStringLiteral("dry") << QStringLiteral("--detector")
            << QStringLiteral("rfdetr") << QStringLiteral("--embedder") << QStringLiteral("transformers") << QStringLiteral("--port") << QStringLiteral("0") << QStringLiteral("--token-file") << workerTokenPath(dataDir_)
            << QStringLiteral("--info-file") << workerInfoPath(dataDir_) << QStringLiteral("--warmup") << QStringLiteral("--exit-on-stdin-eof");
  process->setArguments(arguments);
  if (!command_->workingDirectory.isEmpty()) process->setWorkingDirectory(command_->workingDirectory);
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
  process->setProcessEnvironment(env);
  process->setProcessChannelMode(QProcess::MergedChannels);
  process->setStandardOutputFile(logPath_, QIODevice::Append);
  connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
    if (process != process_) return;
    onFinished(code, status);
  });
  connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
    if (process != process_ || error != QProcess::FailedToStart) return;
    fail(QStringLiteral("failed to start: %1").arg(process->errorString()));
  });
  launchedMonoNs_ = monoNowNs();
  setState(QStringLiteral("starting"));
  qInfo("worker: starting (%s)", qPrintable(QFileInfo(command_->program).fileName()));
  process->start(QIODevice::ReadWrite);
  infoTimer_.start();
}

void WorkerSupervisor::pollInfo() {
  if (!process_) return;
  QFile f(workerInfoPath(dataDir_));
  if (f.open(QIODevice::ReadOnly)) {
    const QJsonObject info = QJsonDocument::fromJson(f.readAll()).object();
    const int port = info.value(QStringLiteral("port")).toInt();
    if (port > 0 && port < 65536) {
      port_ = static_cast<quint16>(port);
      infoTimer_.stop();
      healthTimer_.start();
      checkHealth();
      return;
    }
  }
  if (monoNowNs() - launchedMonoNs_ > kStartTimeoutNs) fail(QStringLiteral("no info file within 60 s"));
}

void WorkerSupervisor::checkHealth() {
  if (!process_ || port_ == 0 || healthInFlight_) return;
  healthInFlight_ = true;
  QNetworkRequest req(QUrl(baseUrl() + QStringLiteral("/v1/health")));
  req.setRawHeader("Authorization", "Bearer " + token_.toLatin1());
  req.setTransferTimeout(kHealthTimeoutMs);
  QNetworkReply* reply = network_->get(req);
  const QProcess* process = process_;
  connect(reply, &QNetworkReply::finished, this, [this, reply, process] {
    reply->deleteLater();
    healthInFlight_ = false;
    if (process != process_) return;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
    if (reply->error() == QNetworkReply::NoError && status == 200) onHealth(true, body, {});
    else onHealth(false, {}, reply->error() != QNetworkReply::NoError ? reply->errorString() : QStringLiteral("HTTP %1").arg(status));
  });
}

void WorkerSupervisor::onHealth(bool ok, const QJsonObject& body, const QString& error) {
  if (!ok) {
    ++healthFailures_;
    qWarning("worker: health check failed (%d/%d): %s", healthFailures_, kMaxHealthFailures, qPrintable(redactText(error)));
    if (healthFailures_ >= kMaxHealthFailures) fail(QStringLiteral("%1 health checks failed: %2").arg(kMaxHealthFailures).arg(error));
    return;
  }
  healthFailures_ = 0;
  const QString previousDetector = detectorState();
  health_ = body;
  gate_.setShared(InferenceGate::sharesGpu(body));
  if (body.value(QStringLiteral("detector_busy_ms")).toInteger() == 0) gate_.laneIdle(InferenceGate::Lane::Detect);
  if (body.value(QStringLiteral("embed_busy_ms")).toInteger() == 0) gate_.laneIdle(InferenceGate::Lane::Embed);
  healthTimer_.setInterval(detectorState() == QLatin1String("loading") ? kHealthLoadingIntervalMs : kHealthIntervalMs);
  if (detectorState() != previousDetector)
    qInfo("worker: detector %s%s", qPrintable(detectorState()),
          body.value(QStringLiteral("detector_load_error")).toString().isEmpty()
              ? ""
              : qPrintable(QStringLiteral(" (") + body.value(QStringLiteral("detector_load_error")).toString() + QLatin1Char(')')));
  if (const QString problem = detectorRestartReason(body, &detectorFailures_); !problem.isEmpty()) {
    fail(problem);
    return;
  }
  if (!ready()) {
    qInfo("worker: ready on port %u", port_);
    setState(QStringLiteral("ready"));
  } else if (detectorState() != QLatin1String("failed") && monoNowNs() - readySinceMonoNs_ > kStableResetNs) {
    backoff_.reset();
  }
}

void WorkerSupervisor::onFinished(int exitCode, QProcess::ExitStatus status) {
  if (stopping_) return;
  fail(status == QProcess::CrashExit ? QStringLiteral("worker crashed") : QStringLiteral("worker exited with code %1").arg(exitCode));
}

void WorkerSupervisor::fail(const QString& reason) {
  infoTimer_.stop();
  healthTimer_.stop();
  lastError_ = reason;
  if (QProcess* process = std::exchange(process_, nullptr)) {
    if (process->state() == QProcess::NotRunning) {
      process->deleteLater();
    } else {
      connect(process, &QProcess::finished, process, &QObject::deleteLater);
      process->kill();
    }
  }
  gate_.reset();
  port_ = 0;
  health_ = {};
  const int64_t delayMs = backoff_.nextDelayMs();
  ++restarts_;
  qWarning("worker: %s; restarting in %lld ms", qPrintable(redactText(reason)), static_cast<long long>(delayMs));
  setState(QStringLiteral("failed"));
  restartTimer_.start(static_cast<int>(delayMs));
}

void WorkerSupervisor::stop() {
  stopping_ = true;
  infoTimer_.stop();
  healthTimer_.stop();
  restartTimer_.stop();
  if (QProcess* process = std::exchange(process_, nullptr)) {
    if (process->state() != QProcess::NotRunning) {
      process->closeWriteChannel();
#ifndef _WIN32
      process->terminate();
#endif
      if (!process->waitForFinished(kStopWaitMs)) {
        process->kill();
        process->waitForFinished(1000);
      }
    }
    delete process;
  }
  if (state_ != QLatin1String("disabled")) setState(QStringLiteral("stopped"));
}

bool WorkerSupervisor::detectorAvailable() const {
  const QString s = detectorState();
  return ready() && (s == QLatin1String("ready") || s == QLatin1String("unloaded"));
}

bool WorkerSupervisor::embedAvailable() const {
  const QString s = detectorState();
  return ready() && (s == QLatin1String("ready") || s == QLatin1String("failed") || s == QLatin1String("unavailable"));
}

QString WorkerSupervisor::detectorState() const { return health_.value(QStringLiteral("detector_state")).toString(); }

QString WorkerSupervisor::baseUrl() const { return QStringLiteral("http://127.0.0.1:%1").arg(port_); }

qint64 WorkerSupervisor::pid() const { return process_ ? process_->processId() : 0; }

QJsonObject WorkerSupervisor::toJson() const {
  QJsonObject o{{"state", state_},
                {"source", command_ ? command_->source : QString()},
                {"pid", static_cast<double>(pid())},
                {"port", port_},
                {"restarts", restarts_},
                {"last_error", lastError_}};
  for (const char* key : {"detector", "detector_model", "detector_state", "detector_device", "detector_load_ms",
                          "detector_load_error", "detector_busy_ms", "detector_turnaround_ms", "embedder", "embed_state",
                          "embed_device", "embed_busy_ms", "embed_turnaround_ms", "embed_versions"})
    if (health_.contains(QLatin1String(key))) o.insert(QLatin1String(key), health_.value(QLatin1String(key)));
  return o;
}

}
