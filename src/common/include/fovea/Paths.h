#pragma once
#include <QString>

namespace fovea {
QString dataDir();
QString recordingsDir();
QString logsDir();
QString coreInfoPath();
QString tokenPath();
QString databasePath();
QString secretsPath();
bool ensureDataDirs();
}
