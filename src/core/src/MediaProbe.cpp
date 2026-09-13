#include "fovea/core/MediaProbe.h"
#include <QFileInfo>
#include <QUrl>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <optional>

namespace fovea::core {
namespace {

constexpr GstClockTime kDiscoverTimeout = 5 * GST_SECOND;
constexpr GstClockTime kParseWallLimit = 6 * GST_SECOND;

struct ParseCount {
  guint64 buffers = 0;
  GstClockTime firstPts = GST_CLOCK_TIME_NONE;
  GstClockTime lastPts = GST_CLOCK_TIME_NONE;
  GstClockTime lastDuration = GST_CLOCK_TIME_NONE;
};

GstPadProbeReturn countBuffers(GstPad*, GstPadProbeInfo* info, gpointer user) {
  auto* count = static_cast<ParseCount*>(user);
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf) return GST_PAD_PROBE_OK;
  const GstClockTime pts = GST_BUFFER_PTS(buf);
  ++count->buffers;
  if (GST_CLOCK_TIME_IS_VALID(pts)) {
    if (!GST_CLOCK_TIME_IS_VALID(count->firstPts) || pts < count->firstPts) count->firstPts = pts;
    if (!GST_CLOCK_TIME_IS_VALID(count->lastPts) || pts > count->lastPts) {
      count->lastPts = pts;
      count->lastDuration = GST_BUFFER_DURATION(buf);
    }
  }
  return GST_PAD_PROBE_OK;
}

QString demuxerFor(const QString& path) {
  const QString ext = QFileInfo(path).suffix().toLower();
  if (ext == QLatin1String("mkv") || ext == QLatin1String("webm")) return QStringLiteral("matroskademux");
  return QStringLiteral("qtdemux");
}

// Parses the container without decoding and takes the last timestamp as the
// duration; used when the file lacks a duration header (a recording that was
// cut short by a crash). Bounded in wall time so recovery cannot hang. A parse
// that neither ends nor fails within the limit has no known duration (nullopt):
// the timestamps seen so far would cut the segment short.
std::optional<int64_t> durationByParsing(const QString& path) {
  const QString launch = QStringLiteral("filesrc location=\"%1\" ! %2 name=demux ! parsebin ! fakesink name=sink sync=false")
                             .arg(QString(path).replace('"', QStringLiteral("\\\"")), demuxerFor(path));
  GError* err = nullptr;
  GstElement* pipeline = gst_parse_launch(launch.toUtf8().constData(), &err);
  if (!pipeline || err) {
    if (err) g_error_free(err);
    if (pipeline) gst_object_unref(pipeline);
    return 0;
  }
  ParseCount count;
  GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
  GstPad* pad = gst_element_get_static_pad(sink, "sink");
  gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, countBuffers, &count, nullptr);
  GstBus* bus = gst_element_get_bus(pipeline);
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  GstMessage* msg =
      gst_bus_timed_pop_filtered(bus, kParseWallLimit, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  const bool settled = msg != nullptr;
  if (msg) gst_message_unref(msg);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(bus);
  gst_object_unref(pad);
  gst_object_unref(sink);
  gst_object_unref(pipeline);
  if (!settled) return std::nullopt;
  if (count.buffers == 0 || !GST_CLOCK_TIME_IS_VALID(count.lastPts)) return 0;
  const GstClockTime first = GST_CLOCK_TIME_IS_VALID(count.firstPts) ? count.firstPts : 0;
  GstClockTime end = count.lastPts;
  if (GST_CLOCK_TIME_IS_VALID(count.lastDuration)) end += count.lastDuration;
  else if (count.buffers > 1 && count.lastPts > first) end += (count.lastPts - first) / (count.buffers - 1);
  return static_cast<int64_t>(end - first);
}

}

// Recordings are probed by parsing the container without decoding: a file
// cut by a crash has no duration header, and the demuxer's estimate for one
// counts from zero rather than from the fragment's first timestamp.
SegmentProbe probeSegmentFile(const QString& path) {
  SegmentProbe probe;
  const QFileInfo fi(path);
  if (!fi.exists() || !fi.isFile()) return probe;
  probe.bytes = fi.size();
  if (probe.bytes == 0) return probe;
  probe.durationNs = durationByParsing(path).value_or(0);
  probe.readable = probe.durationNs > 0;
  return probe;
}

QString probeVideoCodec(const QString& path) {
  GError* err = nullptr;
  GstDiscoverer* discoverer = gst_discoverer_new(kDiscoverTimeout, &err);
  if (!discoverer) {
    if (err) g_error_free(err);
    return {};
  }
  const QByteArray uri = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toEncoded();
  GstDiscovererInfo* info = gst_discoverer_discover_uri(discoverer, uri.constData(), &err);
  if (err) g_error_free(err);
  QString codec;
  if (info) {
    GList* streams = gst_discoverer_info_get_video_streams(info);
    if (streams) {
      GstCaps* caps = gst_discoverer_stream_info_get_caps(GST_DISCOVERER_STREAM_INFO(streams->data));
      if (caps && gst_caps_get_size(caps) > 0) {
        const QString name = QString::fromLatin1(gst_structure_get_name(gst_caps_get_structure(caps, 0)));
        if (name == QLatin1String("video/x-h264")) codec = QStringLiteral("h264");
        else if (name == QLatin1String("video/x-h265")) codec = QStringLiteral("h265");
        else codec = QStringLiteral("unsupported:") + name;
      }
      if (caps) gst_caps_unref(caps);
    }
    gst_discoverer_stream_info_list_free(streams);
    gst_discoverer_info_unref(info);
  }
  g_object_unref(discoverer);
  return codec;
}

QString codecDisplayName(const QString& capsName) {
  const QString name = capsName.trimmed();
  const QString lower = name.toLower();
  if (lower == QLatin1String("video/x-h264") || lower == QLatin1String("h264")) return QStringLiteral("H.264");
  if (lower == QLatin1String("video/x-h265") || lower == QLatin1String("h265")) return QStringLiteral("H.265");
  if (lower == QLatin1String("image/jpeg") || lower == QLatin1String("jpeg")) return QStringLiteral("MJPEG");
  if (lower == QLatin1String("video/mpeg") || lower == QLatin1String("mp4v-es")) return QStringLiteral("MPEG-4");
  if (lower == QLatin1String("video/x-vp8") || lower == QLatin1String("vp8")) return QStringLiteral("VP8");
  if (lower == QLatin1String("video/x-vp9") || lower == QLatin1String("vp9")) return QStringLiteral("VP9");
  if (lower == QLatin1String("video/x-av1") || lower == QLatin1String("av1")) return QStringLiteral("AV1");
  return name;
}

}
