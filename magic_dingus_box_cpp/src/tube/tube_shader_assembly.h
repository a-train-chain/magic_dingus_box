// tube_shader_assembly.h - one program of the fixed table (tube_programs.h) put
// together as GLSL source from the embedded files (tube/tube_shader_sources.h,
// generated from src/tube/shaders/ by cmake/EmbedTubeShaders.cmake).
//
// A fragment program is, in this order:
//   "#version 300 es"
//   one "#define NAME 1" per word of program_defines(p)
//   "#define SCENE_HW_DECODE 1" when the scene is decoded by the texture unit and
//     the program reads the scene (common.glsl's uiTex then skips the pow)
//   "#line 1 1", common.glsl, "#line 1 2", the program's own file
// so a compiler error names source string 1 (common) or 2 (the file) and the
// file's own line. The vertex shader is the version line and fullscreen.vert.
//
// The same strings go to the GPU (Task 10's renderer) and, written out by
// test_tube_shaders.cpp, to glslangValidator in CI: nothing is assembled any
// other way.
//
// Pure: standard library only, no I/O, no GL.
#pragma once

#include <sstream>
#include <string>
#include <string_view>

#include "tube/tube_programs.h"
#include "tube/tube_shader_sources.h"

namespace tube {

// How the scene texture is decoded to linear light (design: the hw_srgb probe).
enum class SceneDecode { Hardware, Shader };

constexpr std::string_view shader_file_source(ShaderFile f) noexcept {
    switch (f) {
        case ShaderFile::glass: return glsl::glass;
        case ShaderFile::quarter: return glsl::quarter;
        case ShaderFile::blur: return glsl::blur;
        case ShaderFile::copy_encode: return glsl::copy_encode;
        case ShaderFile::signal: return glsl::signal;
        case ShaderFile::lines: return glsl::lines;
        case ShaderFile::output: return glsl::output;
    }
    return {};
}

inline std::string assemble_fragment(Program p, SceneDecode decode) {
    const ProgramInfo& info = program_info(p);
    std::string src = "#version 300 es\n";
    std::istringstream words{program_defines(p)};
    for (std::string d; words >> d;) src += "#define " + d + " 1\n";
    if (decode == SceneDecode::Hardware && info.reads_scene) src += "#define SCENE_HW_DECODE 1\n";
    src += "#line 1 1\n";
    src += glsl::common;
    src += "\n#line 1 2\n";
    src += shader_file_source(info.file);
    return src;
}

inline std::string assemble_vertex() {
    std::string src = "#version 300 es\n#line 1\n";
    src += glsl::fullscreen_vert;
    return src;
}

}  // namespace tube
