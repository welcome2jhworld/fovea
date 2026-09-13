#pragma once
#include "fovea/core/Analytics.h"
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <optional>

namespace fovea::core {

class Store;

// Event lifecycle, alert deliveries and operator actions. Triggered opens the
// event, its evidence ref (the rule's pre/post window around the trigger
// frame) and one delivery per channel in one transaction, and copies the
// trigger frame's spool JPEG to <evidenceDir>/<event_id>.jpg. A pending
// delivery that no console confirms counts one attempt per 10 s (the first 10 s
// after its creation or after the service started) and becomes failed after
// three attempts; open deliveries of unresolved events stay listed.
class EventService : public QObject {
  Q_OBJECT
public:
  static constexpr int64_t kDeliveryRetryMs = 10'000;
  static constexpr int kDeliveryAttempts = 3;

  EventService(Store& store, QString evidenceDir, QObject* parent = nullptr);

  // Ends events a previous run left open and starts the delivery timer.
  void start();

  // frameAgeMs: time since the trigger frame's packet arrived (steady clock), -1 when unknown.
  // Returns false when the event could not be stored.
  bool onTriggered(const RuleRecord& rule, const ZoneRecord& zone, const rules::Evaluation& e, const QString& spoolPath,
                   int64_t nowUtcMs, int64_t frameAgeMs);
  void onCondition(const QString& eventId, const QString& condition);
  void onCleared(const QString& eventId, int64_t utcMs);

  std::optional<EventRecord> acknowledge(const QString& id, const QString& operatorName, ServiceError* error);
  std::optional<EventRecord> resolve(const QString& id, const QString& operatorName, ServiceError* error);
  std::optional<EventRecord> review(const QString& id, const QJsonObject& body, ServiceError* error);
  std::optional<AlertDelivery> confirmDelivery(const QString& deliveryId, const QString& consoleId, ServiceError* error);
  void checkDeliveries(int64_t nowUtcMs);

  QJsonArray pendingAlertsJson(const QString& consoleId, int64_t nowUtcMs);
  QJsonObject eventJson(const EventRecord& event);
  std::optional<QJsonObject> eventDetailJson(const QString& id);

signals:
  void eventOpened(const QString& eventId);

private:
  Store& store_;
  QString evidenceDir_;
  QTimer deliveryTimer_;
  QHash<QString, QString> conditions_;
  int64_t lastPollUtcMs_ = 0;
  int64_t startedUtcMs_ = 0;
  QHash<QString, int64_t> lastAttemptUtcMs_;
};

}
