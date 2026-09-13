#pragma once
#include "fovea/Api.h"
#include "fovea/core/Analytics.h"
#include <QObject>
#include <QTimer>

namespace fovea::core {

class Store;

// Resolves an evidence ref against the recordings of its camera:
// - deleted: a segment inside the window was deleted;
// - available: finalized segments cover the whole window and no receive gap
//   or damaged segment intersects it;
// - pending: nothing covers the window yet;
// - partial: some coverage, with the reason (window still in the future, end
//   inside the segment still recording, gap, damaged segment, or a hole).
// Finalized segments must reach both window edges; adjacent segments of one
// session may join with a rounding-sized hole. A ref still not available
// graceMs after its window ended, with no segment recording across it, is
// partial for good.
EvidenceRef assessEvidence(const EvidenceRef& ref, const QVector<RecordingSegment>& segments, const QVector<ReceiveGap>& gaps,
                           int64_t nowUtcMs, int64_t graceMs);

// Re-evaluates pending and partial refs every 5 s and keeps one evidence hold
// per collected segment (open-ended while the event is not resolved, else
// until the event's open time plus the evidence retention period). A ref that
// becomes deleted loses its thumbnail file (<evidenceDir>/<event_id>.jpg).
class EvidenceService : public QObject {
  Q_OBJECT
public:
  static constexpr int64_t kGraceMs = 120'000;

  EvidenceService(Store& store, QString evidenceDir, QObject* parent = nullptr);
  void start();
  // Returns the number of refs whose state or reason changed.
  int runPass(int64_t nowUtcMs);

private:
  Store& store_;
  QString evidenceDir_;
  QTimer timer_;
};

}
