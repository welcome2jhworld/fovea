#include "fovea/Redact.h"
#include <QRegularExpression>

namespace fovea {
namespace {
const QRegularExpression& credentialPattern() {
  static const QRegularExpression re(QStringLiteral("(://)([^/@\\s]+)@"));
  return re;
}
}
QString redactUrl(const QString& url) {
  QString out = url;
  out.replace(credentialPattern(), QStringLiteral("\\1***@"));
  return out;
}
QString redactText(const QString& text) {
  QString out = redactUrl(text);
  static const QRegularExpression pw(QStringLiteral("(\"password\"\\s*:\\s*\")[^\"]*(\")"));
  out.replace(pw, QStringLiteral("\\1***\\2"));
  return out;
}
}
