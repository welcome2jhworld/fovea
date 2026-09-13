#include "dialogs/CameraSettingsDialog.h"
#include "core/CoreClient.h"
#include "dialogs/ConnectionPage.h"
#include "dialogs/RecordingPage.h"
#include "screens/NotImplementedState.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStackedWidget>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kTabRow = 32;
constexpr int kTabListPaddingX = 8;
}

CameraSettingsDialog::CameraSettingsDialog(CoreClient& client, const QVector<fovea::Camera>& cameras,
                                           const std::optional<fovea::Camera>& existing, QWidget* parent)
    : DialogFrame(parent), client_(client), existing_(existing) {
  setObjectName(QStringLiteral("CameraSettingsDialog"));
  setTitle(QStringLiteral("Camera settings"));
  setSubtitle(existing_ ? QStringLiteral("%1 · %2").arg(existing_->code, existing_->name) : QStringLiteral("NEW CAMERA"));

  tabs_ = new QListWidget(body());
  tabs_->setObjectName(QStringLiteral("DialogTabList"));
  tabs_->setFixedWidth(tk::size::dialogTabList);
  tabs_->setFrameShape(QFrame::NoFrame);
  tabs_->setFocusPolicy(Qt::NoFocus);
  tabs_->setSpacing(1);
  tabs_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  tabs_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  for (const char* name : {"Connection", "Stream", "Analytics", "Recording", "Placement"}) {
    auto* item = new QListWidgetItem(QLatin1StringView(name), tabs_);
    item->setSizeHint(QSize(tk::size::dialogTabList - 2 * kTabListPaddingX, kTabRow));
  }

  pages_ = new QStackedWidget(body());
  pages_->setObjectName(QStringLiteral("DialogPages"));
  connection_ = new ConnectionPage(client_, pages_);
  recording_ = new RecordingPage(pages_);
  pages_->addWidget(connection_);
  pages_->addWidget(new NotImplementedState(QStringLiteral("Stream settings are not implemented in this build."),
                                            QStringLiteral("ARRIVES WITH M2"), pages_));
  pages_->addWidget(new NotImplementedState(QStringLiteral("Analytics settings are not implemented in this build."),
                                            QStringLiteral("ARRIVES WITH M3"), pages_));
  pages_->addWidget(recording_);
  pages_->addWidget(new NotImplementedState(QStringLiteral("Placement is not implemented in this build."),
                                            QStringLiteral("ARRIVES WITH M3"), pages_));

  auto* row = new QHBoxLayout();
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(0);
  row->addWidget(tabs_);
  row->addWidget(pages_, 1);
  bodyLayout()->addLayout(row);

  removeButton_ = new QPushButton(QStringLiteral("Remove camera"), footer());
  removeButton_->setObjectName(QStringLiteral("RemoveCamera"));
  removeButton_->setProperty("role", QStringLiteral("destructive-ghost"));
  Theme::setVariant(removeButton_, "lg");
  removeButton_->setCursor(Qt::PointingHandCursor);
  removeButton_->setVisible(existing_.has_value());
  error_ = new QLabel(footer());
  error_->setObjectName(QStringLiteral("DialogError"));
  error_->setProperty("tone", QStringLiteral("critical"));
  error_->hide();
  cancelButton_ = new QPushButton(QStringLiteral("Cancel"), footer());
  cancelButton_->setObjectName(QStringLiteral("CancelButton"));
  cancelButton_->setProperty("role", QStringLiteral("secondary"));
  Theme::setVariant(cancelButton_, "lg");
  cancelButton_->setCursor(Qt::PointingHandCursor);
  saveButton_ = new QPushButton(QStringLiteral("Save camera"), footer());
  saveButton_->setObjectName(QStringLiteral("SaveCamera"));
  saveButton_->setProperty("role", QStringLiteral("primary"));
  Theme::setVariant(saveButton_, "lg");
  saveButton_->setCursor(Qt::PointingHandCursor);
  saveButton_->setDefault(true);
  footerLayout()->addWidget(removeButton_);
  footerLayout()->addSpacing(8);
  footerLayout()->addWidget(error_, 1);
  footerLayout()->addStretch(1);
  footerLayout()->addWidget(cancelButton_);
  footerLayout()->addWidget(saveButton_);

  connect(tabs_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
  connect(cancelButton_, &QPushButton::clicked, this, &DialogFrame::closeRequested);
  connect(saveButton_, &QPushButton::clicked, this, &CameraSettingsDialog::save);
  connect(removeButton_, &QPushButton::clicked, this, &CameraSettingsDialog::remove);

  fovea::Camera initial = existing_.value_or(fovea::Camera{});
  if (!existing_) initial.code = nextFreeCode(cameras);
  connection_->load(initial, groupNames(cameras), existing_.has_value());
  recording_->setSegmentSeconds(initial.segmentSeconds);
  tabs_->setCurrentRow(0);
}

QStringList CameraSettingsDialog::groupNames(const QVector<fovea::Camera>& cameras) {
  QStringList names;
  QSet<QString> seen;
  for (const fovea::Camera& c : cameras) {
    if (c.groupName.isEmpty() || seen.contains(c.groupName)) continue;
    seen.insert(c.groupName);
    names.push_back(c.groupName);
  }
  return names;
}

QString CameraSettingsDialog::nextFreeCode(const QVector<fovea::Camera>& cameras) {
  static const QRegularExpression pattern(QStringLiteral("^CAM-(\\d+)$"), QRegularExpression::CaseInsensitiveOption);
  int highest = 0;
  for (const fovea::Camera& c : cameras) {
    const QRegularExpressionMatch m = pattern.match(c.code.trimmed());
    if (m.hasMatch()) highest = std::max(highest, m.captured(1).toInt());
  }
  return QStringLiteral("CAM-%1").arg(std::max(highest + 1, static_cast<int>(cameras.size()) + 1), 2, 10, QLatin1Char('0'));
}

void CameraSettingsDialog::runConnectionTest() {
  tabs_->setCurrentRow(0);
  connection_->runTest();
}

void CameraSettingsDialog::setBusy(bool busy) {
  busy_ = busy;
  saveButton_->setEnabled(!busy);
  removeButton_->setEnabled(!busy);
  saveButton_->setText(busy ? QStringLiteral("Saving…") : QStringLiteral("Save camera"));
}

void CameraSettingsDialog::showError(const QString& message) {
  error_->setText(message);
  error_->setVisible(!message.isEmpty());
}

void CameraSettingsDialog::save() {
  if (busy_) return;
  QWidget* focusTarget = nullptr;
  const QString problem = connection_->validate(&focusTarget);
  if (!problem.isEmpty()) {
    tabs_->setCurrentRow(0);
    showError(problem);
    if (focusTarget) focusTarget->setFocus();
    return;
  }
  showError({});
  QJsonObject body = connection_->formJson();
  body.insert("segment_seconds", recording_->segmentSeconds());
  if (existing_) body.insert("enabled", existing_->enabled);
  setBusy(true);
  auto done = [this](bool ok, const QJsonDocument& doc, const QString& error) {
    setBusy(false);
    if (!ok) {
      showError(QStringLiteral("Save failed · %1").arg(error));
      return;
    }
    emit saved(fovea::Camera::fromJson(doc.object()));
    emit closeRequested();
  };
  if (existing_) client_.updateCamera(existing_->id, body, done, this);
  else client_.createCamera(body, done, this);
}

void CameraSettingsDialog::remove() {
  if (busy_ || !existing_) return;
  const auto answer = QMessageBox::question(
      window(), QStringLiteral("Remove camera"),
      QStringLiteral("Remove %1 · %2? Recording stops; existing segments stay on disk.").arg(existing_->code, existing_->name),
      QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
  if (answer != QMessageBox::Yes) return;
  showError({});
  setBusy(true);
  const QString id = existing_->id;
  client_.deleteCamera(id, [this, id](bool ok, const QJsonDocument&, const QString& error) {
    setBusy(false);
    if (!ok) {
      showError(QStringLiteral("Remove failed · %1").arg(error));
      return;
    }
    emit removed(id);
    emit closeRequested();
  }, this);
}

}
