#include "fovea/core/AnalysisTap.h"
#include <cstring>

namespace fovea::core {

void AnalysisTap::setEnabled(bool enabled) {
  enabled_.store(enabled, std::memory_order_relaxed);
  if (enabled) return;
  std::lock_guard<std::mutex> lock(mutex_);
  slot_ = AnalysisFrame{};
  filled_ = false;
}

bool AnalysisTap::wants(int64_t nowMonoNs) const {
  return enabled() && nowMonoNs - lastCopyMonoNs_.load(std::memory_order_relaxed) >= kMinCopyIntervalNs;
}

void AnalysisTap::offer(const QString& sessionId, int64_t ptsNs, int64_t recvMonoNs, int64_t utcMs, int width, int height,
                        const uint8_t* pixels, size_t stride, int64_t nowMonoNs) {
  if (width <= 0 || height <= 0 || !pixels || stride < static_cast<size_t>(width) * 4) return;
  lastCopyMonoNs_.store(nowMonoNs, std::memory_order_relaxed);
  const size_t rowBytes = static_cast<size_t>(width) * 4;
  const qsizetype total = static_cast<qsizetype>(rowBytes * static_cast<size_t>(height));
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled()) return;
  if (slot_.bgra.size() != total) slot_.bgra.resize(total);
  char* out = slot_.bgra.data();
  for (int y = 0; y < height; ++y) std::memcpy(out + rowBytes * static_cast<size_t>(y), pixels + stride * static_cast<size_t>(y), rowBytes);
  slot_.width = width;
  slot_.height = height;
  slot_.seq = nextSeq_++;
  slot_.sessionId = sessionId;
  slot_.ptsNs = ptsNs;
  slot_.recvMonoNs = recvMonoNs;
  slot_.utcMs = utcMs;
  filled_ = true;
}

std::optional<AnalysisFrame> AnalysisTap::takeNewer(uint64_t afterSeq) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!filled_ || slot_.seq <= afterSeq) return std::nullopt;
  AnalysisFrame out = std::move(slot_);
  slot_ = AnalysisFrame{};
  slot_.seq = out.seq;
  filled_ = false;
  return out;
}

}
