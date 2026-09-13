#include "alerts/AlertLogic.h"
#include "core/AlertTypes.h"
#include "fovea/FrameRing.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace fovea::ui;

namespace {
constexpr int64_t kSecond = 1'000'000'000;

EventInfo eventWith(const QString& operatorState, const QString& reviewLabel = {}) {
  EventInfo e;
  e.operatorState = operatorState;
  if (!reviewLabel.isEmpty()) e.review = ReviewInfo{reviewLabel, {}, {}, 0};
  return e;
}
}

class TestAlertLogic : public QObject {
  Q_OBJECT
private slots:
  void bucketsFollowOperatorStateAndFalseAlarms();
  void daysAndMinutes();
  void dwellLabels();
  void triggerSentenceFromStructuredFields();
  void overlayFreshnessNeedsSameSessionWithinOneSecond();
  void activityIsNewestFirstWithDeliveriesAndReview();
  void ruleRevisionRoundTripKeepsUneditedFields();
  void detectionFrameParsesNormalizedBoxes();
  void eventParsesReviewEvidenceAndDeliveries();
};

void TestAlertLogic::bucketsFollowOperatorStateAndFalseAlarms() {
  QCOMPARE(alertBucket(eventWith(QStringLiteral("new"))), AlertBucket::Unresolved);
  QCOMPARE(alertBucket(eventWith(QStringLiteral("acknowledged"))), AlertBucket::Acknowledged);
  QCOMPARE(alertBucket(eventWith(QStringLiteral("resolved"))), AlertBucket::Dismissed);
  QCOMPARE(alertBucket(eventWith(QStringLiteral("resolved"), QStringLiteral("confirmed"))), AlertBucket::Dismissed);
  QCOMPARE(alertBucket(eventWith(QStringLiteral("new"), QStringLiteral("false_alarm"))), AlertBucket::Dismissed);
  QCOMPARE(alertBucket(eventWith(QStringLiteral("acknowledged"), QStringLiteral("undecided"))), AlertBucket::Acknowledged);
}

void TestAlertLogic::daysAndMinutes() {
  QCOMPARE(daysLabel(0x7f), QStringLiteral("every day"));
  QCOMPARE(daysLabel(0x1f), QStringLiteral("Mon–Fri"));
  QCOMPARE(daysLabel(0x60), QStringLiteral("Sat, Sun"));
  QCOMPARE(daysLabel(0x15), QStringLiteral("Mon, Wed, Fri"));
  QCOMPARE(daysLabel(0x0e), QStringLiteral("Tue–Thu"));
  QCOMPARE(daysLabel(0x4f), QStringLiteral("Mon–Thu, Sun"));
  QCOMPARE(daysLabel(0), QStringLiteral("no days"));
  QCOMPARE(minuteLabel(450), QStringLiteral("07:30"));
  QCOMPARE(minuteLabel(1440), QStringLiteral("24:00"));
  QCOMPARE(scheduleLabel({ScheduleWindow{0x7f, 0, 1440}}), QStringLiteral("at any time"));
  QCOMPARE(scheduleLabel({ScheduleWindow{0x60, 0, 0}}), QStringLiteral("Sat, Sun, all day"));
  QCOMPARE(scheduleLabel({ScheduleWindow{0x1f, 420, 1140}}), QStringLiteral("Mon–Fri 07:00–19:00"));
  QCOMPARE(scheduleLabel({ScheduleWindow{0x1f, 480, 480}}), QStringLiteral("Mon–Fri 08:00–08:00 next day"));
  QCOMPARE(scheduleLabel({ScheduleWindow{0x7f, 1320, 360}}), QStringLiteral("every day 22:00–06:00 next day"));
}

void TestAlertLogic::dwellLabels() {
  QCOMPARE(dwellLabel(10 * kSecond), QStringLiteral("10 s"));
  QCOMPARE(dwellLabel(1'500'000'000), QStringLiteral("1.5 s"));
  QCOMPARE(dwellLabel(90 * kSecond), QStringLiteral("1 min 30 s"));
  QCOMPARE(dwellLabel(120 * kSecond), QStringLiteral("2 min"));
}

void TestAlertLogic::triggerSentenceFromStructuredFields() {
  RuleInfo rule;
  rule.schedule = {ScheduleWindow{0x1f, 420, 1140}};
  rule.timeZone = QStringLiteral("Asia/Seoul");
  rule.minConfidence = 0.5;
  rule.dwellNs = 10 * kSecond;
  QCOMPARE(triggerSentence(rule, QStringLiteral("Gate apron"), QStringLiteral("CAM-01")),
           QStringLiteral("Alert when a person stays in “Gate apron” on CAM-01 for 10 s, Mon–Fri 07:00–19:00 (Asia/Seoul), "
                          "confidence ≥ 50%."));
  rule.schedule = {ScheduleWindow{}};
  QCOMPARE(triggerSentence(rule, QString(), QString()),
           QStringLiteral("Alert when a person stays in the zone on an unknown camera for 10 s, at any time, confidence ≥ 50%."));
}

void TestAlertLogic::overlayFreshnessNeedsSameSessionWithinOneSecond() {
  const QString session = QStringLiteral("6f1c2b9e-3d4a-4b5c-8d6e-7f8091a2b3c4");
  const std::array<uint8_t, 16> bytes = fovea::sessionIdBytes(session);
  const int64_t frame = 50 * kSecond;
  QVERIFY(detectionMatchesFrame(session, frame - kOverlayMaxAgeNs, bytes, frame));
  QVERIFY(detectionMatchesFrame(session, frame + 200'000'000, bytes, frame));
  QVERIFY(!detectionMatchesFrame(session, frame - kOverlayMaxAgeNs - 1, bytes, frame));
  QVERIFY(!detectionMatchesFrame(QStringLiteral("0b5e8d21-5c7a-4f7e-9a51-0c1d2e3f4a5b"), frame, bytes, frame));
  QVERIFY(!detectionMatchesFrame(QString(), frame, bytes, frame));
  QVERIFY(!detectionMatchesFrame(session, frame, std::array<uint8_t, 16>{}, frame));
}

void TestAlertLogic::activityIsNewestFirstWithDeliveriesAndReview() {
  EventInfo e;
  e.openedUtcMs = 2000;
  EvaluationInfo pending;
  pending.utcMs = 1000;
  pending.transition = QStringLiteral("became_pending");
  pending.trackIds = {QStringLiteral("7")};
  EvaluationInfo triggered;
  triggered.utcMs = 2000;
  triggered.transition = QStringLiteral("triggered");
  triggered.dwellNs = 10'400'000'000;
  triggered.trackIds = {QStringLiteral("7")};
  e.evaluations = {pending, triggered};
  DeliveryInfo console;
  console.channel = QStringLiteral("console");
  console.state = QStringLiteral("failed");
  console.attempts = 3;
  console.lastError = QStringLiteral("no console connected");
  console.createdUtcMs = 2100;
  DeliveryInfo sound;
  sound.channel = QStringLiteral("sound");
  sound.state = QStringLiteral("delivered");
  sound.deliveredUtcMs = 2500;
  e.deliveries = {console, sound};
  e.review = ReviewInfo{QStringLiteral("false_alarm"), QStringLiteral("shadow"), QStringLiteral("console"), 3000};

  const QVector<ActivityLine> lines = activityLines(e);
  QCOMPARE(lines.size(), 5);
  QCOMPARE(lines[0].text, QStringLiteral("reviewed: false alarm · shadow"));
  QCOMPARE(lines[1].text, QStringLiteral("sound played"));
  QCOMPARE(lines[2].text, QStringLiteral("console delivery failed after 3 attempts · no console connected"));
  QCOMPARE(lines[3].text, QStringLiteral("triggered · dwell 10.4 s · track 7"));
  QCOMPARE(lines[4].text, QStringLiteral("became pending · track 7"));
}

void TestAlertLogic::ruleRevisionRoundTripKeepsUneditedFields() {
  const QJsonObject stored{{"id", "r1"}, {"name", "Gate"}, {"revision", 4}, {"enabled", false}, {"camera_id", "c1"},
                           {"zone_id", "z1"}, {"zone_revision", 2},
                           {"schedule", QJsonArray{QJsonObject{{"days", 31}, {"start_minute", 420}, {"end_minute", 1140}}}},
                           {"time_zone", "Asia/Seoul"}, {"target_class", "person"}, {"min_confidence", 0.55},
                           {"dwell_ns", 12e9}, {"severity", "review"},
                           {"actions", QJsonObject{{"sound", true}, {"pop_to_main_view", false}}},
                           {"rearm_ns", 45e9}, {"vlm_role", "none"}, {"runtime", QJsonArray{}}};
  const RuleInfo rule = RuleInfo::fromJson(stored);
  QCOMPARE(rule.revision, 4);
  QCOMPARE(rule.enabled, false);
  QCOMPARE(rule.schedule.size(), 1);
  QCOMPARE(rule.schedule[0].startMinute, 420);
  QCOMPARE(rule.dwellNs, int64_t{12'000'000'000});
  QVERIFY(rule.soundAction);
  QVERIFY(!rule.popAction);

  const QJsonObject body = rule.revisionJson();
  QCOMPARE(body.value("rearm_ns").toDouble(), 45e9);
  QCOMPARE(body.value("vlm_role").toString(), QStringLiteral("none"));
  QVERIFY(!body.contains("runtime"));
  QVERIFY(!body.contains("id"));
  const RuleInfo again = RuleInfo::fromJson(body);
  QCOMPARE(again.zoneRevision, 2);
  QCOMPARE(again.timeZone, QStringLiteral("Asia/Seoul"));
  QCOMPARE(again.minConfidence, 0.55);
  QCOMPARE(again.severity, QStringLiteral("review"));
}

void TestAlertLogic::detectionFrameParsesNormalizedBoxes() {
  const QJsonObject o{{"camera_id", "c1"}, {"session_id", "s1"}, {"frame_id", "f9"}, {"pts_ns", 1.5e9}, {"utc_ms", 1e12},
                      {"width", 640}, {"height", 360},
                      {"detections", QJsonArray{QJsonObject{{"track_id", 7}, {"cls", "person"}, {"confidence", 0.94},
                                                            {"bbox", QJsonArray{0.1, 0.2, 0.3, 0.6}}},
                                                QJsonObject{{"cls", "person"}, {"bbox", QJsonArray{0.1}}}}}};
  const DetectionFrame f = DetectionFrame::fromJson(o);
  QCOMPARE(f.ptsNs, int64_t{1'500'000'000});
  QCOMPARE(f.detections.size(), 1);
  QCOMPARE(f.detections[0].trackId, QStringLiteral("7"));
  QCOMPARE(f.detections[0].box, QRectF(QPointF(0.1, 0.2), QPointF(0.3, 0.6)));
}

void TestAlertLogic::eventParsesReviewEvidenceAndDeliveries() {
  const QJsonObject o{{"id", "e1"}, {"rule_id", "r1"}, {"operator_state", "acknowledged"}, {"severity", "critical"},
                      {"opened_utc_ms", 1000}, {"review", QJsonObject{{"label", "confirmed"}, {"utc_ms", 1200}}},
                      {"evidence", QJsonObject{{"id", "v1"}, {"state", "partial"}, {"reason", "gap"}, {"from_utc_ms", 990},
                                               {"to_utc_ms", 1010}}},
                      {"deliveries", QJsonArray{QJsonObject{{"id", "d1"}, {"channel", "console"}, {"state", "delivered"}}}}};
  const EventInfo e = EventInfo::fromJson(o);
  QVERIFY(e.review);
  QCOMPARE(e.review->label, QStringLiteral("confirmed"));
  QVERIFY(e.evidence);
  QCOMPARE(e.evidence->state, QStringLiteral("partial"));
  QCOMPARE(e.evidence->reason, QStringLiteral("gap"));
  QCOMPARE(e.deliveries.size(), 1);
  QCOMPARE(e.deliveries[0].channel, QStringLiteral("console"));
  QCOMPARE(alertBucket(e), AlertBucket::Acknowledged);
  const EventInfo bare = EventInfo::fromJson(QJsonObject{{"id", "e2"}, {"review", QJsonValue::Null}});
  QVERIFY(!bare.review);
  QVERIFY(!bare.evidence);
}

QTEST_GUILESS_MAIN(TestAlertLogic)
#include "test_alert_logic.moc"
