#include "fovea/rules/RuleEvaluator.h"

#include <QDateTime>
#include <algorithm>

namespace fovea::rules {

namespace {

bool dayOn(int days, int day) { return (days & (1 << day)) != 0; }

bool isLifecycle(Transition t) {
  return t == Transition::Triggered || t == Transition::StillActive || t == Transition::BecameClearing ||
         t == Transition::Cleared;
}

bool occupied(ConditionState s) { return s == ConditionState::Active || s == ConditionState::Clearing; }

Evaluation stale(Evaluation e, const QString& note) {
  e.transition = Transition::Stale;
  e.note = note;
  return e;
}

}

RuleEvaluator::RuleEvaluator(RuleRevision rule, IdGenerator ids) : rule_(std::move(rule)), ids_(std::move(ids)) {
  applyTimeZone();
}

std::optional<Evaluation> RuleEvaluator::setRule(RuleRevision rule, int64_t nowUtcMs) {
  if (rule.ruleId != rule_.ruleId || rule.cameraId != rule_.cameraId || rule.revision < rule_.revision) {
    return std::nullopt;
  }
  rule_ = std::move(rule);
  applyTimeZone();
  Evaluation e = idle(nowUtcMs);
  resetPending();
  if (!rule_.enabled && occupied(state_)) {
    closeOccupancy(std::max<int64_t>(lastPts_, 0), e);
    e.transition = Transition::Cleared;
    e.note = QStringLiteral("disabled");
  } else if (e.before == ConditionState::Pending) {
    e.transition = Transition::PendingReset;
  } else {
    e.transition = rule_.enabled ? Transition::None : Transition::Disabled;
  }
  e.after = state_;
  return e;
}

void RuleEvaluator::applyTimeZone() {
  timeZone_ = QTimeZone(rule_.timeZoneId.toUtf8());
  if (!timeZone_.isValid()) timeZone_ = QTimeZone::utc();
}

void RuleEvaluator::raiseGeneration(uint64_t generation) { generation_ = std::max(generation_, generation); }

void RuleEvaluator::seedRearm(int64_t clearedUtcMs) {
  if (!lastClearedPts_) rearmSeedUtcMs_ = clearedUtcMs;
}

void RuleEvaluator::abandonEvent() { eventId_.clear(); }

std::optional<QString> RuleEvaluator::openEventId() const {
  if (eventId_.isEmpty()) return std::nullopt;
  return eventId_;
}

bool RuleEvaluator::inSchedule(int64_t utcMs) const {
  if (rule_.schedule.isEmpty()) return true;
  const QDateTime local = QDateTime::fromMSecsSinceEpoch(utcMs, timeZone_);
  const int minute = local.time().hour() * 60 + local.time().minute();
  const int today = local.date().dayOfWeek() - 1;
  const int yesterday = (today + 6) % 7;
  for (const TimeWindow& w : rule_.schedule) {
    const bool wraps = w.endMinute <= w.startMinute;
    if (!wraps) {
      if (dayOn(w.days, today) && minute >= w.startMinute && minute < w.endMinute) return true;
    } else {
      if (dayOn(w.days, today) && minute >= w.startMinute) return true;
      if (dayOn(w.days, yesterday) && minute < w.endMinute) return true;
    }
  }
  return false;
}

Evaluation RuleEvaluator::base(const ObservationFrame& frame) const {
  Evaluation e;
  e.ruleId = rule_.ruleId;
  e.ruleRevision = rule_.revision;
  e.cameraId = frame.cameraId;
  e.sessionId = frame.sessionId;
  e.windowStartPtsNs = frame.ptsNs;
  e.windowEndPtsNs = frame.ptsNs;
  e.utcMs = frame.utcMs;
  e.generation = frame.generation;
  e.before = state_;
  e.after = state_;
  e.quality = quality_;
  return e;
}

Evaluation RuleEvaluator::idle(int64_t nowUtcMs) const {
  ObservationFrame f;
  f.cameraId = rule_.cameraId;
  f.sessionId = sessionId_;
  f.ptsNs = std::max<int64_t>(lastPts_, 0);
  f.utcMs = nowUtcMs;
  f.generation = generation_;
  return base(f);
}

void RuleEvaluator::resetPending() {
  tracks_.clear();
  if (state_ == ConditionState::Pending) state_ = ConditionState::Inactive;
}

void RuleEvaluator::beginSession(const QString& sessionId, int64_t firstPts) {
  if (lastPts_ >= 0) shiftTimers(firstPts - lastPts_);
  sessionId_ = sessionId;
  resetPending();
  lastPts_ = -1;
  lastKnownPts_ = -1;
  droppedThroughPts_ = -1;
  coveredThroughPts_ = -1;
  coveredPtsNs_ = 0;
  quality_ = Quality::Unknown;
}

void RuleEvaluator::noteDropped(const ObservationFrame& frame) {
  if (frame.sessionId == sessionId_ && frame.ptsNs > lastPts_) {
    droppedThroughPts_ = std::max(droppedThroughPts_, frame.ptsNs);
    coverUnobserved(frame);
  }
}

void RuleEvaluator::coverUnobserved(const ObservationFrame& frame) {
  if (coveredThroughPts_ >= 0 && frame.ptsNs > coveredThroughPts_)
    coveredPtsNs_ += std::min(frame.ptsNs - coveredThroughPts_, rule_.maxObservationGapNs);
  if (coveredThroughRecvNs_ && frame.recvMonoNs > *coveredThroughRecvNs_)
    coveredRecvNs_ += std::min(frame.recvMonoNs - *coveredThroughRecvNs_, rule_.maxObservationGapNs);
  coveredThroughPts_ = std::max(coveredThroughPts_, frame.ptsNs);
  coveredThroughRecvNs_ = std::max(coveredThroughRecvNs_.value_or(frame.recvMonoNs), frame.recvMonoNs);
}

void RuleEvaluator::applyRearmSeed(const ObservationFrame& frame) {
  if (!rearmSeedUtcMs_ || frame.utcMs <= 0) return;
  if (!lastClearedPts_) {
    const int64_t sinceClearMs = std::clamp<int64_t>(frame.utcMs - *rearmSeedUtcMs_, 0, rule_.rearmNs / 1'000'000 + 1);
    lastClearedPts_ = frame.ptsNs - sinceClearMs * 1'000'000;
  }
  rearmSeedUtcMs_.reset();
}

void RuleEvaluator::shiftTimers(int64_t byNs) {
  clearingSincePts_ += byNs;
  if (lastClearedPts_) *lastClearedPts_ += byNs;
}

void RuleEvaluator::freezeSpan(int64_t fromPts, int64_t toPts) {
  if (fromPts < 0 || toPts <= fromPts) return;
  frozenTotalNs_ += toPts - fromPts;
  shiftTimers(toPts - fromPts);
}

void RuleEvaluator::freezeUncovered(int64_t prevPts, int64_t pts, bool known) {
  int64_t until = known ? prevPts : pts;
  if (known && pts - prevPts > rule_.maxObservationGapNs) until = pts - rule_.maxObservationGapNs;
  if (droppedThroughPts_ < pts) until = std::max(until, droppedThroughPts_);
  if (droppedThroughPts_ <= pts) droppedThroughPts_ = -1;
  freezeSpan(prevPts, until);
}

void RuleEvaluator::updateTracks(const ObservationFrame& frame) {
  const int64_t pts = frame.ptsNs;
  for (const TrackObservation& t : frame.tracks) {
    if (t.cls != rule_.targetClass || t.confidence < rule_.minConfidence) continue;
    if (!rule_.zone.contains(t.anchor)) {
      tracks_.remove(t.trackId);
      continue;
    }
    TrackState& s = tracks_[t.trackId];
    if (s.insideSincePts < 0 || pts - s.lastSeenPts > rule_.maxObservationGapNs) {
      s.insideSincePts = pts;
      s.frozenAtInsideNs = frozenTotalNs_;
    }
    s.lastSeenPts = pts;
    s.frozenAtSeenNs = frozenTotalNs_;
  }
}

void RuleEvaluator::pruneTracks(int64_t pts) {
  for (auto it = tracks_.begin(); it != tracks_.end();) {
    if (pts - it->lastSeenPts > rule_.maxObservationGapNs) it = tracks_.erase(it);
    else ++it;
  }
}

RuleEvaluator::Involvement RuleEvaluator::involvement() const {
  Involvement inv;
  for (auto it = tracks_.cbegin(); it != tracks_.cend(); ++it) {
    inv.trackIds.push_back(it.key());
    if (inv.earliestInsidePts < 0 || it->insideSincePts < inv.earliestInsidePts) {
      inv.earliestInsidePts = it->insideSincePts;
      inv.unknownNs = frozenTotalNs_ - it->frozenAtInsideNs;
    }
    const int64_t frozenNs = it->frozenAtSeenNs - it->frozenAtInsideNs;
    inv.longestDwellNs = std::max(inv.longestDwellNs, it->lastSeenPts - it->insideSincePts - frozenNs);
  }
  std::sort(inv.trackIds.begin(), inv.trackIds.end());
  return inv;
}

bool RuleEvaluator::zoneMatches(const ObservationFrame& frame, QString* note) const {
  const ZoneRevision& z = rule_.zone;
  if (z.refWidth <= 0 || z.refHeight <= 0) return true;
  if (frame.frameWidth == z.refWidth && frame.frameHeight == z.refHeight) return true;
  *note = QStringLiteral("frame %1x%2 differs from zone reference %3x%4")
              .arg(frame.frameWidth)
              .arg(frame.frameHeight)
              .arg(z.refWidth)
              .arg(z.refHeight);
  return false;
}

bool RuleEvaluator::rearmElapsed(int64_t pts) const {
  return !lastClearedPts_ || pts - *lastClearedPts_ >= rule_.rearmNs;
}

Transition RuleEvaluator::enterActive(int64_t pts, Evaluation& e) {
  state_ = ConditionState::Active;
  if (!rearmElapsed(pts)) {
    e.note = QStringLiteral("rearm");
    return Transition::Suppressed;
  }
  eventId_ = ids_();
  e.eventId = eventId_;
  return Transition::Triggered;
}

Transition RuleEvaluator::reportActive(int64_t pts, bool confirmed, Evaluation& e) {
  if (eventId_.isEmpty()) {
    if (confirmed || !rearmElapsed(pts)) return enterActive(pts, e);
    return Transition::None;
  }
  if (!confirmed) return Transition::None;
  e.eventId = eventId_;
  return Transition::StillActive;
}

void RuleEvaluator::closeOccupancy(int64_t pts, Evaluation& e) {
  state_ = ConditionState::Inactive;
  if (eventId_.isEmpty()) return;
  e.eventId = eventId_;
  eventId_.clear();
  lastClearedPts_ = pts;
}

void RuleEvaluator::evaluateKnown(const ObservationFrame& frame, Evaluation& e) {
  const int64_t pts = frame.ptsNs;
  bool closedByGap = false;
  const bool firstInSession = lastKnownPts_ < 0;
  if (firstInSession || pts - lastKnownPts_ > rule_.maxObservationGapNs) {
    int64_t unobservedNs = pts - lastKnownPts_ - coveredPtsNs_;
    if (firstInSession)
      unobservedNs = lastKnownRecvMonoNs_ ? frame.recvMonoNs - *lastKnownRecvMonoNs_ - coveredRecvNs_ : 0;
    resetPending();
    if (occupied(state_) && unobservedNs > rule_.clearAfterNs) {
      closeOccupancy(pts, e);
      e.note = QStringLiteral("observation gap");
      closedByGap = true;
    }
  }
  lastKnownPts_ = pts;
  lastKnownRecvMonoNs_ = frame.recvMonoNs;
  coveredThroughPts_ = pts;
  coveredThroughRecvNs_ = frame.recvMonoNs;
  coveredPtsNs_ = 0;
  coveredRecvNs_ = 0;
  quality_ = Quality::Known;

  const bool scheduled = inSchedule(frame.utcMs);
  if (scheduled) updateTracks(frame);
  else tracks_.clear();
  pruneTracks(pts);

  const Involvement inv = involvement();
  const bool present = !inv.trackIds.isEmpty();
  const bool confirmed = present && inv.longestDwellNs >= rule_.dwellNs;
  e.trackIds = inv.trackIds;
  e.dwellNs = inv.longestDwellNs;
  if (present) {
    e.windowStartPtsNs = inv.earliestInsidePts;
    e.unknownNs = inv.unknownNs;
  }

  Transition t = Transition::None;
  if (closedByGap) {
    if (present) state_ = ConditionState::Pending;
    t = Transition::Cleared;
  } else {
    switch (state_) {
      case ConditionState::Inactive:
        if (confirmed) {
          t = enterActive(pts, e);
        } else if (present) {
          state_ = ConditionState::Pending;
          t = Transition::BecamePending;
        }
        break;
      case ConditionState::Pending:
        if (confirmed) {
          t = enterActive(pts, e);
        } else if (!present) {
          state_ = ConditionState::Inactive;
          t = Transition::PendingReset;
        }
        break;
      case ConditionState::Active:
        if (!present) {
          state_ = ConditionState::Clearing;
          clearingSincePts_ = pts;
          e.eventId = eventId_;
          t = Transition::BecameClearing;
        } else {
          t = reportActive(pts, confirmed, e);
        }
        break;
      case ConditionState::Clearing:
        if (present) {
          state_ = ConditionState::Active;
          if (eventId_.isEmpty()) {
            t = reportActive(pts, confirmed, e);
          } else {
            e.eventId = eventId_;
            t = Transition::StillActive;
          }
        } else if (pts - clearingSincePts_ >= rule_.clearAfterNs) {
          closeOccupancy(pts, e);
          t = Transition::Cleared;
        }
        break;
    }
  }
  if (t == Transition::None && e.before == ConditionState::Pending && state_ == ConditionState::Inactive) {
    t = Transition::PendingReset;
  }
  if (!scheduled && !isLifecycle(t)) t = Transition::OutOfSchedule;
  e.transition = t;
}

Evaluation RuleEvaluator::evaluate(const ObservationFrame& frame, int64_t nowMonoNs) {
  Evaluation e = base(frame);
  if (!rule_.enabled) {
    e.transition = Transition::Disabled;
    return e;
  }
  if (rule_.cameraId.isEmpty()) return stale(std::move(e), QStringLiteral("rule has no camera"));
  if (frame.cameraId != rule_.cameraId) {
    return stale(std::move(e), QStringLiteral("camera %1 is not %2").arg(frame.cameraId, rule_.cameraId));
  }
  if (frame.generation < generation_) {
    noteDropped(frame);
    return stale(std::move(e), QStringLiteral("generation %1 below %2").arg(frame.generation).arg(generation_));
  }
  if (nowMonoNs - frame.recvMonoNs > rule_.resultTtlNs) {
    noteDropped(frame);
    return stale(std::move(e), QStringLiteral("ttl: result age %1 ms").arg((nowMonoNs - frame.recvMonoNs) / 1'000'000));
  }
  const bool sameSession = frame.sessionId == sessionId_;
  if (sameSession && frame.ptsNs <= lastPts_) {
    return stale(std::move(e), QStringLiteral("pts %1 not after %2").arg(frame.ptsNs).arg(lastPts_));
  }
  if (!sameSession && lastRecvMonoNs_ && frame.recvMonoNs <= *lastRecvMonoNs_) {
    return stale(std::move(e), QStringLiteral("session %1 superseded by %2").arg(frame.sessionId, sessionId_));
  }

  if (!sameSession) beginSession(frame.sessionId, frame.ptsNs);
  applyRearmSeed(frame);
  const int64_t prevPts = lastPts_;
  generation_ = frame.generation;
  lastPts_ = frame.ptsNs;
  lastRecvMonoNs_ = frame.recvMonoNs;
  lastEvalMonoNs_ = nowMonoNs;

  QString note;
  Transition unobservable = Transition::None;
  if (frame.quality == Quality::Unknown) {
    unobservable = Transition::Unknown;
  } else if (!zoneMatches(frame, &note)) {
    unobservable = Transition::ZoneMismatch;
  } else if (!rule_.schedule.isEmpty() && frame.utcMs <= 0) {
    unobservable = Transition::Unknown;
    note = QStringLiteral("utc unknown");
  }
  freezeUncovered(prevPts, frame.ptsNs, unobservable == Transition::None);
  if (unobservable == Transition::None) {
    evaluateKnown(frame, e);
  } else {
    coverUnobserved(frame);
    quality_ = Quality::Unknown;
    e.transition = unobservable;
    e.note = note;
  }
  e.after = state_;
  e.quality = quality_;
  return e;
}

Evaluation RuleEvaluator::tick(int64_t nowMonoNs, int64_t nowUtcMs) {
  Evaluation e = idle(nowUtcMs);
  if (!rule_.enabled) {
    e.transition = Transition::Disabled;
    return e;
  }
  const bool silent = lastEvalMonoNs_ < 0 || nowMonoNs - lastEvalMonoNs_ > rule_.maxObservationGapNs;
  if (silent) {
    quality_ = Quality::Unknown;
    e.transition = Transition::Unknown;
    e.note = lastEvalMonoNs_ < 0 ? QStringLiteral("no observation yet")
                                 : QStringLiteral("no observation for %1 ms").arg((nowMonoNs - lastEvalMonoNs_) / 1'000'000);
  }
  e.quality = quality_;
  return e;
}

}
