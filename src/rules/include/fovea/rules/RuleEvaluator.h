#pragma once
#include "fovea/rules/Types.h"
#include <QHash>
#include <QTimeZone>
#include <functional>

namespace fovea::rules {

// State machine for one rule on one camera. Pure: no I/O, no clocks of its
// own. Feed it observation frames in arrival order; it returns what changed.
//
// Condition states:
//   inactive -> pending  : a matching track is inside the zone
//   pending  -> active   : one track has been inside continuously for dwellNs
//   active   -> clearing : no matching track inside; clearAfterNs timer starts
//   clearing -> active   : a matching track is inside again
//   clearing -> inactive : clearAfterNs elapsed (Cleared)
//   pending  -> inactive : no matching track inside (PendingReset)
//
// Presence and dwell:
// - A track is "inside" from the first known frame that places its anchor in
//   the zone. It stays involved while it is not seen for at most
//   maxObservationGapNs of media time; a longer absence or a sighting outside
//   the zone restarts its dwell. Track ids are never merged: a new id starts
//   from zero. Sightings of another class or below minConfidence are ignored:
//   they neither end nor extend presence.
// - A track's dwell runs from its first inside sighting to its latest one,
//   minus frozen media time. dwellNs in an Evaluation is the longest dwell
//   among the involved tracks. windowStartPtsNs is the earliest first-inside
//   pts among them (frame pts when no track is involved), windowEndPtsNs is the
//   frame pts, and unknownNs is the frozen media time inside that window, so
//   the observed time in the window is its length minus unknownNs.
//
// Timing contract:
// - Dwell, clearing and rearm are measured in media time (ptsNs) of known
//   observations. An accepted frame accounts for the media time since the
//   previous accepted frame of its session. The whole span is frozen (does not
//   count) when the frame has quality Unknown, has a frame size different from
//   the zone reference size (ZoneMismatch), or has utcMs <= 0 while the rule
//   has a schedule (Unknown, note "utc unknown"). For a known frame, the part
//   of the span beyond maxObservationGapNs is frozen, and so is the part
//   covered by frames of the session dropped as stale for generation or TTL.
// - After more than maxObservationGapNs without a known frame, all track dwell
//   is discarded and pending resets (PendingReset when the zone is empty). An
//   open event persists across such a gap unless its uncovered part lasted
//   more than clearAfterNs: then the occupancy is over, Active or Clearing
//   reports Cleared (note "observation gap") on the resume frame, and presence
//   on that frame starts a new pending occupancy. The gap is measured in media
//   time within a session and in receive time across a session change; every
//   frame that arrived but could not be observed (Unknown, ZoneMismatch, utc
//   unknown, or dropped as stale for generation or TTL) covers the span since
//   the previous frame, at most maxObservationGapNs, so a worker outage never
//   closes an occupancy: on resume presence keeps Active and absence reports
//   BecameClearing.
// - tick() reports Unknown and marks quality Unknown once no frame has been
//   accepted for more than maxObservationGapNs of wall time; it never changes a
//   timer.
// - A frame from a different session is accepted only when its recvMonoNs is
//   newer than that of the last accepted frame. It drops all track state and
//   pending time, keeps an open event, marks quality Unknown until the first
//   known frame of the new session is processed, and restarts pts monotonic
//   tracking. The elapsed parts of running clearing and rearm timers carry
//   over into the new session.
// - Stale input reports Transition::Stale with the reason in note and never
//   changes the condition, tracks, timers, session, generation or tick
//   liveness: a rule without a camera or a frame for another camera,
//   generation below the last accepted one, receive age above resultTtlNs at
//   nowMonoNs, pts not greater than the last accepted pts of the same session,
//   or a session superseded by the current one.
// - Outside the schedule the condition is false: pending resets, an active
//   event becomes clearing and clears after clearAfterNs. Such frames report
//   OutOfSchedule unless an event lifecycle transition (BecameClearing,
//   Cleared) happens, which is reported instead so that consumers never miss
//   it.
// - A disabled rule reports Disabled and accepts nothing.
// - seedRearm carries a clear from before the evaluator existed (a core
//   restart): the first accepted frame with a known utc places that clear on
//   its media timeline, so rearm counts from the clear's wall time.
//
// Events, dedupe and rearm:
// - An event opens (Triggered) only on a frame where an involved track has
//   completed dwellNs and rearm has elapsed. It stays open through Clearing and
//   is closed by Cleared. While Active with an event, StillActive is reported
//   on frames where an involved track has completed a dwell; frames occupied
//   only by tracks that have not (after a gap, a session change, setRule, a
//   hand-over or a re-entry) report None. Re-entry from Clearing reports
//   StillActive on that frame.
// - After an event clears, a track that completes its dwell before rearmNs of
//   media time since the clear enters Active without an event. While Active
//   without an event, every frame reports Suppressed (note "rearm") until
//   rearmNs has elapsed; after that frames report None until an involved track
//   has completed a dwell, which opens the event. An event-less occupancy that
//   ends reports BecameClearing and Cleared with an empty eventId and does not
//   restart the rearm timer.
class RuleEvaluator {
public:
  using IdGenerator = std::function<QString()>;
  explicit RuleEvaluator(RuleRevision rule, IdGenerator ids);

  // Applies a revision of the same rule id and camera that is not older than
  // the current one; otherwise returns std::nullopt and changes nothing. An
  // enabled revision keeps an open event and the Active/Clearing state,
  // discards all track dwell and applies the new thresholds, zone, time zone
  // and schedule. Disabling ends the occupancy: Active or Clearing reports
  // Cleared (note "disabled") and a closed event starts the rearm timer.
  // Otherwise the result reports PendingReset, Disabled or None.
  [[nodiscard]] std::optional<Evaluation> setRule(RuleRevision rule, int64_t nowUtcMs);
  const RuleRevision& rule() const { return rule_; }
  // Frames of an older generation are stale from now on (a rule-set or
  // session change the evaluator has not seen a frame of yet).
  void raiseGeneration(uint64_t generation);
  // Rearm counts from clearedUtcMs, unless an event cleared since.
  void seedRearm(int64_t clearedUtcMs);
  // The open event could not be stored: forget it without starting rearm, so
  // the next frame with a completed dwell triggers again with a new id.
  void abandonEvent();

  Evaluation evaluate(const ObservationFrame& frame, int64_t nowMonoNs);
  // Called periodically without observations; reports Unknown after
  // maxObservationGapNs of silence (wall time).
  Evaluation tick(int64_t nowMonoNs, int64_t nowUtcMs);

  ConditionState state() const { return state_; }
  Quality quality() const { return quality_; }
  std::optional<QString> openEventId() const;
  bool inSchedule(int64_t utcMs) const;

private:
  struct TrackState {
    int64_t insideSincePts = -1;
    int64_t lastSeenPts = -1;
    int64_t frozenAtInsideNs = 0;
    int64_t frozenAtSeenNs = 0;
  };
  struct Involvement {
    QVector<QString> trackIds;
    int64_t earliestInsidePts = -1;
    int64_t longestDwellNs = 0;
    int64_t unknownNs = 0;
  };
  Evaluation base(const ObservationFrame& frame) const;
  Evaluation idle(int64_t nowUtcMs) const;
  void resetPending();
  void applyTimeZone();
  void beginSession(const QString& sessionId, int64_t firstPts);
  void noteDropped(const ObservationFrame& frame);
  void coverUnobserved(const ObservationFrame& frame);
  void applyRearmSeed(const ObservationFrame& frame);
  void shiftTimers(int64_t byNs);
  void freezeSpan(int64_t fromPts, int64_t toPts);
  void freezeUncovered(int64_t prevPts, int64_t pts, bool known);
  void updateTracks(const ObservationFrame& frame);
  void pruneTracks(int64_t pts);
  Involvement involvement() const;
  bool zoneMatches(const ObservationFrame& frame, QString* note) const;
  bool rearmElapsed(int64_t pts) const;
  Transition enterActive(int64_t pts, Evaluation& e);
  Transition reportActive(int64_t pts, bool confirmed, Evaluation& e);
  void closeOccupancy(int64_t pts, Evaluation& e);
  void evaluateKnown(const ObservationFrame& frame, Evaluation& e);

  RuleRevision rule_;
  IdGenerator ids_;
  QTimeZone timeZone_;
  ConditionState state_ = ConditionState::Inactive;
  Quality quality_ = Quality::Unknown;
  QString sessionId_;
  uint64_t generation_ = 0;
  int64_t lastPts_ = -1;
  int64_t lastKnownPts_ = -1;
  int64_t droppedThroughPts_ = -1;
  int64_t lastEvalMonoNs_ = -1;
  std::optional<int64_t> lastRecvMonoNs_;
  std::optional<int64_t> lastKnownRecvMonoNs_;
  // Span since the last known frame covered by frames that could not be
  // observed: media time in the current session, receive time across sessions.
  int64_t coveredThroughPts_ = -1;
  std::optional<int64_t> coveredThroughRecvNs_;
  int64_t coveredPtsNs_ = 0;
  int64_t coveredRecvNs_ = 0;
  std::optional<int64_t> rearmSeedUtcMs_;
  int64_t frozenTotalNs_ = 0;
  int64_t clearingSincePts_ = 0;
  std::optional<int64_t> lastClearedPts_;
  QString eventId_;
  QHash<QString, TrackState> tracks_;
};

}
