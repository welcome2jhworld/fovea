#pragma once
#include "fovea/Api.h"
#include <QListView>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <optional>

class QLabel;

namespace fovea::ui {

class CoreClient;
class RecordingsModel;

// Provisional M1 panel (no handoff design): the selected camera's segments and
// receive gaps from the last 24 h, refreshed every 5 s.
class RecordingsPanel : public QWidget {
  Q_OBJECT
public:
  static constexpr int kRefreshMs = 5000;
  static constexpr int64_t kWindowMs = 24LL * 3600 * 1000;

  RecordingsPanel(CoreClient& client, QWidget* parent = nullptr);

  void setCamera(const std::optional<fovea::Camera>& camera);
  QString cameraId() const { return camera_ ? camera_->id : QString(); }

signals:
  void playRequested(const fovea::RecordingSegment& segment, const QString& cameraLabel);

protected:
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

private:
  void refresh();
  void finishRefresh();
  void showMessage(const QString& text);
  void requestPlay(const QModelIndex& index);

  CoreClient& client_;
  std::optional<fovea::Camera> camera_;
  RecordingsModel* model_ = nullptr;
  QListView* list_ = nullptr;
  QLabel* cameraLabel_ = nullptr;
  QLabel* message_ = nullptr;
  QTimer timer_;
  int generation_ = 0;
  std::optional<QVector<fovea::RecordingSegment>> segments_;
  std::optional<QVector<fovea::ReceiveGap>> gaps_;
  int64_t refreshedAtUtcMs_ = 0;
};

}
