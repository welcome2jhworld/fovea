#include "core/AlertDispatcher.h"
#include "core/CoreClient.h"
#include "core/EventStore.h"
#include <QDebug>
#include <QHash>
#include <QJsonArray>
#include <QPointer>
#include <QUrl>
#include <QUuid>
#include <utility>

namespace fovea::ui {

AlertDispatcher::AlertDispatcher(CoreClient& client, EventStore& store, QObject* parent)
    : QObject(parent), client_(client), store_(store),
      consoleId_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
  timer_.setInterval(kIntervalMs);
  connect(&timer_, &QTimer::timeout, this, &AlertDispatcher::poll);
  sound_.setSource(QUrl(QStringLiteral("qrc:/sounds/alert.wav")));
  connect(&sound_, &QSoundEffect::statusChanged, this, &AlertDispatcher::onSoundState);
  connect(&sound_, &QSoundEffect::playingChanged, this, &AlertDispatcher::onSoundState);
}

void AlertDispatcher::setActive(bool active) {
  if (active == timer_.isActive()) return;
  if (!active) {
    timer_.stop();
    return;
  }
  timer_.start();
  poll();
}

void AlertDispatcher::poll() {
  if (inFlight_) return;
  inFlight_ = true;
  client_.pendingAlerts(consoleId_, [this](bool ok, const QJsonDocument& doc, const QString&) {
    inFlight_ = false;
    if (!ok) return;
    QVector<DeliveryInfo> fresh;
    for (const QJsonValue& v : doc.array()) {
      const DeliveryInfo d = DeliveryInfo::fromJson(v.toObject());
      if (d.id.isEmpty() || confirmed_.contains(d.id) || d.state == QLatin1StringView("delivered")) continue;
      if (d.channel == QLatin1StringView("sound")) {
        queueSound(d);
      } else if (d.channel == QLatin1StringView("console")) {
        if (presented_.contains(d.id)) confirm(d);
        else if (!presenting_.contains(d.id)) fresh.push_back(d);
      }
    }
    if (!fresh.isEmpty()) present(fresh);
  }, this);
}

void AlertDispatcher::present(const QVector<DeliveryInfo>& deliveries) {
  for (const DeliveryInfo& d : deliveries) presenting_.insert(d.id);
  // The store answers on its own context; the dispatcher may be gone by then.
  const QPointer<AlertDispatcher> self(this);
  store_.refreshEvents([self, this, deliveries] {
    if (!self) return;
    QHash<QString, QVector<DeliveryInfo>> missing;
    for (const DeliveryInfo& delivery : deliveries) {
      if (store_.findEvent(delivery.eventId)) finishPresent(delivery);
      else missing[delivery.eventId].push_back(delivery);
    }
    for (auto it = missing.cbegin(); it != missing.cend(); ++it) {
      client_.getEvent(it.key(), [this, waiting = it.value()](bool ok, const QJsonDocument& doc, const QString&) {
        if (ok) store_.insertEvent(EventInfo::fromJson(doc.object()));
        for (const DeliveryInfo& d : waiting) {
          if (ok) finishPresent(d);
          else abandonPresent(d);
        }
      }, this);
    }
  });
}

void AlertDispatcher::abandonPresent(const DeliveryInfo& delivery) { presenting_.remove(delivery.id); }

void AlertDispatcher::finishPresent(const DeliveryInfo& delivery) {
  const EventInfo* event = store_.findEvent(delivery.eventId);
  if (!event) return abandonPresent(delivery);
  const QString eventId = event->id;
  const QString cameraId = event->cameraId;
  const QString ruleId = event->ruleId;
  const QPointer<AlertDispatcher> self(this);
  auto show = [self, this, delivery, eventId, cameraId, ruleId] {
    if (!self) return;
    const RuleInfo* rule = store_.findRule(ruleId);
    if (rule && rule->popAction) emit popCameraRequested(cameraId);
    presenting_.remove(delivery.id);
    presented_.insert(delivery.id);
    emit alertPresented(eventId);
    confirm(delivery);
  };
  if (store_.findRule(ruleId)) show();
  else store_.refreshRules(show);
}

void AlertDispatcher::queueSound(const DeliveryInfo& delivery) {
  if (soundPlayed_.contains(delivery.id)) {
    confirm(delivery);
    return;
  }
  if (soundQueued_.contains(delivery.id)) return;
  soundQueued_.insert(delivery.id);
  soundWaiting_.push_back(delivery);
  onSoundState();
}

// Confirms waiting sound deliveries once the chime is audibly playing; a sound that
// cannot load leaves them unconfirmed so the service records the failure.
void AlertDispatcher::onSoundState() {
  if (soundWaiting_.isEmpty()) return;
  if (sound_.status() == QSoundEffect::Error) {
    qWarning() << "alert sound could not be loaded; sound deliveries stay unconfirmed";
    for (const DeliveryInfo& d : std::as_const(soundWaiting_)) soundQueued_.remove(d.id);
    soundWaiting_.clear();
    return;
  }
  if (sound_.status() != QSoundEffect::Ready) return;
  if (!sound_.isPlaying()) {
    sound_.play();
    if (!sound_.isPlaying()) return;
  }
  const QVector<DeliveryInfo> played = std::exchange(soundWaiting_, {});
  for (const DeliveryInfo& d : played) {
    soundQueued_.remove(d.id);
    soundPlayed_.insert(d.id);
    confirm(d);
  }
}

void AlertDispatcher::confirm(const DeliveryInfo& delivery) {
  if (confirmed_.contains(delivery.id) || confirming_.contains(delivery.id)) return;
  confirming_.insert(delivery.id);
  client_.confirmAlert(delivery.id, consoleId_, [this, id = delivery.id](bool ok, const QJsonDocument&, const QString&) {
    confirming_.remove(id);
    if (!ok) return;
    confirmed_.insert(id);
    store_.refreshEvents();
  }, this);
}

}
