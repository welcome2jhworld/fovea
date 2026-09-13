#pragma once
#include "fovea/Api.h"
#include "fovea/core/Store.h"
#include <QString>

namespace fovea::core {

// Probes a recorded file with GStreamer discoverer. readable=false when the
// container cannot be parsed; bytes=0 when the file does not exist.
SegmentProbe probeSegmentFile(const QString& path);

// Maps GStreamer caps names to display codec names: video/x-h264 -> H.264.
QString codecDisplayName(const QString& capsName);

// Codec of a media file's first video track: "h264", "h265",
// "unsupported:<caps name>" for other codecs, empty when unreadable.
QString probeVideoCodec(const QString& path);

}
