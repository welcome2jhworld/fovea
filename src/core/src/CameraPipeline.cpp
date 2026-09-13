#include "fovea/core/CameraPipeline.h"
#include "fovea/Clock.h"
#include "fovea/FrameRing.h"
#include "fovea/Ids.h"
#include "fovea/Redact.h"
#include "fovea/core/MediaProbe.h"
#include "fovea/core/SessionClock.h"
#include "fovea/core/Store.h"
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QStorageInfo>
#include <QUrl>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <QMetaObject>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace fovea::core {
namespace {

constexpr int kTickMs = 250;
constexpr int64_t kStopWaitMs = 3000;
constexpr int64_t kHealthyResetNs = 30LL * 1000 * 1000 * 1000;
constexpr int64_t kFpsWindowNs = 1'000'000'000;
constexpr int64_t kLatencyWindowNs = 2'000'000'000;
constexpr int64_t kBitrateWindowNs = 2'000'000'000;
constexpr size_t kPtsMapSize = 64;
constexpr int64_t kConnectGraceMs = 2000;

struct PtsMono {
  GstClockTime pts = GST_CLOCK_TIME_NONE;
  int64_t mono = 0;
};

// Bus messages are queued here by the sync handler (streaming threads) and
// drained on the owner's thread, so no message is lost while stopping.
struct MessageQueue {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<GstMessage*> queue;

  ~MessageQueue() {
    for (GstMessage* m : queue) gst_message_unref(m);
  }
  void push(GstMessage* msg) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      queue.push_back(gst_message_ref(msg));
    }
    cv.notify_all();
  }
  std::deque<GstMessage*> take() {
    std::lock_guard<std::mutex> lock(mutex);
    std::deque<GstMessage*> out;
    out.swap(queue);
    return out;
  }
  void waitFor(int64_t ms) {
    std::unique_lock<std::mutex> lock(mutex);
    if (queue.empty()) cv.wait_for(lock, std::chrono::milliseconds(ms));
  }
};

struct RunStats {
  SessionClock clock;
  uint64_t packets = 0;
  uint64_t bytes = 0;
  int64_t firstPacketMono = 0;
  int64_t lastPacketMono = 0;
  int64_t lastPacketUtc = 0;
  int64_t lastPacketRt = 0;
  uint64_t resumeCount = 0;
  int64_t resumeGapStartUtc = 0;
  int64_t resumeUtc = 0;
  bool ptsBackwards = false;
  QString codec;
  int width = 0;
  int height = 0;
  double fps = 0;
  uint64_t frames = 0;
  uint64_t ringWriteErrors = 0;
  int64_t lastFrameRecvMono = 0;
  int64_t lastFrameRt = -1;
  int64_t framePeriodNs = 0;
  int64_t estimatedPeriodNs = 0;
  int64_t drops = 0;
  std::deque<int64_t> frameTimes;
  std::deque<std::pair<int64_t, int64_t>> latencies;
  std::array<PtsMono, kPtsMapSize> ptsMap{};
  size_t ptsMapNext = 0;
  GstElement* jitterbuffer = nullptr;
};

GstElement* makeElement(const char* factory, const char* name = nullptr) {
  GstElement* e = gst_element_factory_make(factory, name);
  if (!e) qWarning("missing GStreamer element %s", factory);
  return e;
}

void postError(GstElement* pipeline, const QString& text) {
  GError* err = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_CODEC_NOT_FOUND, text.toUtf8().constData());
  gst_element_post_message(pipeline, gst_message_new_error(GST_OBJECT(pipeline), err, nullptr));
  g_error_free(err);
}

QString codecFromCaps(GstCaps* caps) {
  if (!caps || gst_caps_get_size(caps) == 0) return {};
  const GstStructure* s = gst_caps_get_structure(caps, 0);
  const QString name = QString::fromLatin1(gst_structure_get_name(s));
  // rtspsrc names the SDP caps application/x-unknown in select-stream and
  // application/x-rtp on its pads; both carry media and encoding-name.
  if (gst_structure_has_field(s, "media")) {
    const gchar* media = gst_structure_get_string(s, "media");
    const gchar* enc = gst_structure_get_string(s, "encoding-name");
    if (!media || QString::fromLatin1(media) != QLatin1String("video") || !enc) return {};
    const QString e = QString::fromLatin1(enc).toUpper();
    if (e == QLatin1String("H264")) return QStringLiteral("h264");
    if (e == QLatin1String("H265")) return QStringLiteral("h265");
    return QStringLiteral("unsupported:") + e;
  }
  if (name == QLatin1String("video/x-h264")) return QStringLiteral("h264");
  if (name == QLatin1String("video/x-h265")) return QStringLiteral("h265");
  if (name.startsWith(QLatin1String("video/")) || name.startsWith(QLatin1String("image/")))
    return QStringLiteral("unsupported:") + name;
  return {};
}

bool isVideoRtp(GstCaps* caps) {
  if (!caps || gst_caps_get_size(caps) == 0) return false;
  const gchar* media = gst_structure_get_string(gst_caps_get_structure(caps, 0), "media");
  return media && QString::fromLatin1(media) == QLatin1String("video");
}

struct ParsedCaps {
  QString codec;
  int width = 0;
  int height = 0;
  double fps = 0;
  int displayWidth = 0;
  int displayHeight = 0;
};

ParsedCaps parseVideoCaps(GstCaps* caps) {
  ParsedCaps out;
  if (!caps || gst_caps_get_size(caps) == 0) return out;
  const GstStructure* s = gst_caps_get_structure(caps, 0);
  out.codec = codecDisplayName(QString::fromLatin1(gst_structure_get_name(s)));
  gst_structure_get_int(s, "width", &out.width);
  gst_structure_get_int(s, "height", &out.height);
  gint fn = 0, fd = 1;
  if (gst_structure_get_fraction(s, "framerate", &fn, &fd) && fn > 0 && fd > 0) out.fps = static_cast<double>(fn) / fd;
  gint pn = 1, pd = 1;
  gst_structure_get_fraction(s, "pixel-aspect-ratio", &pn, &pd);
  out.displayWidth = out.width;
  out.displayHeight = out.height;
  if (pn > 0 && pd > 0 && pn != pd && out.width > 0)
    out.displayWidth = static_cast<int>(static_cast<int64_t>(out.width) * pn / pd);
  return out;
}

// Largest even size that fits the ring slot while keeping the display aspect.
std::pair<int, int> fitSize(int w, int h, uint32_t maxW, uint32_t maxH) {
  if (w <= 0 || h <= 0) return {0, 0};
  double scale = 1.0;
  scale = std::min(scale, static_cast<double>(maxW) / w);
  scale = std::min(scale, static_cast<double>(maxH) / h);
  int tw = static_cast<int>(w * scale + 0.5) & ~1;
  int th = static_cast<int>(h * scale + 0.5) & ~1;
  tw = std::max(2, std::min(tw, static_cast<int>(maxW)));
  th = std::max(2, std::min(th, static_cast<int>(maxH)));
  return {tw, th};
}

GstCaps* bgraCaps(int w, int h) {
  if (w > 0 && h > 0)
    return gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRA", "width", G_TYPE_INT, w, "height",
                               G_TYPE_INT, h, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, nullptr);
  return gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRA", nullptr);
}

QString localPathFor(const QString& url) {
  if (url.startsWith(QLatin1String("file:"))) return QUrl(url).toLocalFile();
  return url;
}

QString demuxerForPath(const QString& path) {
  const QString ext = QFileInfo(path).suffix().toLower();
  if (ext == QLatin1String("mkv") || ext == QLatin1String("webm")) return QStringLiteral("matroskademux");
  return QStringLiteral("qtdemux");
}

int64_t freeBytesAt(const QString& dir) {
  QStorageInfo info(dir);
  if (!info.isValid()) info = QStorageInfo(QFileInfo(dir).absolutePath());
  return info.isValid() ? info.bytesAvailable() : -1;
}

LatencyStats percentiles(std::vector<int64_t>& samples) {
  LatencyStats out;
  if (samples.empty()) return out;
  std::sort(samples.begin(), samples.end());
  auto at = [&](double q) {
    const size_t idx = std::min(samples.size() - 1, static_cast<size_t>(q * static_cast<double>(samples.size())));
    return static_cast<double>(samples[idx]) / 1e6;
  };
  out.p50Ms = at(0.5);
  out.p95Ms = at(0.95);
  out.p99Ms = at(0.99);
  return out;
}

void pruneFrameStats(RunStats& st, int64_t now) {
  while (!st.frameTimes.empty() && now - st.frameTimes.front() > kFpsWindowNs) st.frameTimes.pop_front();
  while (!st.latencies.empty() && now - st.latencies.front().first > kLatencyWindowNs) st.latencies.pop_front();
}

GstClockTime runningTimeFor(const GstSegment* segment, bool valid, GstClockTime pts) {
  if (!GST_CLOCK_TIME_IS_VALID(pts)) return GST_CLOCK_TIME_NONE;
  if (!valid) return pts;
  return gst_segment_to_running_time(segment, GST_FORMAT_TIME, pts);
}

}

struct CameraPipeline::Run {
  uint64_t id = 0;
  CameraPipeline* owner = nullptr;
  Camera cam;
  bool isFile = false;
  int64_t gapAfterNs = 0;
  uint32_t ringMaxWidth = 0;
  uint32_t ringMaxHeight = 0;

  GstElement* pipeline = nullptr;
  GstElement* source = nullptr;
  GstElement* demux = nullptr;
  GstElement* tee = nullptr;
  GstElement* viewQueue = nullptr;
  GstElement* capsFilter = nullptr;
  GstElement* appsink = nullptr;
  GstElement* splitmux = nullptr;
  GstElement* valve = nullptr;
  GstElement* head = nullptr;
  std::vector<GstElement*> chainElements;
  GstPad* teeSink = nullptr;
  GstBus* bus = nullptr;

  GstSegment segment{};
  bool segmentValid = false;
  GstVideoInfo videoInfo{};
  GstCaps* sinkCaps = nullptr;

  std::atomic<bool> chainClaimed{false};
  std::atomic<bool> chainReady{false};
  std::atomic<bool> chainSynced{false};
  std::atomic<bool> stopping{false};
  std::atomic<bool> eosSeen{false};
  std::atomic<bool> diskPaused{false};
  std::atomic<bool> codecErrorPosted{false};

  MessageQueue messages;
  mutable std::mutex mutex;
  RunStats stats;

  QString sessionId;
  std::array<uint8_t, 16> sessionBytes{};
  int64_t startedUtcMs = 0;
  int64_t startedMonoNs = 0;
  std::unique_ptr<FrameRingWriter> ring;

  QString recordDir;
  QString recordingState = QStringLiteral("disabled");
  QHash<uint, QString> openSegments;
  QHash<QString, QString> openSegmentPaths;
  QString currentSegmentId;
  bool online = false;
  int64_t onlineSinceMono = 0;
  uint64_t seenResume = 0;
  std::deque<std::pair<int64_t, uint64_t>> bitrateSamples;

  ~Run() {
    if (sinkCaps) gst_caps_unref(sinkCaps);
    if (stats.jitterbuffer) gst_object_unref(stats.jitterbuffer);
    if (teeSink) gst_object_unref(teeSink);
    if (bus) {
      gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
      gst_object_unref(bus);
    }
    if (pipeline) gst_object_unref(pipeline);
  }

  void invoke(void (CameraPipeline::*method)(uint64_t)) {
    CameraPipeline* target = owner;
    const uint64_t runId = id;
    QMetaObject::invokeMethod(target, [target, method, runId] { (target->*method)(runId); }, Qt::QueuedConnection);
  }

  static GstBusSyncReply onBusSync(GstBus*, GstMessage* msg, gpointer user) {
    auto* run = static_cast<Run*>(user);
    switch (GST_MESSAGE_TYPE(msg)) {
      case GST_MESSAGE_EOS:
        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(run->pipeline)) run->eosSeen = true;
        break;
      case GST_MESSAGE_ERROR:
      case GST_MESSAGE_ELEMENT:
      case GST_MESSAGE_SEGMENT_DONE:
        break;
      default:
        return GST_BUS_DROP;
    }
    run->messages.push(msg);
    run->invoke(&CameraPipeline::drainMessages);
    return GST_BUS_DROP;
  }

  static gchar* onFormatLocation(GstElement*, guint fragmentId, gpointer user) {
    auto* run = static_cast<Run*>(user);
    const QString path = QStringLiteral("%1/%2_%3.mkv").arg(run->recordDir, run->sessionId,
                                                            QString::number(fragmentId).rightJustified(5, QLatin1Char('0')));
    return g_strdup(path.toUtf8().constData());
  }

  static gboolean onSelectStream(GstElement*, guint, GstCaps* caps, gpointer user) {
    auto* run = static_cast<Run*>(user);
    const QString codec = codecFromCaps(caps);
    if (codec == QLatin1String("h264") || codec == QLatin1String("h265")) return TRUE;
    if (isVideoRtp(caps) && !run->codecErrorPosted.exchange(true))
      postError(run->pipeline, QStringLiteral("unsupported codec %1").arg(codec.mid(codec.indexOf(':') + 1)));
    return FALSE;
  }

  static void onNewJitterbuffer(GstElement*, GstElement* jb, guint, guint, gpointer user) {
    auto* run = static_cast<Run*>(user);
    std::lock_guard<std::mutex> lock(run->mutex);
    if (!run->stats.jitterbuffer) run->stats.jitterbuffer = GST_ELEMENT(gst_object_ref(jb));
  }

  static void onNewManager(GstElement*, GstElement* manager, gpointer user) {
    g_signal_connect(manager, "new-jitterbuffer", G_CALLBACK(onNewJitterbuffer), user);
  }

  static void onPadAdded(GstElement*, GstPad* pad, gpointer user) {
    auto* run = static_cast<Run*>(user);
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    const QString codec = codecFromCaps(caps);
    if (caps) gst_caps_unref(caps);
    if (codec == QLatin1String("h264") || codec == QLatin1String("h265")) {
      if (!run->chainClaimed.exchange(true) && !run->createChain(codec)) return;
      run->attachSource(pad);
    } else if (codec.startsWith(QLatin1String("unsupported:")) && !run->codecErrorPosted.exchange(true)) {
      postError(run->pipeline, QStringLiteral("unsupported codec %1").arg(codec.mid(codec.indexOf(':') + 1)));
    }
  }

  static void onNoMorePads(GstElement*, gpointer user) { static_cast<Run*>(user)->invoke(&CameraPipeline::onNoMorePads); }

  // Creates parse -> tee -> {record, view} for the codec. File sources call
  // this before the first state change so the paced chain takes part in the
  // preroll (adding it later deadlocks: the bin waits for the new sinks to
  // preroll while clocksync holds the first buffer in PAUSED). RTSP sources
  // call it from pad-added once the SDP names the codec.
  bool createChain(const QString& codec) {
    const bool h264 = codec == QLatin1String("h264");
    const bool rtp = !isFile;
    GstElement* depay = rtp ? makeElement(h264 ? "rtph264depay" : "rtph265depay") : nullptr;
    GstElement* parse = makeElement(h264 ? "h264parse" : "h265parse");
    GstElement* pacer = isFile ? makeElement("clocksync") : nullptr;
    tee = makeElement("tee", "t");
    viewQueue = makeElement("queue", "viewq");
    GstElement* dec = makeElement(h264 ? "avdec_h264" : "avdec_h265");
    GstElement* conv = makeElement("videoconvert");
    GstElement* scale = makeElement("videoscale");
    capsFilter = makeElement("capsfilter");
    appsink = makeElement("appsink", "ringsink");
    GstElement* recQueue = nullptr;
    if (cam.recordEnabled) {
      recQueue = makeElement("queue", "recq");
      valve = makeElement("valve", "recvalve");
      splitmux = makeElement("splitmuxsink", "splitmux");
    }
    const bool ok = (rtp ? depay != nullptr : true) && parse && (isFile ? pacer != nullptr : true) && tee && viewQueue &&
                    dec && conv && scale && capsFilter && appsink && (!cam.recordEnabled || (recQueue && valve && splitmux));
    if (!ok) {
      for (GstElement* e : {depay, parse, pacer, tee, viewQueue, dec, conv, scale, capsFilter, appsink, recQueue, valve, splitmux})
        if (e) gst_object_unref(e);
      tee = viewQueue = capsFilter = appsink = valve = splitmux = nullptr;
      postError(pipeline, QStringLiteral("missing GStreamer elements for %1").arg(codec));
      return false;
    }

    gst_util_set_object_arg(G_OBJECT(viewQueue), "leaky", "downstream");
    g_object_set(viewQueue, "max-size-buffers", 5u, "max-size-time", static_cast<guint64>(0), "max-size-bytes", 0u, nullptr);
    GstCaps* caps = bgraCaps(0, 0);
    g_object_set(capsFilter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    g_object_set(appsink, "sync", FALSE, "max-buffers", 2u, "drop", TRUE, "emit-signals", FALSE, nullptr);
    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = onNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, this, nullptr);

    gst_bin_add_many(GST_BIN(pipeline), parse, tee, viewQueue, dec, conv, scale, capsFilter, appsink, nullptr);
    if (depay) gst_bin_add(GST_BIN(pipeline), depay);
    if (pacer) gst_bin_add(GST_BIN(pipeline), pacer);

    if (cam.recordEnabled) {
      GstElement* muxer = makeElement("matroskamux");
      GstElement* filesink = makeElement("filesink");
      if (muxer && filesink) {
        gst_util_set_object_arg(G_OBJECT(filesink), "buffer-mode", "unbuffered");
        g_object_set(splitmux, "max-size-time", static_cast<guint64>(cam.segmentSeconds) * GST_SECOND, "muxer", muxer,
                     "sink", filesink, nullptr);
      }
      g_signal_connect(splitmux, "format-location", G_CALLBACK(onFormatLocation), this);
      g_object_set(valve, "drop", diskPaused.load() ? TRUE : FALSE, nullptr);
      gst_bin_add_many(GST_BIN(pipeline), recQueue, valve, splitmux, nullptr);
      gst_element_link_many(tee, recQueue, valve, nullptr);
      gst_element_link_pads(valve, "src", splitmux, "video");
    }

    head = depay ? depay : parse;
    if (depay) gst_element_link(depay, parse);
    if (pacer) gst_element_link_many(parse, pacer, tee, nullptr);
    else gst_element_link(parse, tee);
    gst_element_link_many(tee, viewQueue, dec, conv, scale, capsFilter, appsink, nullptr);

    teeSink = gst_element_get_static_pad(tee, "sink");
    gst_pad_add_probe(teeSink,
                      static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM |
                                                   GST_PAD_PROBE_TYPE_QUERY_UPSTREAM),
                      onTeeProbe, this, nullptr);
    chainElements = {appsink, capsFilter, scale, conv, dec, viewQueue, splitmux, valve, recQueue, tee, pacer, parse, depay};
    chainReady.store(true, std::memory_order_release);
    return true;
  }

  // Links the source pad to the chain. Runs on the source's streaming
  // thread; a chain created there is synced to the pipeline state sink-first
  // so data never hits an element that is not ready.
  void attachSource(GstPad* srcPad) {
    if (!chainReady.load(std::memory_order_acquire) || !head) return;
    if (!chainSynced.exchange(true) && !isFile)
      for (GstElement* e : chainElements)
        if (e) gst_element_sync_state_with_parent(e);
    GstPad* sink = gst_element_get_static_pad(head, "sink");
    const GstPadLinkReturn link = gst_pad_link(srcPad, sink);
    gst_object_unref(sink);
    if (link != GST_PAD_LINK_OK) postError(pipeline, QStringLiteral("cannot link source pad (%1)").arg(static_cast<int>(link)));
  }

  void applyCaps(GstCaps* caps) {
    const ParsedCaps parsed = parseVideoCaps(caps);
    if (parsed.width <= 0 || parsed.height <= 0) return;
    const auto [tw, th] = fitSize(parsed.displayWidth, parsed.displayHeight, ringMaxWidth, ringMaxHeight);
    GstCaps* target = bgraCaps(tw, th);
    g_object_set(capsFilter, "caps", target, nullptr);
    gst_caps_unref(target);
    {
      std::lock_guard<std::mutex> lock(mutex);
      stats.codec = parsed.codec;
      stats.width = parsed.width;
      stats.height = parsed.height;
      stats.fps = parsed.fps;
      stats.framePeriodNs = parsed.fps > 0 ? static_cast<int64_t>(1e9 / parsed.fps) : 0;
    }
    invoke(&CameraPipeline::onCapsChanged);
  }

  static GstPadProbeReturn onTeeProbe(GstPad*, GstPadProbeInfo* info, gpointer user) {
    auto* run = static_cast<Run*>(user);
    if (info->type & GST_PAD_PROBE_TYPE_QUERY_UPSTREAM) {
      // A file source would answer the muxer's duration query with the clip
      // length, which matroskamux then writes into every fragment header
      // before finalization; a recording cut by a crash must not carry it.
      return GST_QUERY_TYPE(GST_PAD_PROBE_INFO_QUERY(info)) == GST_QUERY_DURATION ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
    }
    if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
      GstEvent* ev = GST_PAD_PROBE_INFO_EVENT(info);
      if (GST_EVENT_TYPE(ev) == GST_EVENT_SEGMENT) {
        const GstSegment* seg = nullptr;
        gst_event_parse_segment(ev, &seg);
        if (seg && seg->format == GST_FORMAT_TIME) {
          gst_segment_copy_into(seg, &run->segment);
          run->segmentValid = true;
        }
      } else if (GST_EVENT_TYPE(ev) == GST_EVENT_CAPS) {
        GstCaps* caps = nullptr;
        gst_event_parse_caps(ev, &caps);
        run->applyCaps(caps);
      }
      return GST_PAD_PROBE_OK;
    }
    GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;
    const int64_t now = monoNowNs();
    GstClockTime pts = GST_BUFFER_PTS(buf);
    if (!GST_CLOCK_TIME_IS_VALID(pts)) pts = GST_BUFFER_DTS(buf);
    const GstClockTime rtRaw = runningTimeFor(&run->segment, run->segmentValid, pts);
    const int64_t rt = GST_CLOCK_TIME_IS_VALID(rtRaw) ? static_cast<int64_t>(rtRaw) : 0;
    bool first = false;
    bool backwards = false;
    {
      std::lock_guard<std::mutex> lock(run->mutex);
      RunStats& st = run->stats;
      ++st.packets;
      st.bytes += gst_buffer_get_size(buf);
      st.ptsMap[st.ptsMapNext] = {pts, now};
      st.ptsMapNext = (st.ptsMapNext + 1) % kPtsMapSize;
      const int64_t utc = utcNowMs();
      if (st.packets == 1) {
        first = true;
        st.clock.start(rt, utc);
        st.firstPacketMono = now;
      } else {
        if (!st.clock.observe(rt) && !st.ptsBackwards) {
          st.ptsBackwards = true;
          backwards = true;
        }
        if (now - st.lastPacketMono > run->gapAfterNs) {
          ++st.resumeCount;
          st.resumeGapStartUtc = st.lastPacketUtc;
          st.resumeUtc = utc;
        }
      }
      st.lastPacketMono = now;
      st.lastPacketUtc = utc;
      st.lastPacketRt = rt;
    }
    if (first) run->invoke(&CameraPipeline::onFirstPacket);
    if (backwards) run->invoke(&CameraPipeline::onPtsBackwards);
    return GST_PAD_PROBE_OK;
  }

  static GstFlowReturn onNewSample(GstAppSink* sink, gpointer user) {
    auto* run = static_cast<Run*>(user);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstCaps* caps = gst_sample_get_caps(sample);
    if (!buf || !caps) {
      gst_sample_unref(sample);
      return GST_FLOW_OK;
    }
    if (caps != run->sinkCaps) {
      if (!gst_video_info_from_caps(&run->videoInfo, caps)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
      }
      if (run->sinkCaps) gst_caps_unref(run->sinkCaps);
      run->sinkCaps = gst_caps_ref(caps);
    }
    GstVideoFrame frame;
    if (!gst_video_frame_map(&frame, &run->videoInfo, buf, GST_MAP_READ)) {
      gst_sample_unref(sample);
      return GST_FLOW_OK;
    }
    const GstClockTime pts = GST_BUFFER_PTS(buf);
    const GstSegment* seg = gst_sample_get_segment(sample);
    const GstClockTime rtRaw = runningTimeFor(seg, seg && seg->format == GST_FORMAT_TIME, pts);
    const int64_t rt = GST_CLOCK_TIME_IS_VALID(rtRaw) ? static_cast<int64_t>(rtRaw) : 0;
    const int64_t now = monoNowNs();

    int64_t recvMono = now;
    {
      std::lock_guard<std::mutex> lock(run->mutex);
      for (const PtsMono& e : run->stats.ptsMap) {
        if (e.pts == pts && GST_CLOCK_TIME_IS_VALID(pts)) {
          recvMono = e.mono;
          break;
        }
      }
    }

    FrameHeader header;
    header.ptsNs = static_cast<uint64_t>(rt);
    header.recvMonoNs = static_cast<uint64_t>(recvMono);
    header.captureUtcMs = 0;
    header.width = static_cast<uint32_t>(GST_VIDEO_FRAME_WIDTH(&frame));
    header.height = static_cast<uint32_t>(GST_VIDEO_FRAME_HEIGHT(&frame));
    header.sessionId = run->sessionBytes;
    const auto* pixels = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    const size_t stride = static_cast<size_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0));
    const bool written = run->ring && run->ring->write(header, pixels, stride);
    gst_video_frame_unmap(&frame);
    const int64_t writeMono = monoNowNs();

    {
      std::lock_guard<std::mutex> lock(run->mutex);
      RunStats& st = run->stats;
      ++st.frames;
      if (!written) ++st.ringWriteErrors;
      st.lastFrameRecvMono = recvMono;
      st.frameTimes.push_back(writeMono);
      st.latencies.emplace_back(writeMono, writeMono - recvMono);
      if (st.lastFrameRt >= 0 && rt > st.lastFrameRt) {
        const int64_t delta = rt - st.lastFrameRt;
        if (st.framePeriodNs == 0)
          st.estimatedPeriodNs = st.estimatedPeriodNs == 0 ? delta : (st.estimatedPeriodNs * 7 + delta) / 8;
        const int64_t period = st.framePeriodNs > 0 ? st.framePeriodNs : st.estimatedPeriodNs;
        if (period > 0 && delta > period * 3 / 2) st.drops += delta / period - 1;
      }
      st.lastFrameRt = rt;
      pruneFrameStats(st, writeMono);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  bool seekSegment(bool flush) {
    GstElement* target = demux ? demux : pipeline;
    const auto flags = static_cast<GstSeekFlags>(GST_SEEK_FLAG_SEGMENT | (flush ? GST_SEEK_FLAG_FLUSH : 0));
    return gst_element_seek(target, 1.0, GST_FORMAT_TIME, flags, GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_NONE,
                            GST_CLOCK_TIME_NONE);
  }
};

CameraPipeline::CameraPipeline(Camera camera, std::optional<Credentials> credentials, Store& store, const CoreConfig& config,
                               QObject* parent)
    : QObject(parent), camera_(std::move(camera)), credentials_(std::move(credentials)), store_(store), config_(config),
      backoff_(1000, 30000, 2.0) {
  tickTimer_.setInterval(kTickMs);
  connect(&tickTimer_, &QTimer::timeout, this, &CameraPipeline::tick);
  reconnectTimer_.setSingleShot(true);
  connect(&reconnectTimer_, &QTimer::timeout, this, [this] {
    if (!wantRunning_ || run_) return;
    ++reconnects_;
    startRun();
  });
}

CameraPipeline::~CameraPipeline() {
  if (run_) endRun(QStringLiteral("stopped"));
}

void CameraPipeline::start() {
  wantRunning_ = true;
  backoff_.reset();
  state_ = QStringLiteral("connecting");
  sinceUtcMs_ = utcNowMs();
  if (!run_) startRun();
  emit statusChanged(camera_.id);
}

void CameraPipeline::stop(const QString& reason) {
  wantRunning_ = false;
  reconnectTimer_.stop();
  if (run_) endRun(reason);
  state_ = QStringLiteral("offline");
  sinceUtcMs_ = utcNowMs();
  emit statusChanged(camera_.id);
}

void CameraPipeline::reconfigure(Camera camera, std::optional<Credentials> credentials) {
  camera_ = std::move(camera);
  credentials_ = std::move(credentials);
  if (!wantRunning_) return;
  reconnectTimer_.stop();
  if (run_) endRun(QStringLiteral("stopped"));
  backoff_.reset();
  state_ = QStringLiteral("connecting");
  sinceUtcMs_ = utcNowMs();
  startRun();
  emit statusChanged(camera_.id);
}

void CameraPipeline::setCamera(const Camera& camera) { camera_ = camera; }

void CameraPipeline::startRun() {
  auto run = std::make_unique<Run>();
  run->id = nextRunId_++;
  run->owner = this;
  run->cam = camera_;
  run->isFile = camera_.kind == QLatin1String("file");
  run->gapAfterNs = static_cast<int64_t>(config_.gapAfterMs) * 1'000'000;
  run->ringMaxWidth = config_.ringMaxWidth;
  run->ringMaxHeight = config_.ringMaxHeight;
  run->sessionId = newId();
  run->sessionBytes = sessionIdBytes(run->sessionId);
  run->startedUtcMs = utcNowMs();
  run->startedMonoNs = monoNowNs();
  run->recordDir = config_.recordingsDir + QLatin1Char('/') + camera_.id;

  StreamSession session;
  session.id = run->sessionId;
  session.cameraId = camera_.id;
  session.startedMonoNs = run->startedMonoNs;
  session.startedUtcMs = run->startedUtcMs;
  session.transport = run->isFile ? QStringLiteral("file") : camera_.transport;
  if (!store_.insertSession(session)) qWarning("camera %s: cannot insert session: %s", qPrintable(camera_.id), qPrintable(store_.lastError()));

  run->ring = FrameRingWriter::create(makeRingName(QStringLiteral("cam:") + camera_.id), config_.ringSlots,
                                      config_.ringMaxWidth, config_.ringMaxHeight);
  if (!run->ring) qWarning("camera %s: cannot create frame ring", qPrintable(camera_.id));

  if (camera_.recordEnabled) {
    if (!QDir().mkpath(run->recordDir)) {
      qWarning("camera %s: cannot create %s", qPrintable(camera_.id), qPrintable(run->recordDir));
      run->recordingState = QStringLiteral("error");
    } else {
      run->recordingState = QStringLiteral("recording");
    }
  }

  run->pipeline = gst_pipeline_new(nullptr);
  if (run->isFile) {
    const QString path = localPathFor(camera_.mainUrl);
    run->source = makeElement("filesrc", "src");
    run->demux = makeElement(demuxerForPath(path).toUtf8().constData(), "demux");
    if (!run->source || !run->demux) {
      if (run->source) gst_object_unref(run->source);
      if (run->demux) gst_object_unref(run->demux);
      run->source = run->demux = nullptr;
      run_ = std::move(run);
      failRun(QStringLiteral("error"), QStringLiteral("missing file source elements"));
      return;
    }
    g_object_set(run->source, "location", path.toUtf8().constData(), nullptr);
    gst_bin_add_many(GST_BIN(run->pipeline), run->source, run->demux, nullptr);
    gst_element_link(run->source, run->demux);
    g_signal_connect(run->demux, "pad-added", G_CALLBACK(Run::onPadAdded), run.get());
    g_signal_connect(run->demux, "no-more-pads", G_CALLBACK(Run::onNoMorePads), run.get());
    const QString codec = probeVideoCodec(path);
    if (codec != QLatin1String("h264") && codec != QLatin1String("h265")) {
      run_ = std::move(run);
      failRun(QStringLiteral("error"), codec.isEmpty() ? QStringLiteral("no readable video track in file")
                                                       : QStringLiteral("unsupported codec %1").arg(codec.mid(codec.indexOf(':') + 1)));
      return;
    }
    run->chainClaimed = true;
    if (!run->createChain(codec)) {
      run_ = std::move(run);
      failRun(QStringLiteral("error"), QStringLiteral("missing GStreamer elements for %1").arg(codec));
      return;
    }
  } else {
    run->source = makeElement("rtspsrc", "src");
    if (!run->source) {
      run_ = std::move(run);
      failRun(QStringLiteral("error"), QStringLiteral("missing rtspsrc"));
      return;
    }
    const guint64 timeoutUs = static_cast<guint64>(std::max(camera_.timeoutMs, 1000)) * 1000;
    g_object_set(run->source, "location", camera_.mainUrl.toUtf8().constData(), "latency",
                 static_cast<guint>(camera_.jitterMs), "tcp-timeout", timeoutUs, "timeout", timeoutUs, nullptr);
    gst_util_set_object_arg(G_OBJECT(run->source), "protocols", camera_.transport == QLatin1String("udp") ? "udp" : "tcp");
    if (credentials_ && !credentials_->username.isEmpty())
      g_object_set(run->source, "user-id", credentials_->username.toUtf8().constData(), "user-pw",
                   credentials_->password.toUtf8().constData(), nullptr);
    gst_bin_add(GST_BIN(run->pipeline), run->source);
    g_signal_connect(run->source, "pad-added", G_CALLBACK(Run::onPadAdded), run.get());
    g_signal_connect(run->source, "select-stream", G_CALLBACK(Run::onSelectStream), run.get());
    g_signal_connect(run->source, "new-manager", G_CALLBACK(Run::onNewManager), run.get());
  }

  run->bus = gst_element_get_bus(run->pipeline);
  gst_bus_set_sync_handler(run->bus, Run::onBusSync, run.get(), nullptr);

  run_ = std::move(run);
  checkDiskFloor();
  qInfo("camera %s: session %s starting (%s %s)", qPrintable(camera_.id), qPrintable(run_->sessionId),
        qPrintable(camera_.kind), qPrintable(redactUrl(camera_.mainUrl)));
  const GstStateChangeReturn ret = gst_element_set_state(run_->pipeline, run_->isFile ? GST_STATE_PAUSED : GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    failRun(QStringLiteral("error"), QStringLiteral("pipeline refused to start"));
    return;
  }
  if (!tickTimer_.isActive()) tickTimer_.start();
}

void CameraPipeline::checkDiskFloor() {
  if (!run_ || !camera_.recordEnabled || run_->diskPaused) return;
  const int64_t freeBytes = freeBytesAt(run_->recordDir);
  if (freeBytes < 0 || freeBytes >= config_.minFreeBytes) return;
  run_->diskPaused = true;
  run_->recordingState = QStringLiteral("paused_disk");
  if (run_->valve) g_object_set(run_->valve, "drop", TRUE, nullptr);
  qWarning("camera %s: free space %lld MB below floor %lld MB, recording paused until the next session start",
           qPrintable(camera_.id), static_cast<long long>(freeBytes / (1024 * 1024)),
           static_cast<long long>(config_.minFreeBytes / (1024 * 1024)));
}

void CameraPipeline::drainMessages(uint64_t runId) {
  if (!run_ || run_->id != runId) return;
  drainNow();
}

void CameraPipeline::drainNow() {
  if (!run_) return;
  for (GstMessage* msg : run_->messages.take()) {
    if (run_) handleMessage(msg);
    gst_message_unref(msg);
  }
}

void CameraPipeline::handleMessage(GstMessage* msg) {
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError* err = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_error(msg, &err, &dbg);
      const QString text = redactText(QString::fromUtf8(err ? err->message : "unknown error"));
      const QString detail = dbg ? redactText(QString::fromUtf8(dbg)).simplified() : QString();
      if (err) g_error_free(err);
      g_free(dbg);
      if (run_->stopping) break;
      qWarning("camera %s: pipeline error: %s%s", qPrintable(camera_.id), qPrintable(text),
               detail.isEmpty() ? "" : qPrintable(QStringLiteral(" (") + detail.left(200) + QLatin1Char(')')));
      failRun(QStringLiteral("error"), text);
      break;
    }
    case GST_MESSAGE_EOS:
      if (run_->stopping) break;
      qWarning("camera %s: end of stream", qPrintable(camera_.id));
      failRun(QStringLiteral("eos"), QStringLiteral("end of stream"));
      break;
    case GST_MESSAGE_ELEMENT: {
      const GstStructure* s = gst_message_get_structure(msg);
      if (!s) break;
      if (gst_structure_has_name(s, "splitmuxsink-fragment-opened")) onFragmentOpened(s);
      else if (gst_structure_has_name(s, "splitmuxsink-fragment-closed")) onFragmentClosed(s);
      break;
    }
    case GST_MESSAGE_SEGMENT_DONE:
      if (run_->isFile && !run_->stopping) run_->seekSegment(false);
      break;
    default:
      break;
  }
}

int64_t CameraPipeline::utcForPts(int64_t ptsNs) const {
  if (!run_) return 0;
  std::lock_guard<std::mutex> lock(run_->mutex);
  return run_->stats.clock.started() ? run_->stats.clock.utcForPts(ptsNs) : utcNowMs();
}

void CameraPipeline::onFragmentOpened(const GstStructure* s) {
  guint fragmentId = 0;
  guint64 runningTime = 0;
  const gchar* location = gst_structure_get_string(s, "location");
  gst_structure_get_uint(s, "fragment-id", &fragmentId);
  gst_structure_get_uint64(s, "running-time", &runningTime);
  if (runningTime == GST_CLOCK_TIME_NONE) {
    std::lock_guard<std::mutex> lock(run_->mutex);
    runningTime = static_cast<guint64>(std::max<int64_t>(run_->stats.lastPacketRt, 0));
  }
  RecordingSegment seg;
  seg.id = newId();
  seg.cameraId = camera_.id;
  seg.sessionId = run_->sessionId;
  seg.path = location ? QString::fromUtf8(location) : QString();
  seg.state = QStringLiteral("recording");
  seg.startPtsNs = static_cast<int64_t>(runningTime);
  seg.startUtcMs = utcForPts(seg.startPtsNs);
  seg.createdUtcMs = utcNowMs();
  if (!store_.insertSegment(seg)) {
    qWarning("camera %s: cannot insert segment: %s", qPrintable(camera_.id), qPrintable(store_.lastError()));
    return;
  }
  run_->openSegments.insert(fragmentId, seg.id);
  run_->openSegmentPaths.insert(seg.id, seg.path);
  run_->currentSegmentId = seg.id;
  checkDiskFloor();
  emit statusChanged(camera_.id);
}

void CameraPipeline::onFragmentClosed(const GstStructure* s) {
  guint fragmentId = 0;
  guint64 runningTime = 0, offset = 0, duration = 0;
  const gchar* location = gst_structure_get_string(s, "location");
  gst_structure_get_uint(s, "fragment-id", &fragmentId);
  const bool hasEnd = gst_structure_get_uint64(s, "running-time", &runningTime) && runningTime != GST_CLOCK_TIME_NONE;
  const bool hasOffset = gst_structure_get_uint64(s, "fragment-offset", &offset) && offset != GST_CLOCK_TIME_NONE;
  const bool hasDuration = gst_structure_get_uint64(s, "fragment-duration", &duration) && duration != GST_CLOCK_TIME_NONE;
  const QString path = location ? QString::fromUtf8(location) : QString();

  QString segId = run_->openSegments.take(fragmentId);
  if (segId.isEmpty() && !path.isEmpty()) {
    if (const auto byPath = store_.getSegmentByPath(path)) segId = byPath->id;
  }
  if (!hasEnd && !hasDuration) {
    // A fragment that closed without data: let the file itself decide.
    if (!segId.isEmpty()) {
      run_->openSegmentPaths.remove(segId);
      finalizeLeftoverSegment(segId, path);
      if (run_->currentSegmentId == segId) run_->currentSegmentId.clear();
    }
    return;
  }
  int64_t startPts = 0;
  if (segId.isEmpty()) {
    RecordingSegment seg;
    seg.id = newId();
    seg.cameraId = camera_.id;
    seg.sessionId = run_->sessionId;
    seg.path = path;
    seg.startPtsNs = hasOffset ? static_cast<int64_t>(offset)
                               : (hasEnd && hasDuration ? static_cast<int64_t>(runningTime - duration) : 0);
    seg.startUtcMs = utcForPts(seg.startPtsNs);
    seg.createdUtcMs = utcNowMs();
    if (!store_.insertSegment(seg)) return;
    segId = seg.id;
    startPts = seg.startPtsNs;
  } else if (const auto existing = store_.getSegment(segId)) {
    startPts = existing->startPtsNs;
  }
  run_->openSegmentPaths.remove(segId);
  const int64_t endPts = hasEnd ? static_cast<int64_t>(runningTime) : startPts + static_cast<int64_t>(duration);
  const int64_t bytes = QFileInfo(path).size();
  const int64_t now = utcNowMs();
  store_.finalizeSegment(segId, endPts, utcForPts(endPts), bytes, now);
  if (run_->currentSegmentId == segId) run_->currentSegmentId.clear();
  qInfo("camera %s: segment %s finalized (%.1f s, %lld bytes)", qPrintable(camera_.id), qPrintable(QFileInfo(path).fileName()),
        static_cast<double>(endPts - startPts) / 1e9, static_cast<long long>(bytes));
}

void CameraPipeline::finalizeLeftoverSegment(const QString& segmentId, const QString& path) {
  const auto row = store_.getSegment(segmentId);
  if (!row || row->state != QLatin1String("recording")) return;
  const SegmentProbe probe = probeSegmentFile(path);
  const int64_t now = utcNowMs();
  if (probe.readable) {
    const int64_t endPts = row->startPtsNs + probe.durationNs;
    store_.finalizeSegment(segmentId, endPts, row->startUtcMs + probe.durationNs / 1'000'000, probe.bytes, now);
  } else {
    store_.setSegmentState(segmentId, QStringLiteral("damaged"));
    qWarning("camera %s: segment %s left unreadable", qPrintable(camera_.id), qPrintable(path));
  }
}

void CameraPipeline::onFirstPacket(uint64_t runId) {
  if (!run_ || run_->id != runId) return;
  int64_t firstPts = 0, startedUtc = 0;
  {
    std::lock_guard<std::mutex> lock(run_->mutex);
    firstPts = run_->stats.clock.firstPtsNs();
    startedUtc = run_->stats.clock.startedUtcMs();
  }
  store_.setSessionFirstPts(run_->sessionId, firstPts);
  run_->online = true;
  run_->onlineSinceMono = monoNowNs();
  state_ = QStringLiteral("online");
  sinceUtcMs_ = startedUtc;
  lastError_.clear();
  if (!openGapId_.isEmpty()) {
    store_.closeGap(openGapId_, startedUtc);
    openGapId_.clear();
  }
  qInfo("camera %s: online, session %s first pts %lld ns after %lld ms", qPrintable(camera_.id), qPrintable(run_->sessionId),
        static_cast<long long>(firstPts), static_cast<long long>((monoNowNs() - run_->startedMonoNs) / 1'000'000));
  emit statusChanged(camera_.id);
}

void CameraPipeline::onCapsChanged(uint64_t runId) {
  if (!run_ || run_->id != runId) return;
  QString codec;
  int w = 0, h = 0;
  double fps = 0;
  {
    std::lock_guard<std::mutex> lock(run_->mutex);
    codec = run_->stats.codec;
    w = run_->stats.width;
    h = run_->stats.height;
    fps = run_->stats.fps;
  }
  store_.setSessionMedia(run_->sessionId, codec, w, h, fps, QStringLiteral("none"));
  emit statusChanged(camera_.id);
}

void CameraPipeline::onPtsBackwards(uint64_t runId) {
  if (!run_ || run_->id != runId || run_->stopping) return;
  qWarning("camera %s: pts jumped backwards, restarting session", qPrintable(camera_.id));
  failRun(QString::fromLatin1(SessionClock::kBackwardsReason), QStringLiteral("pts jumped backwards"));
}

void CameraPipeline::onNoMorePads(uint64_t runId) {
  if (!run_ || run_->id != runId || run_->stopping || !run_->isFile) return;
  if (!run_->chainReady.load(std::memory_order_acquire)) {
    failRun(QStringLiteral("error"), QStringLiteral("no supported video track in file"));
    return;
  }
  if (!run_->seekSegment(true)) qWarning("camera %s: initial segment seek failed", qPrintable(camera_.id));
  gst_element_set_state(run_->pipeline, GST_STATE_PLAYING);
}

void CameraPipeline::failRun(const QString& reason, const QString& error) {
  if (!run_) return;
  if (!error.isEmpty()) lastError_ = error;
  endRun(reason);
  if (wantRunning_) {
    state_ = QStringLiteral("reconnecting");
    sinceUtcMs_ = utcNowMs();
    scheduleReconnect();
  } else {
    state_ = QStringLiteral("offline");
  }
  emit statusChanged(camera_.id);
}

void CameraPipeline::scheduleReconnect() {
  const int64_t delay = backoff_.nextDelayMs();
  qInfo("camera %s: reconnecting in %lld ms", qPrintable(camera_.id), static_cast<long long>(delay));
  reconnectTimer_.start(static_cast<int>(delay));
}

void CameraPipeline::endRun(const QString& reason) {
  if (!run_) return;
  run_->stopping = true;
  drainNow();
  if (run_->chainReady.load(std::memory_order_acquire) && !run_->eosSeen && run_->teeSink) {
    gst_pad_send_event(run_->teeSink, gst_event_new_eos());
    const int64_t deadline = monoNowNs() + kStopWaitMs * 1'000'000;
    while (!run_->eosSeen) {
      const int64_t leftMs = (deadline - monoNowNs()) / 1'000'000;
      if (leftMs <= 0) {
        qWarning("camera %s: no EOS within %lld ms, forcing teardown", qPrintable(camera_.id), static_cast<long long>(kStopWaitMs));
        break;
      }
      run_->messages.waitFor(std::min<int64_t>(leftMs, 100));
      drainNow();
    }
  }
  gst_element_set_state(run_->pipeline, GST_STATE_NULL);
  drainNow();

  const auto leftovers = run_->openSegmentPaths;
  for (auto it = leftovers.cbegin(); it != leftovers.cend(); ++it) finalizeLeftoverSegment(it.key(), it.value());
  run_->openSegments.clear();
  run_->openSegmentPaths.clear();
  run_->currentSegmentId.clear();

  const int64_t now = utcNowMs();
  const bool graceful = reason == QLatin1String("stopped") || reason == QLatin1String("shutdown");
  uint64_t packets = 0;
  int64_t lastPacketUtc = 0;
  {
    std::lock_guard<std::mutex> lock(run_->mutex);
    packets = run_->stats.packets;
    lastPacketUtc = run_->stats.lastPacketUtc;
  }
  if (graceful) {
    if (!openGapId_.isEmpty()) {
      store_.closeGap(openGapId_, now);
      openGapId_.clear();
    }
  } else if (packets > 0 && openGapId_.isEmpty()) {
    ReceiveGap gap;
    gap.id = newId();
    gap.cameraId = camera_.id;
    gap.sessionId = run_->sessionId;
    gap.fromUtcMs = lastPacketUtc;
    gap.reason = reason == QLatin1String("eos") ? QStringLiteral("eos") : QStringLiteral("reconnect");
    if (store_.insertGap(gap)) openGapId_ = gap.id;
  }
  store_.endSession(run_->sessionId, reason, now);
  qInfo("camera %s: session %s ended (%s)", qPrintable(camera_.id), qPrintable(run_->sessionId), qPrintable(reason));
  run_->ring.reset();
  run_.reset();
  if (tickTimer_.isActive()) tickTimer_.stop();
}

void CameraPipeline::tick() {
  if (!run_ || run_->stopping) return;
  uint64_t packets = 0, bytes = 0, resumeCount = 0;
  int64_t lastPacketMono = 0, lastPacketUtc = 0, resumeGapStartUtc = 0, resumeUtc = 0;
  {
    std::lock_guard<std::mutex> lock(run_->mutex);
    const RunStats& st = run_->stats;
    packets = st.packets;
    bytes = st.bytes;
    resumeCount = st.resumeCount;
    lastPacketMono = st.lastPacketMono;
    lastPacketUtc = st.lastPacketUtc;
    resumeGapStartUtc = st.resumeGapStartUtc;
    resumeUtc = st.resumeUtc;
  }
  const int64_t now = monoNowNs();
  if (packets == 0) {
    const int64_t limitMs = static_cast<int64_t>(camera_.timeoutMs) * 2 + camera_.jitterMs + kConnectGraceMs;
    if ((now - run_->startedMonoNs) / 1'000'000 > limitMs)
      failRun(QStringLiteral("error"), QStringLiteral("no data within %1 ms").arg(limitMs));
    return;
  }

  run_->bitrateSamples.emplace_back(now, bytes);
  while (run_->bitrateSamples.size() > 1 && now - run_->bitrateSamples.front().first > kBitrateWindowNs)
    run_->bitrateSamples.pop_front();

  if (resumeCount > run_->seenResume) {
    run_->seenResume = resumeCount;
    if (!openGapId_.isEmpty()) {
      store_.closeGap(openGapId_, resumeUtc);
      openGapId_.clear();
    } else {
      ReceiveGap gap;
      gap.id = newId();
      gap.cameraId = camera_.id;
      gap.sessionId = run_->sessionId;
      gap.fromUtcMs = resumeGapStartUtc;
      gap.toUtcMs = resumeUtc;
      gap.reason = QStringLiteral("timeout");
      store_.insertGap(gap);
    }
    qInfo("camera %s: packets resumed", qPrintable(camera_.id));
    emit statusChanged(camera_.id);
  }

  const int64_t ageMs = (now - lastPacketMono) / 1'000'000;
  if (ageMs > config_.gapAfterMs && openGapId_.isEmpty()) {
    ReceiveGap gap;
    gap.id = newId();
    gap.cameraId = camera_.id;
    gap.sessionId = run_->sessionId;
    gap.fromUtcMs = lastPacketUtc;
    gap.reason = QStringLiteral("timeout");
    if (store_.insertGap(gap)) openGapId_ = gap.id;
    qWarning("camera %s: no packets for %lld ms", qPrintable(camera_.id), static_cast<long long>(ageMs));
    emit statusChanged(camera_.id);
  }
  if (ageMs > camera_.timeoutMs) {
    failRun(QStringLiteral("error"), QStringLiteral("no packets for %1 ms").arg(ageMs));
    return;
  }
  if (run_->online && backoff_.attempts() > 0 && now - run_->onlineSinceMono > kHealthyResetNs && ageMs < config_.staleAfterMs)
    backoff_.reset();
}

CameraStatus CameraPipeline::status() const {
  CameraStatus s;
  s.cameraId = camera_.id;
  s.state = state_;
  s.sinceUtcMs = sinceUtcMs_;
  s.reconnects = reconnects_;
  s.lastError = lastError_;
  s.recording = camera_.recordEnabled ? QStringLiteral("recording") : QStringLiteral("disabled");
  if (!run_) return s;
  s.sessionId = run_->sessionId;
  s.recording = run_->recordingState;
  s.currentSegmentId = run_->currentSegmentId;
  const int64_t now = monoNowNs();
  std::vector<int64_t> latencySamples;
  {
    std::lock_guard<std::mutex> lock(run_->mutex);
    RunStats& st = run_->stats;
    pruneFrameStats(st, now);
    s.codec = st.codec;
    s.width = st.width;
    s.height = st.height;
    if (st.frames > 0) {
      s.lastFrameRecvMonoNs = st.lastFrameRecvMono;
      s.lastFrameAgeMs = (now - st.lastFrameRecvMono) / 1'000'000;
      s.stale = s.lastFrameAgeMs > config_.staleAfterMs;
    }
    s.fpsNew = static_cast<double>(st.frameTimes.size()) * 1e9 / static_cast<double>(kFpsWindowNs);
    latencySamples.reserve(st.latencies.size());
    for (const auto& [mono, latency] : st.latencies) latencySamples.push_back(latency);
    s.drops = st.drops;
    if (st.jitterbuffer) {
      GstStructure* stats = nullptr;
      g_object_get(st.jitterbuffer, "stats", &stats, nullptr);
      if (stats) {
        guint64 lost = 0, late = 0;
        gst_structure_get_uint64(stats, "num-lost", &lost);
        gst_structure_get_uint64(stats, "num-late", &late);
        s.drops += static_cast<int64_t>(lost + late);
        gst_structure_free(stats);
      }
    }
  }
  s.latency = percentiles(latencySamples);
  if (run_->chainReady.load(std::memory_order_acquire) && run_->viewQueue) {
    guint level = 0;
    g_object_get(run_->viewQueue, "current-level-buffers", &level, nullptr);
    s.queueDepth = static_cast<int>(level);
  }
  if (run_->ring) {
    const RingInfo& info = run_->ring->info();
    s.frameRing = RingRef{info.name, info.slotCount, info.slotBytes, info.maxWidth, info.maxHeight, QStringLiteral("BGRA")};
  }
  return s;
}

QJsonObject CameraPipeline::metrics() const {
  const CameraStatus s = status();
  QJsonObject m{{"camera_id", camera_.id}, {"state", s.state}, {"session_id", s.sessionId},
                {"fps_new", s.fpsNew}, {"latency_ms", s.latency.toJson()}, {"drops", static_cast<double>(s.drops)},
                {"queue_depth", s.queueDepth}, {"reconnects", s.reconnects}, {"recording", s.recording},
                {"stale", s.stale}, {"ring_bytes", static_cast<double>(ringBytes())}};
  if (!run_) return m;
  uint64_t bytes = 0, frames = 0, ringErrors = 0;
  GstElement* jb = nullptr;
  {
    std::lock_guard<std::mutex> lock(run_->mutex);
    bytes = run_->stats.bytes;
    frames = run_->stats.frames;
    ringErrors = run_->stats.ringWriteErrors;
    jb = run_->stats.jitterbuffer ? GST_ELEMENT(gst_object_ref(run_->stats.jitterbuffer)) : nullptr;
  }
  double bitrateKbps = 0;
  if (run_->bitrateSamples.size() > 1) {
    const auto& a = run_->bitrateSamples.front();
    const auto& b = run_->bitrateSamples.back();
    const int64_t dtNs = b.first - a.first;
    if (dtNs > 0) bitrateKbps = static_cast<double>(b.second - a.second) * 8.0 * 1e9 / static_cast<double>(dtNs) / 1000.0;
  }
  m.insert("bytes_received", static_cast<double>(bytes));
  m.insert("bitrate_kbps", bitrateKbps);
  m.insert("frames", static_cast<double>(frames));
  m.insert("ring_write_errors", static_cast<double>(ringErrors));
  m.insert("open_segments", run_->openSegments.size());
  if (jb) {
    GstStructure* stats = nullptr;
    g_object_get(jb, "stats", &stats, nullptr);
    if (stats) {
      guint64 lost = 0, late = 0, pushed = 0, jitter = 0;
      gst_structure_get_uint64(stats, "num-lost", &lost);
      gst_structure_get_uint64(stats, "num-late", &late);
      gst_structure_get_uint64(stats, "num-pushed", &pushed);
      gst_structure_get_uint64(stats, "avg-jitter", &jitter);
      m.insert("jitterbuffer", QJsonObject{{"num_lost", static_cast<double>(lost)}, {"num_late", static_cast<double>(late)},
                                           {"num_pushed", static_cast<double>(pushed)},
                                           {"avg_jitter_ms", static_cast<double>(jitter) / 1e6}});
      gst_structure_free(stats);
    }
    gst_object_unref(jb);
  }
  return m;
}

uint64_t CameraPipeline::ringBytes() const { return run_ && run_->ring ? run_->ring->info().totalBytes : 0; }

// ConnectionProbe

struct ConnectionProbe::Impl {
  ConnectionProbe* owner = nullptr;
  Camera cam;
  std::optional<Credentials> creds;
  std::function<void(ConnectionTest)> done;
  bool finished = false;
  bool isFile = false;

  GstElement* pipeline = nullptr;
  GstElement* demux = nullptr;
  GstElement* tee = nullptr;
  GstElement* head = nullptr;
  std::vector<GstElement*> chainElements;
  GstPad* teeSink = nullptr;
  GstBus* bus = nullptr;
  MessageQueue messages;
  QTimer deadline;
  QTimer window;
  std::atomic<bool> chainClaimed{false};
  std::atomic<bool> chainReady{false};
  std::atomic<bool> chainSynced{false};
  std::atomic<bool> codecErrorPosted{false};
  int64_t playingMono = 0;

  std::mutex mutex;
  int64_t firstBufferMono = 0;
  int64_t lastBufferMono = 0;
  GstClockTime firstPts = GST_CLOCK_TIME_NONE;
  GstClockTime lastPts = GST_CLOCK_TIME_NONE;
  uint64_t bytes = 0;
  uint64_t buffers = 0;
  ParsedCaps caps;
  QByteArray jpeg;

  ~Impl() {
    if (teeSink) gst_object_unref(teeSink);
    if (bus) {
      gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
      gst_object_unref(bus);
    }
    if (pipeline) gst_object_unref(pipeline);
  }

  void wake() {
    ConnectionProbe* target = owner;
    QMetaObject::invokeMethod(target, [target] { target->drain(); }, Qt::QueuedConnection);
  }

  static GstBusSyncReply onBusSync(GstBus*, GstMessage* msg, gpointer user) {
    auto* impl = static_cast<Impl*>(user);
    const GstMessageType type = GST_MESSAGE_TYPE(msg);
    if (type != GST_MESSAGE_ERROR && type != GST_MESSAGE_EOS) return GST_BUS_DROP;
    impl->messages.push(msg);
    impl->wake();
    return GST_BUS_DROP;
  }

  static gboolean onSelectStream(GstElement*, guint, GstCaps* caps, gpointer user) {
    auto* impl = static_cast<Impl*>(user);
    const QString codec = codecFromCaps(caps);
    if (codec == QLatin1String("h264") || codec == QLatin1String("h265")) return TRUE;
    if (isVideoRtp(caps) && !impl->codecErrorPosted.exchange(true))
      postError(impl->pipeline, QStringLiteral("unsupported codec %1").arg(codec.mid(codec.indexOf(':') + 1)));
    return FALSE;
  }

  static void onPadAdded(GstElement*, GstPad* pad, gpointer user) {
    auto* impl = static_cast<Impl*>(user);
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    const QString codec = codecFromCaps(caps);
    if (caps) gst_caps_unref(caps);
    if (codec == QLatin1String("h264") || codec == QLatin1String("h265")) {
      if (!impl->chainClaimed.exchange(true) && !impl->createChain(codec)) return;
      impl->attachSource(pad);
    } else if (codec.startsWith(QLatin1String("unsupported:")) && !impl->codecErrorPosted.exchange(true)) {
      postError(impl->pipeline, QStringLiteral("unsupported codec %1").arg(codec.mid(codec.indexOf(':') + 1)));
    }
  }

  static void onNoMorePads(GstElement*, gpointer user) {
    auto* impl = static_cast<Impl*>(user);
    if (!impl->chainClaimed.load() && !impl->codecErrorPosted.exchange(true))
      postError(impl->pipeline, QStringLiteral("no supported video track in file"));
  }

  // Same split as CameraPipeline::Run: a file chain is created before the
  // pipeline starts (a non-live pipeline does not resume PLAYING after sinks
  // are added dynamically), an RTSP chain once the SDP names the codec.
  bool createChain(const QString& codec) {
    const bool h264 = codec == QLatin1String("h264");
    GstElement* depay = isFile ? nullptr : makeElement(h264 ? "rtph264depay" : "rtph265depay");
    GstElement* parse = makeElement(h264 ? "h264parse" : "h265parse");
    tee = makeElement("tee");
    GstElement* mainQueue = makeElement("queue");
    GstElement* fakesink = makeElement("fakesink");
    GstElement* previewQueue = makeElement("queue");
    GstElement* dec = makeElement(h264 ? "avdec_h264" : "avdec_h265");
    GstElement* conv = makeElement("videoconvert");
    GstElement* scale = makeElement("videoscale");
    GstElement* previewCaps = makeElement("capsfilter");
    GstElement* jpegenc = makeElement("jpegenc");
    GstElement* previewSink = makeElement("appsink");
    const bool ok = (isFile || depay) && parse && tee && mainQueue && fakesink && previewQueue && dec && conv && scale &&
                    previewCaps && jpegenc && previewSink;
    if (!ok) {
      for (GstElement* e : {depay, parse, tee, mainQueue, fakesink, previewQueue, dec, conv, scale, previewCaps, jpegenc, previewSink})
        if (e) gst_object_unref(e);
      tee = nullptr;
      postError(pipeline, QStringLiteral("missing GStreamer elements for %1").arg(codec));
      return false;
    }
    g_object_set(fakesink, "sync", FALSE, nullptr);
    // A live stream must never be held back by the preview decoder, so its
    // queue leaks. A file is parsed faster than real time and prerolls in
    // PAUSED; a leaky queue there can drop the keyframe before the decoder
    // sees it, and the pipeline never leaves PAUSED.
    if (!isFile) gst_util_set_object_arg(G_OBJECT(previewQueue), "leaky", "downstream");
    g_object_set(previewQueue, "max-size-buffers", isFile ? 8u : 2u, "max-size-time", static_cast<guint64>(0), "max-size-bytes", 0u,
                 nullptr);
    GstCaps* pc = gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT, 320, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, nullptr);
    g_object_set(previewCaps, "caps", pc, nullptr);
    gst_caps_unref(pc);
    g_object_set(previewSink, "sync", FALSE, "max-buffers", 1u, "drop", TRUE, "emit-signals", FALSE, nullptr);
    GstAppSinkCallbacks callbacks{};
    callbacks.new_preroll = onPreviewPreroll;
    callbacks.new_sample = onPreviewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(previewSink), &callbacks, this, nullptr);

    gst_bin_add_many(GST_BIN(pipeline), parse, tee, mainQueue, fakesink, previewQueue, dec, conv, scale, previewCaps, jpegenc,
                     previewSink, nullptr);
    if (depay) {
      gst_bin_add(GST_BIN(pipeline), depay);
      gst_element_link(depay, parse);
    }
    gst_element_link(parse, tee);
    gst_element_link_many(tee, mainQueue, fakesink, nullptr);
    gst_element_link_many(tee, previewQueue, dec, conv, scale, previewCaps, jpegenc, previewSink, nullptr);

    teeSink = gst_element_get_static_pad(tee, "sink");
    gst_pad_add_probe(teeSink, static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
                      onTeeProbe, this, nullptr);
    head = depay ? depay : parse;
    chainElements = {previewSink, jpegenc, previewCaps, scale, conv, dec, previewQueue, fakesink, mainQueue, tee, parse, depay};
    chainReady.store(true, std::memory_order_release);
    return true;
  }

  void attachSource(GstPad* srcPad) {
    if (!chainReady.load(std::memory_order_acquire) || !head) return;
    if (!chainSynced.exchange(true) && !isFile)
      for (GstElement* e : chainElements)
        if (e) gst_element_sync_state_with_parent(e);
    GstPad* sink = gst_element_get_static_pad(head, "sink");
    const GstPadLinkReturn link = gst_pad_link(srcPad, sink);
    gst_object_unref(sink);
    if (link != GST_PAD_LINK_OK) postError(pipeline, QStringLiteral("cannot link source pad (%1)").arg(static_cast<int>(link)));
  }

  static GstPadProbeReturn onTeeProbe(GstPad*, GstPadProbeInfo* info, gpointer user) {
    auto* impl = static_cast<Impl*>(user);
    if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
      GstEvent* ev = GST_PAD_PROBE_INFO_EVENT(info);
      if (GST_EVENT_TYPE(ev) == GST_EVENT_CAPS) {
        GstCaps* caps = nullptr;
        gst_event_parse_caps(ev, &caps);
        const ParsedCaps parsed = parseVideoCaps(caps);
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->caps = parsed;
      }
      return GST_PAD_PROBE_OK;
    }
    GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;
    const int64_t now = monoNowNs();
    const GstClockTime pts = GST_BUFFER_PTS(buf);
    bool first = false;
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      if (impl->buffers == 0) {
        impl->firstBufferMono = now;
        first = true;
      }
      ++impl->buffers;
      impl->bytes += gst_buffer_get_size(buf);
      impl->lastBufferMono = now;
      if (GST_CLOCK_TIME_IS_VALID(pts)) {
        if (!GST_CLOCK_TIME_IS_VALID(impl->firstPts) || pts < impl->firstPts) impl->firstPts = pts;
        if (!GST_CLOCK_TIME_IS_VALID(impl->lastPts) || pts > impl->lastPts) impl->lastPts = pts;
      }
    }
    if (first) impl->wake();
    return GST_PAD_PROBE_OK;
  }

  static GstFlowReturn onPreviewPreroll(GstAppSink* sink, gpointer user) {
    GstSample* sample = gst_app_sink_pull_preroll(sink);
    if (!sample) return GST_FLOW_OK;
    static_cast<Impl*>(user)->keepPreview(sample);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  static GstFlowReturn onPreviewSample(GstAppSink* sink, gpointer user) {
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    static_cast<Impl*>(user)->keepPreview(sample);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  void keepPreview(GstSample* sample) {
    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
      std::lock_guard<std::mutex> lock(mutex);
      if (jpeg.isEmpty()) jpeg = QByteArray(reinterpret_cast<const char*>(map.data), static_cast<qsizetype>(map.size));
      gst_buffer_unmap(buf, &map);
    }
  }
};

ConnectionProbe::ConnectionProbe(Camera camera, std::optional<Credentials> credentials, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
  impl_->owner = this;
  impl_->cam = std::move(camera);
  impl_->creds = std::move(credentials);
  impl_->isFile = impl_->cam.kind == QLatin1String("file");
  impl_->deadline.setSingleShot(true);
  impl_->window.setSingleShot(true);
  connect(&impl_->deadline, &QTimer::timeout, this, [this] {
    bool gotData = false;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      gotData = impl_->buffers > 0;
    }
    finish(gotData ? QString() : QStringLiteral("no data within %1 ms").arg(impl_->cam.timeoutMs));
  });
  connect(&impl_->window, &QTimer::timeout, this, [this] { finish(QString()); });
}

ConnectionProbe::~ConnectionProbe() {
  if (impl_->pipeline) gst_element_set_state(impl_->pipeline, GST_STATE_NULL);
}

void ConnectionProbe::run(std::function<void(ConnectionTest)> done) {
  impl_->done = std::move(done);
  Impl& p = *impl_;
  p.pipeline = gst_pipeline_new(nullptr);
  GstElement* source = nullptr;
  if (p.isFile) {
    const QString path = localPathFor(p.cam.mainUrl);
    if (!QFileInfo::exists(path)) {
      finish(QStringLiteral("file not found"));
      return;
    }
    source = makeElement("filesrc");
    p.demux = makeElement(demuxerForPath(path).toUtf8().constData(), "demux");
    if (!source || !p.demux) {
      if (source) gst_object_unref(source);
      if (p.demux) gst_object_unref(p.demux);
      p.demux = nullptr;
      finish(QStringLiteral("missing file source elements"));
      return;
    }
    g_object_set(source, "location", path.toUtf8().constData(), nullptr);
    gst_bin_add_many(GST_BIN(p.pipeline), source, p.demux, nullptr);
    gst_element_link(source, p.demux);
    g_signal_connect(p.demux, "pad-added", G_CALLBACK(Impl::onPadAdded), &p);
    g_signal_connect(p.demux, "no-more-pads", G_CALLBACK(Impl::onNoMorePads), &p);
    const QString codec = probeVideoCodec(path);
    if (codec != QLatin1String("h264") && codec != QLatin1String("h265")) {
      finish(codec.isEmpty() ? QStringLiteral("no readable video track in file")
                             : QStringLiteral("unsupported codec %1").arg(codec.mid(codec.indexOf(':') + 1)));
      return;
    }
    p.chainClaimed = true;
    if (!p.createChain(codec)) {
      finish(QStringLiteral("missing GStreamer elements for %1").arg(codec));
      return;
    }
  } else {
    source = makeElement("rtspsrc");
    if (!source) {
      finish(QStringLiteral("missing rtspsrc"));
      return;
    }
    const guint64 timeoutUs = static_cast<guint64>(std::max(p.cam.timeoutMs, 1000)) * 1000;
    g_object_set(source, "location", p.cam.mainUrl.toUtf8().constData(), "latency", static_cast<guint>(p.cam.jitterMs),
                 "tcp-timeout", timeoutUs, "timeout", timeoutUs, nullptr);
    gst_util_set_object_arg(G_OBJECT(source), "protocols", p.cam.transport == QLatin1String("udp") ? "udp" : "tcp");
    if (p.creds && !p.creds->username.isEmpty())
      g_object_set(source, "user-id", p.creds->username.toUtf8().constData(), "user-pw", p.creds->password.toUtf8().constData(),
                   nullptr);
    gst_bin_add(GST_BIN(p.pipeline), source);
    g_signal_connect(source, "pad-added", G_CALLBACK(Impl::onPadAdded), &p);
    g_signal_connect(source, "select-stream", G_CALLBACK(Impl::onSelectStream), &p);
  }
  p.bus = gst_element_get_bus(p.pipeline);
  gst_bus_set_sync_handler(p.bus, Impl::onBusSync, &p, nullptr);
  p.playingMono = monoNowNs();
  if (gst_element_set_state(p.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    finish(QStringLiteral("pipeline refused to start"));
    return;
  }
  p.deadline.start(std::max(p.cam.timeoutMs, 500));
}

void ConnectionProbe::drain() {
  if (impl_->finished) return;
  for (GstMessage* msg : impl_->messages.take()) {
    if (!impl_->finished) {
      if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError* err = nullptr;
        gst_message_parse_error(msg, &err, nullptr);
        const QString text = redactText(QString::fromUtf8(err ? err->message : "unknown error"));
        if (err) g_error_free(err);
        bool gotData = false;
        {
          std::lock_guard<std::mutex> lock(impl_->mutex);
          gotData = impl_->buffers > 0;
        }
        finish(gotData ? QString() : text);
      } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
        finish(QString());
      }
    }
    gst_message_unref(msg);
  }
  if (impl_->finished || impl_->window.isActive()) return;
  bool gotData = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    gotData = impl_->buffers > 0;
  }
  if (gotData) impl_->window.start(2000);
}

void ConnectionProbe::finish(const QString& error) {
  if (impl_->finished) return;
  impl_->finished = true;
  impl_->deadline.stop();
  impl_->window.stop();
  if (impl_->pipeline) gst_element_set_state(impl_->pipeline, GST_STATE_NULL);
  ConnectionTest result;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    result.ok = error.isEmpty() && impl_->buffers > 0;
    result.error = error;
    if (impl_->buffers > 0) {
      result.handshakeMs = static_cast<int>((impl_->firstBufferMono - impl_->playingMono) / 1'000'000);
      result.codec = impl_->caps.codec;
      result.width = impl_->caps.width;
      result.height = impl_->caps.height;
      // A file parses faster than real time, so its rates come from the
      // media timestamps; a live stream is measured against the wall clock.
      int64_t spanNs = impl_->lastBufferMono - impl_->firstBufferMono;
      if (impl_->isFile && GST_CLOCK_TIME_IS_VALID(impl_->firstPts) && GST_CLOCK_TIME_IS_VALID(impl_->lastPts))
        spanNs = static_cast<int64_t>(impl_->lastPts - impl_->firstPts);
      result.fps = impl_->caps.fps;
      if (result.fps <= 0 && spanNs > 0 && impl_->buffers > 1)
        result.fps = static_cast<double>(impl_->buffers - 1) * 1e9 / static_cast<double>(spanNs);
      if (spanNs > 100'000'000)
        result.bitrateKbps = static_cast<double>(impl_->bytes) * 8.0 * 1e9 / static_cast<double>(spanNs) / 1000.0;
      if (!impl_->jpeg.isEmpty()) result.previewJpegBase64 = QString::fromLatin1(impl_->jpeg.toBase64());
    }
  }
  if (!result.ok && result.error.isEmpty()) result.error = QStringLiteral("no data received");
  auto done = std::move(impl_->done);
  if (done) done(result);
  deleteLater();
}

}
