#include "fovea/core/SecretStore.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace fovea::core {

FileSecretStore::FileSecretStore(QString path) : path_(std::move(path)) {}

bool FileSecretStore::load() {
  QFile f(path_);
  if (!f.open(QIODevice::ReadOnly)) { cache_.clear(); return true; }
  cache_ = QString::fromUtf8(f.readAll());
  return true;
}

bool FileSecretStore::save() {
  QSaveFile f(path_);
  if (!f.open(QIODevice::WriteOnly)) return false;
  f.write(cache_.toUtf8());
  if (!f.commit()) return false;
  QFile::setPermissions(path_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  return true;
}

std::optional<Credentials> FileSecretStore::get(const QString& cameraId) {
  load();
  const QJsonObject root = QJsonDocument::fromJson(cache_.toUtf8()).object();
  if (!root.contains(cameraId)) return std::nullopt;
  const QJsonObject o = root.value(cameraId).toObject();
  return Credentials{o.value("username").toString(), o.value("password").toString()};
}

bool FileSecretStore::set(const QString& cameraId, const Credentials& c) {
  load();
  QJsonObject root = QJsonDocument::fromJson(cache_.toUtf8()).object();
  root.insert(cameraId, QJsonObject{{"username", c.username}, {"password", c.password}});
  cache_ = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
  return save();
}

bool FileSecretStore::remove(const QString& cameraId) {
  load();
  QJsonObject root = QJsonDocument::fromJson(cache_.toUtf8()).object();
  root.remove(cameraId);
  cache_ = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
  return save();
}

}
