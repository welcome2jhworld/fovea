#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

#include <cstdio>
#include <cstring>
#include <string>

// Minimal RTSP test server.
//   rtsp_server live            -> videotestsrc + x264enc, keyframe every 25 frames
//   rtsp_server file <clip.mp4> -> mp4 demuxed and payloaded without re-encoding;
//                                  ends after one pass (multifilesrc loop does not survive qtdemux)
// Serves rtsp://127.0.0.1:8554/test (override with PORT / MOUNT env).

static std::string launch_for(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "file") == 0) {
        return std::string("( multifilesrc location=") + argv[2] +
               " loop=true ! qtdemux ! h264parse config-interval=1 "
               "! rtph264pay name=pay0 pt=96 )";
    }
    return "( videotestsrc is-live=true pattern=ball "
           "! video/x-raw,width=640,height=360,framerate=25/1 "
           "! timeoverlay halignment=left valignment=top font-desc=\"Sans 20\" "
           "! x264enc tune=zerolatency speed-preset=veryfast key-int-max=25 bframes=0 bitrate=800 "
           "! h264parse config-interval=1 ! rtph264pay name=pay0 pt=96 )";
}

int main(int argc, char** argv) {
    gst_init(&argc, &argv);

    const char* port = std::getenv("PORT") ? std::getenv("PORT") : "8554";
    const char* mount = std::getenv("MOUNT") ? std::getenv("MOUNT") : "/test";
    const std::string launch = launch_for(argc, argv);

    GstRTSPServer* server = gst_rtsp_server_new();
    gst_rtsp_server_set_address(server, "127.0.0.1");
    gst_rtsp_server_set_service(server, port);

    GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
    gst_rtsp_media_factory_set_launch(factory, launch.c_str());
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    gst_rtsp_media_factory_set_protocols(
        factory, static_cast<GstRTSPLowerTrans>(GST_RTSP_LOWER_TRANS_TCP | GST_RTSP_LOWER_TRANS_UDP));

    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server);
    gst_rtsp_mount_points_add_factory(mounts, mount, factory);
    g_object_unref(mounts);

    if (gst_rtsp_server_attach(server, nullptr) == 0) {
        std::fprintf(stderr, "failed to attach server on port %s\n", port);
        return 1;
    }
    std::printf("rtsp://127.0.0.1:%s%s\n%s\n", port, mount, launch.c_str());
    std::fflush(stdout);

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(loop);
    return 0;
}
