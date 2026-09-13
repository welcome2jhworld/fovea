#include "util/Format.h"
#include <QDateTime>

namespace fovea::ui {

QString codecLabel(const QString& codec) {
  const QString c = codec.toLower();
  if (c.contains(QLatin1StringView("h264")) || c.contains(QLatin1StringView("avc"))) return QStringLiteral("H.264");
  if (c.contains(QLatin1StringView("h265")) || c.contains(QLatin1StringView("hevc"))) return QStringLiteral("H.265");
  if (c.contains(QLatin1StringView("jpeg")) || c.contains(QLatin1StringView("mjpg"))) return QStringLiteral("MJPEG");
  return codec.toUpper();
}

QString clockLabel(int64_t ms) {
  const int64_t totalSeconds = std::max<int64_t>(0, ms / 1000);
  return QStringLiteral("%1:%2").arg(totalSeconds / 60, 2, 10, QLatin1Char('0')).arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
}

QString durationLabel(int64_t ms) {
  if (ms < 0) return QStringLiteral("—");
  if (ms < 60'000) return QStringLiteral("%1 s").arg(QString::number(static_cast<double>(ms) / 1000.0, 'f', ms % 1000 ? 1 : 0));
  const int64_t seconds = ms / 1000;
  return QStringLiteral("%1 min %2 s").arg(seconds / 60).arg(seconds % 60);
}

QString localTimeLabel(int64_t utcMs) {
  const QDateTime at = QDateTime::fromMSecsSinceEpoch(utcMs).toLocalTime();
  const bool today = at.date() == QDate::currentDate();
  return at.toString(today ? QStringLiteral("HH:mm:ss") : QStringLiteral("MM-dd HH:mm:ss"));
}

QString bitrateLabel(double kbps) {
  if (kbps <= 0) return QStringLiteral("—");
  return QStringLiteral("%1 Mbps").arg(QString::number(kbps / 1000.0, 'f', 1));
}

}
