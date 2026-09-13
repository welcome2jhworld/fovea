#pragma once
#include "core/AlertDispatcher.h"
#include "core/CoreClient.h"
#include "core/CoreLauncher.h"
#include "core/EventStore.h"
#include "core/StatusPoller.h"
#include "core/ThumbnailCache.h"
#include "fovea/Api.h"
#include <QMainWindow>
#include <QPoint>
#include <QRect>
#include <QVector>

class QStackedWidget;

namespace fovea::ui {

class AlertsScreen;
class CameraSettingsDialog;
class DialogHost;
class MainTabBar;
class MonitorScreen;
class TitleBar;

class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  // Test hook: "camera-dialog", "camera-dialog-test", "camera-dialog-test-fail" or "playback"
  // opens that dialog once cameras arrive; "alerts", "alert-detail", "rule-editor" and
  // "monitor-feed" open those views once rules and events arrive too.
  void setScreenshotView(const QString& view) { screenshotView_ = view; }

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void closeEvent(QCloseEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;

private:
  static constexpr int kResizeGrip = 6;
  static constexpr int kExitDrainMs = 1500;
  static constexpr int kAlertsTab = 2;
  Qt::Edges edgesAt(const QPoint& pos) const;
  void toggleMaximized();
  void confirmStopService();
  void onSnapshot(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses);
  CameraSettingsDialog* openCameraDialog(const QString& cameraId);
  void openPlayback(const fovea::RecordingSegment& segment, const QString& cameraLabel);
  void openScreenshotView();
  void openAlertsScreenshotView();
  void openEventInAlerts(const QString& eventId);

  CoreClient client_;
  CoreLauncher launcher_;
  StatusPoller poller_;
  EventStore events_;
  AlertDispatcher dispatcher_;
  ThumbnailCache thumbnails_;
  TitleBar* titleBar_ = nullptr;
  MainTabBar* tabBar_ = nullptr;
  QStackedWidget* stack_ = nullptr;
  MonitorScreen* monitor_ = nullptr;
  AlertsScreen* alertsScreen_ = nullptr;
  DialogHost* dialogs_ = nullptr;
  QVector<fovea::Camera> cameras_;
  QString screenshotView_;
  bool screenshotViewOpened_ = false;
  Qt::Edges resizing_;
  QPoint resizeOrigin_;
  QRect resizeGeometry_;
};

}
