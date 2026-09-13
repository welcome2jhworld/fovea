#include "screens/monitor/RecordingsPanel.h"
#include "core/CoreClient.h"
#include "fovea/Clock.h"
#include "screens/monitor/RecordingsModel.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QVBoxLayout>

namespace fovea::ui {
namespace tk = tokens;

RecordingsPanel::RecordingsPanel(CoreClient& client, QWidget* parent) : QWidget(parent), client_(client) {
  setObjectName(QStringLiteral("RecordingsPanel"));
  setAttribute(Qt::WA_StyledBackground, true);
  setFixedWidth(tk::size::inspector);

  auto* header = new QWidget(this);
  header->setObjectName(QStringLiteral("RecordingsHeader"));
  header->setAttribute(Qt::WA_StyledBackground, true);
  header->setFixedHeight(tk::size::panelHeader);
  auto* title = new QLabel(QStringLiteral("RECORDINGS"), header);
  title->setProperty("role", QStringLiteral("section-label"));
  title->setFont(Theme::monoLabel());
  cameraLabel_ = new QLabel(header);
  cameraLabel_->setFont(Theme::mono(tk::font::label));
  cameraLabel_->setProperty("tone", QStringLiteral("muted"));
  cameraLabel_->setToolTip(QStringLiteral("Segments and receive gaps of the last 24 hours"));
  auto* headerRow = new QHBoxLayout(header);
  headerRow->setContentsMargins(tk::size::railHeaderPaddingX, 0, tk::size::railHeaderPaddingX, 0);
  headerRow->setSpacing(8);
  headerRow->addWidget(title);
  headerRow->addStretch(1);
  headerRow->addWidget(cameraLabel_);

  message_ = new QLabel(this);
  message_->setObjectName(QStringLiteral("RecordingsMessage"));
  message_->setWordWrap(true);
  message_->setAlignment(Qt::AlignCenter);
  message_->setContentsMargins(16, 20, 16, 20);

  model_ = new RecordingsModel(this);
  auto* delegate = new RecordingRowDelegate(this);
  list_ = new QListView(this);
  list_->setObjectName(QStringLiteral("RecordingsList"));
  list_->setModel(model_);
  list_->setItemDelegate(delegate);
  list_->setFrameShape(QFrame::NoFrame);
  list_->setSelectionMode(QAbstractItemView::NoSelection);
  list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list_->setFocusPolicy(Qt::NoFocus);
  list_->setUniformItemSizes(true);
  list_->viewport()->setAutoFillBackground(false);

  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(0);
  column->addWidget(header);
  column->addWidget(message_);
  column->addWidget(list_, 1);

  timer_.setInterval(kRefreshMs);
  connect(&timer_, &QTimer::timeout, this, &RecordingsPanel::refresh);
  connect(delegate, &RecordingRowDelegate::playClicked, this, &RecordingsPanel::requestPlay);
  connect(list_, &QListView::doubleClicked, this, &RecordingsPanel::requestPlay);
  setCamera(std::nullopt);
}

void RecordingsPanel::setCamera(const std::optional<fovea::Camera>& camera) {
  const bool sameCamera = camera && camera_ && camera->id == camera_->id;
  camera_ = camera;
  cameraLabel_->setText(camera_ ? (camera_->code.isEmpty() ? camera_->name : camera_->code) : QString());
  if (sameCamera) return;
  ++generation_;
  model_->setRows({}, {}, 0);
  if (!camera_) {
    timer_.stop();
    showMessage(QStringLiteral("Select a camera to list its recordings."));
    return;
  }
  showMessage(QStringLiteral("Loading…"));
  if (isVisible()) {
    timer_.start();
    refresh();
  }
}

void RecordingsPanel::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (camera_) {
    timer_.start();
    refresh();
  }
}

void RecordingsPanel::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  timer_.stop();
}

void RecordingsPanel::showMessage(const QString& text) {
  message_->setText(text);
  message_->setVisible(!text.isEmpty());
}

void RecordingsPanel::refresh() {
  if (!camera_) return;
  const int gen = ++generation_;
  const QString id = camera_->id;
  const int64_t now = fovea::utcNowMs();
  refreshedAtUtcMs_ = now;
  segments_.reset();
  gaps_.reset();
  client_.listSegments(id, now - kWindowMs, now, [this, gen](bool ok, const QJsonDocument& doc, const QString& error) {
    if (gen != generation_) return;
    if (!ok) {
      ++generation_;
      showMessage(QStringLiteral("Could not load recordings · %1").arg(error));
      return;
    }
    QVector<fovea::RecordingSegment> rows;
    for (const QJsonValue& v : doc.array()) rows.push_back(fovea::RecordingSegment::fromJson(v.toObject()));
    segments_ = std::move(rows);
    finishRefresh();
  }, this);
  client_.listGaps(id, now - kWindowMs, now, [this, gen](bool ok, const QJsonDocument& doc, const QString& error) {
    if (gen != generation_) return;
    if (!ok) {
      ++generation_;
      showMessage(QStringLiteral("Could not load receive gaps · %1").arg(error));
      return;
    }
    QVector<fovea::ReceiveGap> rows;
    for (const QJsonValue& v : doc.array()) rows.push_back(fovea::ReceiveGap::fromJson(v.toObject()));
    gaps_ = std::move(rows);
    finishRefresh();
  }, this);
}

void RecordingsPanel::finishRefresh() {
  if (!segments_ || !gaps_) return;
  model_->setRows(*segments_, *gaps_, refreshedAtUtcMs_);
  showMessage(model_->rowCount() == 0 ? QStringLiteral("No segments in the last 24 hours.") : QString());
}

void RecordingsPanel::requestPlay(const QModelIndex& index) {
  if (!index.isValid() || !camera_ || !index.data(RecordingsModel::PlayableRole).toBool()) return;
  const QString label = camera_->code.isEmpty() ? camera_->name : QStringLiteral("%1 · %2").arg(camera_->code, camera_->name);
  emit playRequested(model_->rowAt(index).segment, label);
}

}
