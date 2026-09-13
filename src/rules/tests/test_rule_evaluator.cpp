#include "fovea/rules/RuleEvaluator.h"
#include <QTest>
#include <algorithm>

using namespace fovea::rules;

namespace {

constexpr int64_t kMsNs = 1'000'000;
constexpr int64_t kSecNs = 1'000'000'000;
constexpr int kRefW = 1280;
constexpr int kRefH = 720;
// 2026-09-13 14:00:00 UTC, a Sunday (23:00 in Asia/Seoul).
constexpr int64_t kSundayNightUtcMs = 1789308000000;
// 2026-09-14 03:00:00 UTC, a Monday (12:00 in Asia/Seoul).
constexpr int64_t kMondayNoonUtcMs = 1789354800000;

RuleRevision makeRule() {
  RuleRevision r;
  r.ruleId = QStringLiteral("rule-1");
  r.revision = 1;
  r.cameraId = QStringLiteral("cam-1");
  r.name = QStringLiteral("person in zone");
  r.zone.zoneId = QStringLiteral("zone-1");
  r.zone.points = {{0.2, 0.2}, {0.8, 0.2}, {0.8, 0.8}, {0.2, 0.8}};
  r.zone.refWidth = kRefW;
  r.zone.refHeight = kRefH;
  r.dwellNs = 10 * kSecNs;
  r.maxObservationGapNs = 1500 * kMsNs;
  r.clearAfterNs = 5 * kSecNs;
  r.rearmNs = 30 * kSecNs;
  r.resultTtlNs = 5 * kSecNs;
  return r;
}

TrackObservation track(const QString& id, bool inside = true, const QString& cls = QStringLiteral("person"),
                       double confidence = 0.9) {
  TrackObservation t;
  t.trackId = id;
  t.cls = cls;
  t.anchor = inside ? QPointF(0.5, 0.5) : QPointF(0.1, 0.5);
  t.confidence = confidence;
  return t;
}

QVector<TrackObservation> insideTracks(const QStringList& ids) {
  QVector<TrackObservation> v;
  for (const QString& id : ids) v.push_back(track(id));
  return v;
}

int64_t toMs(int64_t ns) { return ns / kMsNs; }

// QCOMPARE cannot print the rule enums (their toString returns QString), so
// compare the names.
#define QCOMPARE_ENUM(actual, expected) QCOMPARE(fovea::rules::toString(actual), fovea::rules::toString(expected))

// Feeds a RuleEvaluator with frames described in media milliseconds. Receive
// time follows media time plus a per-session offset, wall time adds a fixed
// evaluation latency; utc follows a base.
struct Sim {
  RuleEvaluator ev;
  QString session = QStringLiteral("s1");
  uint64_t generation = 1;
  int64_t baseUtcMs = kSundayNightUtcMs;
  int64_t latencyNs = 100 * kMsNs;
  int64_t recvOffsetMs = 0;
  int64_t lastRecvNs = 0;
  int frameWidth = kRefW;
  int frameHeight = kRefH;
  int issued = 0;

  explicit Sim(RuleRevision rule = makeRule())
      : ev(std::move(rule), [this] { return QStringLiteral("evt-%1").arg(++issued); }) {}
  Sim(const Sim&) = delete;
  Sim& operator=(const Sim&) = delete;

  ObservationFrame frame(int64_t atMs, QVector<TrackObservation> tracks = {}, Quality q = Quality::Known) const {
    ObservationFrame f;
    f.cameraId = QStringLiteral("cam-1");
    f.sessionId = session;
    f.ptsNs = atMs * kMsNs;
    f.utcMs = baseUtcMs + atMs;
    f.recvMonoNs = (atMs + recvOffsetMs) * kMsNs;
    f.generation = generation;
    f.quality = q;
    f.frameWidth = frameWidth;
    f.frameHeight = frameHeight;
    f.tracks = std::move(tracks);
    return f;
  }
  Evaluation feed(const ObservationFrame& f) {
    lastRecvNs = std::max(lastRecvNs, f.recvMonoNs);
    return ev.evaluate(f, f.recvMonoNs + latencyNs);
  }
  // Switches to a new session whose first frame at firstPtsMs is received
  // afterMs after the latest frame fed so far.
  void startSession(const QString& id, int64_t firstPtsMs, int64_t afterMs = 500) {
    session = id;
    recvOffsetMs = lastRecvNs / kMsNs + afterMs - firstPtsMs;
  }
  // Feeds a frame whose receive age exceeds the default TTL.
  Evaluation expired(int64_t atMs, QVector<TrackObservation> tracks = {}) {
    const ObservationFrame f = frame(atMs, std::move(tracks));
    return ev.evaluate(f, f.recvMonoNs + 6 * kSecNs);
  }
  std::optional<Evaluation> apply(RuleRevision rule) { return ev.setRule(std::move(rule), baseUtcMs); }
  Evaluation known(int64_t atMs, const QStringList& ids = {}) { return feed(frame(atMs, insideTracks(ids))); }
  Evaluation raw(int64_t atMs, QVector<TrackObservation> tracks) { return feed(frame(atMs, std::move(tracks))); }
  Evaluation unknown(int64_t atMs) { return feed(frame(atMs, {}, Quality::Unknown)); }
  // The track is seen outside the zone: presence ends on this frame.
  Evaluation leave(int64_t atMs, const QString& id = QStringLiteral("t1")) { return raw(atMs, {track(id, false)}); }

  // Frames every stepMs from fromMs to toMs inclusive; returns all evaluations.
  QVector<Evaluation> run(int64_t fromMs, int64_t toMs, int64_t stepMs, const QStringList& ids = {}) {
    QVector<Evaluation> out;
    for (int64_t atMs = fromMs; atMs <= toMs; atMs += stepMs) out.push_back(known(atMs, ids));
    return out;
  }
  QVector<Evaluation> runUnknown(int64_t fromMs, int64_t toMs, int64_t stepMs) {
    QVector<Evaluation> out;
    for (int64_t atMs = fromMs; atMs <= toMs; atMs += stepMs) out.push_back(unknown(atMs));
    return out;
  }
  // Drives the default rule to Active with an open event: t1 inside from
  // 0 to 10000 ms at 2 fps. Returns the Triggered evaluation.
  Evaluation trigger(const QString& id = QStringLiteral("t1")) { return run(0, 10000, 500, {id}).last(); }
};

bool allTransitions(const QVector<Evaluation>& evals, Transition t) {
  return std::all_of(evals.cbegin(), evals.cend(), [t](const Evaluation& e) { return e.transition == t; });
}

bool noneTriggered(const QVector<Evaluation>& evals) {
  return std::none_of(evals.cbegin(), evals.cend(),
                      [](const Evaluation& e) { return e.transition == Transition::Triggered; });
}

}

class TestRuleEvaluator : public QObject {
  Q_OBJECT
private slots:
  void zoneContainsPoint() {
    ZoneRevision z;
    z.points = {{0.2, 0.2}, {0.8, 0.2}, {0.8, 0.8}, {0.2, 0.8}};
    QVERIFY(z.contains({0.5, 0.5}));
    QVERIFY(!z.contains({0.1, 0.5}));
    QVERIFY(!z.contains({0.5, 0.9}));
  }

  void initialStateIsUnknownAndInactive() {
    Sim sim;
    QCOMPARE_ENUM(sim.ev.state(), ConditionState::Inactive);
    QCOMPARE_ENUM(sim.ev.quality(), Quality::Unknown);
    QVERIFY(!sim.ev.openEventId().has_value());
    const Evaluation e = sim.known(0);
    QCOMPARE_ENUM(e.transition, Transition::None);
    QCOMPARE_ENUM(e.quality, Quality::Known);
    QCOMPARE(e.ruleId, QStringLiteral("rule-1"));
    QCOMPARE(e.ruleRevision, 1);
    QCOMPARE(e.cameraId, QStringLiteral("cam-1"));
    QCOMPARE(e.sessionId, QStringLiteral("s1"));
    QCOMPARE(e.generation, uint64_t(1));
    QCOMPARE(e.utcMs, kSundayNightUtcMs);
    QCOMPARE_ENUM(sim.ev.quality(), Quality::Known);
  }

  void triggersAfterExactDwellAt2Fps() {
    Sim sim;
    const Evaluation first = sim.known(0, {"t1"});
    QCOMPARE_ENUM(first.transition, Transition::BecamePending);
    QCOMPARE_ENUM(first.before, ConditionState::Inactive);
    QCOMPARE_ENUM(first.after, ConditionState::Pending);
    QCOMPARE(first.dwellNs, int64_t(0));

    const auto pending = sim.run(500, 9500, 500, {"t1"});
    QVERIFY(allTransitions(pending, Transition::None));
    QCOMPARE_ENUM(pending.last().after, ConditionState::Pending);
    QCOMPARE(toMs(pending.last().dwellNs), int64_t(9500));
    QVERIFY(!sim.ev.openEventId().has_value());

    const Evaluation fired = sim.known(10000, {"t1"});
    QCOMPARE_ENUM(fired.transition, Transition::Triggered);
    QCOMPARE_ENUM(fired.before, ConditionState::Pending);
    QCOMPARE_ENUM(fired.after, ConditionState::Active);
    QCOMPARE(fired.eventId, QStringLiteral("evt-1"));
    QCOMPARE(fired.trackIds, QVector<QString>{QStringLiteral("t1")});
    QCOMPARE(toMs(fired.windowStartPtsNs), int64_t(0));
    QCOMPARE(toMs(fired.windowEndPtsNs), int64_t(10000));
    QCOMPARE(toMs(fired.dwellNs), int64_t(10000));
    QCOMPARE_ENUM(fired.quality, Quality::Known);
    QCOMPARE(sim.ev.openEventId(), std::optional<QString>(QStringLiteral("evt-1")));
  }

  void triggersAfterExactDwellAt10Fps() {
    Sim sim;
    const auto pending = sim.run(0, 9900, 100, {"t1"});
    QVERIFY(noneTriggered(pending));
    QCOMPARE_ENUM(pending.last().after, ConditionState::Pending);
    const Evaluation fired = sim.known(10000, {"t1"});
    QCOMPARE_ENUM(fired.transition, Transition::Triggered);
    QCOMPARE(toMs(fired.windowStartPtsNs), int64_t(0));
  }

  void ignoredTracksDoNotStartPending() {
    Sim sim;
    Evaluation e = sim.raw(0, {track("car", true, QStringLiteral("car"))});
    QCOMPARE_ENUM(e.transition, Transition::None);
    QCOMPARE_ENUM(e.after, ConditionState::Inactive);
    e = sim.raw(500, {track("faint", true, QStringLiteral("person"), 0.2)});
    QCOMPARE_ENUM(e.transition, Transition::None);
    QCOMPARE_ENUM(e.after, ConditionState::Inactive);
    QVERIFY(e.trackIds.isEmpty());
    e = sim.raw(1000, {track("out", false)});
    QCOMPARE_ENUM(e.transition, Transition::None);
    QCOMPARE_ENUM(e.after, ConditionState::Inactive);
  }

  void shortTrackGapKeepsDwell() {
    Sim sim;
    sim.run(0, 4000, 500, {"t1"});
    // Detector misses t1 for one second (frames still arrive).
    const auto missed = sim.run(4500, 5000, 500);
    QVERIFY(allTransitions(missed, Transition::None));
    QCOMPARE_ENUM(missed.last().after, ConditionState::Pending);
    QCOMPARE(missed.last().trackIds, QVector<QString>{QStringLiteral("t1")});
    const auto rest = sim.run(5500, 9500, 500, {"t1"});
    QVERIFY(allTransitions(rest, Transition::None));
    const Evaluation fired = sim.known(10000, {"t1"});
    QCOMPARE_ENUM(fired.transition, Transition::Triggered);
    QCOMPARE(toMs(fired.windowStartPtsNs), int64_t(0));
    QCOMPARE(toMs(fired.dwellNs), int64_t(10000));
  }

  void longTrackGapResetsDwell() {
    Sim sim;
    sim.run(0, 4000, 500, {"t1"});
    // t1 unseen for two seconds: presence survives up to the gap, then resets.
    const auto tolerated = sim.run(4500, 5500, 500);
    QVERIFY(allTransitions(tolerated, Transition::None));
    const Evaluation reset = sim.known(6000);
    QCOMPARE_ENUM(reset.transition, Transition::PendingReset);
    QCOMPARE_ENUM(reset.after, ConditionState::Inactive);
    QVERIFY(reset.trackIds.isEmpty());
    const Evaluation again = sim.known(6500, {"t1"});
    QCOMPARE_ENUM(again.transition, Transition::BecamePending);
    QCOMPARE(toMs(again.windowStartPtsNs), int64_t(6500));
    QCOMPARE(again.dwellNs, int64_t(0));
    QVERIFY(noneTriggered(sim.run(7000, 16000, 500, {"t1"})));
    const Evaluation fired = sim.known(16500, {"t1"});
    QCOMPARE_ENUM(fired.transition, Transition::Triggered);
    QCOMPARE(toMs(fired.windowStartPtsNs), int64_t(6500));
  }

  void trackSeenOutsideRestartsDwell() {
    Sim sim;
    sim.run(0, 5000, 500, {"t1"});
    const Evaluation outside = sim.leave(5500);
    QCOMPARE_ENUM(outside.transition, Transition::PendingReset);
    QCOMPARE_ENUM(outside.after, ConditionState::Inactive);
    const Evaluation back = sim.known(6000, {"t1"});
    QCOMPARE_ENUM(back.transition, Transition::BecamePending);
    QCOMPARE(toMs(back.windowStartPtsNs), int64_t(6000));
    QVERIFY(noneTriggered(sim.run(6500, 15500, 500, {"t1"})));
    QCOMPARE_ENUM(sim.known(16000, {"t1"}).transition, Transition::Triggered);
  }

  void unknownFramesFreezeTimers() {
    Sim sim;
    sim.run(0, 9500, 100, {"t1"});
    // One second of unknown frames: continuity survives (gap 1.1 s), but the
    // frozen second does not count as dwell.
    const auto frozen = sim.runUnknown(9600, 10500, 100);
    QVERIFY(allTransitions(frozen, Transition::Unknown));
    QCOMPARE_ENUM(frozen.last().quality, Quality::Unknown);
    QCOMPARE_ENUM(frozen.last().after, ConditionState::Pending);
    QCOMPARE_ENUM(sim.ev.quality(), Quality::Unknown);

    const Evaluation resumed = sim.known(10600, {"t1"});
    QCOMPARE_ENUM(resumed.quality, Quality::Known);
    QCOMPARE_ENUM(resumed.transition, Transition::None);
    QCOMPARE(toMs(resumed.dwellNs), int64_t(9600));
    QCOMPARE(toMs(resumed.windowStartPtsNs), int64_t(0));
    QCOMPARE(toMs(resumed.unknownNs), int64_t(1000));
    QVERIFY(noneTriggered(sim.run(10700, 10900, 100, {"t1"})));
    const Evaluation fired = sim.known(11000, {"t1"});
    QCOMPARE_ENUM(fired.transition, Transition::Triggered);
    QCOMPARE(toMs(fired.dwellNs), int64_t(10000));
    QCOMPARE(toMs(fired.windowStartPtsNs), int64_t(0));
    QCOMPARE(toMs(fired.windowEndPtsNs), int64_t(11000));
    QCOMPARE(toMs(fired.unknownNs), int64_t(1000));
  }

  void longUnknownSpanResetsDwell() {
    Sim sim;
    sim.run(0, 9500, 500, {"t1"});
    const auto frozen = sim.runUnknown(10000, 14500, 500);
    QVERIFY(allTransitions(frozen, Transition::Unknown));
    // Known observation resumes 5.5 s after the last known frame: dwell restarts.
    const Evaluation resumed = sim.known(15000, {"t1"});
    QCOMPARE_ENUM(resumed.transition, Transition::BecamePending);
    QCOMPARE_ENUM(resumed.after, ConditionState::Pending);
    QCOMPARE(toMs(resumed.windowStartPtsNs), int64_t(15000));
    QCOMPARE(resumed.dwellNs, int64_t(0));
    QVERIFY(noneTriggered(sim.run(15500, 24500, 500, {"t1"})));
    QCOMPARE_ENUM(sim.known(25000, {"t1"}).transition, Transition::Triggered);
  }

  void unknownFramesFreezeClearingTimer() {
    Sim sim;
    sim.trigger();
    QCOMPARE_ENUM(sim.leave(10500).transition, Transition::BecameClearing);
    sim.run(11000, 12000, 500);
    sim.runUnknown(12500, 13000, 500);
    // Clearing began at 10.5 s; 1 s frozen -> clears at 16.5 s, not 15.5 s.
    const auto waiting = sim.run(13500, 16000, 500);
    QVERIFY(allTransitions(waiting, Transition::None));
    QCOMPARE_ENUM(waiting.last().after, ConditionState::Clearing);
    const Evaluation cleared = sim.known(16500);
    QCOMPARE_ENUM(cleared.transition, Transition::Cleared);
    QCOMPARE(cleared.eventId, QStringLiteral("evt-1"));
  }

  void staleGenerationIgnored() {
    Sim sim;
    sim.trigger();
    ObservationFrame late = sim.frame(10500, {track("t1", false)});
    late.generation = 0;
    const Evaluation e = sim.feed(late);
    QCOMPARE_ENUM(e.transition, Transition::Stale);
    QVERIFY(e.note.contains(QStringLiteral("generation")));
    QCOMPARE_ENUM(e.after, ConditionState::Active);
    QCOMPARE_ENUM(sim.ev.state(), ConditionState::Active);
    QCOMPARE(sim.ev.openEventId(), std::optional<QString>(QStringLiteral("evt-1")));
    // The same pts with the current generation is accepted afterwards and
    // covers the span itself, so nothing is frozen.
    const Evaluation same = sim.known(10500, {"t1"});
    QCOMPARE_ENUM(same.transition, Transition::StillActive);
    QCOMPARE(toMs(same.dwellNs), int64_t(10500));
    QCOMPARE(same.unknownNs, int64_t(0));
    // A newer generation is accepted and becomes the floor.
    sim.generation = 2;
    QCOMPARE_ENUM(sim.known(11000, {"t1"}).transition, Transition::StillActive);
    ObservationFrame older = sim.frame(11500, insideTracks({"t1"}));
    older.generation = 1;
    QCOMPARE_ENUM(sim.feed(older).transition, Transition::Stale);
    QCOMPARE_ENUM(sim.known(11500, {"t1"}).transition, Transition::StillActive);
  }

  void latePtsIgnoredAndNeverRollsBack() {
    Sim sim;
    sim.trigger();
    // A late frame from before the trigger that would read as pending.
    Evaluation e = sim.known(9000, {"t1"});
    QCOMPARE_ENUM(e.transition, Transition::Stale);
    QVERIFY(e.note.contains(QStringLiteral("pts")));
    QCOMPARE_ENUM(e.before, ConditionState::Active);
    QCOMPARE_ENUM(e.after, ConditionState::Active);
    QCOMPARE_ENUM(sim.ev.state(), ConditionState::Active);
    // Equal pts is stale as well, even when it would end presence.
    e = sim.leave(10000);
    QCOMPARE_ENUM(e.transition, Transition::Stale);
    QCOMPARE_ENUM(sim.ev.state(), ConditionState::Active);
    QCOMPARE_ENUM(sim.known(10500, {"t1"}).transition, Transition::StillActive);
  }

  void ttlExpiryIgnored() {
    Sim sim;
    sim.trigger();
    ObservationFrame f = sim.frame(10500, {track("t1", false)});
    const Evaluation e = sim.ev.evaluate(f, f.recvMonoNs + 5 * kSecNs + kMsNs);
    QCOMPARE_ENUM(e.transition, Transition::Stale);
    QVERIFY(e.note.contains(QStringLiteral("ttl")));
    QCOMPARE_ENUM(sim.ev.state(), ConditionState::Active);
    // Exactly at the TTL the frame is still accepted.
    ObservationFrame g = sim.frame(10500, {track("t1", false)});
    QCOMPARE_ENUM(sim.ev.evaluate(g, g.recvMonoNs + 5 * kSecNs).transition, Transition::BecameClearing);
  }

  void otherCameraIgnored() {
    Sim sim;
    sim.trigger();
    ObservationFrame f = sim.frame(10500, {track("t1", false)});
    f.cameraId = QStringLiteral("cam-2");
    const Evaluation e = sim.feed(f);
    QCOMPARE_ENUM(e.transition, Transition::Stale);
    QVERIFY(e.note.contains(QStringLiteral("camera")));
    QCOMPARE_ENUM(sim.ev.state(), ConditionState::Active);
  }

  void sessionChangeKeepsEventAndNeedsFreshDwell() {
    Sim sim;
    sim.trigger();
    sim.run(10500, 20000, 500, {"t1"});
    sim.startSession(QStringLiteral("s2"), 0);
    // pts restarts at zero in the new session and is accepted.
    const Evaluation first = sim.known(0, {"n1"});
    QCOMPARE_ENUM(first.transition, Transition::None);
    QCOMPARE(first.sessionId, QStringLiteral("s2"));
    QCOMPARE_ENUM(first.before, ConditionState::Active);
    QCOMPARE_ENUM(first.after, ConditionState::Active);
    QCOMPARE_ENUM(first.quality, Quality::Known);
    QCOMPARE(first.dwellNs, int64_t(0));
    QCOMPARE(sim.ev.openEventId(), std::optional<QString>(QStringLiteral("evt-1")));
    const auto fresh = sim.run(500, 9500, 500, {"n1"});
    QVERIFY(allTransitions(fresh, Transition::None));
    QCOMPARE_ENUM(fresh.last().after, ConditionState::Active);
    const Evaluation still = sim.known(10000, {"n1"});
    QCOMPARE_ENUM(still.transition, Transition::StillActive);
    QCOMPARE(still.eventId, QStringLiteral("evt-1"));
    QCOMPARE(toMs(still.windowStartPtsNs), int64_t(0));
    QCOMPARE(sim.issued, 1);
  }

  void sessionChangeWithEmptyZoneClearsFromResume() {
    Sim sim;
    sim.trigger();
    sim.startSession(QStringLiteral("s2"), 3000);
    const Evaluation first = sim.known(3000);
    QCOMPARE_ENUM(first.transition, Transition::BecameClearing);
    QCOMPARE(first.eventId, QStringLiteral("evt-1"));
    QCOMPARE_ENUM(first.after, ConditionState::Clearing);
    QVERIFY(allTransitions(sim.run(3500, 7500, 500), Transition::None));
    const Evaluation cleared = sim.known(8000);
    QCOMPARE_ENUM(cleared.transition, Transition::Cleared);
    QCOMPARE(cleared.eventId, QStringLiteral("evt-1"));
    QVERIFY(!sim.ev.openEventId().has_value());
  }

  void sessionChangeStartingUnknownStaysUnknownUntilKnown() {
    Sim sim;
    sim.run(0, 5000, 500, {"t1"});
    sim.startSession(QStringLiteral("s2"), 0);
    const Evaluation u = sim.unknown(0);
    QCOMPARE_ENUM(u.transition, Transition::Unknown);
    QCOMPARE_ENUM(u.quality, Quality::Unknown);
    QCOMPARE_ENUM(u.before, ConditionState::Pending);
    QCOMPARE_ENUM(u.after, ConditionState::Inactive);
    QCOMPARE_ENUM(sim.ev.quality(), Quality::Unknown);
    const Evaluation k = sim.known(500, {"t1"});
    QCOMPARE_ENUM(k.quality, Quality::Known);
    QCOMPARE_ENUM(k.transition, Transition::BecamePending);
    QCOMPARE(toMs(k.windowStartPtsNs), int64_t(500));
  }

  void inScheduleSeoulNightWindowWraps() {
    RuleRevision r = makeRule();
    r.timeZoneId = QStringLiteral("Asia/Seoul");
    r.schedule = {TimeWindow{22 * 60, 6 * 60, 0x7f}};
    Sim sim(r);
    QVERIFY(sim.ev.inSchedule(kSundayNightUtcMs));                              // Sun 23:00 KST
    QVERIFY(sim.ev.inSchedule(kSundayNightUtcMs + 4 * 3600 * 1000));            // Mon 03:00 KST
    QVERIFY(!sim.ev.inSchedule(kMondayNoonUtcMs));                              // Mon 12:00 KST
    QVERIFY(!sim.ev.inSchedule(kSundayNightUtcMs - 2 * 3600 * 1000));           // Sun 21:00 KST
    QVERIFY(sim.ev.inSchedule(kSundayNightUtcMs - 3600 * 1000));                // Sun 22:00 KST
    QVERIFY(!sim.ev.inSchedule(kSundayNightUtcMs + 7 * 3600 * 1000));           // Mon 06:00 KST
    QVERIFY(sim.ev.inSchedule(kSundayNightUtcMs + 7 * 3600 * 1000 - 1000));     // Mon 05:59 KST
  }

  void inScheduleDaysMaskFollowsWindowStartDay() {
    RuleRevision r = makeRule();
    r.timeZoneId = QStringLiteral("Asia/Seoul");
    r.schedule = {TimeWindow{22 * 60, 6 * 60, 1 << 4}};  // Fridays only
    Sim sim(r);
    const int64_t fridayNight = 1789754400000;    // Fri 2026-09-18 18:00 UTC = Sat 03:00 KST
    const int64_t thursdayNight = 1789668000000;  // Thu 2026-09-17 18:00 UTC = Fri 03:00 KST
    const int64_t fridayEvening = 1789743600000;  // Fri 2026-09-18 15:00 UTC = Sat 00:00 KST
    QVERIFY(sim.ev.inSchedule(fridayNight));
    QVERIFY(!sim.ev.inSchedule(thursdayNight));
    QVERIFY(sim.ev.inSchedule(fridayEvening));
    QVERIFY(sim.ev.inSchedule(fridayEvening - 2 * 3600 * 1000));   // Fri 22:00 KST
    QVERIFY(!sim.ev.inSchedule(fridayEvening - 4 * 3600 * 1000));  // Fri 20:00 KST
  }

  void inScheduleEmptyAndInvalidZone() {
    Sim always;
    QVERIFY(always.ev.inSchedule(kMondayNoonUtcMs));
    RuleRevision r = makeRule();
    r.timeZoneId = QStringLiteral("Not/AZone");
    r.schedule = {TimeWindow{12 * 60, 13 * 60, 0x7f}};
    Sim fallback(r);
    QVERIFY(!fallback.ev.inSchedule(kMondayNoonUtcMs));                    // 03:00 UTC
    QVERIFY(fallback.ev.inSchedule(kMondayNoonUtcMs + 9 * 3600 * 1000));   // 12:00 UTC
  }

  void outOfScheduleResetsPendingAndClearsActive() {
    RuleRevision r = makeRule();
    r.timeZoneId = QStringLiteral("Asia/Seoul");
    r.schedule = {TimeWindow{22 * 60, 6 * 60, 0x7f}};
    Sim sim(r);
    sim.baseUtcMs = kMondayNoonUtcMs;
    const auto daytime = sim.run(0, 12000, 500, {"t1"});
    QVERIFY(allTransitions(daytime, Transition::OutOfSchedule));
    QCOMPARE_ENUM(daytime.last().after, ConditionState::Inactive);
    QVERIFY(daytime.last().trackIds.isEmpty());
    QCOMPARE_ENUM(daytime.last().quality, Quality::Known);

    sim.baseUtcMs = kSundayNightUtcMs;
    const auto night = sim.run(12500, 22500, 500, {"t1"});
    QCOMPARE_ENUM(night.first().transition, Transition::BecamePending);
    QCOMPARE_ENUM(night.last().transition, Transition::Triggered);
    QCOMPARE(toMs(night.last().windowStartPtsNs), int64_t(12500));

    sim.baseUtcMs = kMondayNoonUtcMs;
    const Evaluation leaving = sim.known(23000, {"t1"});
    QCOMPARE_ENUM(leaving.transition, Transition::BecameClearing);
    QCOMPARE(leaving.eventId, QStringLiteral("evt-1"));
    QCOMPARE_ENUM(leaving.after, ConditionState::Clearing);
    const auto clearing = sim.run(23500, 27500, 500, {"t1"});
    QVERIFY(allTransitions(clearing, Transition::OutOfSchedule));
    QCOMPARE_ENUM(clearing.last().after, ConditionState::Clearing);
    const Evaluation cleared = sim.known(28000, {"t1"});
    QCOMPARE_ENUM(cleared.transition, Transition::Cleared);
    QCOMPARE(cleared.eventId, QStringLiteral("evt-1"));
    QCOMPARE_ENUM(cleared.after, ConditionState::Inactive);
    QVERIFY(!sim.ev.openEventId().has_value());
    QCOMPARE_ENUM(sim.known(28500, {"t1"}).transition, Transition::OutOfSchedule);
  }

  void pendingResetsWhenScheduleEnds() {
    RuleRevision r = makeRule();
    r.timeZoneId = QStringLiteral("Asia/Seoul");
    r.schedule = {TimeWindow{22 * 60, 6 * 60, 0x7f}};
    Sim sim(r);
    sim.run(0, 5000, 500, {"t1"});
    QCOMPARE_ENUM(sim.ev.state(), ConditionState::Pending);
    sim.baseUtcMs = kMondayNoonUtcMs;
    const Evaluation e = sim.known(5500, {"t1"});
    QCOMPARE_ENUM(e.transition, Transition::OutOfSchedule);
    QCOMPARE_ENUM(e.before, ConditionState::Pending);
    QCOMPARE_ENUM(e.after, ConditionState::Inactive);
  }

  void zoneMismatchFreezes() {
    Sim sim;
    sim.run(0, 5000, 500, {"t1"});
    ObservationFrame resized = sim.frame(5500, insideTracks({"t1"}));
    resized.frameWidth = 1920;
    resized.frameHeight = 1080;
    const Evaluation e = sim.feed(resized);
    QCOMPARE_ENUM(e.transition, Transition::ZoneMismatch);
    QCOMPARE_ENUM(e.quality, Quality::Unknown);
    QCOMPARE_ENUM(e.after, ConditionState::Pending);
    QVERIFY(e.note.contains(QStringLiteral("1920x1080")));
    QVERIFY(e.note.contains(QStringLiteral("1280x720")));
    QCOMPARE_ENUM(sim.ev.quality(), Quality::Unknown);
    // The mismatched frame's span is frozen: trigger moves from 10.0 s to 10.5 s.
    const Evaluation back = sim.known(6000, {"t1"});
    QCOMPARE_ENUM(back.quality, Quality::Known);
    QCOMPARE(toMs(back.dwellNs), int64_t(5500));
    QVERIFY(noneTriggered(sim.run(6500, 10000, 500, {"t1"})));
    QCOMPARE_ENUM(sim.known(10500, {"t1"}).transition, Transition::Triggered);
  }

  void zoneWithoutReferenceAcceptsAnySize() {
    RuleRevision r = makeRule();
    r.zone.refWidth = 0;
    r.zone.refHeight = 0;
    Sim sim(r);
    sim.frameWidth = 640;
    sim.frameHeight = 360;
    const Evaluation e = sim.known(0, {"t1"});
    QCOMPARE_ENUM(e.transition, Transition::BecamePending);
  }

  void dedupeReportsStillActive() {
    Sim sim;
    sim.trigger();
    const auto active = sim.run(10500, 20000, 500, {"t1"});
    QVERIFY(allTransitions(active, Transition::StillActive));
    for (const Evaluation& e : active) {
      QCOMPARE(e.eventId, QStringLiteral("evt-1"));
      QCOMPARE_ENUM(e.after, ConditionState::Active);
    }
    QCOMPARE(toMs(active.last().dwellNs), int64_t(20000));
    QCOMPARE(toMs(active.last().windowStartPtsNs), int64_t(0));
    QCOMPARE(sim.issued, 1);
  }

  void clearingAndReentry() {
    Sim sim;
    sim.trigger();
    QVERIFY(allTransitions(sim.run(10500, 19500, 500, {"t1"}), Transition::StillActive));
    const Evaluation gone = sim.leave(20000);
    QCOMPARE_ENUM(gone.transition, Transition::BecameClearing);
    QCOMPARE(gone.eventId, QStringLiteral("evt-1"));
    QCOMPARE_ENUM(gone.after, ConditionState::Clearing);
    QVERIFY(gone.trackIds.isEmpty());
    const auto clearing = sim.run(20500, 21500, 500);
    QVERIFY(allTransitions(clearing, Transition::None));
    QCOMPARE_ENUM(clearing.last().after, ConditionState::Clearing);
    // A new id re-enters during clearing: active again, same event.
    const Evaluation back = sim.known(22000, {"t2"});
    QCOMPARE_ENUM(back.transition, Transition::StillActive);
    QCOMPARE(back.eventId, QStringLiteral("evt-1"));
    QCOMPARE_ENUM(back.after, ConditionState::Active);
    QCOMPARE(back.trackIds, QVector<QString>{QStringLiteral("t2")});
    QCOMPARE(sim.ev.openEventId(), std::optional<QString>(QStringLiteral("evt-1")));
    // The re-entered track is only confirmed after a fresh dwell.
    QVERIFY(allTransitions(sim.run(22500, 31500, 500, {"t2"}), Transition::None));
    QCOMPARE_ENUM(sim.ev.state(), ConditionState::Active);
    const Evaluation confirmed = sim.known(32000, {"t2"});
    QCOMPARE_ENUM(confirmed.transition, Transition::StillActive);
    QCOMPARE(toMs(confirmed.windowStartPtsNs), int64_t(22000));
    QCOMPARE_ENUM(sim.leave(32500, QStringLiteral("t2")).transition, Transition::BecameClearing);
    QVERIFY(allTransitions(sim.run(33000, 37000, 500), Transition::None));
    const Evaluation cleared = sim.known(37500);
    QCOMPARE_ENUM(cleared.transition, Transition::Cleared);
    QCOMPARE(cleared.eventId, QStringLiteral("evt-1"));
    QCOMPARE_ENUM(cleared.before, ConditionState::Clearing);
    QCOMPARE_ENUM(cleared.after, ConditionState::Inactive);
    QVERIFY(!sim.ev.openEventId().has_value());
    QCOMPARE(sim.issued, 1);
  }

  void trackLostDuringActiveClearsAfterGapPlusClearing() {
    Sim sim;
    sim.trigger();
    // t1 vanishes without being seen outside: presence lasts the gap tolerance.
    const auto lingering = sim.run(10500, 11500, 500);
    QVERIFY(allTransitions(lingering, Transition::StillActive));
    const Evaluation gone = sim.known(12000);
    QCOMPARE_ENUM(gone.transition, Transition::BecameClearing);
    QVERIFY(allTransitions(sim.run(12500, 16500, 500), Transition::None));
    QCOMPARE_ENUM(sim.known(17000).transition, Transition::Cleared);
  }

  void rearmSuppressesThenTriggers() {
    Sim sim;
    sim.trigger();
    sim.run(10500, 19500, 500, {"t1"});
    QCOMPARE_ENUM(sim.leave(20000).transition, Transition::BecameClearing);
    sim.run(20500, 24500, 500);
    QCOMPARE_ENUM(sim.known(25000).transition, Transition::Cleared);
    QCOMPARE_ENUM(sim.known(26000, {"t1"}).transition, Transition::BecamePending);
    QVERIFY(allTransitions(sim.run(26500, 35500, 500, {"t1"}), Transition::None));
    const Evaluation suppressed = sim.known(36000, {"t1"});
    QCOMPARE_ENUM(suppressed.transition, Transition::Suppressed);
    QCOMPARE_ENUM(suppressed.before, ConditionState::Pending);
    QCOMPARE_ENUM(suppressed.after, ConditionState::Active);
    QVERIFY(suppressed.eventId.isEmpty());
    QVERIFY(suppressed.note.contains(QStringLiteral("rearm")));
    QVERIFY(!sim.ev.openEventId().has_value());
    const auto waiting = sim.run(36500, 54500, 500, {"t1"});
    QVERIFY(allTransitions(waiting, Transition::Suppressed));
    QCOMPARE_ENUM(waiting.last().after, ConditionState::Active);
    // rearm elapsed 30 s after the clear at 25 s.
    const Evaluation opened = sim.known(55000, {"t1"});
    QCOMPARE_ENUM(opened.transition, Transition::Triggered);
    QCOMPARE(opened.eventId, QStringLiteral("evt-2"));
    QCOMPARE_ENUM(opened.before, ConditionState::Active);
    QCOMPARE_ENUM(opened.after, ConditionState::Active);
    QCOMPARE(toMs(opened.windowStartPtsNs), int64_t(26000));
    QCOMPARE(sim.ev.openEventId(), std::optional<QString>(QStringLiteral("evt-2")));
    QCOMPARE_ENUM(sim.known(55500, {"t1"}).transition, Transition::StillActive);
  }

  void suppressedOccupancyClearsWithoutEventAndKeepsRearm() {
    Sim sim;
    sim.trigger();
    sim.run(10500, 19500, 500, {"t1"});
    QCOMPARE_ENUM(sim.leave(20000).transition, Transition::BecameClearing);
    sim.run(20500, 24500, 500);
    QCOMPARE_ENUM(sim.known(25000).transition, Transition::Cleared);
    sim.run(26000, 35500, 500, {"t1"});
    QCOMPARE_ENUM(sim.known(36000, {"t1"}).transition, Transition::Suppressed);
    sim.run(36500, 39500, 500, {"t1"});
    const Evaluation gone = sim.leave(40000);
    QCOMPARE_ENUM(gone.transition, Transition::BecameClearing);
    QVERIFY(gone.eventId.isEmpty());
    QCOMPARE_ENUM(gone.after, ConditionState::Clearing);
    sim.run(40500, 44500, 500);
    const Evaluation cleared = sim.known(45000);
    QCOMPARE_ENUM(cleared.transition, Transition::Cleared);
    QVERIFY(cleared.eventId.isEmpty());
    QCOMPARE_ENUM(cleared.after, ConditionState::Inactive);
    // Rearm is still measured from the event clear at 25 s: a full dwell
    // finishing at 56 s opens an event immediately.
    QCOMPARE_ENUM(sim.known(46000, {"t2"}).transition, Transition::BecamePending);
    QVERIFY(noneTriggered(sim.run(46500, 55500, 500, {"t2"})));
    const Evaluation opened = sim.known(56000, {"t2"});
    QCOMPARE_ENUM(opened.transition, Transition::Triggered);
    QCOMPARE(opened.eventId, QStringLiteral("evt-2"));
  }

  void rearmCarriesOverSessionChange() {
    Sim sim;
    sim.trigger();
    sim.run(10500, 19500, 500, {"t1"});
    QCOMPARE_ENUM(sim.leave(20000).transition, Transition::BecameClearing);
    sim.run(20500, 24500, 500);
    QCOMPARE_ENUM(sim.known(25000).transition, Transition::Cleared);
    sim.run(25500, 30000, 500);
    // 5 s of rearm elapsed; the new session needs 25 s more.
    sim.startSession(QStringLiteral("s2"), 0);
    sim.run(0, 9500, 500, {"n1"});
    QCOMPARE_ENUM(sim.known(10000, {"n1"}).transition, Transition::Suppressed);
    QVERIFY(allTransitions(sim.run(10500, 24500, 500, {"n1"}), Transition::Suppressed));
    QCOMPARE_ENUM(sim.known(25000, {"n1"}).transition, Transition::Triggered);
  }

  void setRuleKeepsEventAndRecomputesDwell() {
    Sim sim;
    sim.trigger();
    RuleRevision r2 = makeRule();
    r2.revision = 2;
    r2.dwellNs = 4 * kSecNs;
    const std::optional<Evaluation> applied = sim.apply(r2);
    QVERIFY(applied);
    QCOMPARE_ENUM(applied->transition, Transition::None);
    QCOMPARE(applied->ruleRevision, 2);
    QCOMPARE(sim.ev.rule().revision, 2);
    QCOMPARE_ENUM(sim.ev.state(), ConditionState::Active);
    QCOMPARE(sim.ev.openEventId(), std::optional<QString>(QStringLiteral("evt-1")));
    const Evaluation first = sim.known(10500, {"t1"});
    QCOMPARE(first.ruleRevision, 2);
    QCOMPARE_ENUM(first.transition, Transition::None);
    QCOMPARE_ENUM(first.after, ConditionState::Active);
    QCOMPARE(first.dwellNs, int64_t(0));
    QVERIFY(allTransitions(sim.run(11000, 14000, 500, {"t1"}), Transition::None));
    const Evaluation still = sim.known(14500, {"t1"});
    QCOMPARE_ENUM(still.transition, Transition::StillActive);
    QCOMPARE(still.eventId, QStringLiteral("evt-1"));
    QCOMPARE(toMs(still.windowStartPtsNs), int64_t(10500));
    QCOMPARE(sim.issued, 1);
  }

  void setRuleWhilePendingResetsPending() {
    Sim sim;
    sim.run(0, 8000, 500, {"t1"});
    QCOMPARE_ENUM(sim.ev.state(), ConditionState::Pending);
    const std::optional<Evaluation> applied = sim.apply(makeRule());
    QVERIFY(applied);
    QCOMPARE_ENUM(applied->transition, Transition::PendingReset);
    QCOMPARE_ENUM(applied->before, ConditionState::Pending);
    QCOMPARE_ENUM(applied->after, ConditionState::Inactive);
    QCOMPARE_ENUM(sim.ev.state(), ConditionState::Inactive);
    const Evaluation e = sim.known(8500, {"t1"});
    QCOMPARE_ENUM(e.transition, Transition::BecamePending);
    QCOMPARE(toMs(e.windowStartPtsNs), int64_t(8500));
    QVERIFY(noneTriggered(sim.run(9000, 18000, 500, {"t1"})));
    QCOMPARE_ENUM(sim.known(18500, {"t1"}).transition, Transition::Triggered);
  }

  void setRuleWhileClearingKeepsClearing() {
    Sim sim;
    sim.trigger();
    sim.run(10500, 19500, 500, {"t1"});
    QCOMPARE_ENUM(sim.leave(20000).transition, Transition::BecameClearing);
    QVERIFY(sim.apply(makeRule()));
    QCOMPARE_ENUM(sim.ev.state(), ConditionState::Clearing);
    QVERIFY(allTransitions(sim.run(20500, 24500, 500), Transition::None));
    const Evaluation cleared = sim.known(25000);
    QCOMPARE_ENUM(cleared.transition, Transition::Cleared);
    QCOMPARE(cleared.eventId, QStringLiteral("evt-1"));
  }

  void alternatingTracksNeverTrigger() {
    Sim sim;
    // Each id occupies the zone for 6 s at a time and hands over to the other.
    QVector<Evaluation> all;
    for (int64_t start = 0; start < 60000; start += 12000) {
      all += sim.run(start, start + 5500, 500, {"t1"});
      all += sim.run(start + 6000, start + 11500, 500, {"t2"});
    }
    QVERIFY(noneTriggered(all));
    for (const Evaluation& e : all) QCOMPARE_ENUM(e.after, ConditionState::Pending);
    QVERIFY(std::all_of(all.cbegin(), all.cend(), [](const Evaluation& e) { return e.dwellNs < 10 * kSecNs; }));
    QVERIFY(!sim.ev.openEventId().has_value());
  }

  void newTrackIdRestartsDwell() {
    Sim sim;
    sim.run(0, 7500, 500, {"t1"});
    const Evaluation handover = sim.raw(8000, {track("t1", false), track("t2", true)});
    QCOMPARE_ENUM(handover.transition, Transition::None);
    QCOMPARE_ENUM(handover.after, ConditionState::Pending);
    QCOMPARE(handover.trackIds, QVector<QString>{QStringLiteral("t2")});
    QCOMPARE(handover.dwellNs, int64_t(0));
    QCOMPARE(toMs(handover.windowStartPtsNs), int64_t(8000));
    QVERIFY(noneTriggered(sim.run(8500, 17500, 500, {"t2"})));
    const Evaluation fired = sim.known(18000, {"t2"});
    QCOMPARE_ENUM(fired.transition, Transition::Triggered);
    QCOMPARE(fired.trackIds, QVector<QString>{QStringLiteral("t2")});
    QCOMPARE(toMs(fired.windowStartPtsNs), int64_t(8000));
  }

  void twoTracksReportEarliestWindowAndLongestDwell() {
    Sim sim;
    sim.run(0, 2500, 500, {"t1"});
    const auto both = sim.run(3000, 9500, 500, {"t1", "t2"});
    QCOMPARE(both.last().trackIds, (QVector<QString>{QStringLiteral("t1"), QStringLiteral("t2")}));
    QCOMPARE(toMs(both.last().windowStartPtsNs), int64_t(0));
    QCOMPARE(toMs(both.last().dwellNs), int64_t(9500));
    const Evaluation fired = sim.known(10000, {"t1", "t2"});
    QCOMPARE_ENUM(fired.transition, Transition::Triggered);
    QCOMPARE(fired.trackIds, (QVector<QString>{QStringLiteral("t1"), QStringLiteral("t2")}));
    // t1 leaves; t2 keeps the event active but is only confirmed after its own dwell.
    const Evaluation t1Gone = sim.raw(10500, {track("t1", false), track("t2", true)});
    QCOMPARE_ENUM(t1Gone.transition, Transition::None);
    QCOMPARE_ENUM(t1Gone.after, ConditionState::Active);
    QCOMPARE(toMs(t1Gone.dwellNs), int64_t(7500));
    QCOMPARE(toMs(t1Gone.windowStartPtsNs), int64_t(3000));
    QVERIFY(allTransitions(sim.run(11000, 12500, 500, {"t2"}), Transition::None));
    QCOMPARE_ENUM(sim.known(13000, {"t2"}).transition, Transition::StillActive);
  }

  void tickReportsUnknownAfterSilence() {
    Sim sim;
    sim.run(0, 5000, 500, {"t1"});
    const int64_t lastEval = 5000 * kMsNs + sim.latencyNs;
    Evaluation t = sim.ev.tick(lastEval + 1000 * kMsNs, kSundayNightUtcMs + 6000);
    QCOMPARE_ENUM(t.transition, Transition::None);
    QCOMPARE_ENUM(t.quality, Quality::Known);
    QCOMPARE_ENUM(t.after, ConditionState::Pending);
    QCOMPARE(t.cameraId, QStringLiteral("cam-1"));
    QCOMPARE(t.sessionId, QStringLiteral("s1"));
    QCOMPARE(t.utcMs, kSundayNightUtcMs + 6000);
    QCOMPARE(toMs(t.windowEndPtsNs), int64_t(5000));
    t = sim.ev.tick(lastEval + 1600 * kMsNs, kSundayNightUtcMs + 6600);
    QCOMPARE_ENUM(t.transition, Transition::Unknown);
    QCOMPARE_ENUM(t.quality, Quality::Unknown);
    QCOMPARE_ENUM(t.after, ConditionState::Pending);
    QCOMPARE_ENUM(sim.ev.quality(), Quality::Unknown);
    QCOMPARE_ENUM(sim.ev.state(), ConditionState::Pending);
    // Media time continues without a gap: the dwell is intact.
    const Evaluation resumed = sim.known(5500, {"t1"});
    QCOMPARE_ENUM(resumed.quality, Quality::Known);
    QCOMPARE(toMs(resumed.dwellNs), int64_t(5500));
    QCOMPARE_ENUM(sim.ev.tick(5500 * kMsNs + sim.latencyNs + 100 * kMsNs, 0).transition, Transition::None);
  }

  void tickBeforeAnyFrameIsUnknown() {
    Sim sim;
    const Evaluation t = sim.ev.tick(kSecNs, kSundayNightUtcMs);
    QCOMPARE_ENUM(t.transition, Transition::Unknown);
    QCOMPARE_ENUM(t.quality, Quality::Unknown);
    QCOMPARE_ENUM(t.after, ConditionState::Inactive);
  }

  void disablingClosesEventAndFreezesRearm() {
    Sim sim;
    sim.trigger();
    RuleRevision off = makeRule();
    off.enabled = false;
    const std::optional<Evaluation> closed = sim.apply(off);
    QVERIFY(closed);
    QCOMPARE_ENUM(closed->transition, Transition::Cleared);
    QCOMPARE(closed->eventId, QStringLiteral("evt-1"));
    QVERIFY(closed->note.contains(QStringLiteral("disabled")));
    QCOMPARE_ENUM(closed->before, ConditionState::Active);
    QCOMPARE_ENUM(closed->after, ConditionState::Inactive);
    QCOMPARE(closed->utcMs, kSundayNightUtcMs);
    QCOMPARE(toMs(closed->windowEndPtsNs), int64_t(10000));
    QCOMPARE_ENUM(sim.ev.state(), ConditionState::Inactive);
    QVERIFY(!sim.ev.openEventId().has_value());
    const Evaluation e = sim.known(10500, {"t1"});
    QCOMPARE_ENUM(e.transition, Transition::Disabled);
    QCOMPARE_ENUM(e.after, ConditionState::Inactive);
    QCOMPARE_ENUM(sim.ev.tick(20 * kSecNs, kSundayNightUtcMs).transition, Transition::Disabled);

    const std::optional<Evaluation> on = sim.apply(makeRule());
    QVERIFY(on);
    QCOMPARE_ENUM(on->transition, Transition::None);
    // 30 s disabled: only the 1.5 s gap tolerance counts toward rearm.
    QCOMPARE_ENUM(sim.known(40000, {"x9"}).transition, Transition::BecamePending);
    sim.run(40500, 49500, 500, {"x9"});
    QCOMPARE_ENUM(sim.known(50000, {"x9"}).transition, Transition::Suppressed);
    QVERIFY(allTransitions(sim.run(50500, 68000, 500, {"x9"}), Transition::Suppressed));
    const Evaluation opened = sim.known(68500, {"x9"});
    QCOMPARE_ENUM(opened.transition, Transition::Triggered);
    QCOMPARE(opened.eventId, QStringLiteral("evt-2"));
  }

  void disablingWithoutOccupancyReportsDisabled() {
    Sim sim;
    sim.known(0);
    RuleRevision off = makeRule();
    off.enabled = false;
    const std::optional<Evaluation> applied = sim.apply(off);
    QVERIFY(applied);
    QCOMPARE_ENUM(applied->transition, Transition::Disabled);
    QVERIFY(applied->eventId.isEmpty());
  }

  void staleFrameWithOutsideTrackKeepsPresence() {
    Sim sim;
    sim.trigger();
    QCOMPARE_ENUM(sim.leave(9500).transition, Transition::Stale);
    QCOMPARE_ENUM(sim.known(10500, {"t1"}).transition, Transition::StillActive);
  }

  void gapWithEmptyZoneReportsPendingReset() {
    Sim sim;
    sim.run(0, 5000, 500, {"t1"});
    const Evaluation e = sim.known(8000);
    QCOMPARE_ENUM(e.before, ConditionState::Pending);
    QCOMPARE_ENUM(e.after, ConditionState::Inactive);
    QCOMPARE_ENUM(e.transition, Transition::PendingReset);
  }

  void gapWhileClearingFreezesSpanBeyondTolerance() {
    Sim sim;
    sim.trigger();
    QCOMPARE_ENUM(sim.leave(10500).transition, Transition::BecameClearing);
    sim.run(11000, 13000, 500);
    // 3 s without frames: 1.5 s counts, 1.5 s is frozen -> clears at 17.0 s.
    const Evaluation resumed = sim.known(16000);
    QCOMPARE_ENUM(resumed.transition, Transition::None);
    QCOMPARE_ENUM(resumed.after, ConditionState::Clearing);
    QCOMPARE_ENUM(sim.known(16500).transition, Transition::None);
    const Evaluation cleared = sim.known(17000);
    QCOMPARE_ENUM(cleared.transition, Transition::Cleared);
    QCOMPARE(cleared.eventId, QStringLiteral("evt-1"));
  }

  void sparseFramesStillClear() {
    Sim sim;
    sim.trigger();
    QCOMPARE_ENUM(sim.leave(10500).transition, Transition::BecameClearing);
    // Frames every 1.6 s: each advances clearing by the 1.5 s tolerance.
    QVERIFY(allTransitions({sim.known(12100), sim.known(13700), sim.known(15300)}, Transition::None));
    QCOMPARE_ENUM(sim.ev.state(), ConditionState::Clearing);
    const Evaluation cleared = sim.known(16900);
    QCOMPARE_ENUM(cleared.transition, Transition::Cleared);
    QCOMPARE(cleared.eventId, QStringLiteral("evt-1"));
  }

  void unknownFramesFreezeRearmTimer() {
    Sim sim;
    sim.trigger();
    sim.run(10500, 19500, 500, {"t1"});
    QCOMPARE_ENUM(sim.leave(20000).transition, Transition::BecameClearing);
    sim.run(20500, 24500, 500);
    QCOMPARE_ENUM(sim.known(25000).transition, Transition::Cleared);
    sim.run(25500, 26000, 500);
    QVERIFY(allTransitions(sim.runUnknown(26500, 27000, 500), Transition::Unknown));
    sim.run(27500, 37000, 500, {"t1"});
    QCOMPARE_ENUM(sim.known(37500, {"t1"}).transition, Transition::Suppressed);
    QVERIFY(allTransitions(sim.run(38000, 55500, 500, {"t1"}), Transition::Suppressed));
    const Evaluation opened = sim.known(56000, {"t1"});
    QCOMPARE_ENUM(opened.transition, Transition::Triggered);
    QCOMPARE(opened.eventId, QStringLiteral("evt-2"));
  }

  void silentGapFreezesRearmLikeUnknownSpan() {
    auto scenario = [](bool fillWithUnknown) {
      Sim sim;
      sim.trigger();
      sim.run(10500, 19500, 500, {"t1"});
      sim.leave(20000);
      sim.run(20500, 24500, 500);
      sim.known(25000);
      if (fillWithUnknown) sim.runUnknown(25500, 44500, 500);
      sim.run(45000, 54500, 500, {"t1"});
      return sim.known(55000, {"t1"}).transition;
    };
    QCOMPARE_ENUM(scenario(true), Transition::Suppressed);
    QCOMPARE_ENUM(scenario(false), Transition::Suppressed);
  }

  void suppressionCoversHandoverAndReentry() {
    Sim sim;
    sim.trigger();
    sim.run(10500, 19500, 500, {"t1"});
    QCOMPARE_ENUM(sim.leave(20000).transition, Transition::BecameClearing);
    sim.run(20500, 24500, 500);
    QCOMPARE_ENUM(sim.known(25000).transition, Transition::Cleared);
    sim.run(26000, 35500, 500, {"t1"});
    QCOMPARE_ENUM(sim.known(36000, {"t1"}).transition, Transition::Suppressed);
    const Evaluation handover = sim.raw(36500, {track("t1", false), track("t2", true)});
    QCOMPARE_ENUM(handover.transition, Transition::Suppressed);
    QVERIFY(handover.eventId.isEmpty());
    QCOMPARE_ENUM(sim.leave(37000, QStringLiteral("t2")).transition, Transition::BecameClearing);
    const Evaluation back = sim.known(38000, {"t3"});
    QCOMPARE_ENUM(back.transition, Transition::Suppressed);
    QCOMPARE_ENUM(back.after, ConditionState::Active);
    QVERIFY(back.eventId.isEmpty());
    QCOMPARE(sim.issued, 1);
  }

  void rearmedHandoverOpensEventOnlyAfterDwell() {
    Sim sim;
    sim.trigger();
    QCOMPARE_ENUM(sim.leave(10500).transition, Transition::BecameClearing);
    sim.run(11000, 15000, 500);
    QCOMPARE_ENUM(sim.known(15500).transition, Transition::Cleared);
    sim.run(16000, 25500, 500, {"t1"});
    QCOMPARE_ENUM(sim.known(26000, {"t1"}).transition, Transition::Suppressed);
    QVERIFY(allTransitions(sim.run(26500, 44000, 500, {"t1"}), Transition::Suppressed));
    QCOMPARE_ENUM(sim.raw(44500, {track("t1", false), track("t2", true)}).transition, Transition::Suppressed);
    QCOMPARE_ENUM(sim.known(45000, {"t2"}).transition, Transition::Suppressed);
    // Rearm elapsed at 45.5 s, but t2 has not dwelled.
    const Evaluation rearmed = sim.known(45500, {"t2"});
    QCOMPARE_ENUM(rearmed.transition, Transition::None);
    QCOMPARE_ENUM(rearmed.after, ConditionState::Active);
    QVERIFY(rearmed.eventId.isEmpty());
    QVERIFY(!sim.ev.openEventId().has_value());
    QVERIFY(allTransitions(sim.run(46000, 54000, 500, {"t2"}), Transition::None));
    const Evaluation opened = sim.known(54500, {"t2"});
    QCOMPARE_ENUM(opened.transition, Transition::Triggered);
    QCOMPARE(opened.eventId, QStringLiteral("evt-2"));
    QCOMPARE(opened.trackIds, QVector<QString>{QStringLiteral("t2")});
    QCOMPARE(toMs(opened.dwellNs), int64_t(10000));
    QCOMPARE(toMs(opened.windowStartPtsNs), int64_t(44500));
  }

  void rearmedReentryOpensEventOnlyAfterDwell() {
    Sim sim;
    sim.trigger();
    QCOMPARE_ENUM(sim.leave(10500).transition, Transition::BecameClearing);
    sim.run(11000, 15000, 500);
    QCOMPARE_ENUM(sim.known(15500).transition, Transition::Cleared);
    sim.run(16000, 25500, 500, {"t1"});
    QCOMPARE_ENUM(sim.known(26000, {"t1"}).transition, Transition::Suppressed);
    sim.run(26500, 44000, 500, {"t1"});
    const Evaluation gone = sim.leave(44500);
    QCOMPARE_ENUM(gone.transition, Transition::BecameClearing);
    QVERIFY(gone.eventId.isEmpty());
    QCOMPARE_ENUM(sim.known(45000).transition, Transition::None);
    const Evaluation back = sim.known(46000, {"t9"});
    QCOMPARE_ENUM(back.transition, Transition::None);
    QCOMPARE_ENUM(back.after, ConditionState::Active);
    QVERIFY(back.eventId.isEmpty());
    QCOMPARE(back.dwellNs, int64_t(0));
    QVERIFY(allTransitions(sim.run(46500, 55500, 500, {"t9"}), Transition::None));
    const Evaluation opened = sim.known(56000, {"t9"});
    QCOMPARE_ENUM(opened.transition, Transition::Triggered);
    QCOMPARE(opened.eventId, QStringLiteral("evt-2"));
    QCOMPARE(toMs(opened.dwellNs), int64_t(10000));
    QCOMPARE(toMs(opened.windowStartPtsNs), int64_t(46000));
  }

  void sessionChangeWhileSuppressedNeedsDwell() {
    Sim sim;
    sim.trigger();
    QCOMPARE_ENUM(sim.leave(10500).transition, Transition::BecameClearing);
    sim.run(11000, 15000, 500);
    QCOMPARE_ENUM(sim.known(15500).transition, Transition::Cleared);
    sim.run(16000, 25500, 500, {"t1"});
    QCOMPARE_ENUM(sim.known(26000, {"t1"}).transition, Transition::Suppressed);
    sim.run(26500, 40000, 500, {"t1"});
    // 24.5 s of rearm elapsed; it ends 5.5 s into the new session, before n1 has dwelled.
    sim.startSession(QStringLiteral("s2"), 0);
    QVERIFY(allTransitions(sim.run(0, 5000, 500, {"n1"}), Transition::Suppressed));
    const Evaluation rearmed = sim.known(5500, {"n1"});
    QCOMPARE_ENUM(rearmed.transition, Transition::None);
    QCOMPARE_ENUM(rearmed.after, ConditionState::Active);
    QVERIFY(rearmed.eventId.isEmpty());
    QVERIFY(allTransitions(sim.run(6000, 9500, 500, {"n1"}), Transition::None));
    const Evaluation opened = sim.known(10000, {"n1"});
    QCOMPARE_ENUM(opened.transition, Transition::Triggered);
    QCOMPARE(toMs(opened.dwellNs), int64_t(10000));
  }

  void staleInputSwitchesNoSessionAndRaisesNoFloor() {
    Sim sim;
    sim.run(0, 5000, 500, {"t1"});
    ObservationFrame expired = sim.frame(0, insideTracks({"n1"}));
    expired.sessionId = QStringLiteral("s2");
    expired.generation = 9;
    QCOMPARE_ENUM(sim.ev.evaluate(expired, expired.recvMonoNs + 6 * kSecNs).transition, Transition::Stale);
    ObservationFrame late = sim.frame(4000, insideTracks({"t1"}));
    late.generation = 9;
    QCOMPARE_ENUM(sim.feed(late).transition, Transition::Stale);
    const Evaluation e = sim.known(5500, {"t1"});
    QCOMPARE_ENUM(e.transition, Transition::None);
    QCOMPARE(toMs(e.dwellNs), int64_t(5500));
    QCOMPARE_ENUM(e.quality, Quality::Known);
  }

  void staleFramesDoNotKeepTickAlive() {
    Sim sim;
    sim.run(0, 5000, 500, {"t1"});
    const int64_t lastEval = 5000 * kMsNs + sim.latencyNs;
    ObservationFrame old = sim.frame(4500, insideTracks({"t1"}));
    QCOMPARE_ENUM(sim.ev.evaluate(old, lastEval + 1400 * kMsNs).transition, Transition::Stale);
    QCOMPARE_ENUM(sim.ev.tick(lastEval + 1500 * kMsNs, 0).transition, Transition::None);
    QCOMPARE_ENUM(sim.ev.tick(lastEval + 1501 * kMsNs, 0).transition, Transition::Unknown);
  }

  void lateFrameFromEndedSessionIsStale() {
    Sim sim;
    sim.trigger();
    sim.startSession(QStringLiteral("s2"), 0);
    sim.run(0, 2000, 500, {"t1"});
    ObservationFrame late = sim.frame(10500);
    late.sessionId = QStringLiteral("s1");
    late.recvMonoNs = 10400 * kMsNs;
    const Evaluation e = sim.ev.evaluate(late, sim.lastRecvNs + sim.latencyNs);
    QCOMPARE_ENUM(e.transition, Transition::Stale);
    QVERIFY(e.note.contains(QStringLiteral("superseded")));
    QCOMPARE_ENUM(sim.ev.state(), ConditionState::Active);
    const Evaluation next = sim.known(2500, {"t1"});
    QCOMPARE(next.sessionId, QStringLiteral("s2"));
    QCOMPARE_ENUM(next.transition, Transition::None);
    QCOMPARE(toMs(next.dwellNs), int64_t(2500));
  }

  void ttlDroppedSpanIsFrozen() {
    Sim sim;
    sim.run(0, 8500, 500, {"t1"});
    QCOMPARE_ENUM(sim.expired(9000, {track("t1", false)}).transition, Transition::Stale);
    QCOMPARE_ENUM(sim.expired(9500, {track("t1", false)}).transition, Transition::Stale);
    const Evaluation e = sim.known(10000, {"t1"});
    QCOMPARE_ENUM(e.transition, Transition::None);
    QCOMPARE(toMs(e.dwellNs), int64_t(9000));
    QCOMPARE(toMs(e.unknownNs), int64_t(1000));
    QCOMPARE_ENUM(sim.known(10500, {"t1"}).transition, Transition::None);
    const Evaluation fired = sim.known(11000, {"t1"});
    QCOMPARE_ENUM(fired.transition, Transition::Triggered);
    QCOMPARE(toMs(fired.windowStartPtsNs), int64_t(0));
    QCOMPARE(toMs(fired.unknownNs), int64_t(1000));
  }

  void ttlDroppedSpanFreezesRearm() {
    Sim sim;
    sim.trigger();
    sim.run(10500, 19500, 500, {"t1"});
    QCOMPARE_ENUM(sim.leave(20000).transition, Transition::BecameClearing);
    sim.run(20500, 24500, 500);
    QCOMPARE_ENUM(sim.known(25000).transition, Transition::Cleared);
    for (int64_t atMs = 25500; atMs <= 55000; atMs += 500) sim.expired(atMs);
    sim.run(55500, 65000, 500, {"t1"});
    QCOMPARE_ENUM(sim.known(65500, {"t1"}).transition, Transition::Suppressed);
    QVERIFY(allTransitions(sim.run(66000, 84500, 500, {"t1"}), Transition::Suppressed));
    const Evaluation opened = sim.known(85000, {"t1"});
    QCOMPARE_ENUM(opened.transition, Transition::Triggered);
    QCOMPARE(opened.eventId, QStringLiteral("evt-2"));
  }

  void trackReappearingAfterLongAbsenceRestartsDwell() {
    Sim sim;
    sim.run(0, 4000, 500, {"t1"});
    sim.run(4500, 5500, 500);
    const Evaluation back = sim.known(6000, {"t1"});
    QCOMPARE_ENUM(back.after, ConditionState::Pending);
    QCOMPARE(back.dwellNs, int64_t(0));
    QCOMPARE(toMs(back.windowStartPtsNs), int64_t(6000));
    QVERIFY(noneTriggered(sim.run(6500, 15500, 500, {"t1"})));
    QCOMPARE_ENUM(sim.known(16000, {"t1"}).transition, Transition::Triggered);
  }

  void dwellEndsAtLastSighting() {
    Sim sim;
    sim.run(0, 8500, 500, {"t1"});
    const auto unseen = sim.run(9000, 10000, 500);
    QVERIFY(noneTriggered(unseen));
    QCOMPARE_ENUM(unseen.last().after, ConditionState::Pending);
    QCOMPARE(toMs(unseen.last().dwellNs), int64_t(8500));
    QCOMPARE_ENUM(sim.known(10500).transition, Transition::PendingReset);
  }

  void ignoredSightingsOfTrackedIdNeitherEndNorExtendPresence() {
    Sim sim;
    sim.run(0, 5000, 500, {"t1"});
    Evaluation e = sim.raw(5500, {track("t1", false, QStringLiteral("person"), 0.2)});
    QCOMPARE_ENUM(e.after, ConditionState::Pending);
    QCOMPARE(toMs(e.dwellNs), int64_t(5000));
    e = sim.raw(6000, {track("t1", false, QStringLiteral("car"))});
    QCOMPARE(e.trackIds, QVector<QString>{QStringLiteral("t1")});
    QCOMPARE(toMs(e.dwellNs), int64_t(5000));
    sim.raw(6500, {track("t1", true, QStringLiteral("person"), 0.2)});
    e = sim.raw(7000, {track("t1", true, QStringLiteral("person"), 0.2)});
    QCOMPARE_ENUM(e.transition, Transition::PendingReset);
    QCOMPARE_ENUM(sim.raw(7500, {track("t2", true, QStringLiteral("person"), 0.3)}).transition,
                  Transition::BecamePending);
  }

  void rearmCarriesOverSessionWithNonZeroPts() {
    Sim sim;
    sim.trigger();
    sim.run(10500, 19500, 500, {"t1"});
    QCOMPARE_ENUM(sim.leave(20000).transition, Transition::BecameClearing);
    sim.run(20500, 24500, 500);
    QCOMPARE_ENUM(sim.known(25000).transition, Transition::Cleared);
    sim.run(25500, 30000, 500);
    sim.startSession(QStringLiteral("s2"), 100000);
    sim.run(100000, 109500, 500, {"n1"});
    QCOMPARE_ENUM(sim.known(110000, {"n1"}).transition, Transition::Suppressed);
    QVERIFY(allTransitions(sim.run(110500, 124500, 500, {"n1"}), Transition::Suppressed));
    QCOMPARE_ENUM(sim.known(125000, {"n1"}).transition, Transition::Triggered);
  }

  void clearingCarriesOverSessionChange() {
    Sim sim;
    sim.trigger();
    QCOMPARE_ENUM(sim.leave(10500).transition, Transition::BecameClearing);
    sim.run(11000, 12500, 500);
    sim.startSession(QStringLiteral("s2"), 0);
    const auto waiting = sim.run(0, 2500, 500);
    QVERIFY(allTransitions(waiting, Transition::None));
    QCOMPARE_ENUM(waiting.last().after, ConditionState::Clearing);
    const Evaluation cleared = sim.known(3000);
    QCOMPARE_ENUM(cleared.transition, Transition::Cleared);
    QCOMPARE(cleared.eventId, QStringLiteral("evt-1"));
  }

  void longGapClosesOpenEvent() {
    Sim shortGap;
    shortGap.trigger();
    const Evaluation kept = shortGap.known(14000, {"t1"});
    QCOMPARE_ENUM(kept.transition, Transition::None);
    QCOMPARE_ENUM(kept.after, ConditionState::Active);
    QCOMPARE(shortGap.ev.openEventId(), std::optional<QString>(QStringLiteral("evt-1")));

    Sim sim;
    sim.trigger();
    const Evaluation closed = sim.known(20000, {"x9"});
    QCOMPARE_ENUM(closed.transition, Transition::Cleared);
    QCOMPARE(closed.eventId, QStringLiteral("evt-1"));
    QVERIFY(closed.note.contains(QStringLiteral("observation gap")));
    QCOMPARE_ENUM(closed.before, ConditionState::Active);
    QCOMPARE_ENUM(closed.after, ConditionState::Pending);
    QCOMPARE(closed.trackIds, QVector<QString>{QStringLiteral("x9")});
    QVERIFY(!sim.ev.openEventId().has_value());
    QVERIFY(noneTriggered(sim.run(20500, 29500, 500, {"x9"})));
    QCOMPARE_ENUM(sim.known(30000, {"x9"}).transition, Transition::Suppressed);
    QVERIFY(allTransitions(sim.run(30500, 49500, 500, {"x9"}), Transition::Suppressed));
    const Evaluation opened = sim.known(50000, {"x9"});
    QCOMPARE_ENUM(opened.transition, Transition::Triggered);
    QCOMPARE(opened.eventId, QStringLiteral("evt-2"));
  }

  void longSessionOutageClosesOpenEvent() {
    Sim sim;
    sim.trigger();
    sim.startSession(QStringLiteral("s2"), 0, 60000);
    const Evaluation closed = sim.known(0, {"x9"});
    QCOMPARE_ENUM(closed.transition, Transition::Cleared);
    QCOMPARE(closed.eventId, QStringLiteral("evt-1"));
    QVERIFY(closed.note.contains(QStringLiteral("observation gap")));
    QCOMPARE_ENUM(closed.after, ConditionState::Pending);
    QVERIFY(!sim.ev.openEventId().has_value());
  }

  void longGapEndsSuppressedOccupancyWithoutRestartingRearm() {
    Sim sim;
    sim.trigger();
    sim.run(10500, 19500, 500, {"t1"});
    QCOMPARE_ENUM(sim.leave(20000).transition, Transition::BecameClearing);
    sim.run(20500, 24500, 500);
    QCOMPARE_ENUM(sim.known(25000).transition, Transition::Cleared);
    sim.run(26000, 35500, 500, {"t1"});
    QCOMPARE_ENUM(sim.known(36000, {"t1"}).transition, Transition::Suppressed);
    const Evaluation ended = sim.known(43000);
    QCOMPARE_ENUM(ended.transition, Transition::Cleared);
    QVERIFY(ended.eventId.isEmpty());
    QVERIFY(ended.note.contains(QStringLiteral("observation gap")));
    QCOMPARE_ENUM(ended.after, ConditionState::Inactive);
    // 5.5 s of the 7 s gap is frozen: rearm ends at 60.5 s.
    sim.run(43500, 53000, 500, {"t2"});
    QCOMPARE_ENUM(sim.known(53500, {"t2"}).transition, Transition::Suppressed);
    QVERIFY(allTransitions(sim.run(54000, 60000, 500, {"t2"}), Transition::Suppressed));
    QCOMPARE_ENUM(sim.known(60500, {"t2"}).transition, Transition::Triggered);
  }

  void workerOutageDuringActiveKeepsEvent() {
    Sim sim;
    sim.trigger();
    QVERIFY(allTransitions(sim.runUnknown(10500, 16000, 500), Transition::Unknown));
    const Evaluation resumed = sim.known(16500, {"t1"});
    QCOMPARE_ENUM(resumed.transition, Transition::None);
    QCOMPARE_ENUM(resumed.after, ConditionState::Active);
    QCOMPARE(sim.ev.openEventId(), std::optional<QString>(QStringLiteral("evt-1")));
    QVERIFY(allTransitions(sim.run(17000, 26000, 500, {"t1"}), Transition::None));
    const Evaluation still = sim.known(26500, {"t1"});
    QCOMPARE_ENUM(still.transition, Transition::StillActive);
    QCOMPARE(still.eventId, QStringLiteral("evt-1"));
    QCOMPARE(sim.issued, 1);
  }

  void workerOutageThenAbsenceClearsFromResume() {
    Sim sim;
    sim.trigger();
    sim.runUnknown(10500, 16000, 500);
    const Evaluation away = sim.known(16500);
    QCOMPARE_ENUM(away.transition, Transition::BecameClearing);
    QCOMPARE(away.eventId, QStringLiteral("evt-1"));
    QVERIFY(allTransitions(sim.run(17000, 21000, 500), Transition::None));
    const Evaluation cleared = sim.known(21500);
    QCOMPARE_ENUM(cleared.transition, Transition::Cleared);
    QVERIFY(cleared.note.isEmpty());
  }

  void workerOutageDuringClearingCountsOnlyObservedAbsence() {
    Sim sim;
    sim.trigger();
    QCOMPARE_ENUM(sim.leave(10500).transition, Transition::BecameClearing);
    sim.run(11000, 11500, 500);
    sim.runUnknown(12000, 20000, 500);
    // 1 s of absence observed before the outage: clearing ends 4 s after it.
    const auto waiting = sim.run(20500, 23500, 500);
    QVERIFY(allTransitions(waiting, Transition::None));
    QCOMPARE_ENUM(waiting.last().after, ConditionState::Clearing);
    const Evaluation cleared = sim.known(24000);
    QCOMPARE_ENUM(cleared.transition, Transition::Cleared);
    QCOMPARE(cleared.eventId, QStringLiteral("evt-1"));
    QVERIFY(cleared.note.isEmpty());
  }

  void outageAcrossSessionChangeKeepsEvent() {
    Sim sim;
    sim.trigger();
    sim.startSession(QStringLiteral("s2"), 0);
    QVERIFY(allTransitions(sim.runUnknown(0, 8000, 500), Transition::Unknown));
    const Evaluation resumed = sim.known(8500, {"t1"});
    QCOMPARE_ENUM(resumed.transition, Transition::None);
    QCOMPARE_ENUM(resumed.after, ConditionState::Active);
    QCOMPARE(sim.ev.openEventId(), std::optional<QString>(QStringLiteral("evt-1")));
  }

  void staleResultsCoverAnOutage() {
    Sim sim;
    sim.trigger();
    for (int64_t atMs = 10500; atMs <= 16000; atMs += 500)
      QCOMPARE_ENUM(sim.expired(atMs, {track("t1")}).transition, Transition::Stale);
    const Evaluation resumed = sim.known(16500, {"t1"});
    QCOMPARE_ENUM(resumed.transition, Transition::None);
    QCOMPARE(sim.ev.openEventId(), std::optional<QString>(QStringLiteral("evt-1")));
  }

  void raisedGenerationFreezesTheDroppedSpan() {
    Sim sim;
    sim.trigger();
    QCOMPARE_ENUM(sim.leave(10500).transition, Transition::BecameClearing);
    sim.known(11000);
    sim.ev.raiseGeneration(2);
    const Evaluation dropped = sim.known(11500);
    QCOMPARE_ENUM(dropped.transition, Transition::Stale);
    QVERIFY(dropped.note.startsWith(QStringLiteral("generation 1 below 2")));
    sim.generation = 2;
    QVERIFY(allTransitions(sim.run(12000, 15500, 500), Transition::None));
    QCOMPARE_ENUM(sim.known(16000).transition, Transition::Cleared);
  }

  void seededRearmSuppressesUntilRearmFromTheClear() {
    Sim sim;
    sim.ev.seedRearm(sim.baseUtcMs - 10000);
    sim.run(0, 9500, 500, {"t1"});
    QCOMPARE_ENUM(sim.known(10000, {"t1"}).transition, Transition::Suppressed);
    QVERIFY(allTransitions(sim.run(10500, 19500, 500, {"t1"}), Transition::Suppressed));
    const Evaluation opened = sim.known(20000, {"t1"});
    QCOMPARE_ENUM(opened.transition, Transition::Triggered);
    QCOMPARE(opened.eventId, QStringLiteral("evt-1"));

    Sim old;
    old.ev.seedRearm(old.baseUtcMs - 60000);
    QCOMPARE_ENUM(old.trigger().transition, Transition::Triggered);
  }

  void abandonedEventTriggersAgain() {
    Sim sim;
    QCOMPARE(sim.trigger().eventId, QStringLiteral("evt-1"));
    sim.ev.abandonEvent();
    QVERIFY(!sim.ev.openEventId().has_value());
    const Evaluation retried = sim.known(10500, {"t1"});
    QCOMPARE_ENUM(retried.transition, Transition::Triggered);
    QCOMPARE(retried.eventId, QStringLiteral("evt-2"));
  }

  void unknownUtcWithScheduleFreezes() {
    RuleRevision r = makeRule();
    r.timeZoneId = QStringLiteral("Asia/Seoul");
    r.schedule = {TimeWindow{22 * 60, 6 * 60, 0x7f}};
    Sim sim(r);
    QCOMPARE_ENUM(sim.trigger().transition, Transition::Triggered);
    for (int64_t atMs = 10500; atMs <= 12000; atMs += 500) {
      ObservationFrame f = sim.frame(atMs);
      f.utcMs = 0;
      const Evaluation e = sim.feed(f);
      QCOMPARE_ENUM(e.transition, Transition::Unknown);
      QVERIFY(e.note.contains(QStringLiteral("utc")));
      QCOMPARE_ENUM(e.quality, Quality::Unknown);
      QCOMPARE_ENUM(e.after, ConditionState::Active);
    }
    QCOMPARE(sim.ev.openEventId(), std::optional<QString>(QStringLiteral("evt-1")));
    const Evaluation known = sim.known(12500, {"t1"});
    QCOMPARE_ENUM(known.transition, Transition::None);
    QCOMPARE_ENUM(known.after, ConditionState::Active);
  }

  void emptyFrameWithOtherSizeIsZoneMismatch() {
    Sim sim;
    sim.trigger();
    sim.frameWidth = 1920;
    sim.frameHeight = 1080;
    const auto mismatched = sim.run(10500, 12500, 500);
    QVERIFY(allTransitions(mismatched, Transition::ZoneMismatch));
    QCOMPARE_ENUM(mismatched.last().after, ConditionState::Active);
    QCOMPARE_ENUM(mismatched.last().quality, Quality::Unknown);
    sim.frameWidth = kRefW;
    sim.frameHeight = kRefH;
    QCOMPARE_ENUM(sim.known(13000).transition, Transition::BecameClearing);
    sim.run(13500, 17500, 500);
    QCOMPARE_ENUM(sim.known(18000).transition, Transition::Cleared);
  }

  void ruleWithoutCameraAcceptsNothing() {
    RuleRevision r = makeRule();
    r.cameraId.clear();
    Sim sim(r);
    Evaluation e = sim.known(0, {"t1"});
    QCOMPARE_ENUM(e.transition, Transition::Stale);
    QVERIFY(e.note.contains(QStringLiteral("no camera")));
    ObservationFrame f = sim.frame(500, insideTracks({"t1"}));
    f.cameraId.clear();
    e = sim.feed(f);
    QCOMPARE_ENUM(e.transition, Transition::Stale);
    QCOMPARE_ENUM(sim.ev.state(), ConditionState::Inactive);
  }

  void setRuleRejectsOtherRuleCameraOrOlderRevision() {
    Sim sim;
    sim.trigger();
    RuleRevision r3 = makeRule();
    r3.revision = 3;
    QVERIFY(sim.apply(r3));
    RuleRevision older = makeRule();
    older.revision = 2;
    QVERIFY(!sim.apply(older));
    RuleRevision otherRule = makeRule();
    otherRule.revision = 4;
    otherRule.ruleId = QStringLiteral("other");
    QVERIFY(!sim.apply(otherRule));
    RuleRevision otherCamera = makeRule();
    otherCamera.revision = 4;
    otherCamera.cameraId = QStringLiteral("cam-2");
    QVERIFY(!sim.apply(otherCamera));
    QCOMPARE(sim.ev.rule().revision, 3);
    QCOMPARE(sim.ev.rule().ruleId, QStringLiteral("rule-1"));
    QCOMPARE(sim.ev.rule().cameraId, QStringLiteral("cam-1"));
    QCOMPARE(sim.ev.openEventId(), std::optional<QString>(QStringLiteral("evt-1")));
    r3.dwellNs = 4 * kSecNs;
    QVERIFY(sim.apply(r3));
    QCOMPARE(sim.ev.rule().dwellNs, 4 * kSecNs);
  }

  void setRuleAppliesTimeZone() {
    RuleRevision r = makeRule();
    r.schedule = {TimeWindow{22 * 60, 6 * 60, 0x7f}};
    Sim sim(r);
    QVERIFY(!sim.ev.inSchedule(kSundayNightUtcMs));
    r.timeZoneId = QStringLiteral("Asia/Seoul");
    QVERIFY(sim.apply(r));
    QVERIFY(sim.ev.inSchedule(kSundayNightUtcMs));
  }

  void setRuleAppliesZoneAndClearAfter() {
    Sim sim;
    sim.trigger();
    RuleRevision r2 = makeRule();
    r2.zone.points = {{0.6, 0.6}, {0.9, 0.6}, {0.9, 0.9}, {0.6, 0.9}};
    r2.clearAfterNs = 2 * kSecNs;
    QVERIFY(sim.apply(r2));
    QCOMPARE_ENUM(sim.known(10500, {"t1"}).transition, Transition::BecameClearing);
    QVERIFY(allTransitions(sim.run(11000, 12000, 500, {"t1"}), Transition::None));
    QCOMPARE_ENUM(sim.known(12500, {"t1"}).transition, Transition::Cleared);
  }

  void inScheduleDaysMaskAppliesToSameDayWindow() {
    RuleRevision r = makeRule();
    r.schedule = {TimeWindow{12 * 60, 13 * 60, 1 << 0}};
    Sim sim(r);
    const int64_t mondayNoonUtc = kMondayNoonUtcMs + 9 * 3600 * 1000;
    QVERIFY(sim.ev.inSchedule(mondayNoonUtc));
    QVERIFY(!sim.ev.inSchedule(mondayNoonUtc - 24 * 3600 * 1000));
    QVERIFY(!sim.ev.inSchedule(mondayNoonUtc + 3600 * 1000));
  }
};

QTEST_GUILESS_MAIN(TestRuleEvaluator)
#include "test_rule_evaluator.moc"
