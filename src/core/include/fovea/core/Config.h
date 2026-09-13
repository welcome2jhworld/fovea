#pragma once
#include <QString>
#include <algorithm>
#include <cstdint>

namespace fovea::core {

struct CoreConfig {
  QString dataDir;
  QString recordingsDir;
  int64_t minFreeBytes = 2LL * 1024 * 1024 * 1024;
  // Test-only (FOVEA_RETENTION_SECONDS): replaces every camera's retention_days age limit.
  int64_t retentionSecondsOverride = 0;
  uint32_t ringSlots = 3;
  uint32_t ringMaxWidth = 1280;
  uint32_t ringMaxHeight = 720;
  int staleAfterMs = 3000;
  int gapAfterMs = 2000;
  int maxPlaybackChannels = 4;

  // Recording resumes, and retention stops freeing space, at the floor plus this.
  int64_t diskHeadroomBytes() const { return std::max(minFreeBytes / 10, int64_t{16} * 1024 * 1024); }
};

}
