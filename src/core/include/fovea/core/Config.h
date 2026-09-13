#pragma once
#include <QString>
#include <cstdint>

namespace fovea::core {

struct CoreConfig {
  QString dataDir;
  QString recordingsDir;
  int64_t minFreeBytes = 2LL * 1024 * 1024 * 1024;
  uint32_t ringSlots = 3;
  uint32_t ringMaxWidth = 1280;
  uint32_t ringMaxHeight = 720;
  int staleAfterMs = 3000;
  int gapAfterMs = 2000;
  int maxPlaybackChannels = 4;
};

}
