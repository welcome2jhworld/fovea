#include "screens/monitor/MonitorScreen.h"
#include "screens/monitor/CameraRail.h"
#include "screens/monitor/RecordingsPanel.h"
#include "screens/monitor/ServiceStatusLine.h"
#include "screens/monitor/VideoWall.h"
#include "screens/monitor/WallToolbar.h"
#include <QHBoxLayout>
#include <QSettings>
#include <QVBoxLayout>

namespace fovea::ui {

namespace {
const QString kLayoutKey = QStringLiteral("wall/columns");
}

MonitorScreen::MonitorScreen(CoreClient& client, QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("MonitorScreen"));

  rail_ = new CameraRail(this);
  recordings_ = new RecordingsPanel(client, this);

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

  const WallLayout saved = wallLayoutFromColumns(QSettings().value(kLayoutKey, 3).toInt(), WallLayout::ThreeByThree);
  wall_->setWallLayout(saved);
  toolbar_->setWallLayout(saved);

  connect(toolbar_, &WallToolbar::layoutChosen, wall_, &VideoWall::setWallLayout);
  connect(toolbar_, &WallToolbar::saveDefaultRequested, this, [this] {
    QSettings().setValue(kLayoutKey, wallColumns(wall_->wallLayout()));
  });
  connect(status_, &ServiceStatusLine::retryRequested, this, &MonitorScreen::retryServiceRequested);
  connect(rail_, &CameraRail::addCameraRequested, this, &MonitorScreen::addCameraRequested);
  connect(rail_, &CameraRail::cameraSettingsRequested, this, &MonitorScreen::cameraSettingsRequested);
  connect(rail_, &CameraRail::selectedCameraChanged, this, &MonitorScreen::bindRecordings);
  connect(recordings_, &RecordingsPanel::playRequested, this, &MonitorScreen::playbackRequested);
}

void MonitorScreen::setSnapshot(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses) {
  cameras_ = cameras;
  statuses_ = statuses;
  rail_->setSnapshot(cameras, statuses);
  wall_->setSnapshot(cameras, statuses);
  bindRecordings(rail_->selectedCameraId());
}

void MonitorScreen::markStatusesStale() {
  QVector<fovea::CameraStatus> stale = statuses_;
  for (fovea::CameraStatus& s : stale) s.stale = true;
  rail_->setSnapshot(cameras_, stale);
  wall_->setSnapshot(cameras_, stale);
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
