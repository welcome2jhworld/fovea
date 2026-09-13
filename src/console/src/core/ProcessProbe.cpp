#include "core/ProcessProbe.h"
#include "fovea/Paths.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#endif

namespace fovea::ui {

bool processAlive(qint64 pid) {
  if (pid <= 0) return false;
#ifdef Q_OS_WIN
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
  DWORD code = 0;
  const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  CloseHandle(h);
  return alive;
#else
  return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

qint64 recordedCorePid() {
  QFile f(fovea::coreInfoPath());
  if (!f.open(QIODevice::ReadOnly)) return 0;
  return static_cast<qint64>(QJsonDocument::fromJson(f.readAll()).object().value(QLatin1StringView("pid")).toDouble());
}

}
