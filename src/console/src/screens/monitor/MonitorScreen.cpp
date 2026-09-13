#include "screens/monitor/MonitorScreen.h"
#include "core/EventStore.h"
#include "screens/monitor/CameraRail.h"
#include "screens/monitor/DetectionPoller.h"
#include "screens/monitor/LiveEventFeed.h"
#include "screens/monitor/RecordingsPanel.h"
#include "screens/monitor/ServiceStatusLine.h"
#include "screens/monitor/VideoTile.h"
#include "screens/monitor/VideoWall.h"
#include "screens/monitor/WallToolbar.h"
#include <QHBoxLayout>
#include <QSettings>
#include <QVBoxLayout>

namespace fovea::ui {

namespace {
const QString kLayoutKey = QStringLiteral("wall/columns");
const QString kOverlaysKey = QStringLiteral("wall/overlays");
const QString kRecordingsKey = QStringLiteral("monitor/recordings");
}

MonitorScreen::MonitorScreen(CoreClient& client, EventStore& store, QWidget* parent) : QWidget(parent), store_(store) {
  setObjectName(QStringLiteral("MonitorScreen"));

  rail_ = new CameraRail(this);
  recordings_ = new RecordingsPanel(client, this);
  feed_ = new LiveEventFeed(store, this);
  detections_ = new DetectionPoller(client, this);

  auto* wallArea = new QWidget(this);
  wallArea->setObjectName(QStringLiteral("WallArea"));
  wallArea->setAttribute(Qt::WA_StyledBackground, true);
  status_ = new ServiceStatusLine(wallArea);
  toolbar_ = new WallToolbar(wallArea);
  wall_ = new VideoWall(wallArea);
  auto* column = new QVBoxLayout(wallArea);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(0);
  column->addWidget(status_);
  column->addWidget(toolbar_);
  column->addWidget(wall_, 1);

  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(0);
  row->addWidget(rail_);
  row->addWidget(wallArea, 1);
  row->addWidget(recordings_);
  row->addWidget(feed_);

  const QSettings settings;
  const WallLayout saved = wallLayoutFromColumns(settings.value(kLayoutKey, 3).toInt(), WallLayout::ThreeByThree);
  wall_->setWallLayout(saved);
  toolbar_->setWallLayout(saved);
  const bool recordingsVisible = settings.value(kRecordingsKey, false).toBool();
  toolbar_->setRecordingsChecked(recordingsVisible);
  setRecordingsVisible(recordingsVisible);
  const bool overlays = settings.value(kOverlaysKey, true).toBool();
  toolbar_->setOverlaysChecked(overlays);
  setOverlaysEnabled(overlays);

  connect(toolbar_, &WallToolbar::layoutChosen, wall_, &VideoWall::setWallLayout);
  connect(toolbar_, &WallToolbar::saveDefaultRequested, this, [this] {
    QSettings().setValue(kLayoutKey, wallColumns(wall_->wallLayout()));
  });
  connect(toolbar_, &WallToolbar::overlaysToggled, this, [this](bool on) {
    QSettings().setValue(kOverlaysKey, on);
    setOverlaysEnabled(on);
  });
  connect(toolbar_, &WallToolbar::recordingsToggled, this, [this](bool on) {
    QSettings().setValue(kRecordingsKey, on);
    setRecordingsVisible(on);
  });
  connect(toolbar_, &WallToolbar::ruleEnableRequested, &store_, &EventStore::setRuleEnabled);
  connect(toolbar_, &WallToolbar::manageRulesRequested, this, &MonitorScreen::manageRulesRequested);
  connect(status_, &ServiceStatusLine::retryRequested, this, &MonitorScreen::retryServiceRequested);
  connect(rail_, &CameraRail::addCameraRequested, this, &MonitorScreen::addCameraRequested);
  connect(rail_, &CameraRail::cameraSettingsRequested, this, &MonitorScreen::cameraSettingsRequested);
  connect(rail_, &CameraRail::selectedCameraChanged, this, &MonitorScreen::bindRecordings);
  connect(recordings_, &RecordingsPanel::playRequested, this, &MonitorScreen::playbackRequested);
  connect(feed_, &LiveEventFeed::openCaseRequested, this, &MonitorScreen::openCaseRequested);
  connect(wall_, &VideoWall::visibleCamerasChanged, this, [this] {
    updateDetectionCameras();
    updateWallRules();
  });
  connect(&store_, &EventStore::rulesChanged, this, &MonitorScreen::updateWallRules);
  connect(detections_, &DetectionPoller::detectionsArrived, this, [this](const QString& id, const DetectionFrame& frame) {
    if (VideoTile* tile = wall_->tile(id)) tile->setDetections(frame);
  });
  connect(detections_, &DetectionPoller::detectionsUnavailable, this, [this](const QString& id) {
    if (VideoTile* tile = wall_->tile(id)) tile->setDetections(std::nullopt);
  });
}

void MonitorScreen::setSnapshot(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses) {
  cameras_ = cameras;
  statuses_ = statuses;
  rail_->setSnapshot(cameras, statuses);
  wall_->setSnapshot(cameras, statuses);
  feed_->setCameras(cameras);
  bindRecordings(rail_->selectedCameraId());
  updateDetectionCameras();
  updateWallRules();
}

void MonitorScreen::markStatusesStale() {
  QVector<fovea::CameraStatus> stale = statuses_;
  for (fovea::CameraStatus& s : stale) s.stale = true;
  rail_->setSnapshot(cameras_, stale);
  wall_->setSnapshot(cameras_, stale);
}

void MonitorScreen::setOverlaysEnabled(bool enabled) {
  overlays_ = enabled;
  toolbar_->setOverlaysChecked(enabled);
  wall_->setOverlaysEnabled(enabled);
  updateDetectionCameras();
}

void MonitorScreen::popCamera(const QString& cameraId) { wall_->popToFirstSlot(cameraId); }

void MonitorScreen::setRecordingsVisible(bool visible) {
  recordings_->setVisible(visible);
  feed_->setVisible(!visible);
}

void MonitorScreen::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  updateDetectionCameras();
}

void MonitorScreen::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  updateDetectionCameras();
}

void MonitorScreen::updateDetectionCameras() {
  QStringList ids;
  if (overlays_ && isVisible()) {
    for (const QString& id : wall_->visibleCameraIds()) {
      const VideoTile* tile = wall_->tile(id);
      if (tile && tile->analyticsEnabled()) ids.push_back(id);
    }
  }
  detections_->setCameras(ids);
}

void MonitorScreen::updateWallRules() {
  const QStringList visible = wall_->visibleCameraIds();
  QVector<WallRule> rules;
  for (const RuleInfo& r : store_.rules()) {
    if (!visible.contains(r.cameraId)) continue;
    QString camera;
    for (const fovea::Camera& c : std::as_const(cameras_))
      if (c.id == r.cameraId) camera = c.code.isEmpty() ? c.name : c.code;
    rules.push_back({r.id, camera.isEmpty() ? r.name : QStringLiteral("%1 · %2").arg(r.name, camera), r.enabled});
  }
  toolbar_->setWallRules(rules);
}

void MonitorScreen::bindRecordings(const QString& cameraId) {
  for (const fovea::Camera& c : cameras_) {
    if (c.id == cameraId) {
      recordings_->setCamera(c);
      return;
    }
  }
  recordings_->setCamera(std::nullopt);
}

void MonitorScreen::setServiceState(CoreLauncher::State state, const QString& message) {
  status_->setState(state, message);
}

}
