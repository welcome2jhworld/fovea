#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

GST_DEBUG_CATEGORY_STATIC(cat);
#define GST_CAT_DEFAULT cat

namespace {

struct Options {
  gint port = 8554;
  gchar* path = nullptr;
  gchar* file = nullptr;
  gchar* pattern = nullptr;
  gboolean webcam = FALSE;
  gint device_index = 0;
  gboolean list_devices = FALSE;
  gint width = 0;
  gint height = 0;
  gint fps = 25;
  gint keyint = 25;
  gint bitrate_kbps = 800;
  gboolean overlay = FALSE;
  gboolean no_overlay = FALSE;
  gboolean loop = FALSE;
  gint stop_after = 0;
};

Options opt;

const GOptionEntry kEntries[] = {
    {"port", 'p', 0, G_OPTION_ARG_INT, &opt.port, "Listen port on 127.0.0.1 (0 = ephemeral)", "8554"},
    {"path", 0, 0, G_OPTION_ARG_STRING, &opt.path, "Mount path", "/test"},
    {"file", 'f', 0, G_OPTION_ARG_FILENAME, &opt.file, "Serve an H.264 MP4/MOV file instead of a test pattern", "FILE"},
    {"webcam", 0, 0, G_OPTION_ARG_NONE, &opt.webcam, "Serve a capture device (avfvideosrc on macOS) instead of a test pattern", nullptr},
    {"device-index", 0, 0, G_OPTION_ARG_INT, &opt.device_index, "Capture device for --webcam (see --list-devices)", "0"},
    {"list-devices", 0, 0, G_OPTION_ARG_NONE, &opt.list_devices, "List video capture devices and exit", nullptr},
    {"pattern", 0, 0, G_OPTION_ARG_STRING, &opt.pattern, "videotestsrc pattern (smpte, ball, snow, ...)", "ball"},
    {"width", 0, 0, G_OPTION_ARG_INT, &opt.width, "Frame width (pattern 640, webcam 1280)", "W"},
    {"height", 0, 0, G_OPTION_ARG_INT, &opt.height, "Frame height (pattern 360, webcam 720)", "H"},
    {"fps", 0, 0, G_OPTION_ARG_INT, &opt.fps, "Frame rate for pattern and webcam", "25"},
    {"keyint", 0, 0, G_OPTION_ARG_INT, &opt.keyint, "Keyframe interval in frames", "25"},
    {"bitrate-kbps", 0, 0, G_OPTION_ARG_INT, &opt.bitrate_kbps, "Encoder bitrate", "800"},
    {"overlay", 0, 0, G_OPTION_ARG_NONE, &opt.overlay, "Burn running time and wall clock into the video (default for pattern and webcam; transcodes files)", nullptr},
    {"no-overlay", 0, 0, G_OPTION_ARG_NONE, &opt.no_overlay, "Disable the overlay for pattern and webcam", nullptr},
    {"loop", 0, 0, G_OPTION_ARG_NONE, &opt.loop, "Loop the file seamlessly instead of ending with EOS", nullptr},
    {"stop-after", 0, 0, G_OPTION_ARG_INT, &opt.stop_after, "Exit after N seconds (simulates a camera going away)", "SEC"},
    {nullptr, 0, 0, G_OPTION_ARG_NONE, nullptr, nullptr, nullptr},
};

constexpr const char* kOverlay = "timeoverlay ! textoverlay name=wallclock halignment=right valignment=top";
constexpr const char* kPayloader = "rtph264pay name=pay0 pt=96 config-interval=1";

#if defined(__APPLE__)
constexpr const char* kCameraFactory = "avfvideosrc";
constexpr const char* kCameraDeviceArg = "device-index=";
// Left unconstrained, avfvideosrc negotiates ARGB and then fails in coremediabuffer
// ("Unknown OSType format: 32", GStreamer 1.26.1). NV12 is also vtenc_h264's native input.
constexpr const char* kCameraFormat = ",format=NV12";
#elif defined(_WIN32)
constexpr const char* kCameraFactory = "ksvideosrc";
constexpr const char* kCameraDeviceArg = "device-index=";
constexpr const char* kCameraFormat = "";
#else
constexpr const char* kCameraFactory = "v4l2src";
constexpr const char* kCameraDeviceArg = "device=/dev/video";
constexpr const char* kCameraFormat = "";
#endif

struct Size {
  gint width = 0;
  gint height = 0;
};

bool have_element(const char* factory) {
  GstElementFactory* f = gst_element_factory_find(factory);
  if (!f) return false;
  gst_object_unref(f);
  return true;
}

GList* video_sources() {
  GstDeviceMonitor* monitor = gst_device_monitor_new();
  gst_device_monitor_add_filter(monitor, "Video/Source", nullptr);
  GList* devices = nullptr;
  if (gst_device_monitor_start(monitor)) {
    devices = gst_device_monitor_get_devices(monitor);
    gst_device_monitor_stop(monitor);
  }
  g_object_unref(monitor);
  return devices;
}

std::vector<Size> device_sizes(GstDevice* device) {
  std::vector<Size> sizes;
  GstCaps* caps = gst_device_get_caps(device);
  if (!caps) return sizes;
  for (guint i = 0; i < gst_caps_get_size(caps); ++i) {
    const GstStructure* s = gst_caps_get_structure(caps, i);
    Size size;
    if (!gst_structure_get_int(s, "width", &size.width) || !gst_structure_get_int(s, "height", &size.height)) continue;
    const bool seen = std::any_of(sizes.begin(), sizes.end(),
                                  [&](const Size& o) { return o.width == size.width && o.height == size.height; });
    if (!seen) sizes.push_back(size);
  }
  gst_caps_unref(caps);
  return sizes;
}

// The provider configures the element for its device; on avfvideosrc/ksvideosrc that is device-index.
gint device_index_of(GstDevice* device, gint position) {
  GstElement* element = gst_device_create_element(device, nullptr);
  if (!element) return position;
  gst_object_ref_sink(element);
  gint index = position;
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(element), "device-index")) {
    g_object_get(element, "device-index", &index, nullptr);
  }
  gst_object_unref(element);
  return index;
}

// Smallest native size that covers the target with the closest aspect ratio, else the largest one.
Size pick_source_size(const std::vector<Size>& native, Size target) {
  const auto ratio = [](const Size& s) { return std::log(static_cast<double>(s.width) / s.height); };
  const auto area = [](const Size& s) { return static_cast<gint64>(s.width) * s.height; };
  const double target_ratio = ratio(target);
  const auto better = [&](const Size& s, const Size& best) {
    const double ds = std::fabs(ratio(s) - target_ratio);
    const double db = std::fabs(ratio(best) - target_ratio);
    if (std::fabs(ds - db) > 1e-6) return ds < db;
    return area(s) < area(best);
  };
  Size chosen;
  for (const Size& s : native) {
    if (s.width < target.width || s.height < target.height) continue;
    if (chosen.width == 0 || better(s, chosen)) chosen = s;
  }
  if (chosen.width == 0) {
    for (const Size& s : native) {
      if (area(s) > area(chosen)) chosen = s;
    }
  }
  return chosen;
}

// Native size to pin for --device-index; nullopt when devices were listed but none has that
// index. An empty list means no provider probed, so nothing is pinned and the element decides.
std::optional<Size> camera_source_size(Size target) {
  GList* devices = video_sources();
  if (!devices) return Size{};
  std::optional<Size> chosen;
  gint position = 0;
  for (GList* l = devices; l; l = l->next, ++position) {
    GstDevice* device = GST_DEVICE(l->data);
    if (device_index_of(device, position) != opt.device_index) continue;
    chosen = pick_source_size(device_sizes(device), target);
    break;
  }
  g_list_free_full(devices, gst_object_unref);
  return chosen;
}

int list_devices() {
  struct Row {
    gint index;
    std::string text;
  };
  std::vector<Row> rows;
  GList* devices = video_sources();
  gint position = 0;
  for (GList* l = devices; l; l = l->next, ++position) {
    GstDevice* device = GST_DEVICE(l->data);
    gchar* name = gst_device_get_display_name(device);
    std::string text = name;
    for (const Size& s : device_sizes(device)) text += " " + std::to_string(s.width) + "x" + std::to_string(s.height);
    rows.push_back({device_index_of(device, position), text});
    g_free(name);
  }
  g_list_free_full(devices, gst_object_unref);
  if (rows.empty()) {
    g_print("no video capture devices found\n");
    return 0;
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.index < b.index; });
  for (const Row& row : rows) g_print("%d  %s\n", row.index, row.text.c_str());
  return 0;
}

std::string x264_encoder() {
  return "x264enc tune=zerolatency speed-preset=ultrafast bframes=0 key-int-max=" + std::to_string(opt.keyint) +
         " bitrate=" + std::to_string(opt.bitrate_kbps);
}

std::string webcam_encoder() {
  if (!have_element("vtenc_h264")) return x264_encoder();
  return "vtenc_h264 realtime=true allow-frame-reordering=false max-keyframe-interval=" +
         std::to_string(opt.keyint) + " bitrate=" + std::to_string(opt.bitrate_kbps);
}

// Pinning the size and pixel aspect on the device side keeps videoscale from picking the
// first format the camera lists (a portrait one on MacBooks) and lets it letterbox instead
// of stretching when the native aspect differs.
std::string camera_source(Size source) {
  std::string caps = std::string("video/x-raw") + kCameraFormat + ",pixel-aspect-ratio=1/1";
  if (source.width > 0) caps += ",width=" + std::to_string(source.width) + ",height=" + std::to_string(source.height);
  return std::string(kCameraFactory) + " " + kCameraDeviceArg + std::to_string(opt.device_index) + " ! " + caps;
}

std::string raw_caps() {
  return "video/x-raw,width=" + std::to_string(opt.width) + ",height=" + std::to_string(opt.height) +
         ",framerate=" + std::to_string(opt.fps) + "/1";
}

std::string build_launch(bool overlay, Size camera) {
  std::string s = "( ";
  if (opt.file) {
    s += "filesrc location=\"" + std::string(opt.file) + "\" ! qtdemux name=demux ! h264parse ! ";
    if (overlay) s += "avdec_h264 ! videoconvert ! " + std::string(kOverlay) + " ! " + x264_encoder() + " ! ";
  } else if (opt.webcam) {
    s += camera_source(camera) + " ! videoconvert ! videoscale ! videorate skip-to-first=true ! " + raw_caps() +
         ",pixel-aspect-ratio=1/1 ! ";
    if (overlay) s += std::string(kOverlay) + " ! ";
    s += "videoconvert ! " + webcam_encoder() + " ! h264parse config-interval=1 ! ";
  } else {
    s += "videotestsrc is-live=true pattern=" + std::string(opt.pattern ? opt.pattern : "ball") + " ! " + raw_caps() +
         " ! ";
    if (overlay) s += std::string(kOverlay) + " ! ";
    s += "videoconvert ! " + x264_encoder() + " ! ";
  }
  return s + kPayloader + " )";
}

GstPadProbeReturn wallclock_probe(GstPad*, GstPadProbeInfo*, gpointer overlay) {
  GDateTime* now = g_date_time_new_now_local();
  gchar* hms = g_date_time_format(now, "%H:%M:%S");
  gchar* text = g_strdup_printf("%s.%03d", hms, g_date_time_get_microsecond(now) / 1000);
  g_object_set(overlay, "text", text, nullptr);
  g_free(text);
  g_free(hms);
  g_date_time_unref(now);
  return GST_PAD_PROBE_OK;
}

GstPadProbeReturn open_ended_segment(GstPad*, GstPadProbeInfo* info, gpointer) {
  GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
  if (GST_EVENT_TYPE(event) != GST_EVENT_SEGMENT) return GST_PAD_PROBE_OK;
  const GstSegment* segment = nullptr;
  gst_event_parse_segment(event, &segment);
  if (segment->stop == GST_CLOCK_TIME_NONE && segment->duration == GST_CLOCK_TIME_NONE) return GST_PAD_PROBE_OK;
  GST_DEBUG("hiding segment end %" GST_TIME_FORMAT, GST_TIME_ARGS(segment->stop));
  GstSegment open = *segment;
  open.stop = GST_CLOCK_TIME_NONE;
  open.duration = GST_CLOCK_TIME_NONE;
  GstEvent* replacement = gst_event_new_segment(&open);
  gst_event_set_seqnum(replacement, gst_event_get_seqnum(event));
  gst_event_unref(event);
  GST_PAD_PROBE_INFO_DATA(info) = replacement;
  return GST_PAD_PROBE_OK;
}

GstPadProbeReturn open_ended_query(GstPad*, GstPadProbeInfo* info, gpointer) {
  GstQuery* query = GST_PAD_PROBE_INFO_QUERY(info);
  if (GST_QUERY_TYPE(query) != GST_QUERY_SEGMENT) return GST_PAD_PROBE_OK;
  gdouble rate = 1.0;
  GstFormat format = GST_FORMAT_TIME;
  gint64 start = 0;
  gint64 stop = -1;
  gst_query_parse_segment(query, &rate, &format, &start, &stop);
  gst_query_set_segment(query, rate, format, start, -1);
  return GST_PAD_PROBE_OK;
}

struct LoopCtx {
  GstElement* demux;
  GstBus* bus;
  gulong bus_handler;
  bool armed;
};

gboolean seek_to_start(gpointer demux) {
  gboolean ok = gst_element_seek(GST_ELEMENT(demux), 1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_SEGMENT,
                                 GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
  GST_DEBUG("segment seek to start: %s", ok ? "ok" : "failed");
  return G_SOURCE_REMOVE;
}

void schedule_seek(GstElement* demux) {
  g_idle_add_full(G_PRIORITY_DEFAULT, seek_to_start, gst_object_ref(demux), gst_object_unref);
}

void on_segment_done(GstBus*, GstMessage*, gpointer data) {
  schedule_seek(static_cast<LoopCtx*>(data)->demux);
}

void on_new_state(GstRTSPMedia*, gint state, gpointer data) {
  auto* ctx = static_cast<LoopCtx*>(data);
  if (state != GST_STATE_PLAYING || ctx->armed) return;
  ctx->armed = true;
  schedule_seek(ctx->demux);
}

void free_loop_ctx(gpointer data) {
  auto* ctx = static_cast<LoopCtx*>(data);
  g_signal_handler_disconnect(ctx->bus, ctx->bus_handler);
  gst_object_unref(ctx->bus);
  gst_object_unref(ctx->demux);
  delete ctx;
}

void on_media_configure(GstRTSPMediaFactory*, GstRTSPMedia* media, gpointer) {
  GstElement* bin = gst_rtsp_media_get_element(media);
  if (GstElement* overlay = gst_bin_get_by_name(GST_BIN(bin), "wallclock")) {
    GstPad* pad = gst_element_get_static_pad(overlay, "video_sink");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, wallclock_probe, overlay, nullptr);
    gst_object_unref(pad);
    gst_object_unref(overlay);
  }
  if (opt.loop) {
    GstElement* demux = gst_bin_get_by_name(GST_BIN(bin), "demux");
    GstElement* pipeline = GST_ELEMENT(gst_object_get_parent(GST_OBJECT(bin)));
    if (demux && pipeline) {
      GstElement* pay = gst_bin_get_by_name(GST_BIN(bin), "pay0");
      GstPad* pad = gst_element_get_static_pad(pay, "src");
      gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, open_ended_segment, nullptr, nullptr);
      gst_pad_add_probe(pad, static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_QUERY_UPSTREAM | GST_PAD_PROBE_TYPE_PULL),
                        open_ended_query, nullptr, nullptr);
      gst_object_unref(pad);
      gst_object_unref(pay);
      auto* ctx = new LoopCtx{demux, gst_element_get_bus(pipeline), 0, false};
      gst_bus_enable_sync_message_emission(ctx->bus);
      ctx->bus_handler = g_signal_connect(ctx->bus, "sync-message::segment-done", G_CALLBACK(on_segment_done), ctx);
      g_signal_connect(media, "new-state", G_CALLBACK(on_new_state), ctx);
      g_object_set_data_full(G_OBJECT(media), "fovea-loop", ctx, free_loop_ctx);
    } else if (demux) {
      gst_object_unref(demux);
    }
    if (pipeline) gst_object_unref(pipeline);
  }
  gst_object_unref(bin);
}

gboolean quit_main_loop(gpointer loop) {
  g_main_loop_quit(static_cast<GMainLoop*>(loop));
  return G_SOURCE_REMOVE;
}

}  // namespace

int main(int argc, char** argv) {
  GOptionContext* octx = g_option_context_new("- local RTSP H.264 test source (127.0.0.1 only)");
  g_option_context_add_main_entries(octx, kEntries, nullptr);
  g_option_context_add_group(octx, gst_init_get_option_group());
  GError* err = nullptr;
  if (!g_option_context_parse(octx, &argc, &argv, &err)) {
    g_printerr("rtsp-testsrc: %s\n", err->message);
    return 2;
  }
  g_option_context_free(octx);
  GST_DEBUG_CATEGORY_INIT(cat, "rtsp-testsrc", 0, "Fovea RTSP test source");

  if (opt.list_devices) return list_devices();

  std::string path = opt.path ? opt.path : "/test";
  if (path.empty() || path[0] != '/') path.insert(0, "/");
  if (opt.file && opt.webcam) {
    g_printerr("rtsp-testsrc: --file and --webcam are mutually exclusive\n");
    return 2;
  }
  if (opt.file && !g_file_test(opt.file, G_FILE_TEST_IS_REGULAR)) {
    g_printerr("rtsp-testsrc: no such file: %s\n", opt.file);
    return 2;
  }
  if (opt.width <= 0) opt.width = opt.webcam ? 1280 : 640;
  if (opt.height <= 0) opt.height = opt.webcam ? 720 : 360;
  Size camera;
  if (opt.webcam) {
    if (!have_element(kCameraFactory)) {
      g_printerr("rtsp-testsrc: capture element '%s' is not available in this GStreamer installation\n", kCameraFactory);
      return 2;
    }
    const std::optional<Size> native = camera_source_size({opt.width, opt.height});
    if (!native) {
      g_printerr("rtsp-testsrc: no capture device with index %d (see --list-devices)\n", opt.device_index);
      return 2;
    }
    camera = *native;
  }
  const bool overlay = opt.overlay || (!opt.file && !opt.no_overlay);
  const std::string launch = build_launch(overlay, camera);

  GstElement* check = gst_parse_launch_full(launch.c_str(), nullptr, GST_PARSE_FLAG_FATAL_ERRORS, &err);
  if (!check) {
    g_printerr("rtsp-testsrc: invalid pipeline: %s\n", err->message);
    return 2;
  }
  gst_object_unref(check);

  GstRTSPServer* server = gst_rtsp_server_new();
  gst_rtsp_server_set_address(server, "127.0.0.1");
  gst_rtsp_server_set_service(server, std::to_string(opt.port).c_str());

  GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
  gst_rtsp_media_factory_set_launch(factory, launch.c_str());
  gst_rtsp_media_factory_set_shared(factory, TRUE);
  gst_rtsp_media_factory_set_protocols(
      factory, static_cast<GstRTSPLowerTrans>(GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_TCP));
  g_signal_connect(factory, "media-configure", G_CALLBACK(on_media_configure), nullptr);

  GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server);
  gst_rtsp_mount_points_add_factory(mounts, path.c_str(), factory);
  g_object_unref(mounts);

  const guint attach_id = gst_rtsp_server_attach(server, nullptr);
  if (attach_id == 0) {
    g_printerr("rtsp-testsrc: failed to bind 127.0.0.1:%d\n", opt.port);
    return 1;
  }

  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
#ifdef G_OS_UNIX
  g_unix_signal_add(SIGINT, quit_main_loop, loop);
  g_unix_signal_add(SIGTERM, quit_main_loop, loop);
#endif
  if (opt.stop_after > 0) g_timeout_add_seconds(static_cast<guint>(opt.stop_after), quit_main_loop, loop);

  g_print("rtsp://127.0.0.1:%d%s\n", gst_rtsp_server_get_bound_port(server), path.c_str());
  g_print("launch: %s\n", launch.c_str());
  fflush(stdout);

  g_main_loop_run(loop);

  g_source_remove(attach_id);
  g_main_loop_unref(loop);
  g_object_unref(server);
  return 0;
}
