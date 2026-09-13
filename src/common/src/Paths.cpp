#include "fovea/Paths.h"
#include <QDir>
#include <QStandardPaths>

namespace fovea {
QString dataDir() {
  const QByteArray env = qgetenv("FOVEA_DATA_DIR");
  if (!env.isEmpty()) return QString::fromLocal8Bit(env);
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Fovea";
}
QString recordingsDir() { return dataDir() + "/recordings"; }
QString logsDir() { return dataDir() + "/logs"; }
QString coreInfoPath() { return dataDir() + "/core.json"; }
QString tokenPath() { return dataDir() + "/core.token"; }
QString databasePath() { return dataDir() + "/fovea.sqlite"; }
QString secretsPath() { return dataDir() + "/secrets.json"; }
bool ensureDataDirs() {
  return QDir().mkpath(dataDir()) && QDir().mkpath(recordingsDir()) && QDir().mkpath(logsDir());
}
}
