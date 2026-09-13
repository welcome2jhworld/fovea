#pragma once
#include "fovea/Api.h"
#include <QJsonObject>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QToolButton;

namespace fovea::ui {

class CoreClient;
class FrameSurface;
class StatusChip;
class ToggleSwitch;

class ConnectionPage : public QWidget {
  Q_OBJECT
public:
  ConnectionPage(CoreClient& client, QWidget* parent = nullptr);

  // `existing` cameras never echo credentials: the fields stay empty with a "stored" placeholder.
  void load(const fovea::Camera& camera, const QStringList& groups, bool existing);
  QJsonObject formJson() const;
  // Empty when valid; otherwise the message and the field to focus.
  QString validate(QWidget** focusTarget) const;
  QString displayName() const;
  void runTest();

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  enum class TestState { Idle, Testing, Connected, Failed };
  void buildForm();
  void buildTestBlock();
  void buildToggles();
  void applyProtocol();
  void browseFile();
  void copyCommand();
  void showTestResult(const fovea::ConnectionTest& result);
  void setTestState(TestState state, const QString& detail);
  QString kind() const;
  QString transport() const;

  CoreClient& client_;
  QLineEdit* name_ = nullptr;
  QComboBox* group_ = nullptr;
  QLineEdit* code_ = nullptr;
  QComboBox* protocol_ = nullptr;
  QLabel* mainUrlLabel_ = nullptr;
  QLineEdit* mainUrl_ = nullptr;
  QPushButton* browse_ = nullptr;
  QLineEdit* subUrl_ = nullptr;
  QLineEdit* username_ = nullptr;
  QFrame* passwordField_ = nullptr;
  QLineEdit* password_ = nullptr;
  QToolButton* reveal_ = nullptr;
  QComboBox* transport_ = nullptr;
  QSpinBox* timeout_ = nullptr;
  QSpinBox* jitter_ = nullptr;
  FrameSurface* testFrame_ = nullptr;
  StatusChip* testChip_ = nullptr;
  QLabel* handshake_ = nullptr;
  QLabel* resolution_ = nullptr;
  QLabel* codec_ = nullptr;
  QLabel* frameRate_ = nullptr;
  QLabel* bitrate_ = nullptr;
  QLabel* testError_ = nullptr;
  QPushButton* testButton_ = nullptr;
  QPushButton* copyButton_ = nullptr;
  QLabel* copyNote_ = nullptr;
  ToggleSwitch* analytics_ = nullptr;
  ToggleSwitch* record_ = nullptr;
  bool testInFlight_ = false;
  bool replacesStoredCredentials_ = false;
};

}
