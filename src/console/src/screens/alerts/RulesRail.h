#pragma once
#include "core/AlertTypes.h"
#include "fovea/Api.h"
#include <QHash>
#include <QVector>
#include <QWidget>
#include <optional>

class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace fovea::ui {

class CoreClient;
class EventStore;
class RuleEditor;
class ToggleSwitch;

// Painted chip row of a collapsed rule card: scope, severity, hits today.
class RuleChips : public QWidget {
  Q_OBJECT
public:
  explicit RuleChips(QWidget* parent = nullptr);
  void set(const QString& scope, const QString& severity, const QString& hits);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  QString scope_;
  QString severity_;
  QString hits_;
};

// One rule: name, enable toggle, Edit; collapsed it shows the generated trigger
// sentence and chips, expanded it hosts the structured editor.
class RuleCard : public QWidget {
  Q_OBJECT
public:
  RuleCard(CoreClient& client, EventStore& store, QWidget* parent = nullptr);

  // nullopt is a new rule that exists only in this card until it is saved.
  void setRule(const std::optional<RuleInfo>& rule, const QString& sentence, const QString& scope, const QString& hits);
  void setCameras(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses);
  void setEditing(bool editing);
  bool isEditing() const { return editing_; }

signals:
  void editRequested();
  void editorClosed(const QString& savedRuleId);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  CoreClient& client_;
  EventStore& store_;
  std::optional<RuleInfo> rule_;
  QVBoxLayout* column_ = nullptr;
  QLabel* name_ = nullptr;
  QPushButton* edit_ = nullptr;
  ToggleSwitch* toggle_ = nullptr;
  QWidget* summary_ = nullptr;
  QLabel* sentence_ = nullptr;
  RuleChips* chips_ = nullptr;
  RuleEditor* editor_ = nullptr;
  bool editing_ = false;
};

// Rules rail (400): header with counts and New rule, paginated cards, pager.
class RulesRail : public QWidget {
  Q_OBJECT
public:
  static constexpr int kPageSize = 8;

  RulesRail(CoreClient& client, EventStore& store, QWidget* parent = nullptr);
  void setCameras(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses);
  void editRule(const QString& ruleId);
  void newRule();

private:
  void rebuild();
  void closeEditor(RuleCard* card, const QString& savedRuleId);
  QString cameraLabel(const QString& cameraId) const;

  CoreClient& client_;
  EventStore& store_;
  QVector<fovea::Camera> cameras_;
  QVector<fovea::CameraStatus> statuses_;
  QLabel* counts_ = nullptr;
  QScrollArea* scroll_ = nullptr;
  QVBoxLayout* list_ = nullptr;
  QLabel* empty_ = nullptr;
  QLabel* range_ = nullptr;
  QWidget* pager_ = nullptr;
  QHash<QString, RuleCard*> cards_;
  RuleCard* draft_ = nullptr;
  int page_ = 0;
};

}
