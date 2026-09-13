#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// pipe_probe [--seconds N] [--every N] [--loop] "<launch description with appsink name=sink>"
// Prints negotiated caps, the pipeline LATENCY query, per-frame timing
// (PTS, buffer running time, clock running time, age = clock - buffer) and,
// for rtspsrc pipelines, the rtpjitterbuffer stats. --loop restarts a file
// source with a non-flushing segment seek on SEGMENT_DONE (gapless loop);
// name the demuxer "demux" so the seek bypasses sinks that refuse seeks.

namespace {

struct State {
    GstElement* pipeline = nullptr;
    GstElement* jitterbuffer = nullptr;
    GMainLoop* loop = nullptr;
    gint64 mono_start = 0;
    guint64 frames = 0;
    guint64 gaps = 0;
    GstClockTime last_pts = GST_CLOCK_TIME_NONE;
    double age_sum = 0;
    double age_max = -1e9;
    double age_min = 1e9;
    int every = 25;
    bool loop_file = false;
    bool loop_started = false;
    bool stopping = false;
};

double ms(GstClockTimeDiff t) { return static_cast<double>(t) / GST_MSECOND; }

GstFlowReturn on_sample(GstAppSink* sink, gpointer user) {
    auto* st = static_cast<State*>(user);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    GstBuffer* buf = gst_sample_get_buffer(sample);
    const GstSegment* seg = gst_sample_get_segment(sample);
    GstClockTime pts = GST_BUFFER_PTS(buf);
    GstClockTime rt = gst_segment_to_running_time(seg, GST_FORMAT_TIME, pts);

    GstClock* clock = gst_element_get_clock(st->pipeline);
    GstClockTime now_rt = GST_CLOCK_TIME_NONE;
    if (clock) {
        now_rt = gst_clock_get_time(clock) - gst_element_get_base_time(st->pipeline);
        gst_object_unref(clock);
    }
    gint64 mono_ms = (g_get_monotonic_time() - st->mono_start) / 1000;

    if (st->frames == 0) {
        gchar* caps = gst_caps_to_string(gst_sample_get_caps(sample));
        std::printf("caps: %s\n", caps);
        g_free(caps);
    }
    if (GST_CLOCK_TIME_IS_VALID(st->last_pts) && GST_CLOCK_TIME_IS_VALID(pts) &&
        pts > st->last_pts + 60 * GST_MSECOND)
        st->gaps++;
    st->last_pts = pts;

    double age = ms(GST_CLOCK_DIFF(rt, now_rt));
    st->age_sum += age;
    if (age > st->age_max) st->age_max = age;
    if (age < st->age_min) st->age_min = age;

    if (st->frames % st->every == 0)
        std::printf("frame %6llu pts %8.1f ms  run %8.1f ms  clock %8.1f ms  age %7.1f ms  mono %6lld ms  size %zu\n",
                    static_cast<unsigned long long>(st->frames), ms(pts), ms(rt), ms(now_rt), age,
                    static_cast<long long>(mono_ms), gst_buffer_get_size(buf));
    st->frames++;
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

void on_new_jitterbuffer(GstElement*, GstElement* jb, guint, guint, gpointer user) {
    auto* st = static_cast<State*>(user);
    if (!st->jitterbuffer) st->jitterbuffer = GST_ELEMENT(gst_object_ref(jb));
}

void on_new_manager(GstElement*, GstElement* manager, gpointer user) {
    g_signal_connect(manager, "new-jitterbuffer", G_CALLBACK(on_new_jitterbuffer), user);
}

void print_latency(State* st) {
    GstQuery* q = gst_query_new_latency();
    if (gst_element_query(st->pipeline, q)) {
        gboolean live;
        GstClockTime min, max;
        gst_query_parse_latency(q, &live, &min, &max);
        std::printf("latency query: live=%d min=%.1f ms max=%s\n", live, ms(min),
                    GST_CLOCK_TIME_IS_VALID(max) ? std::to_string(ms(max)).c_str() : "unlimited");
    }
    gst_query_unref(q);
}

void print_jb_stats(State* st) {
    if (!st->jitterbuffer) return;
    GstStructure* s = nullptr;
    g_object_get(st->jitterbuffer, "stats", &s, nullptr);
    if (!s) return;
    gchar* str = gst_structure_to_string(s);
    std::printf("jitterbuffer stats: %s\n", str);
    g_free(str);
    gst_structure_free(s);
}

// Seeking the pipeline fails when a sink (splitmuxsink) refuses seek events,
// so the seek goes to the element named "demux" when one exists. Only the
// initial seek (issued in PAUSED, before any buffer passed clocksync) may
// flush: a flush mid-fragment corrupts the splitmuxsink file being written.
gboolean seek_segment(State* st, bool flush) {
    GstSeekFlags flags = static_cast<GstSeekFlags>(GST_SEEK_FLAG_SEGMENT | (flush ? GST_SEEK_FLAG_FLUSH : 0));
    GstElement* target = gst_bin_get_by_name(GST_BIN(st->pipeline), "demux");
    gboolean ok = gst_element_seek(target ? target : st->pipeline, 1.0, GST_FORMAT_TIME, flags,
                                   GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
    if (target) gst_object_unref(target);
    return ok;
}

// In segment mode the demuxer posts SEGMENT_DONE instead of pushing EOS, so to
// stop cleanly (splitmuxsink finalizes on EOS) push EOS out of its src pads.
gboolean push_eos_cb(GstElement*, GstPad* pad, gpointer) {
    gst_pad_push_event(pad, gst_event_new_eos());
    return TRUE;
}

void stop_after_segment(State* st) {
    GstElement* demux = gst_bin_get_by_name(GST_BIN(st->pipeline), "demux");
    if (demux) {
        gst_element_foreach_src_pad(demux, push_eos_cb, nullptr);
        gst_object_unref(demux);
    } else {
        gst_element_send_event(st->pipeline, gst_event_new_eos());
    }
}

// Initial seek for --loop: the demuxer pads exist once no-more-pads fires, and
// in PAUSED the first buffer is still blocked in clocksync / identity sync=true,
// so a flushing segment seek here reaches the muxer before any data does.
gboolean initial_seek_idle(gpointer user) {
    auto* st = static_cast<State*>(user);
    std::printf("initial flushing segment seek (PAUSED, after no-more-pads): %d\n", seek_segment(st, true));
    gst_element_set_state(st->pipeline, GST_STATE_PLAYING);
    return FALSE;
}

void on_no_more_pads(GstElement*, gpointer user) {
    auto* st = static_cast<State*>(user);
    if (!st->loop_started) {
        st->loop_started = true;
        g_idle_add(initial_seek_idle, st);
    }
}

gboolean on_bus(GstBus*, GstMessage* msg, gpointer user) {
    auto* st = static_cast<State*>(user);
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar* dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            std::printf("ERROR: %s (%s)\n", err->message, dbg ? dbg : "");
            g_error_free(err);
            g_free(dbg);
            g_main_loop_quit(st->loop);
            break;
        }
        case GST_MESSAGE_EOS:
            std::printf("EOS\n");
            g_main_loop_quit(st->loop);
            break;
        case GST_MESSAGE_SEGMENT_DONE:
            std::printf("SEGMENT_DONE (mono %lld ms) -> %s\n",
                        static_cast<long long>((g_get_monotonic_time() - st->mono_start) / 1000),
                        st->stopping ? "push EOS" : "non-flushing seek to 0");
            if (st->stopping) stop_after_segment(st);
            else seek_segment(st, false);
            break;
        case GST_MESSAGE_LATENCY:
            gst_bin_recalculate_latency(GST_BIN(st->pipeline));
            print_latency(st);
            break;
        case GST_MESSAGE_ASYNC_DONE:
            print_latency(st);
            break;
        default:
            break;
    }
    return TRUE;
}

gboolean on_timeout(gpointer user) {
    auto* st = static_cast<State*>(user);
    std::printf("timer: sending EOS\n");
    st->stopping = true;
    gst_element_send_event(st->pipeline, gst_event_new_eos());
    return FALSE;
}

}  // namespace

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    State st;
    int seconds = 15;
    std::string launch;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--every") && i + 1 < argc) st.every = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--loop")) st.loop_file = true;
        else launch = argv[i];
    }
    if (launch.empty()) {
        std::fprintf(stderr, "usage: pipe_probe [--seconds N] [--every N] [--loop] \"<launch>\"\n");
        return 2;
    }

    GError* err = nullptr;
    st.pipeline = gst_parse_launch(launch.c_str(), &err);
    if (!st.pipeline || err) {
        std::fprintf(stderr, "parse error: %s\n", err ? err->message : "?");
        return 1;
    }
    GstElement* sink = gst_bin_get_by_name(GST_BIN(st.pipeline), "sink");
    if (!sink || !GST_IS_APP_SINK(sink)) {
        std::fprintf(stderr, "launch must contain an appsink named sink\n");
        return 1;
    }
    GstAppSinkCallbacks cb = {};
    cb.new_sample = on_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, &st, nullptr);
    gst_object_unref(sink);

    if (st.loop_file) {
        GstElement* demux = gst_bin_get_by_name(GST_BIN(st.pipeline), "demux");
        if (!demux) {
            std::fprintf(stderr, "--loop needs the demuxer named demux\n");
            return 1;
        }
        g_signal_connect(demux, "no-more-pads", G_CALLBACK(on_no_more_pads), &st);
        gst_object_unref(demux);
    }
    if (GstElement* src = gst_bin_get_by_name(GST_BIN(st.pipeline), "src")) {
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(src), "protocols"))
            g_signal_connect(src, "new-manager", G_CALLBACK(on_new_manager), &st);
        gst_object_unref(src);
    }

    st.loop = g_main_loop_new(nullptr, FALSE);
    GstBus* bus = gst_element_get_bus(st.pipeline);
    gst_bus_add_watch(bus, on_bus, &st);
    gst_object_unref(bus);

    st.mono_start = g_get_monotonic_time();
    gst_element_set_state(st.pipeline, st.loop_file ? GST_STATE_PAUSED : GST_STATE_PLAYING);
    g_timeout_add_seconds(seconds, on_timeout, &st);
    g_main_loop_run(st.loop);

    print_latency(&st);
    print_jb_stats(&st);
    double wall = static_cast<double>(g_get_monotonic_time() - st.mono_start) / 1e6;
    std::printf("frames %llu in %.1f s (%.1f fps)  pts gaps>60ms %llu  age min/avg/max %.1f/%.1f/%.1f ms\n",
                static_cast<unsigned long long>(st.frames), wall, st.frames / wall,
                static_cast<unsigned long long>(st.gaps), st.age_min,
                st.frames ? st.age_sum / st.frames : 0.0, st.age_max);

    gst_element_set_state(st.pipeline, GST_STATE_NULL);
    if (st.jitterbuffer) gst_object_unref(st.jitterbuffer);
    gst_object_unref(st.pipeline);
    return 0;
}
