#include "core/EventStore.h"
#include "alerts/AlertLogic.h"
#include "core/CoreClient.h"
#include "fovea/Clock.h"
#include <QJsonArray>
#include <algorithm>
#include <memory>

namespace fovea::ui {

EventStore::EventStore(CoreClient& client, QObject* parent) : QObject(parent), client_(client) {
  eventsTimer_.setInterval(kEventsIntervalMs);
  rulesTimer_.setInterval(kRulesIntervalMs);
  connect(&eventsTimer_, &QTimer::timeout, this, [this] { refreshEvents(); });
  connect(&rulesTimer_, &QTimer::timeout, this, [this] { refreshRules(); });
}

void EventStore::setActive(bool active) {
  if (active == eventsTimer_.isActive()) return;
  if (!active) {
    eventsTimer_.stop();
    rulesTimer_.stop();
    eventsStale_ = true;
    rulesStale_ = true;
    unresolvedTotal_.reset();
    const QString unavailable = QStringLiteral("service unavailable");
    eventsError_ = unavailable;
    rulesError_ = unavailable;
    emit eventsChanged();
    emit rulesChanged();
    return;
  }
  eventsTimer_.start();
  rulesTimer_.start();
  refreshRules();
  refreshEvents();
}

// The list and the counts are applied together, so the badge never disagrees with the list it came with.
void EventStore::refreshEvents(std::function<void()> done) {
  struct Pending {
    QJsonDocument events;
    std::optional<int> unresolved;
    int remaining = 2;
    bool ok = false;
    QString error;
  };
  const int seq = ++eventsRequested_;
  auto pending = std::make_shared<Pending>();
  auto finish = [this, seq, pending, done = std::move(done)] {
    if (--pending->remaining > 0) return;
    if (seq > eventsApplied_) {
      eventsApplied_ = seq;
      if (pending->ok) {
        eventsStale_ = false;
        const bool countChanged = unresolvedTotal_ != pending->unresolved;
        unresolvedTotal_ = pending->unresolved;
        if (!applyEvents(pending->events) && countChanged) emit eventsChanged();
      }
      setEventsError(pending->ok ? QString() : pending->error);
    }
    if (done) done();
  };
  client_.listEvents(fovea::utcNowMs() - kEventWindowMs, kEventLimit,
                     [pending, finish](bool ok, const QJsonDocument& doc, const QString& error) {
    pending->ok = ok;
    if (ok) pending->events = doc;
    else pending->error = error;
    finish();
  }, this);
  client_.eventCounts([pending, finish](bool ok, const QJsonDocument& doc, const QString&) {
    const QJsonValue unresolved = doc.object().value(QStringLiteral("unresolved"));
    if (ok && unresolved.isDouble()) pending->unresolved = unresolved.toInt();
    finish();
  }, this);
}

void EventStore::setEventsError(const QString& error) {
  if (error == eventsError_) return;
  eventsError_ = error;
  emit eventsChanged();
}

void EventStore::setRulesError(const QString& error) {
  if (error == rulesError_) return;
  rulesError_ = error;
  emit rulesChanged();
}

bool EventStore::applyEvents(const QJsonDocument& doc) {
  const QByteArray body = doc.toJson(QJsonDocument::Compact);
  const bool first = !eventsLoaded_;
  eventsLoaded_ = true;
  if (body == eventsBody_ && !first) return false;
  eventsBody_ = body;
  QVector<EventInfo> parsed;
  for (const QJsonValue& v : doc.array()) {
    EventInfo e = EventInfo::fromJson(v.toObject());
    if (!e.id.isEmpty()) parsed.push_back(std::move(e));
  }
  rebuildEvents(std::move(parsed));
  return true;
}

// A pinned event that the polled window now holds is updated from it; the others keep their fetched copy.
void EventStore::rebuildEvents(QVector<EventInfo> polled) {
  polled_ = std::move(polled);
  QVector<EventInfo> merged = polled_;
  for (auto it = pinned_.begin(); it != pinned_.end(); ++it) {
    const auto found = std::find_if(polled_.begin(), polled_.end(), [&](const EventInfo& e) { return e.id == it.key(); });
    if (found != polled_.end()) *it = *found;
    else merged.push_back(it.value());
  }
  std::stable_sort(merged.begin(), merged.end(), [](const EventInfo& a, const EventInfo& b) { return a.openedUtcMs > b.openedUtcMs; });
  events_ = std::move(merged);
  emit eventsChanged();
}

void EventStore::insertEvent(const EventInfo& event) {
  if (event.id.isEmpty()) return;
  pinned_.insert(event.id, event);
  rebuildEvents(polled_);
}

void EventStore::refreshRules(std::function<void()> done) {
  struct Pending {
    QJsonDocument rules;
    QJsonDocument zones;
    int remaining = 2;
    QString error;
  };
  const int seq = ++rulesRequested_;
  auto pending = std::make_shared<Pending>();
  auto finish = [this, seq, pending, done = std::move(done)] {
    if (--pending->remaining > 0) return;
    if (seq > rulesApplied_) {
      rulesApplied_ = seq;
      if (pending->error.isEmpty()) {
        rulesStale_ = false;
        applyRules(pending->rules, pending->zones);
      }
      setRulesError(pending->error);
    }
    if (done) done();
  };
  client_.listRules([pending, finish](bool ok, const QJsonDocument& doc, const QString& error) {
    if (ok) pending->rules = doc;
    else pending->error = error;
    finish();
  }, this);
  client_.listZones([pending, finish](bool ok, const QJsonDocument& doc, const QString& error) {
    if (ok) pending->zones = doc;
    else if (pending->error.isEmpty()) pending->error = error;
    finish();
  }, this);
}

void EventStore::applyRules(const QJsonDocument& rules, const QJsonDocument& zones) {
  const QByteArray body = rules.toJson(QJsonDocument::Compact) + zones.toJson(QJsonDocument::Compact);
  const bool first = !rulesLoaded_;
  rulesLoaded_ = true;
  if (body == rulesBody_ && !first) return;
  rulesBody_ = body;
  rules_.clear();
  for (const QJsonValue& v : rules.array()) {
    RuleInfo r = RuleInfo::fromJson(v.toObject());
    if (!r.id.isEmpty()) rules_.push_back(std::move(r));
  }
  zones_.clear();
  for (const QJsonValue& v : zones.array()) {
    ZoneInfo z = ZoneInfo::fromJson(v.toObject());
    if (!z.id.isEmpty()) zones_.push_back(std::move(z));
  }
  emit rulesChanged();
}

const EventInfo* EventStore::findEvent(const QString& id) const {
  for (const EventInfo& e : events_)
    if (e.id == id) return &e;
  return nullptr;
}

const RuleInfo* EventStore::findRule(const QString& id) const {
  for (const RuleInfo& r : rules_)
    if (r.id == id) return &r;
  return nullptr;
}

const ZoneInfo* EventStore::findZone(const QString& id) const {
  for (const ZoneInfo& z : zones_)
    if (z.id == id) return &z;
  return nullptr;
}

int EventStore::unresolvedCount() const {
  if (eventsStale_) return 0;
  if (unresolvedTotal_) return *unresolvedTotal_;
  return static_cast<int>(std::count_if(events_.begin(), events_.end(),
                                        [](const EventInfo& e) { return alertBucket(e) == AlertBucket::Unresolved; }));
}

QString EventStore::ruleLabel(const QString& ruleId) const {
  if (const RuleInfo* rule = findRule(ruleId)) return rule->name;
  if (rulesLoaded_) return QStringLiteral("deleted rule");
  return rulesError_.isEmpty() ? QStringLiteral("—") : QStringLiteral("rules unavailable");
}

void EventStore::acknowledge(const QString& eventId) {
  eventAction(eventId, QStringLiteral("acknowledge"), {}, QStringLiteral("Acknowledge"));
}

void EventStore::resolve(const QString& eventId) {
  eventAction(eventId, QStringLiteral("resolve"), {}, QStringLiteral("Resolve"));
}

void EventStore::review(const QString& eventId, const QString& label) {
  eventAction(eventId, QStringLiteral("review"), QJsonObject{{"label", label}, {"note", QString()}},
              QStringLiteral("Review"));
}

void EventStore::eventAction(const QString& eventId, const QString& action, const QJsonObject& body,
                             const QString& verb) {
  client_.eventAction(eventId, action, body, [this, eventId, verb](bool ok, const QJsonDocument&, const QString& error) {
    if (!ok) {
      emit actionFailed(QStringLiteral("%1 failed · %2").arg(verb, error));
      return;
    }
    if (!pinned_.contains(eventId)) {
      refreshEvents([this, eventId] { emit eventUpdated(eventId); });
      return;
    }
    client_.getEvent(eventId, [this, eventId](bool fetched, const QJsonDocument& doc, const QString&) {
      if (fetched && pinned_.contains(eventId)) pinned_.insert(eventId, EventInfo::fromJson(doc.object()));
      refreshEvents([this, eventId] { emit eventUpdated(eventId); });
    }, this);
  }, this);
}

void EventStore::setRuleEnabled(const QString& ruleId, bool enabled) {
  client_.setRuleEnabled(ruleId, enabled, [this, enabled](bool ok, const QJsonDocument&, const QString& error) {
    if (!ok) emit actionFailed(QStringLiteral("%1 rule failed · %2").arg(enabled ? QStringLiteral("Enable") : QStringLiteral("Disable"), error));
    refreshRules();
  }, this);
}

}
