#pragma once
#include "screens/monitor/WallLayout.h"
#include "widgets/SelectButton.h"
#include <QAbstractButton>
#include <QIcon>
#include <QVector>
#include <QWidget>

class QToolButton;

namespace fovea::ui {

class LayoutPopup;

// "Rules: N applied ▾": prefix primary, count accent, caret muted.
class RulesButton : public QAbstractButton {
  Q_OBJECT
public:
  explicit RulesButton(QWidget* parent = nullptr);
  void setApplied(int count);
  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  QString value_;
  QIcon caret_;
};

struct WallRule {
  QString id;
  QString label;
  bool enabled = false;
};

class WallToolbar : public QWidget {
  Q_OBJECT
public:
  explicit WallToolbar(QWidget* parent = nullptr);
  void setWallLayout(WallLayout layout);
  // Rules of the cameras on the wall; the button counts the enabled ones.
  void setWallRules(const QVector<WallRule>& rules);
  // Programmatic state; the toggled signals report operator clicks only.
  void setOverlaysChecked(bool checked);
  void setRecordingsChecked(bool checked);

signals:
  void layoutChosen(WallLayout layout);
  void saveDefaultRequested();
  void overlaysToggled(bool checked);
  void recordingsToggled(bool checked);
  void ruleEnableRequested(const QString& ruleId, bool enabled);
  void manageRulesRequested();

private:
  void showRulesMenu();

  RulesButton* rulesButton_ = nullptr;
  QToolButton* overlaysButton_ = nullptr;
  QToolButton* recordingsButton_ = nullptr;
  QVector<WallRule> rules_;
  SelectButton* layoutButton_ = nullptr;
  LayoutPopup* popup_ = nullptr;
  WallLayout layout_ = WallLayout::ThreeByThree;
};

}
