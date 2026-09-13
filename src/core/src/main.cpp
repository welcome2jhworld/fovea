#include "fovea/Clock.h"
#include "fovea/Paths.h"
#include "fovea/Redact.h"
#include "fovea/Token.h"
#include "fovea/core/ApiServer.h"
#include "fovea/core/CameraManager.h"
#include "fovea/core/MediaProbe.h"
#include "fovea/core/PlaybackManager.h"
#include "fovea/core/SecretStore.h"
#include "fovea/core/Store.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QTextStream>
#include <gst/gst.h>
#include <csignal>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

QFile* g_logFile = nullptr;

void messageHandler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
  const char* level = type == QtDebugMsg ? "debug" : type == QtInfoMsg ? "info" : type == QtWarningMsg ? "warn" : "error";
  const QString line = QStringLiteral("%1 %2 %3\n").arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), level, fovea::redactText(msg));
  QTextStream(stderr) << line;
  if (g_logFile) {
    g_logFile->write(line.toUtf8());
    g_logFile->flush();
  }
}

#ifndef _WIN32
int g_signalFds[2] = {-1, -1};
void onSignal(int) {
  const char c = 1;
  const ssize_t r = ::write(g_signalFds[0], &c, 1);
  (void)r;
}
#endif

}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("fovea-core");
  QCoreApplication::setApplicationVersion("0.1.0");

  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addOption({"data-dir", "Data directory (overrides FOVEA_DATA_DIR)", "path"});
  parser.addOption({"port", "Loopback port (default: random free port)", "port", "0"});
  parser.addOption({"min-free-mb", "Free disk floor below which recording pauses", "mb", "2048"});
  parser.process(app);
  if (parser.isSet("data-dir")) qputenv("FOVEA_DATA_DIR", parser.value("data-dir").toLocal8Bit());

  if (!fovea::ensureDataDirs()) {
    QTextStream(stderr) << "cannot create data directory " << fovea::dataDir() << "\n";
    return 2;
  }
  QFile logFile(fovea::logsDir() + "/core.log");
  if (logFile.open(QIODevice::Append | QIODevice::WriteOnly)) g_logFile = &logFile;
  qInstallMessageHandler(messageHandler);

  QLockFile lock(fovea::dataDir() + "/core.lock");
  lock.setStaleLockTime(0);
  if (!lock.tryLock(100)) {
    qCritical("another fovea-core owns %s", qPrintable(fovea::dataDir()));
    return 3;
  }

  gst_init(nullptr, nullptr);

  fovea::core::CoreConfig config;
  config.dataDir = fovea::dataDir();
  config.recordingsDir = fovea::recordingsDir();
  config.minFreeBytes = parser.value("min-free-mb").toLongLong() * 1024 * 1024;
  const QByteArray envFloor = qgetenv("FOVEA_MIN_FREE_MB");
  if (!envFloor.isEmpty()) config.minFreeBytes = envFloor.toLongLong() * 1024 * 1024;

  fovea::core::Store store;
  if (!store.open(fovea::databasePath())) {
    qCritical("cannot open database: %s", qPrintable(store.lastError()));
    return 4;
  }
  const auto recovery = store.recoverOnStartup(fovea::core::probeSegmentFile, fovea::utcNowMs(), config.recordingsDir);
  qInfo("recovery: sessions closed %d, gaps closed %d, segments finalized %d, damaged %d, missing %d, files adopted %d,"
        " quarantined %d",
        recovery.sessionsClosed, recovery.gapsClosed, recovery.segmentsFinalized, recovery.segmentsDamaged,
        recovery.segmentsMissing, recovery.filesAdopted, recovery.filesQuarantined);

  fovea::core::FileSecretStore secrets(fovea::secretsPath());
  const QString token = fovea::loadOrCreateToken(fovea::tokenPath());
  if (token.isEmpty()) {
    qCritical("cannot create token file");
    return 5;
  }

  fovea::core::CameraManager cameras(store, secrets, config);
  fovea::core::PlaybackManager playback(store, config);
  fovea::core::ApiServer api(cameras, playback, store, token);
  const int64_t startedUtcMs = fovea::utcNowMs();
  api.setStartedUtcMs(startedUtcMs);
  if (!api.listen(static_cast<quint16>(parser.value("port").toUInt()))) {
    qCritical("cannot bind loopback port");
    return 6;
  }

  auto shutdown = [&] {
    qInfo("shutting down");
    playback.closeAll();
    cameras.stopAll([] {
      QFile::remove(fovea::coreInfoPath());
      QCoreApplication::quit();
    });
  };
  api.setShutdownHandler(shutdown);

#ifndef _WIN32
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, g_signalFds) == 0) {
    auto* notifier = new QSocketNotifier(g_signalFds[1], QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated, &app, [&, notifier] {
      char c;
      const ssize_t r = ::read(g_signalFds[1], &c, 1);
      (void)r;
      notifier->setEnabled(false);
      shutdown();
    });
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
  }
#endif

  {
    QSaveFile info(fovea::coreInfoPath());
    if (info.open(QIODevice::WriteOnly)) {
      info.write(QJsonDocument(QJsonObject{{"port", api.port()}, {"pid", static_cast<double>(QCoreApplication::applicationPid())},
                                           {"started_utc_ms", static_cast<double>(startedUtcMs)}, {"version", "0.1.0"}})
                     .toJson(QJsonDocument::Compact));
      info.commit();
    }
  }

  cameras.start();
  qInfo("fovea-core listening on 127.0.0.1:%u, data %s", api.port(), qPrintable(fovea::dataDir()));
  const int rc = app.exec();
  g_logFile = nullptr;
  return rc;
}
