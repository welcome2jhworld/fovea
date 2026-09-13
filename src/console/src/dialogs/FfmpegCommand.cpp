#include "dialogs/FfmpegCommand.h"
#include <QStringList>
#include <QUrl>

namespace fovea::ui {

QString shellQuote(const QString& value) {
  QString escaped = value;
  escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
  return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

QString ffplayCommand(const StreamSource& source) {
  QStringList parts{QStringLiteral("ffplay")};
  if (source.kind == QLatin1StringView("file")) {
    parts << QStringLiteral("-i") << shellQuote(source.url);
    return parts.join(QLatin1Char(' '));
  }
  QUrl url = QUrl::fromUserInput(source.url.trimmed());
  if (!source.username.isEmpty()) url.setUserName(source.username, QUrl::DecodedMode);
  if (!source.password.isEmpty()) url.setPassword(source.password, QUrl::DecodedMode);
  const QString transport = source.transport == QLatin1StringView("udp") ? QStringLiteral("udp") : QStringLiteral("tcp");
  parts << QStringLiteral("-rtsp_transport") << transport << QStringLiteral("-i")
        << shellQuote(url.toString(QUrl::FullyEncoded));
  return parts.join(QLatin1Char(' '));
}

}
