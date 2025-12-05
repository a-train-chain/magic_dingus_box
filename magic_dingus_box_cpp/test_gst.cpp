#include <gst/gst.h>
#include <iostream>
#include <thread>
#include <chrono>

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);

    std::cout << "Testing GStreamer..." << std::endl;

    // Create playbin
    GstElement* playbin = gst_element_factory_make("playbin", "playbin");
    if (!playbin) {
        std::cerr << "Failed to create playbin" << std::endl;
        return 1;
    }

    // Set URI to a test file
    std::string uri = "file:///home/alexanderchaney/magic_dingus_box/dev_data/media/intro.30fps.mp4";
    g_object_set(G_OBJECT(playbin), "uri", uri.c_str(), nullptr);

    // Configure audio sink
    GstElement* alsasink = gst_element_factory_make("alsasink", "audio-sink");
    if (alsasink) {
        g_object_set(G_OBJECT(alsasink), "device", "plughw:CARD=vc4hdmi0,DEV=0", nullptr);
        g_object_set(G_OBJECT(playbin), "audio-sink", alsasink, nullptr);
    }

    // Configure video sink to fakesink
    GstElement* fakesink = gst_element_factory_make("fakesink", "video-sink");
    if (fakesink) {
        g_object_set(G_OBJECT(playbin), "video-sink", fakesink, nullptr);
    }

    std::cout << "Setting pipeline to PLAYING..." << std::endl;
    GstStateChangeReturn ret = gst_element_set_state(playbin, GST_STATE_PLAYING);
    std::cout << "State change return: " << ret << std::endl;

    // Wait a bit
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // Check current state
    GstState current, pending;
    gst_element_get_state(playbin, &current, &pending, GST_CLOCK_TIME_NONE);
    std::cout << "Current state: " << gst_element_state_get_name(current) << std::endl;
    std::cout << "Pending state: " << gst_element_state_get_name(pending) << std::endl;

    // Clean up
    gst_element_set_state(playbin, GST_STATE_NULL);
    gst_object_unref(playbin);

    std::cout << "Test complete" << std::endl;
    return 0;
}
