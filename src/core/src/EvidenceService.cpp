#include "fovea/core/EvidenceService.h"
#include "fovea/Clock.h"
#include "fovea/core/Store.h"
#include <algorithm>
#include <utility>

namespace fovea::core {
namespace {

constexpr int kPassIntervalMs = 5000;
// Adjacent segments of one session hold contiguous media, but their stored
// boundaries differ by the splitmuxsink bookkeeping (15 to 170 ms measured).
constexpr int64_t kJoinToleranceMs = 500;
constexpr int kGapLimit = 500;

struct Covered {
  int64_t start = 0;
  int64_t end = 0;
  QString sessionId;
};

// Overlapping segments form one run, and so do adjacent segments of the same
// session; the window is covered when one run reaches both of its edges.
bool covers(QVector<Covered> intervals, int64_t from, int64_t to) {
  std::sort(intervals.begin(), intervals.end(), [](const Covered& a, const Covered& b) { return a.start < b.start; });
  bool open = false;
  Covered run;
  for (const Covered& c : intervals) {
    const bool joins = open && (c.start <= run.end || (c.sessionId == run.sessionId && c.start <= run.end + kJoinToleranceMs));
    if (!joins) {
      if (open && run.start <= from && run.end >= to) return true;
      run = c;
      open = true;
      continue;
    }
    if (c.end > run.end) {
      run.end = c.end;
      run.sessionId = c.sessionId;
    }
  }
  return open && run.start <= from && run.end >= to;
}

}

EvidenceRef assessEvidence(const EvidenceRef& ref, const QVector<RecordingSegment>& segments, const QVector<ReceiveGap>& gaps,
                           int64_t nowUtcMs, int64_t graceMs) {
  EvidenceRef out = ref;
  out.updatedUtcMs = nowUtcMs;
  out.segmentIds.clear();
  QVector<Covered> finalized;
  QString deletedId;
  bool recording = false;
  bool damaged = false;
  for (const RecordingSegment& seg : segments) {
    if (seg.state == QLatin1String("deleted")) {
      if (deletedId.isEmpty()) deletedId = seg.id;
      continue;
    }
    out.segmentIds.push_back(seg.id);
    if (seg.state == QLatin1String("finalized")) finalized.push_back({seg.startUtcMs, seg.endUtcMs, seg.sessionId});
    else if (seg.state == QLatin1String("recording")) recording = true;
    else damaged = true;
  }
  const auto set = [&out](const QString& state, const QString& reason) {
    out.state = state;
    out.reason = reason;
    return out;
  };
  if (!deletedId.isEmpty()) return set(QStringLiteral("deleted"), QStringLiteral("segment %1 was deleted").arg(deletedId));
  const bool gap = std::any_of(gaps.begin(), gaps.end(), [&ref](const ReceiveGap& g) {
    return g.fromUtcMs < ref.toUtcMs && (g.toUtcMs == 0 || g.toUtcMs > ref.fromUtcMs);
  });
  if (!gap && !damaged && covers(finalized, ref.fromUtcMs, ref.toUtcMs)) return set(QStringLiteral("available"), QString());
  // A segment still recording across the window may yet cover it, however long it runs.
  const bool ended = !recording && nowUtcMs > ref.toUtcMs + graceMs;
  if (out.segmentIds.isEmpty())
    return ended ? set(QStringLiteral("partial"), QStringLiteral("no recording covers the window"))
                 : set(QStringLiteral("pending"), QStringLiteral("waiting for recordings"));
  if (gap) return set(QStringLiteral("partial"), QStringLiteral("a receive gap intersects the window"));
  if (damaged) return set(QStringLiteral("partial"), QStringLiteral("a damaged segment intersects the window"));
  if (!ended && nowUtcMs < ref.toUtcMs) return set(QStringLiteral("partial"), QStringLiteral("the window ends in the future"));
  if (recording) return set(QStringLiteral("partial"), QStringLiteral("the window ends inside the segment still recording"));
  return set(QStringLiteral("partial"), QStringLiteral("recordings do not cover the whole window"));
}

EvidenceService::EvidenceService(Store& store, QString evidenceDir, QObject* parent)
    : QObject(parent), store_(store), evidenceDir_(std::move(evidenceDir)) {
  timer_.setInterval(kPassIntervalMs);
  connect(&timer_, &QTimer::timeout, this, [this] { runPass(utcNowMs()); });
}

void EvidenceService::start() {
  timer_.start();
  runPass(utcNowMs());
}

int EvidenceService::runPass(int64_t nowUtcMs) {
  int changed = 0;
  for (const EvidenceRef& ref : store_.evidenceToEvaluate(kGraceMs)) {
    EvidenceRef next = assessEvidence(ref, store_.segmentsOverlapping(ref.cameraId, ref.fromUtcMs, ref.toUtcMs),
                                      store_.listGaps(ref.cameraId, ref.fromUtcMs, ref.toUtcMs, kGapLimit), nowUtcMs, kGraceMs);
    if (next.state == QLatin1String("deleted")) next.thumbnailPath.clear();
    const std::optional<EventRecord> event = store_.getEvent(ref.eventId);
    const int64_t holdUntil =
        event && event->operatorState == QLatin1String("resolved") ? event->openedUtcMs + kEvidenceRetentionMs : 0;
    if (!store_.updateEvidence(next, holdUntil)) {
      qWarning("evidence: cannot update %s: %s", qPrintable(ref.id), qPrintable(store_.lastError()));
      continue;
    }
    if (ref.thumbnailPath != next.thumbnailPath) Store::removeEvidenceFiles({ref.thumbnailPath}, evidenceDir_);
    if (next.state != ref.state || next.reason != ref.reason) {
      ++changed;
      qInfo("evidence: %s of event %s %s%s", qPrintable(ref.id), qPrintable(ref.eventId), qPrintable(next.state),
            next.reason.isEmpty() ? "" : qPrintable(QStringLiteral(" (") + next.reason + QLatin1Char(')')));
    }
  }
  return changed;
}

}
