#pragma once
#include <QString>
#include <QVector>
#include <atomic>
#include <cstdint>

namespace fovea::core {

struct SampleRequest {
  QString segmentPath;
  int64_t startUtcMs = 0;
  int64_t endUtcMs = 0;
  int64_t startPtsNs = 0;
  int intervalMs = 1000;
  // <spoolDir>/<n>.jpg for the worker (at most 960 px wide).
  QString spoolDir;
  // <thumbnailDir>/<thumbnailPrefix>-<n>.jpg (at most 320 px wide).
  QString thumbnailDir;
  QString thumbnailPrefix;
};

struct SampledFrame {
  int index = 0;
  int64_t ptsNs = 0;
  int64_t utcMs = 0;
  QString jpegPath;
  QString thumbnailPath;
};

struct SampleResult {
  QString error;
  QVector<SampledFrame> frames;
  int64_t expected = 0;
  int64_t decodedFrames = 0;
  int64_t elapsedMs = 0;
};

inline constexpr int kSampleJpegMaxWidth = 960;
inline constexpr int kThumbnailMaxWidth = 320;

// Decodes a finalized recording segment (filesrc ! demux ! parse ! avdec) and
// takes one frame per grid point: the multiples of intervalMs (in UTC) in
// [startUtcMs, endUtcMs). A grid point takes the frame on screen at that
// instant (the latest frame at or before it; the first frame for points before
// it), so a frame is sampled at most once and a stall in the media leaves grid
// points unsampled. Frame time is the segment start plus the frame's offset
// from the first decoded frame. Blocks the calling thread (not the core
// thread); cancel, when set, stops it with error "cancelled".
SampleResult sampleSegment(const SampleRequest& request, const std::atomic<bool>* cancel);

}
