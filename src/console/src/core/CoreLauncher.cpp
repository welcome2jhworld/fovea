#include "core/CoreLauncher.h"
#include "core/CoreClient.h"
#include "core/ProcessProbe.h"
#include "fovea/Paths.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <utility>

namespace fovea::ui {

namespace {
QString executableName() {
#ifdef Q_OS_WIN
  return QStringLiteral("fovea-core.exe");
#else
  return QStringLiteral("fovea-core");
#endif
}
}

CoreLauncher::CoreLauncher(CoreClient& client, QObject* parent) : QObject(parent), client_(client) {
  pollTimer_.setInterval(kPollIntervalMs);
  connect(&pollTimer_, &QTimer::timeout, this, &CoreLauncher::pollUntilReady);
}

QString CoreLauncher::coreBinary() const {
  const QString fromEnv = QString::fromLocal8Bit(qgetenv("FOVEA_CORE_BIN"));
  if (!fromEnv.isEmpty()) return fromEnv;
  const QString sibling = QDir(QCoreApplication::applicationDirPath()).filePath(executableName());
  if (QFileInfo::exists(sibling)) return sibling;
  const QString dev = QStringLiteral(FOVEA_DEV_CORE_BIN);
  if (QFileInfo::exists(dev)) return dev;
  return {};
}

void CoreLauncher::setState(State state, const QString& message) {
  if (state_ == state && message_ == message) return;
  state_ = state;
  message_ = message;
  emit stateChanged(state_, message_);
}

void CoreLauncher::start() {
  setState(State::Starting, QStringLiteral("Looking for the service"));
  probe(true);
}

void CoreLauncher::recheck() {
  if (probing_ || pollTimer_.isActive()) return;
  setState(State::Starting, QStringLiteral("Reconnecting to the service"));
  probe(true);
}

void CoreLauncher::probe(bool launchIfDown) {
  launchIfDown_ = launchIfDown_ || launchIfDown;
  if (probing_) return;
  probing_ = true;
  client_.resetEndpoint();
  client_.probeHealth([this](const CoreClient::ProbeResult& result) {
    probing_ = false;
    const bool mayLaunch = std::exchange(launchIfDown_, false);
    if (result.ok) {
      pollTimer_.stop();
      setState(State::Ready, QStringLiteral("Service ready"));
      return;
    }
    const bool nothingServing = result.failure == CoreClient::Failure::NoDiscovery ||
                                result.failure == CoreClient::Failure::Refused;
    const qint64 recorded = recordedCorePid();
    const qint64 alivePid = processAlive(recorded) ? recorded : processAlive(launchedPid_) ? launchedPid_ : 0;
    if (!nothingServing || alivePid != 0) {
      if (!pollTimer_.isActive()) {
        startedAt_.start();
        pollTimer_.start();
      }
      setState(State::Starting, nothingServing
                                    ? QStringLiteral("Waiting for the service to listen (pid %1)").arg(alivePid)
                                    : QStringLiteral("Service is running but not answering: %1").arg(result.error));
      return;
    }
    if (mayLaunch) {
      launch();
    } else if (!pollTimer_.isActive()) {
      setState(State::Failed, QStringLiteral("Service unavailable: %1").arg(result.error));
    }
  }, this);
}

void CoreLauncher::launch() {
  const QString bin = coreBinary();
  if (bin.isEmpty()) {
    setState(State::Failed, QStringLiteral("Service binary not found (set FOVEA_CORE_BIN or place fovea-core next to the console)"));
    return;
  }
  if (!fovea::ensureDataDirs()) {
    setState(State::Failed, QStringLiteral("Cannot create the data directory %1").arg(fovea::dataDir()));
    return;
  }
  QProcess process;
  process.setProgram(bin);
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert(QStringLiteral("FOVEA_DATA_DIR"), fovea::dataDir());
  process.setProcessEnvironment(env);
  process.setWorkingDirectory(fovea::dataDir());
  process.setStandardOutputFile(QProcess::nullDevice());
  process.setStandardErrorFile(QProcess::nullDevice());
  qint64 pid = 0;
  if (!process.startDetached(&pid)) {
    setState(State::Failed, QStringLiteral("Could not start %1: %2").arg(bin, process.errorString()));
    return;
  }
  launchedPid_ = pid;
  setState(State::Starting, QStringLiteral("Starting the service (pid %1)").arg(pid));
  startedAt_.start();
  pollTimer_.start();
}

void CoreLauncher::pollUntilReady() {
  if (startedAt_.elapsed() > kStartupTimeoutMs) {
    pollTimer_.stop();
    setState(State::Failed, QStringLiteral("Service did not answer within %1 s").arg(kStartupTimeoutMs / 1000));
    return;
  }
  probe(false);
}

void CoreLauncher::stopService() {
  client_.shutdownService([this](bool ok, const QJsonDocument&, const QString& error) {
    if (ok) {
      setState(State::Failed, QStringLiteral("Service stopped by the operator"));
    } else {
      setState(state_, QStringLiteral("Stop request failed: %1").arg(error));
    }
  }, this);
}

}
