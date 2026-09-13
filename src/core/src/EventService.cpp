#include "fovea/core/EventService.h"
#include "fovea/Clock.h"
#include "fovea/Ids.h"
#include "fovea/core/Store.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <algorithm>

namespace fovea::core {
namespace {

constexpr int kDeliveryCheckMs = 1000;
constexpr int kMaxNoteChars = 2000;
constexpr int64_t kTimelineMarginMs = 5000;
constexpr int kTimelineLimit = 500;

const QStringList kLabels{QStringLiteral("confirmed"), QStringLiteral("false_alarm"), QStringLiteral("undecided")};

QString compact(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

ServiceError notFound(const QString& what) { return {404, QStringLiteral("not_found"), QStringLiteral("no such %1").arg(what)}; }

QString seconds(int64_t ns) {
  const double s = static_cast<double>(ns) / 1e9;
  return s == static_cast<double>(static_cast<int64_t>(s)) ? QString::number(static_cast<int64_t>(s)) : QString::number(s, 'f', 1);
}

}

EventService::EventService(Store& store, QString evidenceDir, QObject* parent)
    : QObject(parent), store_(store), evidenceDir_(std::move(evidenceDir)) {
  deliveryTimer_.setInterval(kDeliveryCheckMs);
  connect(&deliveryTimer_, &QTimer::timeout, this, [this] { checkDeliveries(utcNowMs()); });
}

void EventService::start() {
  const int64_t now = utcNowMs();
  startedUtcMs_ = now;
  const QStringList closed = store_.clearOpenEvents(now);
  if (!closed.isEmpty()) qInfo("events: %lld left open by the previous run cleared", static_cast<long long>(closed.size()));
  QDir().mkpath(evidenceDir_);
  deliveryTimer_.start();
}

bool EventService::onTriggered(const RuleRecord& rule, const ZoneRecord& zone, const rules::Evaluation& e, const QString& spoolPath,
                               int64_t nowUtcMs, int64_t frameAgeMs) {
  EventRecord ev;
  ev.id = e.eventId;
  ev.ruleId = rule.id;
  ev.ruleRevision = rule.revision;
  ev.cameraId = rule.cameraId;
  ev.sessionId = e.sessionId;
  ev.severity = rule.severity;
  ev.openedUtcMs = e.utcMs > 0 ? e.utcMs : nowUtcMs;
  ev.openedPtsNs = e.windowStartPtsNs;
  ev.triggerPtsNs = e.windowEndPtsNs;
  QString cls = rule.targetClass;
  if (!cls.isEmpty()) cls[0] = cls[0].toUpper();
  ev.title = QStringLiteral("%1 in %2 for %3 s").arg(cls, zone.name, seconds(rule.dwellNs));
  const bool one = e.trackIds.size() == 1;
  ev.detail = QStringLiteral("%1 %2 in the zone (%3 %4), longest dwell %5 s")
                  .arg(e.trackIds.size())
                  .arg(one ? rule.targetClass : rule.targetClass + QLatin1Char('s'), one ? QStringLiteral("track") : QStringLiteral("tracks"),
                       QStringList(e.trackIds).join(QStringLiteral(", ")), seconds(e.dwellNs));

  EvidenceRef ref;
  ref.id = newId();
  ref.eventId = ev.id;
  ref.cameraId = ev.cameraId;
  ref.fromUtcMs = ev.openedUtcMs - rule.evidencePreNs / 1'000'000;
  ref.toUtcMs = ev.openedUtcMs + rule.evidencePostNs / 1'000'000;
  ref.reason = QStringLiteral("waiting for recordings");
  ref.updatedUtcMs = nowUtcMs;
  if (!spoolPath.isEmpty()) {
    const QString target = evidenceDir_ + QLatin1Char('/') + ev.id + QStringLiteral(".jpg");
    QFile::remove(target);
    if (QDir().mkpath(evidenceDir_) && QFile::copy(spoolPath, target)) ref.thumbnailPath = target;
    else qWarning("events: cannot copy the trigger frame of %s", qPrintable(ev.id));
  }

  QVector<AlertDelivery> deliveries;
  QStringList channels{QStringLiteral("console")};
  if (rule.soundAction) channels.push_back(QStringLiteral("sound"));
  for (const QString& channel : channels) {
    AlertDelivery d;
    d.id = newId();
    d.eventId = ev.id;
    d.channel = channel;
    d.createdUtcMs = nowUtcMs;
    deliveries.push_back(d);
  }
  if (!store_.openEvent(ev, ref, deliveries)) {
    qWarning("events: cannot store event %s: %s", qPrintable(ev.id), qPrintable(store_.lastError()));
    if (!ref.thumbnailPath.isEmpty()) QFile::remove(ref.thumbnailPath);
    return false;
  }
  conditions_.insert(ev.id, ev.condition);
  store_.appendAudit(QStringLiteral("rules"), QStringLiteral("event.open"), ev.id,
                     compact({{"rule_id", rule.id},
                              {"rule_revision", rule.revision},
                              {"camera_id", rule.cameraId},
                              {"title", ev.title},
                              {"dwell_ns", static_cast<double>(e.dwellNs)},
                              {"frame_age_ms", static_cast<double>(frameAgeMs)}}),
                     nowUtcMs);
  qInfo("events: %s opened on camera %s: %s", qPrintable(ev.id), qPrintable(ev.cameraId), qPrintable(ev.title));
  emit eventOpened(ev.id);
  return true;
}

void EventService::onCondition(const QString& eventId, const QString& condition) {
  if (conditions_.value(eventId) == condition) return;
  if (store_.setEventCondition(eventId, condition, 0)) conditions_.insert(eventId, condition);
}

void EventService::onCleared(const QString& eventId, int64_t utcMs) {
  conditions_.remove(eventId);
  if (store_.setEventCondition(eventId, QStringLiteral("cleared"), utcMs)) qInfo("events: %s cleared", qPrintable(eventId));
}

std::optional<EventRecord> EventService::acknowledge(const QString& id, const QString& operatorName, ServiceError* error) {
  const std::optional<EventRecord> ev = store_.getEvent(id);
  if (!ev) {
    *error = notFound(QStringLiteral("event"));
    return std::nullopt;
  }
  if (ev->operatorState == QLatin1String("new") &&
      !store_.applyOperatorAction(id, QStringLiteral("acknowledged"), std::nullopt,
                                  {operatorName, QStringLiteral("event.acknowledge"), id, QString(), utcNowMs()})) {
    *error = {500, QStringLiteral("store_failed"), store_.lastError()};
    return std::nullopt;
  }
  return store_.getEvent(id);
}

std::optional<EventRecord> EventService::resolve(const QString& id, const QString& operatorName, ServiceError* error) {
  const std::optional<EventRecord> ev = store_.getEvent(id);
  if (!ev) {
    *error = notFound(QStringLiteral("event"));
    return std::nullopt;
  }
  const int64_t holdsUntil = ev->openedUtcMs + kEvidenceRetentionMs;
  // A repeated resolve re-applies the hold expiry, so holds written after an
  // earlier resolve never stay open-ended.
  const bool stored = ev->operatorState == QLatin1String("resolved")
                          ? store_.setEventHoldsUntil(id, holdsUntil)
                          : store_.applyOperatorAction(id, QStringLiteral("resolved"), holdsUntil,
                                                       {operatorName, QStringLiteral("event.resolve"), id, QString(), utcNowMs()});
  if (!stored) {
    *error = {500, QStringLiteral("store_failed"), store_.lastError()};
    return std::nullopt;
  }
  return store_.getEvent(id);
}

std::optional<EventRecord> EventService::review(const QString& id, const QJsonObject& body, ServiceError* error) {
  if (!store_.getEvent(id)) {
    *error = notFound(QStringLiteral("event"));
    return std::nullopt;
  }
  EventReview r;
  r.id = newId();
  r.eventId = id;
  r.label = body.value(QStringLiteral("label")).toString();
  r.note = body.value(QStringLiteral("note")).toString();
  r.operatorName = body.value(QStringLiteral("operator")).toString(QStringLiteral("operator"));
  r.utcMs = utcNowMs();
  if (!kLabels.contains(r.label)) {
    *error = {400, QStringLiteral("invalid_label"), QStringLiteral("label must be confirmed, false_alarm or undecided")};
    return std::nullopt;
  }
  if (r.note.size() > kMaxNoteChars) {
    *error = {400, QStringLiteral("invalid_note"), QStringLiteral("note is longer than %1 characters").arg(kMaxNoteChars)};
    return std::nullopt;
  }
  if (!store_.insertReview(r, {r.operatorName, QStringLiteral("event.review"), id, compact({{"label", r.label}, {"note", r.note}}), r.utcMs})) {
    *error = {500, QStringLiteral("store_failed"), store_.lastError()};
    return std::nullopt;
  }
  return store_.getEvent(id);
}

std::optional<AlertDelivery> EventService::confirmDelivery(const QString& deliveryId, const QString& consoleId, ServiceError* error) {
  const std::optional<AlertDelivery> d = store_.getDelivery(deliveryId);
  if (!d) {
    *error = notFound(QStringLiteral("delivery"));
    return std::nullopt;
  }
  if (d->state != QLatin1String("delivered")) {
    if (!store_.markDelivered(deliveryId, utcNowMs())) {
      *error = {500, QStringLiteral("store_failed"), store_.lastError()};
      return std::nullopt;
    }
    qInfo("events: %s delivery of %s confirmed by console %s", qPrintable(d->channel), qPrintable(d->eventId),
          qPrintable(consoleId.left(64)));
  }
  return store_.getDelivery(deliveryId);
}

// An attempt is due kDeliveryRetryMs after the later of the delivery's
// creation, its previous attempt in this run and the start of this run, so a
// restart or a stalled thread never counts several attempts at once.
void EventService::checkDeliveries(int64_t nowUtcMs) {
  QHash<QString, int64_t> lastAttempts;
  for (const AlertDelivery& d : store_.openDeliveries()) {
    if (d.state != QLatin1String("pending")) continue;
    const int64_t lastAttempt = lastAttemptUtcMs_.value(d.id, startedUtcMs_);
    lastAttempts.insert(d.id, lastAttempt);
    if (nowUtcMs < std::max(d.createdUtcMs, lastAttempt) + kDeliveryRetryMs) continue;
    const int attempts = d.attempts + 1;
    const QString state = attempts >= kDeliveryAttempts ? QStringLiteral("failed") : QStringLiteral("pending");
    const QString reason = nowUtcMs - lastPollUtcMs_ <= kDeliveryRetryMs ? QStringLiteral("console did not confirm")
                                                                        : QStringLiteral("no console connected");
    if (!store_.recordDeliveryAttempt(d.id, attempts, state, reason)) continue;
    lastAttempts.insert(d.id, nowUtcMs);
    if (state == QLatin1String("failed"))
      qWarning("events: %s delivery of %s failed after %d attempts: %s", qPrintable(d.channel), qPrintable(d.eventId), attempts,
               qPrintable(reason));
  }
  lastAttemptUtcMs_ = std::move(lastAttempts);
}

QJsonArray EventService::pendingAlertsJson(const QString& consoleId, int64_t nowUtcMs) {
  if (!consoleId.isEmpty()) lastPollUtcMs_ = nowUtcMs;
  QJsonArray out;
  QHash<QString, QJsonObject> events;
  for (const AlertDelivery& d : store_.openDeliveries()) {
    QJsonObject o = d.toJson();
    if (!events.contains(d.eventId)) {
      const std::optional<EventRecord> ev = store_.getEvent(d.eventId);
      events.insert(d.eventId, ev ? QJsonObject{{"id", ev->id}, {"rule_id", ev->ruleId}, {"camera_id", ev->cameraId},
                                                {"severity", ev->severity}, {"title", ev->title},
                                                {"opened_utc_ms", static_cast<double>(ev->openedUtcMs)}}
                                  : QJsonObject{});
    }
    o.insert("event", events.value(d.eventId));
    out.push_back(o);
  }
  return out;
}

QJsonObject EventService::eventJson(const EventRecord& event) {
  QJsonObject o = event.toJson();
  const std::optional<EventReview> review = store_.latestReview(event.id);
  o.insert("review", review ? QJsonValue(review->toJson()) : QJsonValue(QJsonValue::Null));
  const std::optional<EvidenceRef> evidence = store_.evidenceForEvent(event.id);
  o.insert("evidence", evidence ? QJsonValue(evidence->toJson()) : QJsonValue(QJsonValue::Null));
  QJsonArray deliveries;
  for (const AlertDelivery& d : store_.deliveriesForEvent(event.id)) deliveries.push_back(d.toJson());
  o.insert("deliveries", deliveries);
  return o;
}

std::optional<QJsonObject> EventService::eventDetailJson(const QString& id) {
  const std::optional<EventRecord> ev = store_.getEvent(id);
  if (!ev) return std::nullopt;
  QJsonObject o = eventJson(*ev);
  const int64_t occupancyMs = std::max<int64_t>(0, (ev->triggerPtsNs - ev->openedPtsNs) / 1'000'000);
  const int64_t from = ev->openedUtcMs - occupancyMs - kTimelineMarginMs;
  const int64_t to = ev->clearedUtcMs > 0 ? ev->clearedUtcMs + kTimelineMarginMs : 0;
  QJsonArray evaluations;
  for (const EvaluationRecord& e : store_.listEvaluations(ev->ruleId, from, to, kTimelineLimit)) evaluations.push_back(e.toJson());
  o.insert("evaluations", evaluations);
  return o;
}

}
