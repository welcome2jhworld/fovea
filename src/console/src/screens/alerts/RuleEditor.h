#pragma once
#include "core/AlertTypes.h"
#include "fovea/Api.h"
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <functional>
#include <optional>

class QComboBox;
class QLabel;
class QDoubleSpinBox;
class QLineEdit;
class QPushButton;
class QTimeEdit;
class QToolButton;

namespace fovea::ui {

class CoreClient;
class EventStore;
class ToggleSwitch;
class ZoneCanvas;

// Structured M3 rule editor inside an expanded rule card: camera, zone drawn on
// the live frame, schedule, class, confidence, dwell, severity, actions.
// Saving writes the zone (new or new revision) and then the rule revision. An
// existing rule keeps its camera, and confidence and dwell keep their stored
// values unless the operator changed them.
class RuleEditor : public QWidget {
  Q_OBJECT
public:
  RuleEditor(CoreClient& client, EventStore& store, QWidget* parent = nullptr);

  // nullopt starts a new rule.
  void load(const std::optional<RuleInfo>& rule);
  void setCameras(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses);
  // Where the card's enable toggle is read from when the rule is saved.
  void setEnabledSource(std::function<bool()> source) { enabledSource_ = std::move(source); }

signals:
  void saved(const QString& ruleId);
  void cancelled();

private:
  void onCameraChanged();
  void updateZoneHint();
  void updateSeverityTone();
  void save();
  void saveRule(RuleInfo rule);
  void fail(const QString& message);
  int selectedDays() const;

  CoreClient& client_;
  EventStore& store_;
  std::optional<RuleInfo> base_;
  std::optional<ZoneInfo> loadedZone_;
  QVector<fovea::Camera> cameras_;
  QVector<fovea::CameraStatus> statuses_;
  std::function<bool()> enabledSource_;
  // Camera the points on the canvas belong to.
  QString zoneCameraId_;
  // Label and id pairs currently in the camera combo.
  QStringList cameraItems_;

  QLineEdit* name_ = nullptr;
  QComboBox* camera_ = nullptr;
  ZoneCanvas* zone_ = nullptr;
  QLabel* zoneHint_ = nullptr;
  QVector<QToolButton*> days_;
  QTimeEdit* start_ = nullptr;
  QTimeEdit* end_ = nullptr;
  QComboBox* timeZone_ = nullptr;
  QComboBox* targetClass_ = nullptr;
  QDoubleSpinBox* confidence_ = nullptr;
  QDoubleSpinBox* dwell_ = nullptr;
  double loadedConfidence_ = 0;
  double loadedDwell_ = 0;
  QComboBox* severity_ = nullptr;
  ToggleSwitch* sound_ = nullptr;
  ToggleSwitch* pop_ = nullptr;
  QLabel* error_ = nullptr;
  QPushButton* saveButton_ = nullptr;
  bool saving_ = false;
};

}
