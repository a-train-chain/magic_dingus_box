#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>

#include "retroarch/launch_contract.h"

namespace {

void require_line(const std::string& config, const std::string& line) {
    REQUIRE(config.find(line + "\n") != std::string::npos);
}

}  // namespace

TEST_CASE("Modern TV keeps its native-core 4:3 bezel contract",
          "[retroarch][video]") {
    retroarch::LaunchOptions options;
    options.display_mode = app::DisplayMode::MODERN_TV;
    options.bezel_file = "mdb_kv19.png";

    std::ostringstream output;
    retroarch::write_video_config(output, options);
    const std::string config = output.str();

    require_line(config, "video_driver = \"vulkan\"");
    require_line(config, "video_context_driver = \"kms\"");
    require_line(config, "video_threaded = \"false\"");
    require_line(config, "video_max_swapchain_images = \"2\"");
    require_line(config, "video_vsync = \"true\"");
    require_line(config, "video_frame_delay = \"4\"");
    require_line(config, "video_shader_enable = \"false\"");
    require_line(config, "video_smooth = \"false\"");
    require_line(config, "video_fullscreen_x = \"1920\"");
    require_line(config, "video_fullscreen_y = \"1080\"");
    require_line(config, "video_custom_viewport_enable = \"true\"");
    require_line(config, "video_custom_viewport_x = \"251\"");
    require_line(config, "video_custom_viewport_y = \"10\"");
    require_line(config, "video_custom_viewport_width = \"1415\"");
    require_line(config, "video_custom_viewport_height = \"1059\"");
    require_line(config, "aspect_ratio_index = \"22\"");
    require_line(config, "input_overlay_enable = \"true\"");
    require_line(config, "input_overlay_opacity = \"1.0\"");
    require_line(config, "input_overlay_hide_in_menu = \"true\"");
}

TEST_CASE("CRT Native remains 640x480 without a custom viewport",
          "[retroarch][video]") {
    retroarch::LaunchOptions options;
    options.display_mode = app::DisplayMode::CRT_NATIVE;
    options.bezel_file = "mdb_kv19.png";

    std::ostringstream output;
    retroarch::write_video_config(output, options);
    const std::string config = output.str();

    require_line(config, "video_context_driver = \"kms\"");
    require_line(config, "video_fullscreen_x = \"640\"");
    require_line(config, "video_fullscreen_y = \"480\"");
    require_line(config, "video_custom_viewport_enable = \"false\"");
    require_line(config, "aspect_ratio_index = \"23\"");
    REQUIRE(config.find("input_overlay =") == std::string::npos);
}
