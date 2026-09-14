#pragma once
#include "fovea/Api.h"
#include "fovea/core/Store.h"
#include <QString>

namespace fovea::core {

// Probes a recorded file by parsing its container. readable=false when it
// cannot be parsed within the time limit; bytes=0 when the file does not exist.
SegmentProbe probeSegmentFile(const QString& path);

// Maps GStreamer caps names to display codec names: video/x-h264 -> H.264.
QString codecDisplayName(const QString& capsName);

struct MediaInfo {
  // "h264", "h265", "unsupported:<caps name>" for other codecs, empty when unreadable.
  QString codec;
  int64_t durationNs = 0;
  int width = 0;
  int height = 0;
};

// The first video track of a media file, as the discoverer reports it.
MediaInfo probeMedia(const QString& path);

// Codec of a media file's first video track (MediaInfo::codec).
QString probeVideoCodec(const QString& path);

}
