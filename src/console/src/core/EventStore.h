#pragma once
#include "core/AlertTypes.h"
#include <QByteArray>
#include <QJsonDocument>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QHash>
#include <functional>
#include <optional>

namespace fovea::ui {

class CoreClient;

// The console's copy of rules, zones and recent events. Polls while the service
// is ready; views read it and never talk to the rule or event endpoints for lists.
// Events fetched on their own (an alert older than the polled window) stay
// pinned into the list. When the service stops being ready the lists are
// stale: both errors read "service unavailable" until the next refresh answers.
class EventStore : public QObject {
  Q_OBJECT
public:
  static constexpr int kEventsIntervalMs = 2000;
  static constexpr int kRulesIntervalMs = 5000;
  static constexpr int kEventLimit = 500;
  static constexpr int64_t kEventWindowMs = 7LL * 24 * 3600 * 1000;

  explicit EventStore(CoreClient& client, QObject* parent = nullptr);

  void setActive(bool active);
  void refreshEvents(std::function<void()> done = {});
  void refreshRules(std::function<void()> done = {});
  // Pins an event fetched on its own (an alert older than the polled window).
  void insertEvent(const EventInfo& event);
  bool isPinned(const QString& eventId) const { return pinned_.contains(eventId); }

  bool eventsLoaded() const { return eventsLoaded_; }
  bool rulesLoaded() const { return rulesLoaded_; }
  // Newest first, at most kEventLimit from the last kEventWindowMs.
  const QVector<EventInfo>& events() const { return events_; }
  bool eventsSaturated() const { return events_.size() >= kEventLimit; }
  const QVector<RuleInfo>& rules() const { return rules_; }
  const QVector<ZoneInfo>& zones() const { return zones_; }
  const EventInfo* findEvent(const QString& id) const;
  const RuleInfo* findRule(const QString& id) const;
  const ZoneInfo* findZone(const QString& id) const;
  // The Unresolved tab over all events (GET /v1/events/counts), or over the
  // loaded list until the service answered that; 0 while the lists are stale.
  int unresolvedCount() const;
  // The rule's name, "deleted rule" when the loaded rules lack it, "—" before
  // rules loaded and "rules unavailable" when loading them failed.
  QString ruleLabel(const QString& ruleId) const;
  bool eventsStale() const { return eventsStale_; }
  bool rulesStale() const { return rulesStale_; }
  // Why the last list request failed; empty once one succeeds.
  QString eventsError() const { return eventsError_; }
  QString rulesError() const { return rulesError_; }
  // A first answer arrived, successful or not.
  bool eventsAnswered() const { return eventsLoaded_ || !eventsError_.isEmpty(); }
  bool rulesAnswered() const { return rulesLoaded_ || !rulesError_.isEmpty(); }

  void acknowledge(const QString& eventId);
  void resolve(const QString& eventId);
  void review(const QString& eventId, const QString& label);
  void setRuleEnabled(const QString& ruleId, bool enabled);

signals:
  void eventsChanged();
  void rulesChanged();
  void eventUpdated(const QString& eventId);
  void actionFailed(const QString& message);

private:
  void eventAction(const QString& eventId, const QString& action, const QJsonObject& body, const QString& verb);
  // Returns whether the list changed (eventsChanged was emitted).
  bool applyEvents(const QJsonDocument& doc);
  void rebuildEvents(QVector<EventInfo> polled);
  void applyRules(const QJsonDocument& rules, const QJsonDocument& zones);
  void setEventsError(const QString& error);
  void setRulesError(const QString& error);

  CoreClient& client_;
  QTimer eventsTimer_;
  QTimer rulesTimer_;
  QVector<EventInfo> events_;
  QVector<EventInfo> polled_;
  QHash<QString, EventInfo> pinned_;
  std::optional<int> unresolvedTotal_;
  bool eventsStale_ = false;
  bool rulesStale_ = false;
  QVector<RuleInfo> rules_;
  QVector<ZoneInfo> zones_;
  QByteArray eventsBody_;
  QByteArray rulesBody_;
  bool eventsLoaded_ = false;
  bool rulesLoaded_ = false;
  // Replies apply in request order: a slower older reply never overwrites a newer one.
  int eventsRequested_ = 0;
  int eventsApplied_ = 0;
  int rulesRequested_ = 0;
  int rulesApplied_ = 0;
  QString eventsError_;
  QString rulesError_;
};

}
