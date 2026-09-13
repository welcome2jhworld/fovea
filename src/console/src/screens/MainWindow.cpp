#include "screens/MainWindow.h"
#include "dialogs/CameraSettingsDialog.h"
#include "dialogs/DialogHost.h"
#include "dialogs/PlaybackDialog.h"
#include "fovea/Clock.h"
#include "screens/MainTabBar.h"
#include "screens/NotImplementedState.h"
#include "screens/TitleBar.h"
#include "screens/monitor/MonitorScreen.h"
#include "theme/Tokens.h"
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWindow>

namespace fovea::ui {
namespace tk = tokens;

namespace {
bool isTextInput(QWidget* w) {
  return qobject_cast<QLineEdit*>(w) || qobject_cast<QTextEdit*>(w) || qobject_cast<QPlainTextEdit*>(w) ||
         qobject_cast<QAbstractSpinBox*>(w);
}
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), launcher_(client_, this), poller_(client_, this) {
  setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
  setWindowTitle(QStringLiteral("Fovea"));
  setMinimumSize(tk::size::windowMinWidth, tk::size::windowMinHeight);

  auto* root = new QWidget(this);
  root->setObjectName(QStringLiteral("AppRoot"));
  root->setAttribute(Qt::WA_StyledBackground, true);
  setCentralWidget(root);

  titleBar_ = new TitleBar(root);
  tabBar_ = new MainTabBar(root);
  stack_ = new QStackedWidget(root);
  stack_->setObjectName(QStringLiteral("ScreenStack"));
  monitor_ = new MonitorScreen(client_, stack_);
  stack_->addWidget(monitor_);
  stack_->addWidget(new NotImplementedState(QStringLiteral("Search is not implemented in this build."),
                                            QStringLiteral("ARRIVES WITH M4"), stack_));
  stack_->addWidget(new NotImplementedState(QStringLiteral("Alerts & Analytics are not implemented in this build."),
                                            QStringLiteral("ARRIVES WITH M3"), stack_));
  stack_->addWidget(new NotImplementedState(QStringLiteral("Model Train is not implemented in this build."),
                                            QStringLiteral("ARRIVES AFTER M5"), stack_));

  auto* column = new QVBoxLayout(root);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(0);
  column->addWidget(titleBar_);
  column->addWidget(tabBar_);
  column->addWidget(stack_, 1);

  connect(titleBar_, &TitleBar::minimizeRequested, this, &QWidget::showMinimized);
  connect(titleBar_, &TitleBar::maximizeRequested, this, &MainWindow::toggleMaximized);
  connect(titleBar_, &TitleBar::closeRequested, this, &QWidget::close);
  connect(titleBar_->stopServiceAction(), &QAction::triggered, this, &MainWindow::confirmStopService);
  connect(tabBar_->tabs(), &NavTabs::currentChanged, stack_, &QStackedWidget::setCurrentIndex);

  connect(&launcher_, &CoreLauncher::stateChanged, this, [this](CoreLauncher::State state, const QString& message) {
    monitor_->setServiceState(state, message);
    poller_.setActive(state == CoreLauncher::State::Ready);
    if (state != CoreLauncher::State::Ready) monitor_->markStatusesStale();
  });
  connect(&poller_, &StatusPoller::snapshot, this, &MainWindow::onSnapshot);
  connect(&poller_, &StatusPoller::pollFailed, monitor_, &MonitorScreen::markStatusesStale);
  connect(&poller_, &StatusPoller::unreachable, &launcher_, &CoreLauncher::recheck);
  connect(monitor_, &MonitorScreen::retryServiceRequested, &launcher_, &CoreLauncher::start);
  connect(monitor_, &MonitorScreen::addCameraRequested, this, [this] { openCameraDialog(QString()); });
  connect(monitor_, &MonitorScreen::cameraSettingsRequested, this, &MainWindow::openCameraDialog);
  connect(monitor_, &MonitorScreen::playbackRequested, this, &MainWindow::openPlayback);

  dialogs_ = new DialogHost(this);
  connect(dialogs_, &DialogHost::opened, this, [this] { centralWidget()->setEnabled(false); });
  connect(dialogs_, &DialogHost::closed, this, [this] { centralWidget()->setEnabled(true); });

  qApp->installEventFilter(this);
  launcher_.start();
}

// Dialogs talk to client_, a member destroyed before QObject deletes child widgets.
MainWindow::~MainWindow() { dialogs_->shutdown(); }

// Closing a dialog issues requests (DELETE /v1/playback/{id}); they need the event loop, which
// is gone by the time the destructor runs.
void MainWindow::closeEvent(QCloseEvent* event) {
  poller_.setActive(false);
  dialogs_->shutdown();
  client_.drain(kExitDrainMs);
  QMainWindow::closeEvent(event);
}

void MainWindow::onSnapshot(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses) {
  cameras_ = cameras;
  monitor_->setSnapshot(cameras, statuses);
  openScreenshotView();
}

CameraSettingsDialog* MainWindow::openCameraDialog(const QString& cameraId) {
  std::optional<fovea::Camera> existing;
  for (const fovea::Camera& c : cameras_) {
    if (c.id == cameraId) existing = c;
  }
  auto* dialog = new CameraSettingsDialog(client_, cameras_, existing);
  connect(dialog, &CameraSettingsDialog::saved, &poller_, &StatusPoller::pollNow);
  connect(dialog, &CameraSettingsDialog::removed, &poller_, &StatusPoller::pollNow);
  dialogs_->open(dialog);
  return dialog;
}

void MainWindow::openPlayback(const fovea::RecordingSegment& segment, const QString& cameraLabel) {
  auto* dialog = new PlaybackDialog(client_, segment, cameraLabel);
  dialogs_->open(dialog);
  dialog->start();
}

void MainWindow::openScreenshotView() {
  if (screenshotView_.isEmpty() || screenshotViewOpened_ || cameras_.isEmpty()) return;
  screenshotViewOpened_ = true;
  const fovea::Camera first = cameras_.first();
  if (screenshotView_ == QLatin1StringView("camera-dialog")) {
    openCameraDialog(first.id);
    return;
  }
  if (screenshotView_ == QLatin1StringView("camera-dialog-test")) {
    openCameraDialog(first.id)->runConnectionTest();
    return;
  }
  if (screenshotView_ == QLatin1StringView("camera-dialog-test-fail")) {
    openCameraDialog(cameras_.last().id)->runConnectionTest();
    return;
  }
  if (screenshotView_ != QLatin1StringView("playback")) return;
  const int64_t now = fovea::utcNowMs();
  client_.listSegments(first.id, now - 24LL * 3600 * 1000, now, [this, first](bool ok, const QJsonDocument& doc, const QString&) {
    if (!ok) return;
    const QJsonArray rows = doc.array();
    for (const QJsonValue& v : rows) {
      const fovea::RecordingSegment s = fovea::RecordingSegment::fromJson(v.toObject());
      if (s.state != QLatin1StringView("finalized")) continue;
      openPlayback(s, QStringLiteral("%1 · %2").arg(first.code, first.name));
      return;
    }
  }, this);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  dialogs_->setGeometry(rect());
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::KeyPress && !dialogs_->isOpen()) {
    auto* key = static_cast<QKeyEvent*>(event);
    const bool plain = (key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
    if (plain && key->key() >= Qt::Key_1 && key->key() <= Qt::Key_4 && !isTextInput(QApplication::focusWidget())) {
      tabBar_->tabs()->setCurrentIndex(key->key() - Qt::Key_1);
      return true;
    }
  }
  return QMainWindow::eventFilter(watched, event);
}

void MainWindow::toggleMaximized() {
  if (isMaximized()) showNormal();
  else showMaximized();
}

void MainWindow::confirmStopService() {
  const auto answer = QMessageBox::question(
      this, QStringLiteral("Stop service"),
      QStringLiteral("Stop the Fovea service? Every camera stops recording until the service is started again."),
      QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
  if (answer == QMessageBox::Yes) launcher_.stopService();
}

Qt::Edges MainWindow::edgesAt(const QPoint& pos) const {
  Qt::Edges edges;
  if (pos.x() <= kResizeGrip) edges |= Qt::LeftEdge;
  if (pos.x() >= width() - kResizeGrip) edges |= Qt::RightEdge;
  if (pos.y() <= kResizeGrip) edges |= Qt::TopEdge;
  if (pos.y() >= height() - kResizeGrip) edges |= Qt::BottomEdge;
  return edges;
}

void MainWindow::mousePressEvent(QMouseEvent* event) {
  const Qt::Edges edges = edgesAt(event->position().toPoint());
  if (event->button() == Qt::LeftButton && edges && !isMaximized()) {
    if (!windowHandle() || !windowHandle()->startSystemResize(edges)) {
      resizing_ = edges;
      resizeOrigin_ = event->globalPosition().toPoint();
      resizeGeometry_ = geometry();
    }
    event->accept();
    return;
  }
  QMainWindow::mousePressEvent(event);
}

void MainWindow::mouseMoveEvent(QMouseEvent* event) {
  if (!resizing_) {
    QMainWindow::mouseMoveEvent(event);
    return;
  }
  const QPoint delta = event->globalPosition().toPoint() - resizeOrigin_;
  QRect g = resizeGeometry_;
  if (resizing_ & Qt::LeftEdge) g.setLeft(std::min(g.left() + delta.x(), g.right() - minimumWidth()));
  if (resizing_ & Qt::RightEdge) g.setRight(std::max(g.right() + delta.x(), g.left() + minimumWidth()));
  if (resizing_ & Qt::TopEdge) g.setTop(std::min(g.top() + delta.y(), g.bottom() - minimumHeight()));
  if (resizing_ & Qt::BottomEdge) g.setBottom(std::max(g.bottom() + delta.y(), g.top() + minimumHeight()));
  setGeometry(g);
}

void MainWindow::mouseReleaseEvent(QMouseEvent* event) {
  resizing_ = Qt::Edges();
  QMainWindow::mouseReleaseEvent(event);
}

}
