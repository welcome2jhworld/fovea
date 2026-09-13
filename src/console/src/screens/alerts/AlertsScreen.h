#pragma once
#include "fovea/Api.h"
#include <QVector>
#include <QWidget>

class QStackedWidget;
class QToolButton;

namespace fovea::ui {

class AlertDetailPanel;
class AlertTable;
class CoreClient;
class EventStore;
class RulesRail;
class ThumbnailCache;

// Alerts & Analytics: sub-tab bar, Alert log (rules rail | alert table | alert detail)
// and the Statistics sub-tab, which is not implemented in M3.
class AlertsScreen : public QWidget {
  Q_OBJECT
public:
  AlertsScreen(CoreClient& client, EventStore& store, ThumbnailCache& thumbnails, QWidget* parent = nullptr);

  void setSnapshot(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses);
  void openEvent(const QString& eventId);
  RulesRail* rules() const { return rules_; }

private:
  void showAlertLog();

  QToolButton* alertLogTab_ = nullptr;
  QStackedWidget* pages_ = nullptr;
  RulesRail* rules_ = nullptr;
  AlertTable* table_ = nullptr;
  AlertDetailPanel* detail_ = nullptr;
};

}
