#include "fovea/rules/Types.h"

namespace fovea::rules {

QString toString(ConditionState s) {
  switch (s) {
    case ConditionState::Inactive: return QStringLiteral("inactive");
    case ConditionState::Pending: return QStringLiteral("pending");
    case ConditionState::Active: return QStringLiteral("active");
    case ConditionState::Clearing: return QStringLiteral("clearing");
  }
  return QStringLiteral("inactive");
}

QString toString(Quality q) { return q == Quality::Known ? QStringLiteral("known") : QStringLiteral("unknown"); }

QString toString(Transition t) {
  switch (t) {
    case Transition::None: return QStringLiteral("none");
    case Transition::BecamePending: return QStringLiteral("became_pending");
    case Transition::Triggered: return QStringLiteral("triggered");
    case Transition::StillActive: return QStringLiteral("still_active");
    case Transition::BecameClearing: return QStringLiteral("became_clearing");
    case Transition::Cleared: return QStringLiteral("cleared");
    case Transition::PendingReset: return QStringLiteral("pending_reset");
    case Transition::Unknown: return QStringLiteral("unknown");
    case Transition::Suppressed: return QStringLiteral("suppressed");
    case Transition::Stale: return QStringLiteral("stale");
    case Transition::OutOfSchedule: return QStringLiteral("out_of_schedule");
    case Transition::ZoneMismatch: return QStringLiteral("zone_mismatch");
    case Transition::Disabled: return QStringLiteral("disabled");
  }
  return QStringLiteral("none");
}

bool ZoneRevision::contains(const QPointF& p) const {
  const int n = static_cast<int>(points.size());
  if (n < 3) return false;
  bool inside = false;
  for (int i = 0, j = n - 1; i < n; j = i++) {
    const QPointF& a = points[i];
    const QPointF& b = points[j];
    const bool crosses = (a.y() > p.y()) != (b.y() > p.y());
    if (crosses) {
      const double x = (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x();
      if (p.x() < x) inside = !inside;
    }
  }
  return inside;
}

}
