// The Tube renderer's GLSL: each program of the fixed table assembled from the
// embedded files (tube/tube_shader_sources.h, generated at configure time) and
// checked for what the renderer and the hardware rely on. CI writes the same
// assembled sources out (MDB_WRITE_TUBE_GLSL) and runs glslangValidator on them.
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "tube/tube_programs.h"
#include "tube/tube_shader_assembly.h"

using namespace tube;

TEST_CASE("tube shaders: every program assembles with its defines first", "[tube][tube_shaders]") {
    for (const auto& info : kPrograms) for (auto decode : {SceneDecode::Hardware, SceneDecode::Shader}) {
        const std::string src = assemble_fragment(info.id, decode); INFO(info.name);
        REQUIRE(src.rfind("#version 300 es\n", 0) == 0);
        REQUIRE(src.find("#version", 1) == std::string::npos);
        std::istringstream words{program_defines(info.id)}; std::string d;
        while (words >> d) REQUIRE(src.find("#define " + d + " 1\n") != std::string::npos);
        const bool hw = decode == SceneDecode::Hardware && info.reads_scene;
        REQUIRE((src.find("#define SCENE_HW_DECODE 1\n") != std::string::npos) == hw);
        REQUIRE(src.find("TUBE_MASK_TYPE") == std::string::npos);      // the mask is data
        REQUIRE(src.find("OUT_MESH") == std::string::npos);            // the bend is per pixel
        REQUIRE(src.find("TUBE_GLSL_TABLE_V1") != std::string::npos);  // the binary's marker
    }
    REQUIRE(assemble_vertex().rfind("#version 300 es\n", 0) == 0);
}
TEST_CASE("tube shaders: crt.metal's flash-safety caps are in the GLSL", "[tube][tube_shaders]") {
    const std::string out = assemble_fragment(Program::out_rich_scene, SceneDecode::Hardware);
    REQUIRE(out.find("min(noise, 0.2)") != std::string::npos);                 // NOISE: +-1 % + grain
    REQUIRE(out.find("min(flicker, 1.0) * 0.025") != std::string::npos);       // FLICKER: +-2.5 %
    REQUIRE(out.find("min(hum, 1.0) * 0.25") != std::string::npos);            // HUM: 25 % darker at most
    REQUIRE(out.find("min(jitter, 1.0) * 0.50 * mm") != std::string::npos);    // JITTER: +-0.25 mm a line
    REQUIRE(assemble_fragment(Program::sig_sharp, SceneDecode::Hardware).find("min(jitter, 1.0) * 0.50 * mm") != std::string::npos);
}
TEST_CASE("tube shaders: each program declares the uniforms the renderer uploads", "[tube][tube_shaders]") {
    for (const auto& info : kPrograms) {
        const std::string src = assemble_fragment(info.id, SceneDecode::Hardware);
        for (std::string_view u : uniforms_of(info.id)) {
            INFO(info.name << " " << u);
            const std::regex decl("uniform\\s+\\w+\\s+[^;]*\\b" + std::string(u) + "\\b");
            REQUIRE(std::regex_search(src, decl));
        }
    }
}
// DRIVER HAZARD (docs/engineering/measurements/2026-11-15-tube-hang-isolation.txt):
// the Pi 5's compiler hung on the signal program with chroma bleed and convergence
// but without the sharpness path. The table always defines SIG_SHARP on a signal
// program (test_tube_programs.cpp); this checks the define really compiles the
// path in, by its code, in every signal program and both scene decodes.
TEST_CASE("tube shaders: every signal program compiles the sharpness path", "[tube][tube_shaders]") {
    int signal_programs = 0;
    for (const auto& info : kPrograms) {
        if (info.file != ShaderFile::signal) continue;
        ++signal_programs;
        for (auto decode : {SceneDecode::Hardware, SceneDecode::Shader}) {
            INFO(info.name);
            const std::string src = assemble_fragment(info.id, decode);
            // the left/right taps' 1-2-1 blend, which only the SIG_SHARP block holds
            REQUIRE(src.find("(l + 2.0 * c + r) * 0.25") != std::string::npos);
            // and the guards that refuse a build without it
            REQUIRE(src.find("#error") != std::string::npos);
        }
    }
    REQUIRE(signal_programs == 4);
}
// GLSL ES limits the source character set; a stray non-ASCII character (a dash
// or a plus-minus pasted into a comment) is a compile error on some drivers.
TEST_CASE("tube shaders: every file is plain ASCII", "[tube][tube_shaders]") {
    for (std::string_view file : {glsl::common, glsl::fullscreen_vert, glsl::glass, glsl::quarter, glsl::blur,
                                  glsl::copy_encode, glsl::signal, glsl::lines, glsl::output}) {
        REQUIRE_FALSE(file.empty());
        for (char ch : file) {
            const auto byte = static_cast<unsigned char>(ch);
            REQUIRE((byte == '\n' || (byte >= 0x20 && byte < 0x7f)));
        }
    }
}
TEST_CASE("tube shaders: written out for the validator when asked", "[tube][tube_shaders]") {
    const char* dir = std::getenv("MDB_WRITE_TUBE_GLSL");
    if (dir == nullptr || *dir == '\0') return;
    std::filesystem::create_directories(dir); int n = 0;
    for (const auto& info : kPrograms) for (auto decode : {SceneDecode::Hardware, SceneDecode::Shader}) {
        if (decode == SceneDecode::Shader && !info.reads_scene) continue;
        std::ofstream(std::string(dir) + "/" + std::string(info.name) + (decode == SceneDecode::Hardware ? ".hw" : ".sw") + ".frag")
            << assemble_fragment(info.id, decode);
        ++n;
    }
    std::ofstream(std::string(dir) + "/fullscreen.vert") << assemble_vertex();
    REQUIRE(n == 28);
}
