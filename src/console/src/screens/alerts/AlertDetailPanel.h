#pragma once
#include "core/AlertTypes.h"
#include "fovea/Api.h"
#include <QVector>
#include <QWidget>
#include <array>
#include <optional>

class QLabel;
class QPushButton;
class QStackedWidget;

namespace fovea::ui {

class CoreClient;
class EventStore;
class EvidencePlayer;
class ThumbnailCache;
class ToneChip;

// Alert detail (340): evidence player and state, summary, metadata, activity
// from the event's evaluations, and the operator actions.
class AlertDetailPanel : public QWidget {
  Q_OBJECT
public:
  AlertDetailPanel(CoreClient& client, EventStore& store, ThumbnailCache& thumbnails, QWidget* parent = nullptr);
  void setCameras(const QVector<fovea::Camera>& cameras);
  void setEventId(const QString& eventId);

private:
  void fetch();
  void apply(const EventInfo& event);
  void setMeta(int row, const QString& value);
  QString cameraLabel(const QString& cameraId) const;

  CoreClient& client_;
  EventStore& store_;
  QVector<fovea::Camera> cameras_;
  QString eventId_;
  int fetchSeq_ = 0;
  std::optional<EventInfo> shown_;

  ToneChip* severity_ = nullptr;
  QStackedWidget* pages_ = nullptr;
  EvidencePlayer* player_ = nullptr;
  ToneChip* evidenceChip_ = nullptr;
  QLabel* evidenceReason_ = nullptr;
  QLabel* title_ = nullptr;
  QLabel* detail_ = nullptr;
  QVector<QLabel*> metaValues_;
  QLabel* activity_ = nullptr;
  std::array<QPushButton*, 3> review_{};
  QPushButton* acknowledge_ = nullptr;
  QPushButton* resolve_ = nullptr;
  QLabel* error_ = nullptr;
};

}
