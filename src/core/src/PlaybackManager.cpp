#include "fovea/core/PlaybackManager.h"
#include "fovea/Clock.h"
#include "fovea/FrameRing.h"
#include "fovea/Ids.h"
#include "fovea/core/MediaProbe.h"
#include "fovea/core/Store.h"
#include <QFileInfo>
#include <QMetaObject>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>

namespace fovea::core {
namespace {

constexpr GstClockTime kPrerollTimeout = 8 * GST_SECOND;
constexpr GstClockTime kSeekSettleTimeout = 2 * GST_SECOND;

QString codecFromCaps(GstCaps* caps) {
  if (!caps || gst_caps_get_size(caps) == 0) return {};
  const QString name = QString::fromLatin1(gst_structure_get_name(gst_caps_get_structure(caps, 0)));
  if (name == QLatin1String("video/x-h264")) return QStringLiteral("h264");
  if (name == QLatin1String("video/x-h265")) return QStringLiteral("h265");
  return {};
}

QString demuxerForPath(const QString& path) {
  const QString ext = QFileInfo(path).suffix().toLower();
  if (ext == QLatin1String("mkv") || ext == QLatin1String("webm")) return QStringLiteral("matroskademux");
  return QStringLiteral("qtdemux");
}

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

}

struct PlaybackManager::Impl {
  struct Channel {
    Impl* mgr = nullptr;
    PlaybackState st;
    uint32_t ringMaxWidth = 0;
    uint32_t ringMaxHeight = 0;
    GstElement* pipeline = nullptr;
    GstElement* demux = nullptr;
    GstElement* capsFilter = nullptr;
    GstElement* appsink = nullptr;
    GstElement* head = nullptr;
    GstBus* bus = nullptr;
    std::atomic<bool> linked{false};
    std::atomic<bool> errorPosted{false};
    std::atomic<int64_t> lastFramePts{-1};
    std::unique_ptr<FrameRingWriter> ring;
    std::array<uint8_t, 16> sessionBytes{};
    GstVideoInfo videoInfo{};
    GstCaps* sinkCaps = nullptr;
    int64_t basePtsNs = -1;
    std::mutex messageMutex;
    std::deque<GstMessage*> messages;

    ~Channel() {
      if (sinkCaps) gst_caps_unref(sinkCaps);
      if (bus) {
        gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
        gst_object_unref(bus);
      }
      if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
      }
      for (GstMessage* m : messages) gst_message_unref(m);
    }

    void postError(const QString& text) {
      if (errorPosted.exchange(true)) return;
      GError* err = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED, text.toUtf8().constData());
      gst_element_post_message(pipeline, gst_message_new_error(GST_OBJECT(pipeline), err, nullptr));
      g_error_free(err);
    }

    static GstBusSyncReply onBusSync(GstBus*, GstMessage* msg, gpointer user) {
      auto* ch = static_cast<Channel*>(user);
      const GstMessageType type = GST_MESSAGE_TYPE(msg);
      if (type != GST_MESSAGE_ERROR && type != GST_MESSAGE_EOS) return GST_BUS_DROP;
      {
        std::lock_guard<std::mutex> lock(ch->messageMutex);
        ch->messages.push_back(gst_message_ref(msg));
      }
      Impl* mgr = ch->mgr;
      const QString id = ch->st.id;
      QMetaObject::invokeMethod(mgr->self, [mgr, id] { mgr->drain(id); }, Qt::QueuedConnection);
      return GST_BUS_DROP;
    }

    static void onPadAdded(GstElement*, GstPad* pad, gpointer user) {
      auto* ch = static_cast<Channel*>(user);
      GstCaps* caps = gst_pad_get_current_caps(pad);
      if (!caps) caps = gst_pad_query_caps(pad, nullptr);
      const QString codec = codecFromCaps(caps);
      if (caps) gst_caps_unref(caps);
      if (codec.isEmpty() || ch->linked.exchange(true)) return;
      GstPad* sink = gst_element_get_static_pad(ch->head, "sink");
      const GstPadLinkReturn link = gst_pad_link(pad, sink);
      gst_object_unref(sink);
      if (link != GST_PAD_LINK_OK) ch->postError(QStringLiteral("cannot link demuxer pad (%1)").arg(static_cast<int>(link)));
    }

    // The decode chain exists before the first state change so the PAUSED
    // transition prerolls through it; only the demuxer pad link is dynamic.
    bool createChain(const QString& codec) {
      const bool h264 = codec == QLatin1String("h264");
      GstElement* parse = gst_element_factory_make(h264 ? "h264parse" : "h265parse", nullptr);
      GstElement* dec = gst_element_factory_make(h264 ? "avdec_h264" : "avdec_h265", nullptr);
      GstElement* conv = gst_element_factory_make("videoconvert", nullptr);
      GstElement* scale = gst_element_factory_make("videoscale", nullptr);
      capsFilter = gst_element_factory_make("capsfilter", nullptr);
      appsink = gst_element_factory_make("appsink", nullptr);
      if (!parse || !dec || !conv || !scale || !capsFilter || !appsink) {
        for (GstElement* e : {parse, dec, conv, scale, capsFilter, appsink})
          if (e) gst_object_unref(e);
        capsFilter = appsink = nullptr;
        return false;
      }
      GstCaps* caps = bgraCaps(0, 0);
      g_object_set(capsFilter, "caps", caps, nullptr);
      gst_caps_unref(caps);
      g_object_set(appsink, "sync", TRUE, "max-buffers", 4u, "drop", FALSE, "emit-signals", FALSE, nullptr);
      GstAppSinkCallbacks callbacks{};
      callbacks.new_preroll = onNewPreroll;
      callbacks.new_sample = onNewSample;
      gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, this, nullptr);

      gst_bin_add_many(GST_BIN(pipeline), parse, dec, conv, scale, capsFilter, appsink, nullptr);
      gst_element_link_many(parse, dec, conv, scale, capsFilter, appsink, nullptr);
      GstPad* parseSrc = gst_element_get_static_pad(parse, "src");
      gst_pad_add_probe(parseSrc, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, onParseEvent, this, nullptr);
      gst_object_unref(parseSrc);
      head = parse;
      return true;
    }

    static GstPadProbeReturn onParseEvent(GstPad*, GstPadProbeInfo* info, gpointer user) {
      auto* ch = static_cast<Channel*>(user);
      GstEvent* ev = GST_PAD_PROBE_INFO_EVENT(info);
      if (GST_EVENT_TYPE(ev) != GST_EVENT_CAPS) return GST_PAD_PROBE_OK;
      GstCaps* caps = nullptr;
      gst_event_parse_caps(ev, &caps);
      if (!caps || gst_caps_get_size(caps) == 0) return GST_PAD_PROBE_OK;
      const GstStructure* s = gst_caps_get_structure(caps, 0);
      int w = 0, h = 0;
      gst_structure_get_int(s, "width", &w);
      gst_structure_get_int(s, "height", &h);
      gint pn = 1, pd = 1;
      gst_structure_get_fraction(s, "pixel-aspect-ratio", &pn, &pd);
      if (pn > 0 && pd > 0 && pn != pd) w = static_cast<int>(static_cast<int64_t>(w) * pn / pd);
      if (w <= 0 || h <= 0) return GST_PAD_PROBE_OK;
      const auto [tw, th] = fitSize(w, h, ch->ringMaxWidth, ch->ringMaxHeight);
      GstCaps* target = bgraCaps(tw, th);
      g_object_set(ch->capsFilter, "caps", target, nullptr);
      gst_caps_unref(target);
      return GST_PAD_PROBE_OK;
    }

    void writeSample(GstSample* sample) {
      GstBuffer* buf = gst_sample_get_buffer(sample);
      GstCaps* caps = gst_sample_get_caps(sample);
      if (!buf || !caps) return;
      if (caps != sinkCaps) {
        if (!gst_video_info_from_caps(&videoInfo, caps)) return;
        if (sinkCaps) gst_caps_unref(sinkCaps);
        sinkCaps = gst_caps_ref(caps);
      }
      GstVideoFrame frame;
      if (!gst_video_frame_map(&frame, &videoInfo, buf, GST_MAP_READ)) return;
      const GstClockTime pts = GST_BUFFER_PTS(buf);
      FrameHeader header;
      header.ptsNs = GST_CLOCK_TIME_IS_VALID(pts) ? static_cast<uint64_t>(pts) : 0;
      header.recvMonoNs = static_cast<uint64_t>(monoNowNs());
      header.captureUtcMs = 0;
      header.width = static_cast<uint32_t>(GST_VIDEO_FRAME_WIDTH(&frame));
      header.height = static_cast<uint32_t>(GST_VIDEO_FRAME_HEIGHT(&frame));
      header.sessionId = sessionBytes;
      if (ring)
        ring->write(header, static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)),
                    static_cast<size_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0)));
      gst_video_frame_unmap(&frame);
      if (GST_CLOCK_TIME_IS_VALID(pts)) lastFramePts.store(static_cast<int64_t>(pts));
    }

    static GstFlowReturn onNewPreroll(GstAppSink* sink, gpointer user) {
      GstSample* sample = gst_app_sink_pull_preroll(sink);
      if (!sample) return GST_FLOW_OK;
      static_cast<Channel*>(user)->writeSample(sample);
      gst_sample_unref(sample);
      return GST_FLOW_OK;
    }

    static GstFlowReturn onNewSample(GstAppSink* sink, gpointer user) {
      GstSample* sample = gst_app_sink_pull_sample(sink);
      if (!sample) return GST_FLOW_OK;
      static_cast<Channel*>(user)->writeSample(sample);
      gst_sample_unref(sample);
      return GST_FLOW_OK;
    }

    // Position relative to the start of the file: the last frame handed to
    // the ring is what the viewer sees (its PTS minus the file's first PTS);
    // the position query is the fallback before any frame arrived.
    std::optional<int64_t> queryPosition() const {
      const int64_t last = lastFramePts.load();
      if (last >= 0) return std::max<int64_t>(0, last - std::max<int64_t>(basePtsNs, 0));
      gint64 pos = 0;
      if (gst_element_query_position(pipeline, GST_FORMAT_TIME, &pos) && pos >= 0) return pos;
      return std::nullopt;
    }

    // Flushing seek to a position relative to the start of the file (the
    // demuxer's seek domain, even when the buffers carry the session's
    // running time); waits for the new preroll so the position reported
    // right after is the target rather than the old one.
    bool seekTo(int64_t offsetNs, double rate) {
      const auto flags = static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE);
      if (!gst_element_seek(pipeline, rate, GST_FORMAT_TIME, flags, GST_SEEK_TYPE_SET, std::max<int64_t>(offsetNs, 0),
                            GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE)) {
        qWarning("playback %s: seek to %.3f s refused", qPrintable(st.id), static_cast<double>(offsetNs) / 1e9);
        return false;
      }
      gst_element_get_state(pipeline, nullptr, nullptr, kSeekSettleTimeout);
      return true;
    }
  };

  Impl(Store& s, PlaybackManager* owner, const CoreConfig& cfg) : store(s), self(owner), config(cfg) {}

  Store& store;
  PlaybackManager* self;
  const CoreConfig& config;
  std::map<QString, std::unique_ptr<Channel>> channels;

  Channel* find(const QString& id) {
    const auto it = channels.find(id);
    return it == channels.end() ? nullptr : it->second.get();
  }

  void refreshPosition(Channel& ch) {
    if (const auto pos = ch.queryPosition()) ch.st.positionNs = *pos;
  }

  void drain(const QString& id) {
    Channel* ch = find(id);
    if (!ch) return;
    std::deque<GstMessage*> pending;
    {
      std::lock_guard<std::mutex> lock(ch->messageMutex);
      pending.swap(ch->messages);
    }
    for (GstMessage* msg : pending) {
      if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError* err = nullptr;
        gst_message_parse_error(msg, &err, nullptr);
        ch->st.state = QStringLiteral("error");
        ch->st.lastError = QString::fromUtf8(err ? err->message : "unknown error");
        ch->st.playing = false;
        if (err) g_error_free(err);
        qWarning("playback %s: %s", qPrintable(id), qPrintable(ch->st.lastError));
      } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
        refreshPosition(*ch);
        ch->st.state = QStringLiteral("ended");
        ch->st.playing = false;
        if (ch->st.durationNs > 0) ch->st.positionNs = ch->st.durationNs;
      }
      gst_message_unref(msg);
    }
  }

  std::optional<PlaybackState> openSegment(const RecordingSegment& seg, int64_t offsetNs, QString* error) {
    if (static_cast<int>(channels.size()) >= config.maxPlaybackChannels) {
      if (error) *error = QStringLiteral("too many playback channels (max %1)").arg(config.maxPlaybackChannels);
      return std::nullopt;
    }
    if (seg.state == QLatin1String("deleted")) {
      if (error) *error = QStringLiteral("segment is deleted");
      return std::nullopt;
    }
    if (seg.state == QLatin1String("recording")) {
      if (error) *error = QStringLiteral("segment is still being recorded");
      return std::nullopt;
    }
    if (!QFileInfo::exists(seg.path)) {
      if (error) *error = QStringLiteral("recording file is missing");
      return std::nullopt;
    }
    auto ch = std::make_unique<Channel>();
    ch->mgr = this;
    ch->st.id = newId();
    ch->st.segmentId = seg.id;
    ch->st.cameraId = seg.cameraId;
    ch->st.path = seg.path;
    ch->st.startUtcMs = seg.startUtcMs;
    ch->st.durationNs = std::max<int64_t>(0, seg.endPtsNs - seg.startPtsNs);
    ch->ringMaxWidth = config.ringMaxWidth;
    ch->ringMaxHeight = config.ringMaxHeight;
    ch->sessionBytes = sessionIdBytes(seg.sessionId);
    ch->ring = FrameRingWriter::create(makeRingName(QStringLiteral("pb:") + ch->st.id), config.ringSlots, config.ringMaxWidth,
                                       config.ringMaxHeight);
    if (!ch->ring) {
      if (error) *error = QStringLiteral("cannot create frame ring");
      return std::nullopt;
    }

    ch->pipeline = gst_pipeline_new(nullptr);
    GstElement* source = gst_element_factory_make("filesrc", nullptr);
    ch->demux = gst_element_factory_make(demuxerForPath(seg.path).toUtf8().constData(), "demux");
    if (!source || !ch->demux) {
      if (source) gst_object_unref(source);
      if (ch->demux) gst_object_unref(ch->demux);
      ch->demux = nullptr;
      if (error) *error = QStringLiteral("missing GStreamer demuxer");
      return std::nullopt;
    }
    g_object_set(source, "location", seg.path.toUtf8().constData(), nullptr);
    gst_bin_add_many(GST_BIN(ch->pipeline), source, ch->demux, nullptr);
    gst_element_link(source, ch->demux);
    const QString codec = probeVideoCodec(seg.path);
    if (codec != QLatin1String("h264") && codec != QLatin1String("h265")) {
      if (error) *error = codec.isEmpty() ? QStringLiteral("recording is not readable")
                                          : QStringLiteral("unsupported codec %1").arg(codec.mid(codec.indexOf(':') + 1));
      return std::nullopt;
    }
    if (!ch->createChain(codec)) {
      if (error) *error = QStringLiteral("missing GStreamer elements for %1").arg(codec);
      return std::nullopt;
    }
    g_signal_connect(ch->demux, "pad-added", G_CALLBACK(Channel::onPadAdded), ch.get());
    ch->bus = gst_element_get_bus(ch->pipeline);
    gst_bus_set_sync_handler(ch->bus, Channel::onBusSync, ch.get(), nullptr);

    GstStateChangeReturn ret = gst_element_set_state(ch->pipeline, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_ASYNC) ret = gst_element_get_state(ch->pipeline, nullptr, nullptr, kPrerollTimeout);
    if (ret == GST_STATE_CHANGE_FAILURE || ret == GST_STATE_CHANGE_ASYNC) {
      if (error) *error = ret == GST_STATE_CHANGE_FAILURE ? QStringLiteral("cannot decode recording") : QStringLiteral("preroll timed out");
      return std::nullopt;
    }
    ch->basePtsNs = std::max<int64_t>(0, ch->lastFramePts.load());
    gint64 duration = 0;
    if (ch->st.durationNs <= 0 && gst_element_query_duration(ch->pipeline, GST_FORMAT_TIME, &duration) && duration > 0)
      ch->st.durationNs = duration;
    ch->st.state = QStringLiteral("ready");
    const RingInfo& info = ch->ring->info();
    ch->st.frameRing = RingRef{info.name, info.slotCount, info.slotBytes, info.maxWidth, info.maxHeight, QStringLiteral("BGRA")};
    if (offsetNs > 0) ch->seekTo(offsetNs, 1.0);
    refreshPosition(*ch);
    qInfo("playback %s: opened %s (%.1f s)", qPrintable(ch->st.id), qPrintable(QFileInfo(seg.path).fileName()),
          static_cast<double>(ch->st.durationNs) / 1e9);
    const PlaybackState state = ch->st;
    channels.emplace(state.id, std::move(ch));
    return state;
  }
};

PlaybackManager::PlaybackManager(Store& store, CoreConfig config, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(store, this, config_)), config_(std::move(config)) {}

PlaybackManager::~PlaybackManager() { closeAll(); }

std::optional<PlaybackState> PlaybackManager::open(const QString& segmentId, QString* error) {
  const auto seg = impl_->store.getSegment(segmentId);
  if (!seg) {
    if (error) *error = QStringLiteral("no such segment");
    return std::nullopt;
  }
  return impl_->openSegment(*seg, 0, error);
}

std::optional<PlaybackState> PlaybackManager::openAt(const QString& cameraId, int64_t atUtcMs, QString* error) {
  const QVector<RecordingSegment> segments = impl_->store.listSegments(cameraId, atUtcMs, atUtcMs, 50);
  const RecordingSegment* best = nullptr;
  for (const RecordingSegment& s : segments) {
    if (s.state != QLatin1String("finalized") && s.state != QLatin1String("damaged")) continue;
    if (s.startUtcMs <= atUtcMs && atUtcMs <= s.endUtcMs) {
      best = &s;
      break;
    }
  }
  if (!best) {
    if (error) *error = QStringLiteral("no recording covers that time");
    return std::nullopt;
  }
  return impl_->openSegment(*best, (atUtcMs - best->startUtcMs) * 1'000'000, error);
}

std::optional<PlaybackState> PlaybackManager::state(const QString& id) const {
  Impl::Channel* ch = impl_->find(id);
  if (!ch) return std::nullopt;
  if (ch->st.state == QLatin1String("playing") || ch->st.state == QLatin1String("paused") || ch->st.state == QLatin1String("ready"))
    impl_->refreshPosition(*ch);
  return ch->st;
}

bool PlaybackManager::play(const QString& id) {
  Impl::Channel* ch = impl_->find(id);
  if (!ch) return false;
  if (ch->st.state == QLatin1String("ended")) ch->seekTo(0, ch->st.rate);
  if (gst_element_set_state(ch->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) return false;
  ch->st.playing = true;
  ch->st.state = QStringLiteral("playing");
  return true;
}

bool PlaybackManager::pause(const QString& id) {
  Impl::Channel* ch = impl_->find(id);
  if (!ch) return false;
  if (gst_element_set_state(ch->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) return false;
  ch->st.playing = false;
  if (ch->st.state != QLatin1String("error")) ch->st.state = QStringLiteral("paused");
  impl_->refreshPosition(*ch);
  return true;
}

bool PlaybackManager::seek(const QString& id, int64_t ptsNs) {
  Impl::Channel* ch = impl_->find(id);
  if (!ch) return false;
  const int64_t target = std::clamp<int64_t>(ptsNs, 0, ch->st.durationNs > 0 ? ch->st.durationNs : ptsNs);
  if (!ch->seekTo(target, ch->st.rate)) return false;
  if (ch->st.state == QLatin1String("ended")) ch->st.state = ch->st.playing ? QStringLiteral("playing") : QStringLiteral("paused");
  ch->st.positionNs = target;
  return true;
}

bool PlaybackManager::setRate(const QString& id, double rate) {
  Impl::Channel* ch = impl_->find(id);
  if (!ch || !(rate > 0.0) || rate > 16.0) return false;
  impl_->refreshPosition(*ch);
  if (!ch->seekTo(ch->st.positionNs, rate)) return false;
  ch->st.rate = rate;
  return true;
}

bool PlaybackManager::close(const QString& id) {
  const auto it = impl_->channels.find(id);
  if (it == impl_->channels.end()) return false;
  qInfo("playback %s: closed", qPrintable(id));
  impl_->channels.erase(it);
  return true;
}

void PlaybackManager::closeAll() { impl_->channels.clear(); }

int PlaybackManager::openCount() const { return static_cast<int>(impl_->channels.size()); }

}
