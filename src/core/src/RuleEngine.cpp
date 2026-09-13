#include "fovea/core/RuleEngine.h"
#include "fovea/Clock.h"
#include "fovea/Ids.h"
#include "fovea/core/AnalysisScheduler.h"
#include "fovea/core/EventService.h"
#include "fovea/core/Store.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <iterator>

namespace fovea::core {
namespace {

constexpr int kTickMs = 1000;

QString compact(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

ServiceError invalid(const QString& code, const QString& message) { return {400, code, message}; }
ServiceError notFound(const QString& what) { return {404, QStringLiteral("not_found"), QStringLiteral("no such %1").arg(what)}; }
ServiceError storeFailed(const Store& store) { return {500, QStringLiteral("store_failed"), store.lastError()}; }

bool repeating(rules::Transition t) {
  switch (t) {
    case rules::Transition::Unknown:
    case rules::Transition::Suppressed:
    case rules::Transition::Stale:
    case rules::Transition::OutOfSchedule:
    case rules::Transition::ZoneMismatch:
    case rules::Transition::Disabled:
      return true;
    default:
      return false;
  }
}

}

RuleEngine::RuleEngine(Store& store, EventService& events, CameraLookup cameras, QObject* parent)
    : QObject(parent), store_(store), events_(events), cameras_(std::move(cameras)) {
  tickTimer_.setInterval(kTickMs);
  connect(&tickTimer_, &QTimer::timeout, this, [this] { tick(monoNowNs(), utcNowMs()); });
}

void RuleEngine::setGenerations(Generation current, BumpGeneration bump) {
  generation_ = std::move(current);
  bumpGeneration_ = std::move(bump);
}

void RuleEngine::start() {
  int loaded = 0;
  for (const RuleRecord& rule : store_.listRules()) {
    if (!store_.getCamera(rule.cameraId)) {
      qWarning("rules: %s names camera %s, which was deleted; not evaluated", qPrintable(rule.id), qPrintable(rule.cameraId));
      continue;
    }
    const std::optional<ZoneRecord> zone = store_.getZone(rule.zoneId, rule.zoneRevision);
    if (!zone) {
      qWarning("rules: %s names zone %s revision %d, which does not exist; not evaluated", qPrintable(rule.id),
               qPrintable(rule.zoneId), rule.zoneRevision);
      continue;
    }
    apply(rule, *zone);
    if (const int64_t cleared = store_.lastClearedUtcMs(rule.id); cleared > 0) entries_.at(rule.id).evaluator->seedRearm(cleared);
    ++loaded;
  }
  qInfo("rules: %d loaded", loaded);
  tickTimer_.start();
}

std::optional<ZoneRecord> RuleEngine::createZone(const QJsonObject& body, ServiceError* error) {
  ZoneRecord zone;
  zone.id = newId();
  zone.cameraId = body.value(QStringLiteral("camera_id")).toString();
  zone.revision = 1;
  zone.createdUtcMs = utcNowMs();
  zone.revisionUtcMs = zone.createdUtcMs;
  if (!cameras_(zone.cameraId)) {
    *error = invalid(QStringLiteral("invalid_zone"), QStringLiteral("camera_id does not name a camera"));
    return std::nullopt;
  }
  if (const QString e = applyZoneBody(zone, body); !e.isEmpty()) {
    *error = invalid(QStringLiteral("invalid_zone"), e);
    return std::nullopt;
  }
  if (!store_.insertZone(zone)) {
    *error = storeFailed(store_);
    return std::nullopt;
  }
  store_.appendAudit(QStringLiteral("api"), QStringLiteral("zone.create"), zone.id, compact({{"camera_id", zone.cameraId}}), zone.createdUtcMs);
  return zone;
}

std::optional<ZoneRecord> RuleEngine::updateZone(const QString& id, const QJsonObject& body, ServiceError* error) {
  const std::optional<ZoneRecord> existing = store_.getZone(id);
  if (!existing || existing->deletedUtcMs > 0) {
    *error = notFound(QStringLiteral("zone"));
    return std::nullopt;
  }
  if (body.contains(QStringLiteral("camera_id")) && body.value(QStringLiteral("camera_id")).toString() != existing->cameraId) {
    *error = invalid(QStringLiteral("invalid_zone"), QStringLiteral("camera_id cannot change; create a zone on the other camera"));
    return std::nullopt;
  }
  ZoneRecord zone = *existing;
  if (const QString e = applyZoneBody(zone, body); !e.isEmpty()) {
    *error = invalid(QStringLiteral("invalid_zone"), e);
    return std::nullopt;
  }
  zone.revision = existing->revision + 1;
  zone.revisionUtcMs = utcNowMs();
  if (!store_.insertZoneRevision(zone)) {
    *error = storeFailed(store_);
    return std::nullopt;
  }
  store_.appendAudit(QStringLiteral("api"), QStringLiteral("zone.update"), id, compact({{"revision", zone.revision}}), zone.revisionUtcMs);
  QVector<RuleRecord> affected;
  for (const auto& [ruleId, entry] : entries_)
    if (entry.rule.zoneId == id) affected.push_back(entry.rule);
  for (RuleRecord rule : affected) {
    rule.revision += 1;
    rule.zoneRevision = zone.revision;
    ServiceError ignored;
    if (!storeRevision(rule, zone, false, &ignored))
      qWarning("rules: %s not moved to zone revision %d: %s", qPrintable(rule.id), zone.revision, qPrintable(ignored.message));
  }
  return zone;
}

bool RuleEngine::deleteZone(const QString& id, ServiceError* error) {
  const std::optional<ZoneRecord> existing = store_.getZone(id);
  if (!existing || existing->deletedUtcMs > 0) {
    *error = notFound(QStringLiteral("zone"));
    return false;
  }
  const int64_t now = utcNowMs();
  if (!store_.softDeleteZone(id, now)) {
    *error = storeFailed(store_);
    return false;
  }
  store_.appendAudit(QStringLiteral("api"), QStringLiteral("zone.delete"), id, QString(), now);
  QVector<RuleRecord> affected;
  for (const auto& [ruleId, entry] : entries_)
    if (entry.rule.zoneId == id && entry.rule.enabled) affected.push_back(entry.rule);
  for (RuleRecord rule : affected) {
    rule.revision += 1;
    rule.enabled = false;
    ServiceError ignored;
    if (!storeRevision(rule, *existing, false, &ignored))
      qWarning("rules: %s not disabled after zone delete: %s", qPrintable(rule.id), qPrintable(ignored.message));
  }
  return true;
}

std::optional<ZoneRecord> RuleEngine::zoneForRule(const RuleRecord& rule, ServiceError* error) {
  if (!cameras_(rule.cameraId)) {
    *error = invalid(QStringLiteral("invalid_rule"), QStringLiteral("camera_id does not name a camera"));
    return std::nullopt;
  }
  const std::optional<ZoneRecord> zone = store_.getZone(rule.zoneId);
  if (!zone || zone->deletedUtcMs > 0) {
    *error = invalid(QStringLiteral("invalid_rule"), QStringLiteral("zone_id does not name a zone"));
    return std::nullopt;
  }
  if (zone->cameraId != rule.cameraId) {
    *error = invalid(QStringLiteral("invalid_rule"), QStringLiteral("zone %1 belongs to another camera").arg(rule.zoneId));
    return std::nullopt;
  }
  return zone;
}

std::optional<RuleRecord> RuleEngine::createRule(const QJsonObject& body, ServiceError* error) {
  RuleRecord rule;
  rule.id = newId();
  rule.revision = 1;
  rule.createdUtcMs = utcNowMs();
  if (const QString e = applyRuleBody(rule, body); !e.isEmpty()) {
    *error = invalid(QStringLiteral("invalid_rule"), e);
    return std::nullopt;
  }
  const std::optional<ZoneRecord> zone = zoneForRule(rule, error);
  if (!zone) return std::nullopt;
  rule.zoneRevision = zone->revision;
  return storeRevision(rule, *zone, true, error);
}

std::optional<RuleRecord> RuleEngine::updateRule(const QString& id, const QJsonObject& body, ServiceError* error) {
  const auto it = entries_.find(id);
  if (it == entries_.end()) {
    *error = notFound(QStringLiteral("rule"));
    return std::nullopt;
  }
  const Entry& entry = it->second;
  if (body.contains(QStringLiteral("camera_id")) && body.value(QStringLiteral("camera_id")).toString() != entry.rule.cameraId) {
    *error = invalid(QStringLiteral("invalid_rule"), QStringLiteral("camera_id cannot change; create a rule for the other camera"));
    return std::nullopt;
  }
  RuleRecord rule = entry.rule;
  if (const QString e = applyRuleBody(rule, body); !e.isEmpty()) {
    *error = invalid(QStringLiteral("invalid_rule"), e);
    return std::nullopt;
  }
  // A disabled revision keeps its zone even when the zone was deleted meanwhile.
  const std::optional<ZoneRecord> zone =
      !rule.enabled && rule.zoneId == entry.rule.zoneId ? std::optional<ZoneRecord>(entry.zone) : zoneForRule(rule, error);
  if (!zone) return std::nullopt;
  rule.revision = entry.rule.revision + 1;
  rule.zoneRevision = zone->revision;
  return storeRevision(rule, *zone, false, error);
}

std::optional<RuleRecord> RuleEngine::storeRevision(RuleRecord rule, const ZoneRecord& zone, bool created, ServiceError* error) {
  rule.revisionUtcMs = utcNowMs();
  if (!(created ? store_.insertRule(rule) : store_.insertRuleRevision(rule))) {
    *error = storeFailed(store_);
    return std::nullopt;
  }
  apply(rule, zone);
  store_.appendAudit(QStringLiteral("api"), created ? QStringLiteral("rule.create") : QStringLiteral("rule.update"), rule.id,
                     compact({{"revision", rule.revision}, {"enabled", rule.enabled}, {"camera_id", rule.cameraId}}),
                     rule.revisionUtcMs);
  return rule;
}

bool RuleEngine::deleteRule(const QString& id, ServiceError* error) {
  const auto it = entries_.find(id);
  if (it == entries_.end()) {
    *error = notFound(QStringLiteral("rule"));
    return false;
  }
  const int64_t now = utcNowMs();
  if (!store_.softDeleteRule(id, now)) {
    *error = storeFailed(store_);
    return false;
  }
  removeRule(it, QStringLiteral("rule_deleted"), QStringLiteral("api"), now);
  return true;
}

// The rule row is already soft-deleted.
void RuleEngine::removeRule(std::map<QString, Entry>::iterator it, const QString& note, const QString& actor, int64_t nowUtcMs) {
  Entry& entry = it->second;
  const QString id = it->first;
  if (const std::optional<QString> open = entry.evaluator->openEventId()) {
    rules::Evaluation e;
    e.ruleId = id;
    e.ruleRevision = entry.rule.revision;
    e.cameraId = entry.rule.cameraId;
    e.utcMs = nowUtcMs;
    e.before = entry.evaluator->state();
    e.after = rules::ConditionState::Inactive;
    e.quality = entry.evaluator->quality();
    e.transition = rules::Transition::Cleared;
    e.eventId = *open;
    e.note = note;
    persist(entry, e);
    events_.onCleared(*open, nowUtcMs);
  }
  const QString cameraId = entry.rule.cameraId;
  entries_.erase(it);
  if (bumpGeneration_) bumpGeneration_(cameraId);
  store_.appendAudit(actor, QStringLiteral("rule.delete"), id, compact({{"note", note}}), nowUtcMs);
}

void RuleEngine::onCameraDeleted(const QString& cameraId) {
  const int64_t now = utcNowMs();
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->second.rule.cameraId != cameraId) {
      ++it;
      continue;
    }
    const auto next = std::next(it);
    if (store_.softDeleteRule(it->first, now))
      removeRule(it, QStringLiteral("camera_deleted"), QStringLiteral("api"), now);
    else
      qWarning("rules: %s of deleted camera %s not deleted: %s", qPrintable(it->first), qPrintable(cameraId), qPrintable(store_.lastError()));
    it = next;
  }
  for (const ZoneRecord& zone : store_.listZones(cameraId)) {
    if (store_.softDeleteZone(zone.id, now))
      store_.appendAudit(QStringLiteral("api"), QStringLiteral("zone.delete"), zone.id, compact({{"note", "camera_deleted"}}), now);
  }
}

DetectHints RuleEngine::detectHints(const QString& cameraId) const {
  DetectHints hints;
  double lowest = kDetectThreshold;
  for (const auto& [id, entry] : entries_) {
    if (entry.rule.cameraId != cameraId || !entry.rule.enabled) continue;
    lowest = std::min(lowest, entry.rule.minConfidence);
    hints.maxGapNs = std::max(hints.maxGapNs, entry.rule.maxObservationGapNs);
    hints.threshold = std::max(kMinDetectThreshold, lowest);
  }
  return hints;
}

void RuleEngine::apply(const RuleRecord& rule, const ZoneRecord& zone) {
  rules::RuleRevision revision = rule.toEvaluatorRevision(zone);
  const auto it = entries_.find(rule.id);
  if (it == entries_.end()) {
    Entry entry;
    entry.rule = rule;
    entry.zone = zone;
    entry.evaluator = std::make_unique<rules::RuleEvaluator>(std::move(revision), [] { return newId(); });
    entries_.emplace(rule.id, std::move(entry));
  } else {
    Entry& entry = it->second;
    entry.rule = rule;
    entry.zone = zone;
    if (const std::optional<rules::Evaluation> e = entry.evaluator->setRule(std::move(revision), utcNowMs()))
      handle(entry, *e, nullptr);
    else
      qWarning("rules: evaluator of %s refused revision %d", qPrintable(rule.id), rule.revision);
  }
  if (bumpGeneration_) bumpGeneration_(rule.cameraId);
}

QJsonObject RuleEngine::ruleJson(const RuleRecord& rule) const {
  QJsonObject o = rule.toJson();
  QJsonArray runtime;
  if (const auto it = entries_.find(rule.id); it != entries_.end()) {
    const rules::RuleEvaluator& ev = *it->second.evaluator;
    runtime.push_back(QJsonObject{{"camera_id", rule.cameraId},
                                  {"condition", rules::toString(ev.state())},
                                  {"quality", rules::toString(ev.quality())},
                                  {"open_event_id", ev.openEventId().value_or(QString())}});
  }
  o.insert("runtime", runtime);
  return o;
}

void RuleEngine::onAnalysis(const AnalysisResult& result) {
  const int64_t nowMono = monoNowNs();
  const uint64_t current = generation_ ? generation_(result.frame.cameraId) : 0;
  for (auto& [id, entry] : entries_) {
    if (entry.rule.cameraId != result.frame.cameraId) continue;
    entry.evaluator->raiseGeneration(current);
    rules::ObservationFrame frame = result.frame;
    if (frame.quality == rules::Quality::Known) {
      const bool center = entry.zone.anchor == QLatin1String("center");
      for (const DetectedObject& d : result.detections)
        if (!d.trackId.isEmpty()) frame.tracks.push_back({d.trackId, d.cls, center ? d.center : d.foot, d.confidence});
    }
    const rules::Evaluation e = entry.evaluator->evaluate(frame, nowMono);
    if (e.transition == rules::Transition::None || e.transition == rules::Transition::StillActive) entry.lastRepeatKey.clear();
    handle(entry, e, &result);
  }
}

void RuleEngine::tick(int64_t nowMonoNs, int64_t nowUtcMs) {
  for (auto& [id, entry] : entries_) {
    const rules::Evaluation e = entry.evaluator->tick(nowMonoNs, nowUtcMs);
    if (e.transition != rules::Transition::None) handle(entry, e, nullptr);
  }
}

void RuleEngine::handle(Entry& entry, const rules::Evaluation& e, const AnalysisResult* result) {
  if (e.transition == rules::Transition::Triggered &&
      !events_.onTriggered(entry.rule, entry.zone, e, result ? result->spoolPath : QString(), utcNowMs(),
                           result ? (monoNowNs() - result->frame.recvMonoNs) / 1'000'000 : -1)) {
    entry.evaluator->abandonEvent();
    return;
  }
  if (e.transition == rules::Transition::Unknown && e.note.isEmpty() && result && !result->reason.isEmpty()) {
    rules::Evaluation noted = e;
    noted.note = result->reason;
    persist(entry, noted);
  } else {
    persist(entry, e);
  }
  switch (e.transition) {
    case rules::Transition::StillActive:
    case rules::Transition::BecameClearing:
      if (!e.eventId.isEmpty()) events_.onCondition(e.eventId, rules::toString(e.after));
      break;
    case rules::Transition::Cleared:
      if (!e.eventId.isEmpty()) events_.onCleared(e.eventId, e.utcMs > 0 ? e.utcMs : utcNowMs());
      break;
    default:
      break;
  }
}

void RuleEngine::persist(Entry& entry, const rules::Evaluation& e) {
  if (e.transition == rules::Transition::None || e.transition == rules::Transition::StillActive) return;
  if (repeating(e.transition)) {
    const QString key = rules::toString(e.transition) + QLatin1Char('/') + rules::toString(e.after);
    if (key == entry.lastRepeatKey) return;
    entry.lastRepeatKey = key;
  } else {
    entry.lastRepeatKey.clear();
  }
  if (!store_.insertEvaluation(EvaluationRecord::from(e)))
    qWarning("rules: cannot store evaluation of %s: %s", qPrintable(e.ruleId), qPrintable(store_.lastError()));
}

}
