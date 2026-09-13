#include "fovea/Token.h"
#include <QFile>
#include <QRandomGenerator>
#include <QSaveFile>

namespace fovea {
QString loadToken(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return {};
  return QString::fromLatin1(f.readAll()).trimmed();
}

QString loadOrCreateToken(const QString& path) {
  const QString existing = loadToken(path);
  if (existing.size() >= 32) return existing;
  QByteArray bytes(32, Qt::Uninitialized);
  QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(bytes.data()), bytes.size() / 4);
  const QString token = QString::fromLatin1(bytes.toHex());
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly)) return {};
  f.write(token.toLatin1());
  f.commit();
  QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  return token;
}

bool tokenEquals(const QString& a, const QString& b) {
  const QByteArray x = a.toUtf8();
  const QByteArray y = b.toUtf8();
  if (x.size() != y.size()) return false;
  unsigned char diff = 0;
  for (int i = 0; i < x.size(); ++i) diff |= static_cast<unsigned char>(x[i] ^ y[i]);
  return diff == 0;
}
}
