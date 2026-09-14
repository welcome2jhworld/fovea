#include "fovea/core/SegmentSampler.h"
#include "fovea/Clock.h"
#include "fovea/core/Index.h"
#include "fovea/core/MediaProbe.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <algorithm>
#include <cmath>

namespace fovea::core {
namespace {

constexpr GstClockTime kPullSlice = 200 * GST_MSECOND;
constexpr int64_t kStallNs = 10'000'000'000;
constexpr GstClockTime kEncodeTimeout = 5 * GST_SECOND;

std::pair<int, int> scaledSize(int w, int h, int maxWidth) {
  if (w <= 0 || h <= 0) return {0, 0};
  if (w <= maxWidth) return {std::max(2, w & ~1), std::max(2, h & ~1)};
  const int sh = static_cast<int>(std::lround(static_cast<double>(h) * maxWidth / w));
  return {maxWidth, std::max(2, sh & ~1)};
}

QString writeJpeg(GstSample* frame, int maxWidth, const QString& path) {
  const GstCaps* caps = gst_sample_get_caps(frame);
  GstVideoInfo info;
  if (!caps || !gst_video_info_from_caps(&info, caps)) return QStringLiteral("frame without video caps");
  const auto [w, h] = scaledSize(GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info), maxWidth);
  GstCaps* target = gst_caps_new_simple("image/jpeg", "width", G_TYPE_INT, w, "height", G_TYPE_INT, h, "pixel-aspect-ratio",
                                        GST_TYPE_FRACTION, 1, 1, nullptr);
  GError* error = nullptr;
  GstSample* jpeg = gst_video_convert_sample(frame, target, kEncodeTimeout, &error);
  gst_caps_unref(target);
  if (!jpeg) {
    const QString message = error ? QString::fromUtf8(error->message) : QStringLiteral("no output");
    if (error) g_error_free(error);
    return QStringLiteral("jpeg encode failed: ") + message;
  }
  QString result;
  GstBuffer* buffer = gst_sample_get_buffer(jpeg);
  GstMapInfo map;
  if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    result = QStringLiteral("cannot map the jpeg buffer");
  } else {
    QFile f(path);
    const qint64 bytes = static_cast<qint64>(map.size);
    if (!f.open(QIODevice::WriteOnly) || f.write(reinterpret_cast<const char*>(map.data), bytes) != bytes)
      result = QStringLiteral("cannot write %1").arg(QFileInfo(path).fileName());
    gst_buffer_unmap(buffer, &map);
  }
  gst_sample_unref(jpeg);
  return result;
}

QString busError(GstElement* pipeline) {
  GstBus* bus = gst_element_get_bus(pipeline);
  GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
  QString text;
  if (msg) {
    GError* err = nullptr;
    gst_message_parse_error(msg, &err, nullptr);
    text = QString::fromUtf8(err ? err->message : "decode error");
    if (err) g_error_free(err);
    gst_message_unref(msg);
  }
  gst_object_unref(bus);
  return text;
}

struct Sampler {
  const SampleRequest& request;
  SampleResult& result;
  int64_t nextGrid = 0;
  int64_t firstPts = -1;
  GstSample* previous = nullptr;
  int64_t previousUtc = 0;
  int64_t previousPts = 0;
  bool previousTaken = false;

  ~Sampler() {
    if (previous) gst_sample_unref(previous);
  }

  bool take(GstSample* frame, int64_t utcMs, int64_t ptsNs) {
    SampledFrame s;
    s.index = static_cast<int>(result.frames.size());
    s.utcMs = utcMs;
    s.ptsNs = ptsNs;
    s.jpegPath = QStringLiteral("%1/%2.jpg").arg(request.spoolDir).arg(s.index);
    s.thumbnailPath = QStringLiteral("%1/%2-%3.jpg").arg(request.thumbnailDir, request.thumbnailPrefix).arg(s.index);
    if (QString e = writeJpeg(frame, kSampleJpegMaxWidth, s.jpegPath); !e.isEmpty()) {
      result.error = e;
      return false;
    }
    if (QString e = writeJpeg(frame, kThumbnailMaxWidth, s.thumbnailPath); !e.isEmpty()) {
      result.error = e;
      return false;
    }
    result.frames.push_back(s);
    return true;
  }

  void keep(GstSample* frame, int64_t utcMs, int64_t ptsNs, bool taken) {
    if (previous) gst_sample_unref(previous);
    previous = gst_sample_ref(frame);
    previousUtc = utcMs;
    previousPts = ptsNs;
    previousTaken = taken;
  }

  // Grid points before a new frame's time were reached while the previous
  // frame was on screen (or, before the first frame, are given the first one).
  bool onFrame(GstSample* frame, int64_t bufferPts) {
    if (firstPts < 0) firstPts = bufferPts;
    const int64_t offsetNs = bufferPts - firstPts;
    const int64_t utcMs = request.startUtcMs + offsetNs / 1'000'000;
    const int64_t ptsNs = request.startPtsNs + offsetNs;
    bool frameTaken = false;
    while (nextGrid < request.endUtcMs && nextGrid < utcMs) {
      if (previous && !previousTaken) {
        if (!take(previous, previousUtc, previousPts)) return false;
        previousTaken = true;
      } else if (!previous && !frameTaken) {
        if (!take(frame, utcMs, ptsNs)) return false;
        frameTaken = true;
      }
      nextGrid += request.intervalMs;
    }
    keep(frame, utcMs, ptsNs, frameTaken);
    return true;
  }

  bool onEnd() {
    if (previous && !previousTaken && nextGrid < request.endUtcMs && nextGrid >= previousUtc) {
      if (!take(previous, previousUtc, previousPts)) return false;
      previousTaken = true;
    }
    return true;
  }
};

}

SampleResult sampleSegment(const SampleRequest& request, const std::atomic<bool>* cancel) {
  SampleResult result;
  const int64_t started = monoNowNs();
  const auto finish = [&result, started]() -> SampleResult {
    result.elapsedMs = (monoNowNs() - started) / 1'000'000;
    return result;
  };
  if (request.intervalMs <= 0 || request.endUtcMs <= request.startUtcMs || request.startUtcMs <= 0) {
    result.error = QStringLiteral("segment has no time range");
    return finish();
  }
  result.expected = expectedSamples(request.startUtcMs, request.endUtcMs, request.intervalMs);
  if (!QDir().mkpath(request.spoolDir) || !QDir().mkpath(request.thumbnailDir)) {
    result.error = QStringLiteral("cannot create the sample directories");
    return finish();
  }
  const QString codec = probeVideoCodec(request.segmentPath);
  if (codec != QLatin1String("h264") && codec != QLatin1String("h265")) {
    result.error = codec.isEmpty() ? QStringLiteral("recording is not readable") : QStringLiteral("unsupported codec %1").arg(codec);
    return finish();
  }
  const bool h264 = codec == QLatin1String("h264");
  const QString demux = QFileInfo(request.segmentPath).suffix().compare(QLatin1String("mkv"), Qt::CaseInsensitive) == 0
                            ? QStringLiteral("matroskademux")
                            : QStringLiteral("qtdemux");
  const QString launch = QStringLiteral("filesrc name=src ! %1 ! %2 ! %3 ! appsink name=sink sync=false max-buffers=4 drop=false")
                             .arg(demux, h264 ? QStringLiteral("h264parse") : QStringLiteral("h265parse"),
                                  h264 ? QStringLiteral("avdec_h264") : QStringLiteral("avdec_h265"));
  GError* err = nullptr;
  GstElement* pipeline = gst_parse_launch(launch.toUtf8().constData(), &err);
  if (!pipeline || err) {
    result.error = QStringLiteral("cannot build the decode pipeline: %1").arg(err ? QString::fromUtf8(err->message) : QString());
    if (err) g_error_free(err);
    if (pipeline) gst_object_unref(pipeline);
    return finish();
  }
  GstElement* source = gst_bin_get_by_name(GST_BIN(pipeline), "src");
  g_object_set(source, "location", request.segmentPath.toUtf8().constData(), nullptr);
  gst_object_unref(source);
  GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
  Sampler sampler{request, result};
  sampler.nextGrid = (request.startUtcMs + request.intervalMs - 1) / request.intervalMs * request.intervalMs;
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    result.error = busError(pipeline);
    if (result.error.isEmpty()) result.error = QStringLiteral("decode pipeline refused to start");
  }
  int64_t lastFrameNs = monoNowNs();
  while (result.error.isEmpty()) {
    if (cancel && cancel->load()) {
      result.error = QStringLiteral("cancelled");
      break;
    }
    GstSample* frame = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), kPullSlice);
    if (!frame) {
      if (gst_app_sink_is_eos(GST_APP_SINK(sink))) {
        sampler.onEnd();
        break;
      }
      result.error = busError(pipeline);
      if (result.error.isEmpty() && monoNowNs() - lastFrameNs > kStallNs) result.error = QStringLiteral("decoder stalled");
      continue;
    }
    lastFrameNs = monoNowNs();
    const GstBuffer* buffer = gst_sample_get_buffer(frame);
    if (buffer && GST_BUFFER_PTS_IS_VALID(buffer)) {
      ++result.decodedFrames;
      sampler.onFrame(frame, static_cast<int64_t>(GST_BUFFER_PTS(buffer)));
    }
    gst_sample_unref(frame);
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(sink);
  gst_object_unref(pipeline);
  if (result.error.isEmpty() && result.decodedFrames == 0) result.error = QStringLiteral("no frames decoded");
  return finish();
}

}
