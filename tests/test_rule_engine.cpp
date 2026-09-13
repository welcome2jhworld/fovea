#include "fovea/Clock.h"
#include "fovea/core/AnalysisScheduler.h"
#include "fovea/core/EventService.h"
#include "fovea/core/RuleEngine.h"
#include "fovea/core/Store.h"
#include <QJsonArray>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <memory>

using namespace fovea;
using namespace fovea::core;

namespace {

constexpr int64_t kStepNs = 500'000'000;
constexpr int64_t kBaseUtcMs = 1'757'700'000'000;

QJsonArray square(double lo, double hi) { return QJsonArray{QJsonArray{lo, lo}, QJsonArray{hi, lo}, QJsonArray{hi, hi}, QJsonArray{lo, hi}}; }

struct Harness {
  QTemporaryDir dir;
  Store store;
  std::unique_ptr<EventService> events;
  std::unique_ptr<RuleEngine> engine;
  uint64_t generation = 1;
  int64_t ptsNs = 0;
  int64_t baseUtcMs = kBaseUtcMs;
  QString zoneId;
  QString ruleId;

  explicit Harness(const QJsonObject& ruleOverrides = {}) {
    store.open(dir.path() + "/t.sqlite");
    Camera camera;
    camera.id = "cam1";
    camera.name = "Gate";
    camera.kind = "file";
    camera.mainUrl = "gate.mp4";
    store.insertCamera(camera);
    events = std::make_unique<EventService>(store, dir.path() + "/evidence");
    engine = std::make_unique<RuleEngine>(store, *events, [](const QString& id) -> std::optional<Camera> {
      if (id != QLatin1String("cam1")) return std::nullopt;
      Camera c;
      c.id = id;
      return c;
    });
    engine->setGenerations([this](const QString&) { return generation; }, [this](const QString&) { ++generation; });
    ServiceError error;
    const auto zone = engine->createZone(
        {{"camera_id", "cam1"}, {"name", "Gate"}, {"points", square(0.2, 0.8)}, {"ref_width", 960}, {"ref_height", 540}}, &error);
    if (zone) zoneId = zone->id;
    QJsonObject body{{"name", "Gate dwell"},   {"camera_id", "cam1"},          {"zone_id", zoneId},
                     {"dwell_ns", 10e9},       {"clear_after_ns", 5e9},        {"rearm_ns", 30e9}};
    for (auto it = ruleOverrides.begin(); it != ruleOverrides.end(); ++it) body.insert(it.key(), it.value());
    const auto rule = engine->createRule(body, &error);
    if (rule) ruleId = rule->id;
  }

  AnalysisResult frame(bool inside, rules::Quality quality) {
    ptsNs += kStepNs;
    AnalysisResult r;
    r.frame.cameraId = "cam1";
    r.frame.sessionId = "s1";
    r.frame.ptsNs = ptsNs;
    r.frame.utcMs = baseUtcMs + ptsNs / 1'000'000;
    r.frame.recvMonoNs = monoNowNs();
    r.frame.generation = generation;
    r.frame.quality = quality;
    r.frame.frameWidth = 960;
    r.frame.frameHeight = 540;
    if (quality == rules::Quality::Known) {
      const QPointF foot = inside ? QPointF(0.5, 0.6) : QPointF(0.95, 0.95);
      r.detections.push_back({"7", "person", 0.9, {0.4, 0.2, 0.6, foot.y()}, foot, QPointF(foot.x(), 0.4)});
    }
    return r;
  }

  void feed(int frames, bool inside, rules::Quality quality = rules::Quality::Known) {
    for (int i = 0; i < frames; ++i) engine->onAnalysis(frame(inside, quality));
  }

  QVector<EventRecord> allEvents() { return store.listEvents({}); }

  QStringList column(const QString& sql) {
    QStringList out;
    const QString name = QUuid::createUuid().toString();
    {
      QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
      db.setDatabaseName(dir.path() + "/t.sqlite");
      QSqlQuery q(db);
      if (db.open() && q.exec(sql))
        while (q.next()) out.push_back(q.value(0).toString());
    }
    QSqlDatabase::removeDatabase(name);
    return out;
  }

  QStringList transitions() { return column("SELECT transition FROM rule_evaluations ORDER BY id"); }
  // Frames outside the zone until the event clears; returns the media time from the first of them to the clear.
  int64_t clearAfterLeaving(const QString& eventId, const std::function<void(AnalysisResult&)>& staleThird) {
    int64_t leftPts = 0;
    for (int i = 0; i < 40 && store.getEvent(eventId)->condition != QLatin1String("cleared"); ++i) {
      AnalysisResult r = frame(false, rules::Quality::Known);
      if (i == 0) leftPts = r.frame.ptsNs;
      if (i == 2) staleThird(r);
      engine->onAnalysis(r);
    }
    return ptsNs - leftPts;
  }
  QString runtime(const char* key) {
    return engine->ruleJson(*store.getRule(ruleId)).value("runtime").toArray().first().toObject().value(QLatin1String(key)).toString();
  }
};

}

class TestRuleEngine : public QObject {
  Q_OBJECT
private slots:
  void dwellOpensExactlyOneEvent();
  void workerOutageKeepsTheOpenEvent();
  void failedEventInsertTriggersAgain();
  void staleResultsFreezeClearing();
  void restartClearsWithEvaluationAndSeedsRearm();
  void deletingCameraDeletesItsRulesAndZones();
  void detectHintsFollowEnabledRules();
  void unknownFreezesDwell();
  void longUnknownRestartsDwell();
  void staleResultsAreIgnored();
  void ruleRevisionKeepsOpenEvent();
  void clearedThenRearmSuppressesNextOccupancy();
  void deletingRuleClearsOpenEvent();
  void deliveriesRetryThenFail();
  void overdueDeliveryCountsOneAttemptPerInterval();
  void validatesZonesAndRules();
  void parsesDetectReply();
};

void TestRuleEngine::dwellOpensExactlyOneEvent() {
  Harness h;
  QVERIFY(!h.ruleId.isEmpty());
  h.feed(20, true);
  QCOMPARE(h.allEvents().size(), 0);
  QCOMPARE(h.runtime("condition"), QString("pending"));
  h.feed(1, true);
  QCOMPARE(h.allEvents().size(), 1);
  h.feed(40, true);
  const QVector<EventRecord> events = h.allEvents();
  QCOMPARE(events.size(), 1);
  const EventRecord& e = events.first();
  QCOMPARE(e.condition, QString("active"));
  QCOMPARE(e.operatorState, QString("new"));
  QCOMPARE(e.triggerPtsNs, 21 * kStepNs);
  QCOMPARE(e.openedPtsNs, kStepNs);
  QCOMPARE(e.openedUtcMs, kBaseUtcMs + 21 * kStepNs / 1'000'000);
  QCOMPARE(h.runtime("open_event_id"), e.id);
  QCOMPARE(h.transitions(), QStringList({"became_pending", "triggered"}));
  const std::optional<EvidenceRef> ref = h.store.evidenceForEvent(e.id);
  QVERIFY(ref.has_value());
  QCOMPARE(ref->fromUtcMs, e.openedUtcMs - 10'000);
  QCOMPARE(ref->toUtcMs, e.openedUtcMs + 10'000);
  QCOMPARE(ref->state, QString("pending"));
  const QVector<AlertDelivery> deliveries = h.store.deliveriesForEvent(e.id);
  QCOMPARE(deliveries.size(), 2);
  QCOMPARE(deliveries[0].state, QString("pending"));
  QCOMPARE(h.column("SELECT COUNT(*) FROM audit_log WHERE action='event.open'"), QStringList{"1"});
}

void TestRuleEngine::workerOutageKeepsTheOpenEvent() {
  Harness h;
  h.feed(21, true);
  const QString eventId = h.allEvents().first().id;
  h.feed(12, false, rules::Quality::Unknown);
  h.feed(30, true);
  QCOMPARE(h.allEvents().size(), 1);
  QCOMPARE(h.store.getEvent(eventId)->condition, QString("active"));
  QCOMPARE(h.runtime("open_event_id"), eventId);
  QCOMPARE(h.transitions(), QStringList({"became_pending", "triggered", "unknown"}));
}

void TestRuleEngine::failedEventInsertTriggersAgain() {
  Harness h;
  QVERIFY(h.column("CREATE TRIGGER fail_event BEFORE INSERT ON events BEGIN SELECT RAISE(ABORT, 'disk full'); END").isEmpty());
  h.feed(21, true);
  QCOMPARE(h.allEvents().size(), 0);
  QCOMPARE(h.runtime("open_event_id"), QString());
  QCOMPARE(h.transitions(), QStringList{"became_pending"});
  QVERIFY(h.column("DROP TRIGGER fail_event").isEmpty());
  h.feed(1, true);
  const QVector<EventRecord> events = h.allEvents();
  QCOMPARE(events.size(), 1);
  QCOMPARE(h.runtime("open_event_id"), events.first().id);
  QCOMPARE(h.column("SELECT event_id FROM rule_evaluations WHERE transition='triggered'"), QStringList{events.first().id});
  QCOMPARE(h.store.deliveriesForEvent(events.first().id).size(), 2);
}

void TestRuleEngine::staleResultsFreezeClearing() {
  for (const bool byGeneration : {true, false}) {
    Harness h;
    h.feed(21, true);
    const QString eventId = h.allEvents().first().id;
    const int64_t clearedAfter = h.clearAfterLeaving(eventId, [&h, byGeneration](AnalysisResult& r) {
      if (byGeneration) r.frame.generation = h.generation - 1;
      else r.frame.recvMonoNs = monoNowNs() - 6'000'000'000LL;
    });
    QCOMPARE(clearedAfter, 5'500'000'000LL);
    QCOMPARE(h.column("SELECT COUNT(*) FROM rule_evaluations WHERE transition='stale'"), QStringList{"1"});
  }
}

void TestRuleEngine::restartClearsWithEvaluationAndSeedsRearm() {
  Harness h;
  h.baseUtcMs = utcNowMs() - 11'000;
  h.feed(21, true);
  const QString first = h.allEvents().first().id;
  h.engine.reset();
  h.events = std::make_unique<EventService>(h.store, h.dir.path() + "/evidence");
  h.events->start();
  QCOMPARE(h.store.getEvent(first)->condition, QString("cleared"));
  h.engine = std::make_unique<RuleEngine>(h.store, *h.events, [](const QString& id) -> std::optional<Camera> {
    Camera c;
    c.id = id;
    return c;
  });
  h.engine->setGenerations([&h](const QString&) { return h.generation; }, [&h](const QString&) { ++h.generation; });
  h.engine->start();
  h.feed(21, true);
  QCOMPARE(h.allEvents().size(), 1);
  QCOMPARE(h.column("SELECT note FROM rule_evaluations WHERE transition='cleared'"), QStringList{"core_restart"});
  QVERIFY(h.transitions().endsWith("suppressed"));
  QStringList timeline;
  for (const QJsonValue& v : h.events->eventDetailJson(first)->value("evaluations").toArray())
    timeline << v.toObject().value("transition").toString();
  QVERIFY(timeline.contains("cleared"));
}

void TestRuleEngine::deletingCameraDeletesItsRulesAndZones() {
  Harness h;
  h.feed(21, true);
  const QString eventId = h.allEvents().first().id;
  h.engine->onCameraDeleted("cam1");
  QCOMPARE(h.store.getEvent(eventId)->condition, QString("cleared"));
  QVERIFY(!h.store.getRule(h.ruleId).has_value());
  QCOMPARE(h.store.listZones("cam1").size(), 0);
  QCOMPARE(h.column("SELECT note FROM rule_evaluations WHERE transition='cleared'"), QStringList{"camera_deleted"});
  h.feed(30, true);
  QCOMPARE(h.allEvents().size(), 1);
}

void TestRuleEngine::detectHintsFollowEnabledRules() {
  Harness h({{"min_confidence", 0.5}, {"max_observation_gap_ns", 4e9}});
  DetectHints hints = h.engine->detectHints("cam1");
  QCOMPARE(hints.threshold, kDetectThreshold);
  QCOMPARE(hints.maxGapNs, 4'000'000'000LL);
  ServiceError error;
  QVERIFY(h.engine->createRule({{"name", "Low"}, {"camera_id", "cam1"}, {"zone_id", h.zoneId}, {"min_confidence", 0.02}}, &error));
  hints = h.engine->detectHints("cam1");
  QCOMPARE(hints.threshold, kMinDetectThreshold);
  QCOMPARE(hints.maxGapNs, 4'000'000'000LL);
  QVERIFY(h.engine->updateRule(h.ruleId, {{"enabled", false}}, &error));
  QCOMPARE(h.engine->detectHints("cam1").maxGapNs, 1'500'000'000LL);
  QCOMPARE(h.engine->detectHints("cam2").threshold, 0.0);
}

void TestRuleEngine::unknownFreezesDwell() {
  Harness h;
  h.feed(10, true);
  h.feed(2, false, rules::Quality::Unknown);
  h.feed(10, true);
  QCOMPARE(h.ptsNs, 11'000'000'000LL);
  QCOMPARE(h.allEvents().size(), 0);
  h.feed(1, true);
  QCOMPARE(h.allEvents().size(), 1);
  QCOMPARE(h.allEvents().first().triggerPtsNs, 11'500'000'000LL);
  QCOMPARE(h.transitions(), QStringList({"became_pending", "unknown", "triggered"}));
  QCOMPARE(h.column("SELECT quality FROM rule_evaluations WHERE transition='unknown'"), QStringList{"unknown"});
}

void TestRuleEngine::longUnknownRestartsDwell() {
  Harness h;
  h.feed(10, true);
  h.feed(6, false, rules::Quality::Unknown);
  h.feed(20, true);
  QCOMPARE(h.ptsNs, 18'000'000'000LL);
  QCOMPARE(h.allEvents().size(), 0);
  h.feed(1, true);
  QCOMPARE(h.allEvents().size(), 1);
  QCOMPARE(h.allEvents().first().openedPtsNs, 8'500'000'000LL);
}

void TestRuleEngine::staleResultsAreIgnored() {
  Harness h;
  const uint64_t current = h.generation;
  h.generation = current + 1;
  for (int i = 0; i < 30; ++i) {
    AnalysisResult r = h.frame(true, rules::Quality::Known);
    r.frame.generation = current;
    h.engine->onAnalysis(r);
  }
  QCOMPARE(h.allEvents().size(), 0);
  QCOMPARE(h.runtime("condition"), QString("inactive"));
  QCOMPARE(h.transitions(), QStringList{"stale"});
  QVERIFY(h.column("SELECT note FROM rule_evaluations").first().startsWith("generation"));

  for (int i = 0; i < 30; ++i) {
    AnalysisResult r = h.frame(true, rules::Quality::Known);
    r.frame.recvMonoNs = monoNowNs() - 6'000'000'000LL;
    h.engine->onAnalysis(r);
  }
  QCOMPARE(h.allEvents().size(), 0);
  QCOMPARE(h.runtime("condition"), QString("inactive"));

  h.feed(20, true);
  AnalysisResult replay = h.frame(true, rules::Quality::Known);
  replay.frame.ptsNs -= 2 * kStepNs;
  h.engine->onAnalysis(replay);
  h.ptsNs -= kStepNs;
  QCOMPARE(h.allEvents().size(), 0);
  h.feed(1, true);
  QCOMPARE(h.allEvents().size(), 1);
}

void TestRuleEngine::ruleRevisionKeepsOpenEvent() {
  Harness h;
  h.feed(21, true);
  QCOMPARE(h.allEvents().size(), 1);
  const QString eventId = h.allEvents().first().id;
  ServiceError error;
  const auto revised = h.engine->updateRule(h.ruleId, {{"min_confidence", 0.5}, {"severity", "review"}}, &error);
  QVERIFY2(revised.has_value(), qPrintable(error.message));
  QCOMPARE(revised->revision, 2);
  QCOMPARE(h.runtime("open_event_id"), eventId);
  h.feed(30, true);
  const auto zone = h.engine->updateZone(h.zoneId, {{"points", square(0.1, 0.9)}}, &error);
  QVERIFY2(zone.has_value(), qPrintable(error.message));
  QCOMPARE(zone->revision, 2);
  QCOMPARE(h.store.getRule(h.ruleId)->revision, 3);
  QCOMPARE(h.store.getRule(h.ruleId)->zoneRevision, 2);
  h.feed(30, true);
  QCOMPARE(h.allEvents().size(), 1);
  QCOMPARE(h.allEvents().first().id, eventId);
  QCOMPARE(h.allEvents().first().condition, QString("active"));
  QCOMPARE(h.runtime("open_event_id"), eventId);
  QVERIFY(!h.engine->updateRule(h.ruleId, {{"camera_id", "cam2"}}, &error));
  QCOMPARE(error.status, 400);
}

void TestRuleEngine::clearedThenRearmSuppressesNextOccupancy() {
  Harness h;
  h.feed(21, true);
  const QString first = h.allEvents().first().id;
  h.feed(1, false);
  QCOMPARE(h.store.getEvent(first)->condition, QString("clearing"));
  h.feed(9, false);
  QCOMPARE(h.store.getEvent(first)->condition, QString("clearing"));
  h.feed(1, false);
  QCOMPARE(h.store.getEvent(first)->condition, QString("cleared"));
  const int64_t clearedPts = h.ptsNs;
  h.feed(59, true);
  QCOMPARE(h.ptsNs - clearedPts, 29'500'000'000LL);
  QCOMPARE(h.allEvents().size(), 1);
  QVERIFY(h.transitions().contains("suppressed"));
  QCOMPARE(h.transitions().count("suppressed"), 1);
  h.feed(1, true);
  QCOMPARE(h.allEvents().size(), 2);
  const EventRecord second = h.allEvents().first();
  QVERIFY(second.id != first);
  QCOMPARE(second.triggerPtsNs - clearedPts, 30'000'000'000LL);
  QCOMPARE(h.transitions(),
           QStringList({"became_pending", "triggered", "became_clearing", "cleared", "became_pending", "suppressed", "triggered"}));
}

void TestRuleEngine::deletingRuleClearsOpenEvent() {
  Harness h;
  h.feed(21, true);
  const QString eventId = h.allEvents().first().id;
  ServiceError error;
  QVERIFY(h.engine->deleteRule(h.ruleId, &error));
  QCOMPARE(h.store.getEvent(eventId)->condition, QString("cleared"));
  QCOMPARE(h.column("SELECT note FROM rule_evaluations WHERE transition='cleared'"), QStringList{"rule_deleted"});
  h.feed(30, true);
  QCOMPARE(h.allEvents().size(), 1);
  QVERIFY(!h.engine->deleteRule(h.ruleId, &error));
  QCOMPARE(error.status, 404);

  Harness z;
  z.feed(21, true);
  QVERIFY(z.engine->deleteZone(z.zoneId, &error));
  QCOMPARE(z.store.getRule(z.ruleId)->enabled, false);
  QCOMPARE(z.allEvents().first().condition, QString("cleared"));
  QVERIFY(z.transitions().contains("cleared"));
}

void TestRuleEngine::deliveriesRetryThenFail() {
  Harness h;
  h.feed(21, true);
  const EventRecord e = h.allEvents().first();
  const int64_t created = h.store.deliveriesForEvent(e.id).first().createdUtcMs;
  h.events->checkDeliveries(created + 9'999);
  QCOMPARE(h.store.deliveriesForEvent(e.id).first().attempts, 0);
  h.events->checkDeliveries(created + 10'000);
  QCOMPARE(h.store.deliveriesForEvent(e.id).first().attempts, 1);
  QCOMPARE(h.store.deliveriesForEvent(e.id).first().lastError, QString("no console connected"));
  h.events->checkDeliveries(created + 20'000);
  h.events->checkDeliveries(created + 30'000);
  const QVector<AlertDelivery> failed = h.store.deliveriesForEvent(e.id);
  QCOMPARE(failed.first().state, QString("failed"));
  QCOMPARE(failed.first().attempts, 3);
  QCOMPARE(h.events->pendingAlertsJson("console-a", created + 31'000).size(), 2);
  ServiceError error;
  const auto delivered = h.events->confirmDelivery(failed.first().id, "console-a", &error);
  QVERIFY(delivered.has_value());
  QCOMPARE(delivered->state, QString("delivered"));
  QCOMPARE(h.events->pendingAlertsJson("console-a", created + 32'000).size(), 1);
  QVERIFY(h.events->acknowledge(e.id, "op", &error).has_value());
  QVERIFY(h.events->review(e.id, {{"label", "confirmed"}, {"note", "seen"}, {"operator", "op"}}, &error).has_value());
  QVERIFY(!h.events->review(e.id, {{"label", "maybe"}}, &error).has_value());
  QCOMPARE(error.status, 400);
  QCOMPARE(h.events->resolve(e.id, "op", &error)->operatorState, QString("resolved"));
  QCOMPARE(h.events->pendingAlertsJson("console-a", created + 33'000).size(), 0);
  const QJsonObject detail = *h.events->eventDetailJson(e.id);
  QCOMPARE(detail.value("review").toObject().value("label").toString(), QString("confirmed"));
  QCOMPARE(detail.value("evaluations").toArray().size(), 2);
  QCOMPARE(h.column("SELECT action FROM audit_log WHERE actor='op' ORDER BY id"),
           QStringList({"event.acknowledge", "event.review", "event.resolve"}));
}

void TestRuleEngine::overdueDeliveryCountsOneAttemptPerInterval() {
  Harness h;
  h.feed(21, true);
  const EventRecord e = h.allEvents().first();
  const int64_t created = h.store.deliveriesForEvent(e.id).first().createdUtcMs;
  const int64_t stalled = created + 3'600'000;
  for (int64_t offset : {0, 1'000, 2'000, 9'999}) h.events->checkDeliveries(stalled + offset);
  QCOMPARE(h.store.deliveriesForEvent(e.id).first().attempts, 1);
  QCOMPARE(h.store.deliveriesForEvent(e.id).first().state, QString("pending"));
  h.events->checkDeliveries(stalled + 10'000);
  QCOMPARE(h.store.deliveriesForEvent(e.id).first().attempts, 2);
}

void TestRuleEngine::validatesZonesAndRules() {
  Harness h;
  ServiceError error;
  const auto zoneFails = [&](const QJsonObject& body) { return !h.engine->createZone(body, &error).has_value() && error.status == 400; };
  QJsonObject zone{{"camera_id", "cam1"}, {"name", "Z"}, {"points", square(0.1, 0.2)}};
  QVERIFY(h.engine->createZone(zone, &error).has_value());
  QVERIFY(zoneFails(QJsonObject{{"camera_id", "nope"}, {"name", "Z"}, {"points", square(0.1, 0.2)}}));
  QVERIFY(zoneFails(QJsonObject{{"camera_id", "cam1"}, {"name", "Z"}, {"points", QJsonArray{QJsonArray{0.1, 0.1}, QJsonArray{0.2, 0.2}}}}));
  QVERIFY(zoneFails(QJsonObject{{"camera_id", "cam1"}, {"name", "Z"}, {"points", square(0.5, 1.2)}}));
  QJsonArray many;
  for (int i = 0; i < 33; ++i) many.push_back(QJsonArray{0.5, i / 40.0});
  QVERIFY(zoneFails(QJsonObject{{"camera_id", "cam1"}, {"name", "Z"}, {"points", many}}));
  QVERIFY(zoneFails(QJsonObject{{"camera_id", "cam1"}, {"name", "Z"}, {"points", square(0.1, 0.2)}, {"anchor", "head"}}));
  QVERIFY(zoneFails(QJsonObject{{"camera_id", "cam1"}, {"name", ""}, {"points", square(0.1, 0.2)}}));

  const auto ruleFails = [&](const QJsonObject& overrides) {
    QJsonObject body{{"name", "R"}, {"camera_id", "cam1"}, {"zone_id", h.zoneId}};
    for (auto it = overrides.begin(); it != overrides.end(); ++it) body.insert(it.key(), it.value());
    return !h.engine->createRule(body, &error).has_value() && error.status == 400;
  };
  QVERIFY(!ruleFails({}));
  QVERIFY(ruleFails({{"dwell_ns", 0.5e9}}));
  QVERIFY(ruleFails({{"dwell_ns", 3601e9}}));
  QVERIFY(ruleFails({{"dwell_ns", "10"}}));
  QVERIFY(ruleFails({{"min_confidence", 1.5}}));
  QVERIFY(ruleFails({{"time_zone", "Mars/Olympus"}}));
  QVERIFY(ruleFails({{"camera_id", "cam9"}}));
  QVERIFY(ruleFails({{"zone_id", "missing"}}));
  QVERIFY(ruleFails({{"target_class", "dragon"}}));
  QVERIFY(ruleFails({{"severity", "urgent"}}));
  QVERIFY(ruleFails({{"vlm_role", "verify"}}));
  QVERIFY(ruleFails({{"schedule", QJsonArray{QJsonObject{{"days", 0}, {"start_minute", 0}, {"end_minute", 60}}}}}));
  QVERIFY(ruleFails({{"schedule", QJsonArray{QJsonObject{{"days", 3}, {"start_minute", 0}, {"end_minute", 1441}}}}}));
  QVERIFY(!ruleFails({{"time_zone", "Asia/Seoul"},
                      {"schedule", QJsonArray{QJsonObject{{"days", 0x1f}, {"start_minute", 22 * 60}, {"end_minute", 6 * 60}}}}}));
  QCOMPARE(h.engine->updateRule("missing", {}, &error).has_value(), false);
  QCOMPARE(error.status, 404);
}

void TestRuleEngine::parsesDetectReply() {
  const QJsonObject detection{{"track_id", "a.1-3"},
                              {"cls", "person"},
                              {"confidence", 0.8},
                              {"bbox", QJsonArray{0.1, 0.2, 0.3, 0.9}},
                              {"anchor_foot", QJsonArray{0.2, 0.9}},
                              {"anchor_center", QJsonArray{0.2, 0.55}}};
  QJsonObject untracked = detection;
  untracked.insert("track_id", QJsonValue::Null);
  QJsonObject body{{"job_id", "j"},
                   {"generation", 4},
                   {"status", "ok"},
                   {"contract_violations", QJsonArray{}},
                   {"frames", QJsonArray{QJsonObject{{"frame_id", "f"},
                                                     {"pts_ns", 123456789012.0},
                                                     {"width", 960},
                                                     {"height", 540},
                                                     {"detections", QJsonArray{detection, untracked}}}}}};
  QVector<DetectedObject> out;
  QCOMPARE(parseDetectReply(body, "j", 4, "f", 123456789012, &out), QString());
  QCOMPARE(out.size(), 2);
  QCOMPARE(out[0].trackId, QString("a.1-3"));
  QCOMPARE(out[0].foot, QPointF(0.2, 0.9));
  QVERIFY(out[1].trackId.isEmpty());
  QVERIFY(parseDetectReply(body, "other", 4, "f", 123456789012, &out).startsWith("contract_violation"));
  QVERIFY(parseDetectReply(body, "j", 5, "f", 123456789012, &out).startsWith("contract_violation"));
  QVERIFY(parseDetectReply(body, "j", 4, "g", 123456789012, &out).startsWith("contract_violation"));
  QVERIFY(parseDetectReply(body, "j", 4, "f", 1, &out).startsWith("contract_violation"));
  QJsonObject bad = body;
  QJsonObject badDetection = detection;
  badDetection.insert("bbox", QJsonArray{0.1, 0.2, 1.3, 0.9});
  QJsonObject frame = body.value("frames").toArray().first().toObject();
  frame.insert("detections", QJsonArray{badDetection});
  bad.insert("frames", QJsonArray{frame});
  QVERIFY(parseDetectReply(bad, "j", 4, "f", 123456789012, &out).startsWith("contract_violation"));
  QJsonObject violated = body;
  violated.insert("contract_violations", QJsonArray{"frame 0: unknown frame id"});
  QVERIFY(parseDetectReply(violated, "j", 4, "f", 123456789012, &out).startsWith("contract_violation"));
  QJsonObject partial = body;
  partial.insert("status", "partial");
  QVERIFY(parseDetectReply(partial, "j", 4, "f", 123456789012, &out).startsWith("worker_status_partial"));
  QVERIFY(parseDetectReply(QJsonObject{{"error", QJsonObject{{"code", "deadline_exceeded"}, {"message", "busy"}}}}, "j", 4, "f", 1, &out)
              .startsWith("worker_error"));
}

QTEST_GUILESS_MAIN(TestRuleEngine)
#include "test_rule_engine.moc"
