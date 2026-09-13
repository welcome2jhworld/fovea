#pragma once
#include <QString>
#include <optional>

namespace fovea::core {

struct Credentials {
  QString username;
  QString password;
};

class SecretStore {
public:
  virtual ~SecretStore() = default;
  virtual std::optional<Credentials> get(const QString& cameraId) = 0;
  virtual bool set(const QString& cameraId, const Credentials& c) = 0;
  virtual bool remove(const QString& cameraId) = 0;
};

class FileSecretStore : public SecretStore {
public:
  explicit FileSecretStore(QString path);
  std::optional<Credentials> get(const QString& cameraId) override;
  bool set(const QString& cameraId, const Credentials& c) override;
  bool remove(const QString& cameraId) override;

private:
  bool load();
  bool save();
  QString path_;
  QString cache_;
};

}
