#include "fovea/core/CameraPipeline.h"
#include "fovea/Clock.h"
#include "fovea/core/AnalysisTap.h"
#include "fovea/FrameRing.h"
#include "fovea/Ids.h"
#include "fovea/Redact.h"
#include "fovea/core/MediaProbe.h"
#include "fovea/core/SessionClock.h"
#include "fovea/core/Store.h"
#include "fovea/core/SystemStats.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QUrl>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <QMetaObject>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace fovea::core {
namespace {

constexpr int kTickMs = 250;
constexpr std::chrono::milliseconds kStopWait{3000};
constexpr int64_t kHealthyResetNs = 30LL * 1000 * 1000 * 1000;
constexpr int64_t kBackwardsRestartAfterNs = 5LL * 1000 * 1000 * 1000;
constexpr int64_t kFpsWindowNs = 1'000'000'000;
constexpr int64_t kLatencyWindowNs = 2'000'000'000;
constexpr int64_t kBitrateWindowNs = 2'000'000'000;
constexpr int64_t kDiskCheckIntervalNs = 1'000'000'000;
constexpr guint kViewQueueMaxBuffers = 5;
constexpr size_t kPtsMapSize = 64;
constexpr int64_t kConnectGraceMs = 2000;

struct RtMono {
  int64_t rt = -1;
  int64_t mono = 0;
};

// Bus messages are queued here by the sync handler (streaming threads) and
// drained on the owner's thread, so no message is lost while stopping.
struct MessageQueue {
  std::mutex mutex;
  std::deque<GstMessage*> queue;

  ~MessageQueue() {
    for (GstMessage* m : queue) gst_message_unref(m);
  }
  void push(GstMessage* msg) {
    std::lock_guard<std::mutex> lock(mutex);
    queue.push_back(gst_message_ref(msg));
  }
  std::deque<GstMessage*> take() {
    std::lock_guard<std::mutex> lock(mutex);
    std::deque<GstMessage*> out;
    out.swap(queue);
    return out;
  }
};

struct RunStats {
  SessionClock clock;
  uint64_t packets = 0;
  uint64_t bytes = 0;
  int64_t lastPacketMono = 0;
  int64_t lastPacketEndUtc = 0;
  uint64_t resumeCount = 0;
  int64_t resumeFromUtc = 0;
  int64_t resumeToUtc = 0;
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
  std::array<RtMono, kPtsMapSize> ptsMap{};
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

// splitmuxsink reports a missing running time as GST_CLOCK_STIME_NONE, which
// reads as 2^63 through the unsigned field.
bool readTime(const GstStructure* s, const char* field, int64_t& out) {
  guint64 value = 0;
  if (!gst_structure_get_uint64(s, field, &value) || value > static_cast<guint64>(std::numeric_limits<int64_t>::max()))
    return false;
  out = static_cast<int64_t>(value);
  return true;
}

}

// Streaming and worker threads post to the pipeline through this, so a post
// can never race the pipeline's destruction.
struct CameraPipeline::Mailbox {
  std::mutex mutex;
  CameraPipeline* target = nullptr;

  void post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mutex);
    if (target) QMetaObject::invokeMethod(target, std::move(fn), Qt::QueuedConnection);
  }
  void detach() {
    std::lock_guard<std::mutex> lock(mutex);
    target = nullptr;
  }
};

struct CameraPipeline::Run {
  uint64_t id = 0;
  CameraPipeline* owner = nullptr;
  std::shared_ptr<Mailbox> mailbox;
  Camera cam;
  bool isFile = false;
  bool diskPaused = false;
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
  std::atomic<bool> codecErrorPosted{false};

  MessageQueue messages;
  mutable std::mutex mutex;
  RunStats stats;

  QString sessionId;
  std::array<uint8_t, 16> sessionBytes{};
  int64_t startedUtcMs = 0;
  int64_t startedMonoNs = 0;
  std::mutex ringMutex;
  std::unique_ptr<FrameRingWriter> ring;
  std::shared_ptr<AnalysisTap> tap;

  QString recordDir;
  QString recordingState = QStringLiteral("disabled");
  QHash<uint, QString> openSegments;
  QHash<QString, QString> openSegmentPaths;
  QString currentSegmentId;
  bool online = false;
  int64_t onlineSinceMono = 0;
  uint64_t seenResume = 0;
  std::deque<std::pair<int64_t, uint64_t>> bitrateSamples;

  struct EosJob {
    GstPad* pad = nullptr;
    std::shared_ptr<Mailbox> mailbox;
    CameraPipeline* owner = nullptr;
    uint64_t runId = 0;
  };

  struct Teardown {
    std::unique_ptr<Run> run;
    std::vector<std::pair<QString, QString>> leftovers;
  };

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
    mailbox->post([target, method, runId] { (target->*method)(runId); });
  }

  static void sendEos(GstElement*, gpointer user) {
    std::unique_ptr<EosJob> job(static_cast<EosJob*>(user));
    const bool sent = gst_pad_send_event(job->pad, gst_event_new_eos()) != FALSE;
    gst_object_unref(job->pad);
    // A refused EOS (the pads are already flushing) never reaches the bus.
    if (!sent) job->mailbox->post([owner = job->owner, runId = job->runId] { owner->finishRetire(runId); });
  }

  // Pool thread: the state change can wait on a sink stuck in write(), and a
  // segment probe parses the whole file once the sink has closed it.
  static void teardown(GstElement*, gpointer user) {
    std::unique_ptr<Teardown> job(static_cast<Teardown*>(user));
    const std::shared_ptr<Mailbox> mailbox = job->run->mailbox;
    CameraPipeline* owner = job->run->owner;
    gst_element_set_state(job->run->pipeline, GST_STATE_NULL);
    job->run.reset();
    mailbox->post([owner] { owner->onTornDown(); });
    for (const auto& [segmentId, path] : job->leftovers) {
      const SegmentProbe probe = probeSegmentFile(path);
      mailbox->post([owner, id = segmentId, probe] { owner->onLeftoverProbed(id, probe); });
    }
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
    GstElement* muxer = nullptr;
    GstElement* filesink = nullptr;
    if (cam.recordEnabled) {
      recQueue = makeElement("queue", "recq");
      valve = makeElement("valve", "recvalve");
      splitmux = makeElement("splitmuxsink", "splitmux");
      muxer = makeElement("matroskamux");
      filesink = makeElement("filesink");
    }
    const bool ok = (rtp ? depay != nullptr : true) && parse && (isFile ? pacer != nullptr : true) && tee && viewQueue &&
                    dec && conv && scale && capsFilter && appsink &&
                    (!cam.recordEnabled || (recQueue && valve && splitmux && muxer && filesink));
    if (!ok) {
      for (GstElement* e :
           {depay, parse, pacer, tee, viewQueue, dec, conv, scale, capsFilter, appsink, recQueue, valve, splitmux, muxer, filesink})
        if (e) gst_object_unref(e);
      tee = viewQueue = capsFilter = appsink = valve = splitmux = nullptr;
      postError(pipeline, QStringLiteral("missing GStreamer elements for %1").arg(codec));
      return false;
    }

    gst_util_set_object_arg(G_OBJECT(viewQueue), "leaky", "downstream");
    g_object_set(viewQueue, "max-size-buffers", kViewQueueMaxBuffers, "max-size-time", static_cast<guint64>(0), "max-size-bytes", 0u,
                 nullptr);
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
      gst_util_set_object_arg(G_OBJECT(filesink), "buffer-mode", "unbuffered");
      g_object_set(splitmux, "max-size-time", static_cast<guint64>(cam.segmentSeconds) * GST_SECOND, "muxer", muxer, "sink",
                   filesink, nullptr);
      g_signal_connect(splitmux, "format-location", G_CALLBACK(onFormatLocation), this);
      // The default drop-all mode also swallows the EOS that finalizes the
      // open file.
      gst_util_set_object_arg(G_OBJECT(valve), "drop-mode", "forward-sticky-events");
      g_object_set(valve, "drop", diskPaused ? TRUE : FALSE, nullptr);
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
    const bool timed = GST_CLOCK_TIME_IS_VALID(rtRaw);
    const int64_t rt = timed ? static_cast<int64_t>(rtRaw) : 0;
    const int64_t endRt = rt + (GST_BUFFER_DURATION_IS_VALID(buf) ? static_cast<int64_t>(GST_BUFFER_DURATION(buf)) : 0);
    bool first = false;
    bool backwards = false;
    bool drop = false;
    {
      std::lock_guard<std::mutex> lock(run->mutex);
      RunStats& st = run->stats;
      const int64_t utc = utcNowMs();
      if (st.packets > 0 && now - st.lastPacketMono > run->gapAfterNs) {
        ++st.resumeCount;
        st.resumeFromUtc = st.lastPacketEndUtc;
        st.resumeToUtc = timed && st.clock.started() ? st.clock.utcForPts(rt) : utc;
      }
      ++st.packets;
      st.bytes += gst_buffer_get_size(buf);
      st.lastPacketMono = now;
      // A buffer without a usable timestamp is traffic, but it must neither
      // anchor nor move the session clock.
      if (timed) {
        if (!st.clock.started()) {
          st.clock.start(rt, utc);
          first = true;
        } else if (!st.clock.observe(rt) && !st.ptsBackwards) {
          st.ptsBackwards = true;
          backwards = true;
        }
        if (!st.ptsBackwards) {
          st.ptsMap[st.ptsMapNext] = {rt, now};
          st.ptsMapNext = (st.ptsMapNext + 1) % kPtsMapSize;
          st.lastPacketEndUtc = std::max(st.lastPacketEndUtc, st.clock.utcForPts(endRt));
        }
      } else if (!st.clock.started()) {
        st.lastPacketEndUtc = utc;
      }
      drop = st.ptsBackwards;
    }
    if (first) run->invoke(&CameraPipeline::onFirstPacket);
    if (backwards) run->invoke(&CameraPipeline::onPtsBackwards);
    // After a backwards jump nothing more reaches this session's recording or
    // ring; the manager thread replaces the session.
    return drop ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
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
    const bool timed = GST_CLOCK_TIME_IS_VALID(rtRaw);
    const int64_t rt = timed ? static_cast<int64_t>(rtRaw) : 0;
    const int64_t now = monoNowNs();

    // Keyed by running time: unlike the raw PTS, which a looping file repeats
    // on every pass, it never repeats within a session.
    int64_t recvMono = now;
    int64_t utc = 0;
    if (timed) {
      std::lock_guard<std::mutex> lock(run->mutex);
      for (const RtMono& e : run->stats.ptsMap) {
        if (e.rt == rt) {
          recvMono = e.mono;
          break;
        }
      }
      if (run->stats.clock.started()) utc = run->stats.clock.utcForPts(rt);
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
    bool written = false;
    {
      std::lock_guard<std::mutex> lock(run->ringMutex);
      written = run->ring && run->ring->write(header, pixels, stride);
    }
    if (timed && run->tap && run->tap->wants(now))
      run->tap->offer(run->sessionId, rt, recvMono, utc, static_cast<int>(header.width), static_cast<int>(header.height), pixels,
                      stride, now);
    gst_video_frame_unmap(&frame);
    const int64_t writeMono = monoNowNs();

    {
      std::lock_guard<std::mutex> lock(run->mutex);
      RunStats& st = run->stats;
      ++st.frames;
      if (written) {
        st.lastFrameRecvMono = recvMono;
        st.frameTimes.push_back(writeMono);
        st.latencies.emplace_back(writeMono, writeMono - recvMono);
      } else {
        ++st.ringWriteErrors;
      }
      if (timed) {
        if (st.lastFrameRt >= 0 && rt > st.lastFrameRt) {
          const int64_t delta = rt - st.lastFrameRt;
          if (st.framePeriodNs == 0)
            st.estimatedPeriodNs = st.estimatedPeriodNs == 0 ? delta : (st.estimatedPeriodNs * 7 + delta) / 8;
          const int64_t period = st.framePeriodNs > 0 ? st.framePeriodNs : st.estimatedPeriodNs;
          if (period > 0 && delta > period * 3 / 2) st.drops += delta / period - 1;
        }
        st.lastFrameRt = rt;
      }
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
                               std::shared_ptr<AnalysisTap> tap, QObject* parent)
    : QObject(parent), camera_(std::move(camera)), credentials_(std::move(credentials)), store_(store), config_(config),
      tap_(std::move(tap)), mailbox_(std::make_shared<Mailbox>()), backoff_(1000, 30000, 2.0) {
  mailbox_->target = this;
  tickTimer_.setInterval(kTickMs);
  connect(&tickTimer_, &QTimer::timeout, this, &CameraPipeline::tick);
  reconnectTimer_.setSingleShot(true);
  connect(&reconnectTimer_, &QTimer::timeout, this, [this] {
    if (!wantRunning_ || run_) return;
    ++reconnects_;
    requestStart();
  });
}

QString CameraPipeline::ringName(const QString& cameraId, const QString& sessionId) {
  return makeRingName(QStringLiteral("cam:%1:%2").arg(cameraId, sessionId));
}

CameraPipeline::~CameraPipeline() {
  mailbox_->detach();
  if (run_) {
    run_->stopping = true;
    drain(*run_);
    closeSession(*run_, QStringLiteral("shutdown"));
    retiring_.push_back(std::move(run_));
  }
  for (std::unique_ptr<Run>& run : retiring_) {
    run->openSegmentPaths.clear();
    disposeRun(std::move(run));
  }
  closeOpenGap(utcNowMs());
}

void CameraPipeline::start() {
  wantRunning_ = true;
  backoff_.reset();
  state_ = QStringLiteral("connecting");
  sinceUtcMs_ = utcNowMs();
  if (!run_) requestStart();
  emit statusChanged(camera_.id);
}

void CameraPipeline::stop(const QString& reason) {
  wantRunning_ = false;
  startPending_ = false;
  reconnectTimer_.stop();
  if (run_) retireRun(reason);
  else closeOpenGap(utcNowMs());
  state_ = QStringLiteral("offline");
  sinceUtcMs_ = utcNowMs();
  emit statusChanged(camera_.id);
}

void CameraPipeline::reconfigure(Camera camera, std::optional<Credentials> credentials) {
  camera_ = std::move(camera);
  credentials_ = std::move(credentials);
  if (!wantRunning_) return;
  reconnectTimer_.stop();
  backoff_.reset();
  restartRun(QStringLiteral("stopped"));
}

void CameraPipeline::setCamera(const Camera& camera) { camera_ = camera; }

bool CameraPipeline::idle() const { return !run_ && retiring_.empty() && pendingTeardowns_ == 0 && pendingProbes_ == 0; }

void CameraPipeline::restartRun(const QString& reason) {
  if (run_) retireRun(reason);
  state_ = QStringLiteral("connecting");
  sinceUtcMs_ = utcNowMs();
  requestStart();
  emit statusChanged(camera_.id);
}

// A new run waits until the previous one is finalized and torn down, so a
// camera never holds two connections and its sessions never overlap.
void CameraPipeline::requestStart() {
  if (!retiring_.empty() || pendingTeardowns_ > 0) {
    startPending_ = true;
    return;
  }
  startPending_ = false;
  startRun();
}

void CameraPipeline::startRun() {
  auto run = std::make_unique<Run>();
  run->id = nextRunId_++;
  run->owner = this;
  run->mailbox = mailbox_;
  run->cam = camera_;
  run->isFile = camera_.kind == QLatin1String("file");
  run->gapAfterNs = static_cast<int64_t>(config_.gapAfterMs) * 1'000'000;
  run->ringMaxWidth = config_.ringMaxWidth;
  run->ringMaxHeight = config_.ringMaxHeight;
  run->sessionId = newId();
  run->sessionBytes = sessionIdBytes(run->sessionId);
  run->tap = tap_;
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

  // Named per session: a reader still mapped to the previous session's ring
  // sees a new name and reopens instead of freezing on an unlinked segment.
  run->ring = FrameRingWriter::create(ringName(camera_.id, run->sessionId),
                                      config_.ringSlots, config_.ringMaxWidth, config_.ringMaxHeight);
  if (!run->ring) qWarning("camera %s: cannot create frame ring", qPrintable(camera_.id));

  if (camera_.recordEnabled) {
    run->diskPaused = refreshDiskPaused();
    if (!QDir().mkpath(run->recordDir)) {
      qWarning("camera %s: cannot create %s", qPrintable(camera_.id), qPrintable(run->recordDir));
      run->recordingState = QStringLiteral("error");
    } else {
      run->recordingState = run->diskPaused ? QStringLiteral("paused_disk") : QStringLiteral("recording");
    }
  }
  nextDiskCheckNs_ = monoNowNs() + kDiskCheckIntervalNs;

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
  qInfo("camera %s: session %s starting (%s %s)", qPrintable(camera_.id), qPrintable(run_->sessionId),
        qPrintable(camera_.kind), qPrintable(redactUrl(camera_.mainUrl)));
  const GstStateChangeReturn ret = gst_element_set_state(run_->pipeline, run_->isFile ? GST_STATE_PAUSED : GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    failRun(QStringLiteral("error"), QStringLiteral("pipeline refused to start"));
    return;
  }
  if (!tickTimer_.isActive()) tickTimer_.start();
}

// Below the floor no new segment may start. Pausing or resuming restarts the
// run: the EOS closes the open file, and the next session opens its valve
// (and its first file) at a clean start.
void CameraPipeline::checkDiskFloor() {
  const int64_t now = monoNowNs();
  if (!camera_.recordEnabled || now < nextDiskCheckNs_) return;
  nextDiskCheckNs_ = now + kDiskCheckIntervalNs;
  if (refreshDiskPaused() == run_->diskPaused) return;
  restartRun(QStringLiteral("stopped"));
}

bool CameraPipeline::refreshDiskPaused() {
  const int64_t freeBytes = freeDiskBytes(config_.recordingsDir + QLatin1Char('/') + camera_.id);
  if (freeBytes < 0) return diskPaused_;
  const int64_t resumeBytes = config_.minFreeBytes + config_.diskHeadroomBytes();
  if (!diskPaused_ && freeBytes < config_.minFreeBytes) {
    diskPaused_ = true;
    qWarning("camera %s: free space %lld MB below floor %lld MB, recording paused", qPrintable(camera_.id),
             static_cast<long long>(freeBytes / (1024 * 1024)), static_cast<long long>(config_.minFreeBytes / (1024 * 1024)));
  } else if (diskPaused_ && freeBytes >= resumeBytes) {
    diskPaused_ = false;
    qInfo("camera %s: free space %lld MB, recording resumed", qPrintable(camera_.id),
          static_cast<long long>(freeBytes / (1024 * 1024)));
  }
  return diskPaused_;
}

CameraPipeline::Run* CameraPipeline::findRun(uint64_t runId) const {
  if (run_ && run_->id == runId) return run_.get();
  for (const std::unique_ptr<Run>& run : retiring_)
    if (run->id == runId) return run.get();
  return nullptr;
}

void CameraPipeline::drainMessages(uint64_t runId) {
  Run* run = findRun(runId);
  if (!run) return;
  drain(*run);
  if (run != run_.get() && run->eosSeen) finishRetire(runId);
}

void CameraPipeline::drain(Run& run) {
  for (GstMessage* msg : run.messages.take()) {
    handleMessage(run, msg);
    gst_message_unref(msg);
  }
}

void CameraPipeline::handleMessage(Run& run, GstMessage* msg) {
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError* err = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_error(msg, &err, &dbg);
      const QString text = redactText(QString::fromUtf8(err ? err->message : "unknown error"));
      const QString detail = dbg ? redactText(QString::fromUtf8(dbg)).simplified() : QString();
      if (err) g_error_free(err);
      g_free(dbg);
      if (run.stopping) break;
      qWarning("camera %s: pipeline error: %s%s", qPrintable(camera_.id), qPrintable(text),
               detail.isEmpty() ? "" : qPrintable(QStringLiteral(" (") + detail.left(200) + QLatin1Char(')')));
      failRun(QStringLiteral("error"), text);
      break;
    }
    case GST_MESSAGE_EOS:
      if (run.stopping) break;
      qWarning("camera %s: end of stream", qPrintable(camera_.id));
      failRun(QStringLiteral("eos"), QStringLiteral("end of stream"));
      break;
    case GST_MESSAGE_ELEMENT: {
      const GstStructure* s = gst_message_get_structure(msg);
      if (!s) break;
      if (gst_structure_has_name(s, "splitmuxsink-fragment-opened")) onFragmentOpened(run, s);
      else if (gst_structure_has_name(s, "splitmuxsink-fragment-closed")) onFragmentClosed(run, s);
      break;
    }
    case GST_MESSAGE_SEGMENT_DONE:
      if (run.isFile && !run.stopping) run.seekSegment(false);
      break;
    default:
      break;
  }
}

int64_t CameraPipeline::utcForPts(const Run& run, int64_t ptsNs) const {
  {
    std::lock_guard<std::mutex> lock(run.mutex);
    if (run.stats.clock.started()) return run.stats.clock.utcForPts(ptsNs);
  }
  qWarning("camera %s: session %s has no clock yet, segment time unknown", qPrintable(camera_.id), qPrintable(run.sessionId));
  return 0;
}

void CameraPipeline::onFragmentOpened(Run& run, const GstStructure* s) {
  guint fragmentId = 0;
  int64_t runningTime = 0;
  gst_structure_get_uint(s, "fragment-id", &fragmentId);
  // splitmuxsink opens a fragment at EOS even when no buffer ever reached
  // it; such a fragment has no running time and no media.
  if (!readTime(s, "running-time", runningTime)) return;
  const gchar* location = gst_structure_get_string(s, "location");
  RecordingSegment seg;
  seg.id = newId();
  seg.cameraId = camera_.id;
  seg.sessionId = run.sessionId;
  seg.path = location ? QString::fromUtf8(location) : QString();
  seg.state = QStringLiteral("recording");
  seg.startPtsNs = runningTime;
  seg.startUtcMs = utcForPts(run, seg.startPtsNs);
  seg.createdUtcMs = utcNowMs();
  if (!store_.insertSegment(seg)) {
    qWarning("camera %s: cannot insert segment: %s", qPrintable(camera_.id), qPrintable(store_.lastError()));
    return;
  }
  run.openSegments.insert(fragmentId, seg.id);
  run.openSegmentPaths.insert(seg.id, seg.path);
  run.currentSegmentId = seg.id;
  if (&run == run_.get()) nextDiskCheckNs_ = 0;
  emit statusChanged(camera_.id);
}

void CameraPipeline::onFragmentClosed(Run& run, const GstStructure* s) {
  guint fragmentId = 0;
  int64_t runningTime = 0, offset = 0, duration = 0;
  gst_structure_get_uint(s, "fragment-id", &fragmentId);
  const bool hasEnd = readTime(s, "running-time", runningTime);
  const bool hasOffset = readTime(s, "fragment-offset", offset);
  const bool hasDuration = readTime(s, "fragment-duration", duration);
  const gchar* location = gst_structure_get_string(s, "location");
  const QString path = location ? QString::fromUtf8(location) : QString();

  QString segId = run.openSegments.take(fragmentId);
  if (segId.isEmpty() && !path.isEmpty()) {
    if (const auto byPath = store_.getSegmentByPath(path)) segId = byPath->id;
  }
  if (!hasEnd && !hasDuration) {
    if (segId.isEmpty()) {
      if (path.startsWith(run.recordDir + QLatin1Char('/'))) QFile::remove(path);
    } else if (run.currentSegmentId == segId) {
      // Closed without data: the row stays open and the file is probed once
      // the pipeline has released it.
      run.currentSegmentId.clear();
    }
    return;
  }
  int64_t startPts = 0;
  if (segId.isEmpty()) {
    RecordingSegment seg;
    seg.id = newId();
    seg.cameraId = camera_.id;
    seg.sessionId = run.sessionId;
    seg.path = path;
    seg.startPtsNs = hasOffset ? offset : (hasEnd && hasDuration ? runningTime - duration : 0);
    seg.startUtcMs = utcForPts(run, seg.startPtsNs);
    seg.createdUtcMs = utcNowMs();
    if (!store_.insertSegment(seg)) return;
    segId = seg.id;
    startPts = seg.startPtsNs;
  } else if (const auto existing = store_.getSegment(segId)) {
    startPts = existing->startPtsNs;
  }
  run.openSegmentPaths.remove(segId);
  // At EOS running-time is the start of the last buffer; offset plus
  // duration also covers that buffer.
  const int64_t endPts = hasOffset && hasDuration ? offset + duration : hasEnd ? runningTime : startPts + duration;
  const int64_t bytes = QFileInfo(path).size();
  store_.finalizeSegment(segId, endPts, utcForPts(run, endPts), bytes, utcNowMs());
  if (run.currentSegmentId == segId) run.currentSegmentId.clear();
  qInfo("camera %s: segment %s finalized (%.1f s, %lld bytes)", qPrintable(camera_.id), qPrintable(QFileInfo(path).fileName()),
        static_cast<double>(endPts - startPts) / 1e9, static_cast<long long>(bytes));
}

void CameraPipeline::onTornDown() {
  --pendingTeardowns_;
  if (startPending_ && retiring_.empty() && pendingTeardowns_ == 0) {
    startPending_ = false;
    if (wantRunning_ && !run_) startRun();
  }
  emitIdleIfDone();
}

void CameraPipeline::onLeftoverProbed(const QString& segmentId, const SegmentProbe& probe) {
  --pendingProbes_;
  const auto row = store_.getSegment(segmentId);
  if (row && row->state == QLatin1String("recording")) {
    if (probe.readable) {
      store_.finalizeSegment(segmentId, row->startPtsNs + probe.durationNs, row->startUtcMs + probe.durationNs / 1'000'000,
                             probe.bytes, utcNowMs());
    } else {
      store_.markSegmentDamaged(segmentId, probe.bytes);
      qWarning("camera %s: segment %s left unreadable", qPrintable(camera_.id), qPrintable(QFileInfo(row->path).fileName()));
    }
  }
  emitIdleIfDone();
}

void CameraPipeline::onFirstPacket(uint64_t runId) {
  if (!run_ || run_->id != runId) return;
  int64_t firstPts = 0, startedUtc = 0;
  {
    std::lock_guard<std::mutex> lock(run_->mutex);
    firstPts = run_->stats.clock.firstPtsNs();
    startedUtc = run_->stats.clock.startedUtcMs();
  }
  store_.setSessionAnchor(run_->sessionId, firstPts, startedUtc);
  run_->online = true;
  run_->onlineSinceMono = monoNowNs();
  state_ = QStringLiteral("online");
  sinceUtcMs_ = startedUtc;
  lastError_.clear();
  closeOpenGap(startedUtc);
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

// The session closes and a new one opens at once. Only a session that jumps
// again soon after going online takes the reconnect backoff, so a stream with
// broken timestamps cannot spin connections.
void CameraPipeline::onPtsBackwards(uint64_t runId) {
  if (!run_ || run_->id != runId || run_->stopping) return;
  const QString reason = QString::fromLatin1(SessionClock::kBackwardsReason);
  if (!run_->online || monoNowNs() - run_->onlineSinceMono < kBackwardsRestartAfterNs) {
    qWarning("camera %s: pts jumped backwards soon after connecting, reconnecting", qPrintable(camera_.id));
    failRun(reason, QStringLiteral("pts jumped backwards"));
    return;
  }
  qWarning("camera %s: pts jumped backwards, starting a new session", qPrintable(camera_.id));
  lastError_ = QStringLiteral("pts jumped backwards");
  restartRun(reason);
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
  retireRun(reason);
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

// Ends the session now and hands the pipeline to the retiring list. The EOS
// is serialized, so sending it takes the tee's stream lock, which a streaming
// thread stalled on a slow disk can hold indefinitely: it goes out from a
// GStreamer pool thread, and a timer bounds the wait for it.
void CameraPipeline::retireRun(const QString& reason) {
  std::unique_ptr<Run> run = std::move(run_);
  tickTimer_.stop();
  run->stopping = true;
  drain(*run);
  closeSession(*run, reason);
  const uint64_t runId = run->id;
  if (run->chainReady.load(std::memory_order_acquire) && !run->eosSeen && run->teeSink) {
    auto job = std::make_unique<Run::EosJob>();
    job->pad = GST_PAD(gst_object_ref(run->teeSink));
    job->mailbox = mailbox_;
    job->owner = this;
    job->runId = runId;
    gst_element_call_async(run->tee, Run::sendEos, job.release(), nullptr);
    QTimer::singleShot(kStopWait, this, [this, runId] { finishRetire(runId); });
  } else {
    QMetaObject::invokeMethod(this, [this, runId] { finishRetire(runId); }, Qt::QueuedConnection);
  }
  retiring_.push_back(std::move(run));
}

void CameraPipeline::closeSession(Run& run, const QString& reason) {
  const int64_t now = utcNowMs();
  uint64_t packets = 0;
  int64_t lastPacketEndUtc = 0;
  {
    std::lock_guard<std::mutex> lock(run.mutex);
    packets = run.stats.packets;
    lastPacketEndUtc = run.stats.lastPacketEndUtc;
    if (run.stats.lastFrameRecvMono > 0) lastFrameRecvMonoNs_ = run.stats.lastFrameRecvMono;
  }
  if (run.cam.recordEnabled)
    recordingState_ = run.recordingState == QLatin1String("error") ? run.recordingState : QStringLiteral("recording");
  const bool graceful = reason == QLatin1String("stopped") || reason == QLatin1String("shutdown");
  if (graceful) {
    closeOpenGap(now);
  } else if (packets > 0 && openGapId_.isEmpty()) {
    ReceiveGap gap;
    gap.id = newId();
    gap.cameraId = camera_.id;
    gap.sessionId = run.sessionId;
    gap.fromUtcMs = lastPacketEndUtc;
    gap.reason = reason == QLatin1String("eos") ? QStringLiteral("eos") : QStringLiteral("reconnect");
    if (store_.insertGap(gap)) openGapId_ = gap.id;
  }
  store_.endSession(run.sessionId, reason, std::max(now, lastPacketEndUtc));
  qInfo("camera %s: session %s ended (%s)", qPrintable(camera_.id), qPrintable(run.sessionId), qPrintable(reason));
}

void CameraPipeline::finishRetire(uint64_t runId) {
  const auto it = std::find_if(retiring_.begin(), retiring_.end(),
                               [runId](const std::unique_ptr<Run>& r) { return r->id == runId; });
  if (it == retiring_.end()) return;
  std::unique_ptr<Run> run = std::move(*it);
  retiring_.erase(it);
  drain(*run);
  if (run->chainReady.load(std::memory_order_acquire) && !run->eosSeen)
    qWarning("camera %s: session %s saw no EOS, forcing teardown", qPrintable(camera_.id), qPrintable(run->sessionId));
  disposeRun(std::move(run));
}

void CameraPipeline::disposeRun(std::unique_ptr<Run> run) {
  {
    std::lock_guard<std::mutex> lock(run->ringMutex);
    run->ring.reset();
  }
  auto job = std::make_unique<Run::Teardown>();
  for (auto it = run->openSegmentPaths.cbegin(); it != run->openSegmentPaths.cend(); ++it)
    job->leftovers.emplace_back(it.key(), it.value());
  ++pendingTeardowns_;
  pendingProbes_ += static_cast<int>(job->leftovers.size());
  GstElement* pipeline = run->pipeline;
  job->run = std::move(run);
  gst_element_call_async(pipeline, Run::teardown, job.release(), nullptr);
}

void CameraPipeline::closeOpenGap(int64_t toUtcMs) {
  if (openGapId_.isEmpty()) return;
  store_.closeGap(openGapId_, toUtcMs);
  openGapId_.clear();
}

void CameraPipeline::emitIdleIfDone() {
  if (idle()) emit idleReached(camera_.id);
}

void CameraPipeline::tick() {
  if (!run_ || run_->stopping) return;
  uint64_t packets = 0, bytes = 0, resumeCount = 0;
  int64_t lastPacketMono = 0, lastPacketEndUtc = 0, resumeFromUtc = 0, resumeToUtc = 0;
  {
    std::lock_guard<std::mutex> lock(run_->mutex);
    const RunStats& st = run_->stats;
    packets = st.packets;
    bytes = st.bytes;
    resumeCount = st.resumeCount;
    lastPacketMono = st.lastPacketMono;
    lastPacketEndUtc = st.lastPacketEndUtc;
    resumeFromUtc = st.resumeFromUtc;
    resumeToUtc = st.resumeToUtc;
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
      closeOpenGap(resumeToUtc);
    } else {
      ReceiveGap gap;
      gap.id = newId();
      gap.cameraId = camera_.id;
      gap.sessionId = run_->sessionId;
      gap.fromUtcMs = resumeFromUtc;
      gap.toUtcMs = resumeToUtc;
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
    gap.fromUtcMs = lastPacketEndUtc;
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
  checkDiskFloor();
}

CameraStatus CameraPipeline::status() const {
  CameraStatus s;
  s.cameraId = camera_.id;
  s.state = state_;
  s.sinceUtcMs = sinceUtcMs_;
  s.reconnects = reconnects_;
  s.lastError = lastError_;
  if (!camera_.recordEnabled) s.recording = QStringLiteral("disabled");
  else if (run_) s.recording = run_->recordingState;
  else s.recording = diskPaused_ ? QStringLiteral("paused_disk") : recordingState_;
  const int64_t now = monoNowNs();
  int64_t lastFrameMono = lastFrameRecvMonoNs_;
  if (run_) {
    s.sessionId = run_->sessionId;
    s.currentSegmentId = run_->currentSegmentId;
    std::vector<int64_t> latencySamples;
    {
      std::lock_guard<std::mutex> lock(run_->mutex);
      RunStats& st = run_->stats;
      pruneFrameStats(st, now);
      s.codec = st.codec;
      s.width = st.width;
      s.height = st.height;
      if (st.lastFrameRecvMono > 0) lastFrameMono = st.lastFrameRecvMono;
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
  }
  // Carried across sessions within one core run, so a reconnecting camera
  // still reports how long ago its last frame arrived.
  if (lastFrameMono > 0) {
    s.lastFrameRecvMonoNs = lastFrameMono;
    s.lastFrameAgeMs = (now - lastFrameMono) / 1'000'000;
    s.stale = s.lastFrameAgeMs > config_.staleAfterMs;
  }
  return s;
}

QJsonObject CameraPipeline::metrics() const {
  const CameraStatus s = status();
  QJsonObject m{{"camera_id", camera_.id}, {"state", s.state}, {"session_id", s.sessionId},
                {"fps_new", s.fpsNew}, {"latency_ms", s.latency.toJson()}, {"drops", static_cast<double>(s.drops)},
                {"queue_depth", s.queueDepth}, {"queue_max", static_cast<int>(kViewQueueMaxBuffers)},
                {"reconnects", s.reconnects}, {"recording", s.recording},
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
