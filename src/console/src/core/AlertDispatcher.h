#pragma once
#include "core/AlertTypes.h"
#include <QObject>
#include <QSet>
#include <QSoundEffect>
#include <QString>
#include <QTimer>
#include <QVector>

namespace fovea::ui {

class CoreClient;
class EventStore;

// Polls GET /v1/alerts/pending, presents each console delivery once and plays
// the bundled chime for sound deliveries. Presenting puts the event into the
// EventStore, which every alert view (tab badge, live feed, alert log) renders,
// and pops the camera to the main view when the rule asks for it; the new
// deliveries of one poll share a single events refresh. A console delivery is
// confirmed only once it was presented, a sound delivery once the chime plays;
// a poll that lists a delivery still being presented leaves it alone.
class AlertDispatcher : public QObject {
  Q_OBJECT
public:
  static constexpr int kIntervalMs = 1000;

  AlertDispatcher(CoreClient& client, EventStore& store, QObject* parent = nullptr);

  void setActive(bool active);

signals:
  void alertPresented(const QString& eventId);
  void popCameraRequested(const QString& cameraId);

private:
  void poll();
  void present(const QVector<DeliveryInfo>& deliveries);
  void finishPresent(const DeliveryInfo& delivery);
  void abandonPresent(const DeliveryInfo& delivery);
  void queueSound(const DeliveryInfo& delivery);
  void onSoundState();
  void confirm(const DeliveryInfo& delivery);

  CoreClient& client_;
  EventStore& store_;
  QString consoleId_;
  QTimer timer_;
  QSoundEffect sound_;
  bool inFlight_ = false;
  QSet<QString> presenting_;
  QSet<QString> presented_;
  QSet<QString> confirmed_;
  QSet<QString> confirming_;
  QSet<QString> soundQueued_;
  QSet<QString> soundPlayed_;
  QVector<DeliveryInfo> soundWaiting_;
};

}
