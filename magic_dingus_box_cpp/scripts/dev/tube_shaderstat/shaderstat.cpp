// shaderstat.cpp - compile one Tube fragment program on the Pi's GPU off-screen and
// report what Mesa's V3D compiler made of it (instructions, QPU threads, max
// temporaries, spills), optionally time it, or render two programs with identical
// inputs and compare the pictures. A DEVELOPER TOOL: not built by CMake, not
// installed, not run by CI. See README.md next to it for the build line and usage.
//
// It opens the GPU's RENDER node (/dev/dri/renderD128) through GBM + EGL, or EGL's
// surfaceless platform when that fails; it never opens a card node, never becomes DRM
// master and never touches KMS, so it runs beside the kiosk that owns the display.
//
// Fidelity to the kiosk (tube_renderer.cpp / tube_renderer_passes.cpp): Mesa's V3D
// shader-variant key includes the sampler formats and the render-target format, so
// every sampler is bound to a texture of the internal format the renderer uses on the
// texture unit the renderer uses (kSamplerUnits), and the target is the program's own:
// an OUT_HISTORY program draws into an SRGB8_ALPHA8 picture-sized target with
// FRAMEBUFFER_SRGB on (uOrigin 0,0); any other into an RGBA8 framebuffer-sized target
// at the picture's viewport (uOrigin = the viewport's corner), cleared first, as
// clear_bars() does.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif
#ifndef GL_TEXTURE_SRGB_DECODE_EXT
#define GL_TEXTURE_SRGB_DECODE_EXT 0x8A48
#endif
#ifndef GL_DECODE_EXT
#define GL_DECODE_EXT 0x8A49
#endif
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

namespace {

[[noreturn]] void die(int code, const std::string& msg) {
    std::fprintf(stderr, "shaderstat: %s\n", msg.c_str());
    std::exit(code);
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) die(2, "cannot read " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---- The uniform file ---------------------------------------------------------------
// `name=v[,v...]` per line; `#` comments; directives: @fb=W,H (the framebuffer, i.e.
// the scene texture's size), @pic=X,Y,W,H (the picture inside it, top-down, as
// video::IRect), @mask=W,H,<hex RGB bytes> (the mask tile, one period).
struct Inputs {
    std::map<std::string, std::vector<float>> values;
    int fb_w = 1440, fb_h = 1080;
    int pic_x = 0, pic_y = 0, pic_w = 1440, pic_h = 1080;
    int sig_w = 720, sig_h = 1080;       // @sig: the signal target (also what lines and outputs read)
    int lines_w = 720, lines_h = 270;    // @lines: the lines target
    int quarter_w = 360, quarter_h = 270;   // @quarter: the quarter and glow targets
    int mask_w = 3, mask_h = 1;
    std::vector<std::uint8_t> mask_rgb{255, 64, 64, 64, 255, 64, 64, 64, 255};
};

std::vector<float> parse_floats(const std::string& s) {
    std::vector<float> out;
    std::stringstream ss(s);
    for (std::string item; std::getline(ss, item, ',');) {
        char* end = nullptr;
        const float v = std::strtof(item.c_str(), &end);
        if (end == item.c_str()) die(2, "not a number in the uniform file: '" + item + "'");
        out.push_back(v);
    }
    return out;
}

void read_inputs(const std::string& path, Inputs& in) {
    std::ifstream f(path);
    if (!f) die(2, "uniform file missing or unreadable: " + path);
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) die(2, "bad line in the uniform file: '" + line + "'");
        const std::string name = line.substr(0, eq), value = line.substr(eq + 1);
        if (name == "@fb") {
            const auto v = parse_floats(value);
            if (v.size() != 2) die(2, "@fb needs W,H");
            in.fb_w = static_cast<int>(v[0]);
            in.fb_h = static_cast<int>(v[1]);
        } else if (name == "@pic") {
            const auto v = parse_floats(value);
            if (v.size() != 4) die(2, "@pic needs X,Y,W,H");
            in.pic_x = static_cast<int>(v[0]);
            in.pic_y = static_cast<int>(v[1]);
            in.pic_w = static_cast<int>(v[2]);
            in.pic_h = static_cast<int>(v[3]);
        } else if (name == "@sig" || name == "@lines" || name == "@quarter") {
            const auto v = parse_floats(value);
            if (v.size() != 2 || v[0] < 1 || v[1] < 1) die(2, name + " needs W,H");
            int& w = name == "@sig" ? in.sig_w : name == "@lines" ? in.lines_w : in.quarter_w;
            int& h = name == "@sig" ? in.sig_h : name == "@lines" ? in.lines_h : in.quarter_h;
            w = static_cast<int>(v[0]);
            h = static_cast<int>(v[1]);
        } else if (name == "@mask") {
            std::stringstream ss(value);
            std::string w, h, hex;
            std::getline(ss, w, ',');
            std::getline(ss, h, ',');
            std::getline(ss, hex, ',');
            in.mask_w = std::atoi(w.c_str());
            in.mask_h = std::atoi(h.c_str());
            if (in.mask_w <= 0 || in.mask_h <= 0 || hex.size() != static_cast<std::size_t>(in.mask_w * in.mask_h * 6))
                die(2, "@mask needs W,H,<W*H*3 bytes in hex>");
            in.mask_rgb.clear();
            for (std::size_t i = 0; i < hex.size(); i += 2)
                in.mask_rgb.push_back(static_cast<std::uint8_t>(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
        } else if (!name.empty() && name[0] == '@') {
            die(2, "unknown directive " + name);
        } else {
            in.values[name] = parse_floats(value);
        }
    }
}

// ---- EGL: the render node only -------------------------------------------------------
struct Gpu {
    int fd = -1;
    gbm_device* gbm = nullptr;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    std::string how;
};

bool try_display(Gpu& g, EGLenum platform, void* native) {
    auto get = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!get) return false;
    g.dpy = get(platform, native, nullptr);
    if (g.dpy == EGL_NO_DISPLAY) return false;
    EGLint major = 0, minor = 0;
    if (!eglInitialize(g.dpy, &major, &minor)) {
        g.dpy = EGL_NO_DISPLAY;
        return false;
    }
    return true;
}

void open_gpu(Gpu& g) {
    g.fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (g.fd >= 0) g.gbm = gbm_create_device(g.fd);
    if (g.gbm && try_display(g, EGL_PLATFORM_GBM_KHR, g.gbm)) {
        g.how = "GBM on /dev/dri/renderD128";
    } else if (try_display(g, EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY)) {
        g.how = "EGL surfaceless platform";
    } else {
        die(1, "no EGL display on the render node (GBM and surfaceless both failed)");
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) die(1, "eglBindAPI(GLES) failed");
    const EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    if (!eglChooseConfig(g.dpy, cfg_attr, &cfg, 1, &n) || n < 1) die(1, "no ES3 EGL config");
    const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    g.ctx = eglCreateContext(g.dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g.ctx == EGL_NO_CONTEXT) die(1, "eglCreateContext(ES 3.1) failed");
    if (!eglMakeCurrent(g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g.ctx))
        die(1, "eglMakeCurrent without a surface failed (no EGL_KHR_surfaceless_context?)");
}

void close_gpu(Gpu& g) {
    eglMakeCurrent(g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(g.dpy, g.ctx);
    eglTerminate(g.dpy);
    if (g.gbm) gbm_device_destroy(g.gbm);
    if (g.fd >= 0) close(g.fd);
}

// ---- stderr capture: Mesa's shaderdb lines -------------------------------------------
class StderrCapture {
public:
    StderrCapture() {
        std::fflush(stderr);
        saved_ = dup(2);
        file_ = std::tmpfile();
        if (saved_ < 0 || !file_) die(1, "cannot capture stderr");
        dup2(fileno(file_), 2);
    }
    std::string finish() {
        std::fflush(stderr);
        dup2(saved_, 2);
        close(saved_);
        std::string out;
        std::rewind(file_);
        char buf[4096];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, file_)) > 0;) out.append(buf, n);
        std::fclose(file_);
        return out;
    }

private:
    int saved_ = -1;
    std::FILE* file_ = nullptr;
};

// "SHADER-DB-... MESA_SHADER_FRAGMENT shader: 479 inst, 2 threads, 0 loops, 37 uniforms,
// 43 max-temps, 0:0 spills:fills, ..." -> "479 inst, 2 threads, 43 max-temps, 0:0 spills:fills"
std::string field(const std::string& line, const std::string& suffix) {
    const auto at = line.find(" " + suffix);
    if (at == std::string::npos) return "?";
    auto start = line.rfind(' ', at - 1);
    start = start == std::string::npos ? 0 : start + 1;
    return line.substr(start, at - start);
}

struct Stats {
    std::vector<std::string> lines;   // the fragment shaderdb lines, raw
    std::string compact() const {
        std::string out;
        for (const auto& l : lines) {
            if (!out.empty()) out += " | ";
            out += field(l, "inst,") + " inst, " + field(l, "threads,") + " threads, " + field(l, "max-temps,") +
                   " max-temps, " + field(l, "spills:fills") + " spills:fills";
        }
        return out.empty() ? "(no fragment shaderdb line: is V3D_DEBUG=shaderdb set?)" : out;
    }
};

Stats parse_shaderdb(const std::string& text, std::string& other) {
    Stats s;
    std::stringstream ss(text);
    for (std::string line; std::getline(ss, line);) {
        if (line.find("SHADER-DB") != std::string::npos && line.find("FRAGMENT") != std::string::npos)
            s.lines.push_back(line);
        else if (line.find("SHADER-DB") == std::string::npos && !line.empty())
            other += line + "\n";   // anything else Mesa said (warnings): passed through
    }
    return s;
}

// ---- GL helpers ----------------------------------------------------------------------
GLuint compile(GLenum type, const std::string& src, const std::string& name) {
    GLuint sh = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[8192] = {};
        glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        die(1, "compile error in " + name + ":\n" + log);
    }
    return sh;
}

GLuint link(const std::string& vs, const std::string& fs, const std::string& name) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs, "the vertex shader"));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs, name));
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[8192] = {};
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        die(1, "link error in " + name + ":\n" + log);
    }
    return p;
}

// Deterministic, non-trivial content: gradients plus fine detail (a one-pixel
// checker, a fast ripple, a coarse step), different per channel and per seed, so a
// wrong tap, a missing term or a swapped channel shows in a comparison.
float pattern(int x, int y, int w, int h, int ch, int seed) {
    const float gx = static_cast<float>(x) / static_cast<float>(std::max(w - 1, 1));
    const float gy = static_cast<float>(y) / static_cast<float>(std::max(h - 1, 1));
    const float grad = ch == 0 ? gx : ch == 1 ? gy : 0.5f * (gx + (1.0f - gy));
    const float checker = static_cast<float>((x + y + ch + seed) & 1);
    const float ripple = 0.5f + 0.5f * std::sin(0.37f * x + 0.23f * y + 1.7f * ch + seed);
    const float step = static_cast<float>(((x / 7) * 3 + (y / 5) * 5 + ch + seed) % 4) / 3.0f;
    return std::clamp(0.5f * grad + 0.15f * checker + 0.2f * ripple + 0.15f * step, 0.0f, 1.0f);
}

GLuint make_tex(GLenum fmt, int w, int h, int seed, float scale, bool nearest_repeat = false) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (fmt == GL_RGBA16F) {
        std::vector<float> px(static_cast<std::size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c)
                    px[(static_cast<std::size_t>(y) * w + x) * 4 + c] = c == 3 ? 1.0f : scale * pattern(x, y, w, h, c, seed);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, px.data());
    } else {
        std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c)
                    px[(static_cast<std::size_t>(y) * w + x) * 4 + c] = static_cast<std::uint8_t>(
                        c == 3 ? 255 : std::lround(255.0f * std::min(1.0f, scale * pattern(x, y, w, h, c, seed))));
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(fmt), w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    }
    const GLint filter = nearest_repeat ? GL_NEAREST : GL_LINEAR, wrap = nearest_repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    return t;
}

// The baked glass (glass.frag): RGBA8, A = inside * vignette * rim (0 outside the
// opening, falling off towards the edges), RGB = the glass light x32 (small values).
GLuint make_glass(int w, int h) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float nx = 2.0f * x / w - 1.0f, ny = 2.0f * y / h - 1.0f;
            const float edge = std::max(std::fabs(nx), std::fabs(ny));
            const float a = edge > 0.985f ? 0.0f : std::clamp(1.0f - 0.35f * (nx * nx + ny * ny), 0.0f, 1.0f);
            std::uint8_t* p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
            for (int c = 0; c < 3; ++c) p[c] = static_cast<std::uint8_t>(std::lround(12.0f * pattern(x, y, w, h, c, 5)));
            p[3] = static_cast<std::uint8_t>(std::lround(255.0f * a));
        }
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

GLuint make_mask(const Inputs& in, GLuint t = 0) {
    std::vector<std::uint8_t> rgba;
    for (std::size_t i = 0; i + 2 < in.mask_rgb.size(); i += 3) {
        rgba.insert(rgba.end(), {in.mask_rgb[i], in.mask_rgb[i + 1], in.mask_rgb[i + 2], 255});
    }
    if (!t) glGenTextures(1, &t);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, in.mask_w, in.mask_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return t;
}

// ---- One program, as the kiosk draws it -----------------------------------------------
struct Setup {
    Inputs in;
    bool float_target = false;   // --float: an RGBA32F target instead (a finer comparison;
                                 // a different render-target format, so NOT the kiosk's variant)
};

// Which pass a fragment program draws, from its source: its target follows
// (tube_renderer.cpp ensure_targets, tube_renderer_passes.cpp draw_frame).
enum class Kind { output, signal, lines, quarter, blur, copy_encode };

struct Program {
    std::string name;
    GLuint prog = 0;
    Kind kind = Kind::output;
    GLenum fmt = GL_RGBA8;            // the target's internal format
    bool history = false, hw_decode = false;
    GLuint fbo = 0, target = 0;
    int tw = 0, th = 0;               // the target's size
    int vx = 0, vy = 0, vw = 0, vh = 0;   // the viewport (GL, bottom-left)
    GLuint tex[8] = {};               // per texture unit
    Stats stats;
};

bool has_define(const std::string& src, const std::string& name) {
    return src.find("#define " + name + " 1") != std::string::npos;
}

int sampler_unit(const std::string& name) {
    // tube_renderer_internal.h kSamplerUnits
    static const std::map<std::string, int> kUnits{{"ui", 0},       {"src", 0},       {"glowTex", 1},
                                                   {"prev", 2},     {"linesTex", 3},  {"quarter", 4},
                                                   {"signalTex", 5}, {"glassTex", 6}, {"maskTex", 7}};
    const auto it = kUnits.find(name);
    return it == kUnits.end() ? -1 : it->second;
}

void set_uniforms(Program& p, const Setup& s) {
    const Inputs& in = s.in;
    glUseProgram(p.prog);
    // The renderer's own (tube_renderer_passes.cpp), from the picture and the target.
    std::map<std::string, std::vector<float>> own;
    own["uOrigin"] = {static_cast<float>(p.vx), static_cast<float>(p.vy)};
    own["uMaskSize"] = {static_cast<float>(in.mask_w), static_cast<float>(in.mask_h)};
    const float W = static_cast<float>(in.fb_w), H = static_cast<float>(in.fb_h), x = static_cast<float>(in.pic_x),
                y = static_cast<float>(in.pic_y), w = static_cast<float>(in.pic_w), h = static_cast<float>(in.pic_h);
    own["uSceneMap"] = {x / W, (H - y) / H, w / W, h / H};
    own["uSceneClamp"] = {(x + 0.5f) / W, (H - y - h + 0.5f) / H, (x + w - 0.5f) / W, (H - y - 0.5f) / H};

    GLint count = 0;
    glGetProgramiv(p.prog, GL_ACTIVE_UNIFORMS, &count);
    std::vector<std::string> unset;
    for (GLint i = 0; i < count; ++i) {
        char name[256] = {};
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(p.prog, static_cast<GLuint>(i), sizeof name, nullptr, &size, &type, name);
        const GLint loc = glGetUniformLocation(p.prog, name);
        if (char* bracket = std::strstr(name, "[0]")) *bracket = '\0';   // an array: by its name
        if (type == GL_SAMPLER_2D) {
            const int unit = sampler_unit(name);
            if (unit < 0) die(1, std::string("unknown sampler ") + name + " (not in the renderer's unit table)");
            glUniform1i(loc, unit);
            continue;
        }
        const std::vector<float>* v = nullptr;
        const auto o = own.find(name);
        if (o != own.end()) v = &o->second;
        const auto f = in.values.find(name);
        if (f != in.values.end()) v = &f->second;   // the file wins, but uOrigin is the program's
        if (std::string(name) == "uOrigin") v = &o->second;
        if (!v) {
            unset.push_back(name);
            continue;
        }
        const std::size_t need = type == GL_FLOAT ? 1 : type == GL_FLOAT_VEC2 ? 2 : type == GL_FLOAT_VEC3 ? 3
                                 : type == GL_FLOAT_VEC4 ? 4 : type == GL_INT ? 1 : 0;
        if (need == 0) die(1, std::string("uniform ") + name + " has a type this tool does not set");
        if (size > 1) {   // a float array: every element from the file
            if (type != GL_FLOAT || v->size() != static_cast<std::size_t>(size))
                die(2, std::string("uniform ") + name + " needs " + std::to_string(size) + " values");
            glUniform1fv(loc, size, v->data());
            continue;
        }
        if (v->size() != need) die(2, std::string("uniform ") + name + " needs " + std::to_string(need) + " values");
        const float* d = v->data();
        switch (type) {
            case GL_FLOAT: glUniform1f(loc, d[0]); break;
            case GL_FLOAT_VEC2: glUniform2f(loc, d[0], d[1]); break;
            case GL_FLOAT_VEC3: glUniform3f(loc, d[0], d[1], d[2]); break;
            case GL_FLOAT_VEC4: glUniform4f(loc, d[0], d[1], d[2], d[3]); break;
            case GL_INT: glUniform1i(loc, static_cast<GLint>(d[0])); break;
            default: break;
        }
    }
    if (!unset.empty() && !in.values.empty()) {
        std::string list;
        for (const auto& u : unset) list += " " + u;
        std::fprintf(stderr, "shaderstat: %s: uniforms not in the file, left 0:%s\n", p.name.c_str(), list.c_str());
    }
}

void make_target(Program& p, const Setup& s) {
    const Inputs& in = s.in;
    if (p.kind != Kind::output && p.kind != Kind::copy_encode) {
        // A whole intermediate target, viewport (0,0): the signal and lines RGBA16F, the
        // quarter and glow SRGB8_ALPHA8.
        p.tw = p.kind == Kind::signal ? in.sig_w : p.kind == Kind::lines ? in.lines_w : in.quarter_w;
        p.th = p.kind == Kind::signal ? in.sig_h : p.kind == Kind::lines ? in.lines_h : in.quarter_h;
        p.vx = p.vy = 0;
        p.vw = p.tw;
        p.vh = p.th;
        p.fmt = s.float_target ? GL_RGBA32F
                : (p.kind == Kind::signal || p.kind == Kind::lines) ? GL_RGBA16F : GL_SRGB8_ALPHA8;
    } else if (p.history) {
        // The history: picture-sized SRGB8_ALPHA8, viewport (0,0), uOrigin (0,0).
        p.tw = in.pic_w;
        p.th = in.pic_h;
        p.vx = p.vy = 0;
    } else {
        // The default framebuffer: framebuffer-sized, the picture's viewport (GL is bottom-left).
        p.tw = in.fb_w;
        p.th = in.fb_h;
        p.vx = in.pic_x;
        p.vy = in.fb_h - in.pic_y - in.pic_h;
    }
    if (p.kind == Kind::output || p.kind == Kind::copy_encode) {
        p.vw = in.pic_w;
        p.vh = in.pic_h;
        p.fmt = s.float_target ? GL_RGBA32F : p.history ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    }
    const GLenum fmt = p.fmt;
    glGenTextures(1, &p.target);
    glBindTexture(GL_TEXTURE_2D, p.target);
    glTexStorage2D(GL_TEXTURE_2D, 1, fmt, p.tw, p.th);
    glGenFramebuffers(1, &p.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, p.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, p.target, 0);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "framebuffer incomplete (0x%x) for format 0x%x %dx%d", st, fmt, p.tw, p.th);
        die(1, buf);
    }
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

void make_inputs(Program& p, const Setup& s) {
    const Inputs& in = s.in;
    const auto lines_of = [&in]() {
        const auto it = in.values.find("lines");
        return it == in.values.end() ? 240 : std::max(1, static_cast<int>(std::ceil(it->second[0])));
    };
    // The scene: framebuffer-sized; SRGB8_ALPHA8 sampled with DECODE on the hardware path.
    p.tex[0] = make_tex(p.hw_decode ? GL_SRGB8_ALPHA8 : GL_RGBA8, in.fb_w, in.fb_h, 0, 1.0f);
    if (p.hw_decode) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_DECODE_EXT);
    p.tex[1] = make_tex(GL_SRGB8_ALPHA8, (in.pic_w + 3) / 4, (in.pic_h + 3) / 4, 1, 0.6f);   // glow_out
    p.tex[2] = make_tex(GL_SRGB8_ALPHA8, in.pic_w, in.pic_h, 2, 0.8f);                     // the previous history
    p.tex[3] = make_tex(GL_RGBA16F, in.lines_w, lines_of(), 3, 1.2f);                      // lines
    p.tex[4] = make_tex(GL_SRGB8_ALPHA8, in.quarter_w, in.quarter_h, 4, 1.0f);             // quarter
    p.tex[5] = make_tex(GL_RGBA16F, in.sig_w, in.sig_h, 6, 1.2f);                          // signal
    if (p.kind == Kind::blur)        // `src` (unit 0): the quarter picture, or the H pass's result
        p.tex[0] = make_tex(GL_SRGB8_ALPHA8, in.quarter_w, in.quarter_h, 7, 1.0f);
    if (p.kind == Kind::copy_encode)  // `src` (unit 0): the history just written
        p.tex[0] = make_tex(GL_SRGB8_ALPHA8, in.pic_w, in.pic_h, 8, 1.0f);
    p.tex[6] = make_glass(in.pic_w, in.pic_h);
    p.tex[7] = make_mask(in);
}

void bind_inputs(const Program& p) {
    for (int u = 0; u < 8; ++u) {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + u));
        glBindTexture(GL_TEXTURE_2D, p.tex[u]);
    }
}

GLuint quad_vao() {
    static const float kQuad[] = {-1.f, -1.f, 0.f, 0.f, 1.f, -1.f, 1.f, 0.f, 1.f, 1.f, 1.f, 1.f,
                                  -1.f, -1.f, 0.f, 0.f, 1.f, 1.f,  1.f, 1.f, -1.f, 1.f, 0.f, 1.f};
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof kQuad, kQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
    return vao;
}

void draw(const Program& p, bool srgb_switch) {
    glBindFramebuffer(GL_FRAMEBUFFER, p.fbo);
    if (srgb_switch) {
        // On for every pass into a target, off for the default framebuffer (the renderer's rule).
        if (p.fmt == GL_RGBA8) glDisable(GL_FRAMEBUFFER_SRGB_EXT); else glEnable(GL_FRAMEBUFFER_SRGB_EXT);
    }
    if (p.fmt == GL_RGBA8) {   // clear_bars(): the whole framebuffer, then the picture's viewport
        glViewport(0, 0, p.tw, p.th);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glViewport(p.vx, p.vy, p.vw, p.vh);
    glUseProgram(p.prog);
    bind_inputs(p);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void check_gl(const std::string& what) {
    const GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "GL error 0x%x ", e);
        die(1, buf + what);
    }
}

Program build(const std::string& vs, const std::string& fs_path, const Setup& s, bool srgb_switch) {
    Program p;
    p.name = fs_path.substr(fs_path.find_last_of('/') + 1);
    const std::string fs = read_file(fs_path);
    p.history = has_define(fs, "OUT_HISTORY");
    p.kind = fs.find("#define OUT_") != std::string::npos   ? Kind::output
             : fs.find("#define SIG_") != std::string::npos ? Kind::signal
             : fs.find("uniform vec2 uDir") != std::string::npos ? Kind::blur
             : fs.find("uniform ivec2 uSrcOffset") != std::string::npos ? Kind::copy_encode
             : fs.find("uniform sampler2D signalTex") != std::string::npos ? Kind::lines
             : Kind::quarter;
    p.hw_decode = has_define(fs, "SCENE_HW_DECODE");
    make_target(p, s);
    make_inputs(p, s);
    // Capture what Mesa prints from the link to the first draw's finish: V3D compiles the
    // real variant (its key: the bound formats and the target) inside the first draw.
    StderrCapture cap;
    p.prog = link(vs, fs, p.name);
    set_uniforms(p, s);
    draw(p, srgb_switch);
    glFinish();
    std::string other;
    p.stats = parse_shaderdb(cap.finish(), other);
    if (!other.empty()) std::fputs(other.c_str(), stderr);
    check_gl("drawing " + p.name);
    return p;
}

bool float_format(GLenum f) { return f == GL_RGBA16F || f == GL_RGBA32F; }

std::vector<float> read_back(const Program& p) {
    const bool float_target = float_format(p.fmt);
    glBindFramebuffer(GL_FRAMEBUFFER, p.fbo);
    std::vector<float> out(static_cast<std::size_t>(p.vw) * p.vh * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (float_target) {
        glReadPixels(p.vx, p.vy, p.vw, p.vh, GL_RGBA, GL_FLOAT, out.data());
    } else {
        std::vector<std::uint8_t> b(out.size());
        glReadPixels(p.vx, p.vy, p.vw, p.vh, GL_RGBA, GL_UNSIGNED_BYTE, b.data());
        for (std::size_t i = 0; i < b.size(); ++i) out[i] = static_cast<float>(b[i]);   // in 1/255 steps
    }
    check_gl("reading back " + p.name);
    return out;
}

void usage() {
    std::fputs(
        "usage: shaderstat [--uniforms FILE] [--time N] [--float] VERT FRAG\n"
        "       shaderstat [--uniforms FILE]... [--float] --compare VERT A.frag B.frag\n"
        "Run under: timeout 20 env V3D_DEBUG=shaderdb ./shaderstat ...\n",
        stderr);
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
    Setup s;
    std::vector<std::string> uniforms;
    int time_frames = 0;
    bool compare = false;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--uniforms" && i + 1 < argc) uniforms.push_back(argv[++i]);
        else if (a == "--time" && i + 1 < argc) time_frames = std::atoi(argv[++i]);
        else if (a == "--float") s.float_target = true;
        else if (a == "--compare") compare = true;
        else if (!a.empty() && a[0] == '-') usage();
        else files.push_back(a);
    }
    if (files.size() != (compare ? 3u : 2u)) usage();
    if (uniforms.size() > 1 && !compare) usage();
    if (!uniforms.empty()) read_inputs(uniforms[0], s.in);

    // Mesa's on-disk shader cache would skip the compile (and its shaderdb line) and
    // write under ~/.cache: off, for this process only.
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);

    Gpu gpu;
    open_gpu(gpu);
    const std::string ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const bool srgb_switch = ext.find("GL_EXT_sRGB_write_control") != std::string::npos;
    std::printf("gpu: %s | %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION), gpu.how.c_str());
    if (s.float_target && ext.find("GL_EXT_color_buffer_float") == std::string::npos)
        die(1, "--float needs GL_EXT_color_buffer_float");

    const std::string vs = read_file(files[0]);
    quad_vao();
    if (!compare) {
        Program p = build(vs, files[1], s, srgb_switch);
        std::printf("%s: %s | target 0x%x %dx%d viewport %d,%d %dx%d\n", p.name.c_str(), p.stats.compact().c_str(),
                    p.fmt, p.tw, p.th, p.vx, p.vy, p.vw, p.vh);
        if (time_frames > 0) {
            using Clock = std::chrono::steady_clock;
            std::vector<double> ms;
            for (int f = 0; f < time_frames; ++f) {
                glFinish();
                const auto t0 = Clock::now();
                draw(p, srgb_switch);
                glFinish();
                ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
            }
            std::vector<double> sorted = ms;
            std::sort(sorted.begin(), sorted.end());
            double sum = 0;
            for (double v : ms) sum += v;
            std::printf("%s: time %d frames: median %.2f ms, mean %.2f ms, min %.2f ms (wall clock, GPU shared: relative)\n",
                        p.name.c_str(), time_frames, sorted[sorted.size() / 2], sum / ms.size(), sorted.front());
            check_gl("timing " + p.name);
        }
    } else {
        Program a = build(vs, files[1], s, srgb_switch);
        Program b = build(vs, files[2], s, srgb_switch);
        if (a.fmt != b.fmt || a.vw != b.vw || a.vh != b.vh) die(1, "the two programs draw into different targets");
        std::printf("A %s: %s\nB %s: %s\n", a.name.c_str(), a.stats.compact().c_str(), b.name.c_str(),
                    b.stats.compact().c_str());
        long worst = 0;
        // One compile each; every uniform file drawn through both (the first one's picture
        // and framebuffer for all: a file that differs there is refused).
        for (std::size_t k = 0; k < std::max<std::size_t>(uniforms.size(), 1); ++k) {
            Setup sk = s;
            if (!uniforms.empty()) read_inputs(uniforms[k], sk.in);
            if (sk.in.fb_w != s.in.fb_w || sk.in.fb_h != s.in.fb_h || sk.in.pic_x != s.in.pic_x ||
                sk.in.pic_y != s.in.pic_y || sk.in.pic_w != s.in.pic_w || sk.in.pic_h != s.in.pic_h)
                die(2, "every uniform file of one comparison must describe the same picture");
            for (Program* q : {&a, &b}) {
                q->tex[7] = make_mask(sk.in, q->tex[7]);
                set_uniforms(*q, sk);
                draw(*q, srgb_switch);
            }
            const std::vector<float> ra = read_back(a);
            const std::vector<float> rb = read_back(b);
            const bool fl = float_format(a.fmt);
            float max_diff[3] = {0, 0, 0};
            long over = 0;
            const float tol = fl ? 1.0f / 255.0f : 1.0f;
            for (std::size_t i = 0; i < ra.size(); i += 4) {
                bool bad = false;
                for (int c = 0; c < 3; ++c) {
                    const float d = std::fabs(ra[i + c] - rb[i + c]);
                    max_diff[c] = std::max(max_diff[c], d);
                    bad = bad || d > tol;
                }
                over += bad;
            }
            worst = std::max(worst, over);
            const float unit = fl ? 255.0f : 1.0f;   // reported in 1/255 steps
            const std::string label = uniforms.empty() ? "(no uniform file)" : uniforms[k];
            std::printf("compare %s %s: max |A-B| R %.3f G %.3f B %.3f (1/255 steps); %ld of %ld pixels differ by more "
                        "than 1/255\n",
                        a.fmt == GL_RGBA32F ? "RGBA32F" : a.fmt == GL_RGBA16F ? "RGBA16F"
                        : a.fmt == GL_SRGB8_ALPHA8 ? "SRGB8_ALPHA8" : "RGBA8", label.c_str(),
                        max_diff[0] * unit, max_diff[1] * unit, max_diff[2] * unit, over,
                        static_cast<long>(ra.size() / 4));
        }
        check_gl("comparing");
        close_gpu(gpu);
        return worst > 0 ? 3 : 0;
    }
    close_gpu(gpu);
    return 0;
}
