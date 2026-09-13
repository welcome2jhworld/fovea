#pragma once
#include "dialogs/DialogFrame.h"
#include "fovea/Api.h"
#include <QVector>
#include <optional>

class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;

namespace fovea::ui {

class ConnectionPage;
class CoreClient;
class RecordingPage;

class CameraSettingsDialog : public DialogFrame {
  Q_OBJECT
public:
  // `existing` empty = add mode; the other cameras supply group names and the next free code.
  CameraSettingsDialog(CoreClient& client, const QVector<fovea::Camera>& cameras,
                       const std::optional<fovea::Camera>& existing, QWidget* parent = nullptr);

  // Test hook: presses "Test connection" on the Connection tab.
  void runConnectionTest();

signals:
  void saved(const fovea::Camera& camera);
  void removed(const QString& cameraId);

private:
  void save();
  void remove();
  void setBusy(bool busy);
  void showError(const QString& message);
  static QString nextFreeCode(const QVector<fovea::Camera>& cameras);
  static QStringList groupNames(const QVector<fovea::Camera>& cameras);

  CoreClient& client_;
  std::optional<fovea::Camera> existing_;
  QListWidget* tabs_ = nullptr;
  QStackedWidget* pages_ = nullptr;
  ConnectionPage* connection_ = nullptr;
  RecordingPage* recording_ = nullptr;
  QLabel* error_ = nullptr;
  QPushButton* removeButton_ = nullptr;
  QPushButton* cancelButton_ = nullptr;
  QPushButton* saveButton_ = nullptr;
  bool busy_ = false;
};

}
