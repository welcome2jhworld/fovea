#pragma once
#include <QByteArray>
#include <QString>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

namespace fovea::core {

// One decoded frame for analysis, tightly packed BGRA (4 bytes per pixel, no
// row padding). seq is issued by the tap and increases with every copy.
struct AnalysisFrame {
  QByteArray bgra;
  int width = 0;
  int height = 0;
  uint64_t seq = 0;
  QString sessionId;
  int64_t ptsNs = 0;
  int64_t recvMonoNs = 0;
  int64_t utcMs = 0;
};

// Newest decoded frame of one camera for the analysis scheduler: a single slot
// that GStreamer streaming threads overwrite and the core thread takes. Copies
// are rate limited to one per minIntervalNs, so a 30 fps stream costs a few
// copies per second instead of thirty; memory is one frame plus the frames the
// scheduler has taken and not yet encoded.
class AnalysisTap {
public:
  static constexpr int64_t kMinCopyIntervalNs = 100'000'000;

  void setEnabled(bool enabled);
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
  // Cheap check for the streaming thread before mapping or converting anything.
  bool wants(int64_t nowMonoNs) const;
  void offer(const QString& sessionId, int64_t ptsNs, int64_t recvMonoNs, int64_t utcMs, int width, int height,
             const uint8_t* pixels, size_t stride, int64_t nowMonoNs);
  // Moves the newest frame out when its seq is above afterSeq.
  std::optional<AnalysisFrame> takeNewer(uint64_t afterSeq);

private:
  std::atomic<bool> enabled_{false};
  std::atomic<int64_t> lastCopyMonoNs_{0};
  mutable std::mutex mutex_;
  AnalysisFrame slot_;
  bool filled_ = false;
  uint64_t nextSeq_ = 1;
};

}
