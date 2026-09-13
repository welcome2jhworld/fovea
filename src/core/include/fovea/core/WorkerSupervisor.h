#pragma once
#include "fovea/Backoff.h"
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <optional>

class QNetworkAccessManager;

namespace fovea::core {

struct WorkerCommand {
  QString program;
  QStringList arguments;
  QString workingDirectory;
  QString source;
};

// Finds the worker CLI: FOVEA_WORKER_CMD (a full command line that runs the
// CLI; "serve" and its options are appended), else the packaged
// <appDir>/worker/.venv (bin/fovea-worker, Scripts\fovea-worker.exe on
// Windows), else a development tree above appDir with worker/.venv and
// worker/fovea_worker (python -m fovea_worker.cli).
std::optional<WorkerCommand> discoverWorker(const QString& appDir, const QString& envCommand);

inline constexpr qint64 kDetectorStuckMs = 30'000;
inline constexpr int kMaxDetectorFailures = 3;

// Why a worker that answers its health checks must still be restarted, empty
// when it may keep running: its detector lane has been inside one job for more
// than kDetectorStuckMs (far above the 2.5 s job deadline, and above a model
// rebuild on CPU after an MPS failure), or the detector reported failed in
// kMaxDetectorFailures consecutive checks (a load that failed is only retried
// by a job, and the scheduler sends none while the detector is failed).
// detectorFailures carries the count between checks.
QString detectorRestartReason(const QJsonObject& health, int* detectorFailures);

// Runs the model worker as a child of the core: token and info files in the
// data directory, health every 2 s, restart with backoff (1..30 s) when it
// exits, fails three health checks in a row, or detectorRestartReason says so;
// the backoff resets after 30 s ready with a detector that has not failed. The
// worker gets --exit-on-stdin-eof, so it also stops when the core dies without
// cleanup, and inherits no descriptor but stdin, stdout and stderr.
// States: disabled (no worker found), starting, ready, failed, stopped.
class WorkerSupervisor : public QObject {
  Q_OBJECT
public:
  WorkerSupervisor(QString dataDir, QString logPath, std::optional<WorkerCommand> command, QObject* parent = nullptr);
  ~WorkerSupervisor() override;

  void start();
  // Closes stdin and terminates; kills after three seconds.
  void stop();

  QString state() const { return state_; }
  bool ready() const { return state_ == QLatin1String("ready"); }
  // The detector lane reported ready or loads lazily on the first job.
  bool detectorAvailable() const;
  QString detectorState() const;
  QString baseUrl() const;
  QString token() const { return token_; }
  QString lastError() const { return lastError_; }
  qint64 pid() const;
  QJsonObject toJson() const;

signals:
  void stateChanged(const QString& state);

private:
  void launch();
  void setState(const QString& state);
  void pollInfo();
  void checkHealth();
  void onHealth(bool ok, const QJsonObject& body, const QString& error);
  void onFinished(int exitCode, QProcess::ExitStatus status);
  void fail(const QString& reason);

  QString dataDir_;
  QString logPath_;
  std::optional<WorkerCommand> command_;
  QNetworkAccessManager* network_ = nullptr;
  QProcess* process_ = nullptr;
  QTimer infoTimer_;
  QTimer healthTimer_;
  QTimer restartTimer_;
  Backoff backoff_;
  QString state_ = QStringLiteral("stopped");
  QString token_;
  QString lastError_;
  QJsonObject health_;
  quint16 port_ = 0;
  int healthFailures_ = 0;
  int detectorFailures_ = 0;
  int restarts_ = 0;
  int64_t launchedMonoNs_ = 0;
  int64_t readySinceMonoNs_ = 0;
  bool healthInFlight_ = false;
  bool stopping_ = false;
};

}
