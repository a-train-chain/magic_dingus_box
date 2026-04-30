#include "gst_renderer.h"
#include "gst_player.h"
#include "../utils/logger.h"
#include <vector>
#include <cstring>

namespace video {

// Simple vertex shader
static const char* vertex_shader_src = R"(
    #version 300 es
    layout(location = 0) in vec2 aPos;
    layout(location = 1) in vec2 aTexCoord;
    out vec2 TexCoord;
    void main() {
        gl_Position = vec4(aPos, 0.0, 1.0);
        TexCoord = aTexCoord;
    }
)";

// Fragment shader for RGBA (format 0)
static const char* fragment_shader_rgba = R"(
    #version 300 es
    precision mediump float;
    out vec4 FragColor;
    in vec2 TexCoord;
    uniform sampler2D textureY; // Used as main texture for RGBA
    void main() {
        FragColor = texture(textureY, TexCoord);
    }
)";

// Fragment shader for I420 (format 1) - YUV to RGB
// Y is full res, U and V are half res
static const char* fragment_shader_i420 = R"(
    #version 300 es
    precision mediump float;
    out vec4 FragColor;
    in vec2 TexCoord;
    uniform sampler2D textureY;
    uniform sampler2D textureU;
    uniform sampler2D textureV;
    
    void main() {
        float y = texture(textureY, TexCoord).r;
        float u = texture(textureU, TexCoord).r - 0.5;
        float v = texture(textureV, TexCoord).r - 0.5;
        
        float r = y + 1.402 * v;
        float g = y - 0.344136 * u - 0.714136 * v;
        float b = y + 1.772 * u;
        
        FragColor = vec4(r, g, b, 1.0);
    }
)";

// Fragment shader for NV12 (format 2) - YUV to RGB
// Y is full res, UV is interleaved half res
static const char* fragment_shader_nv12 = R"(
    #version 300 es
    precision mediump float;
    out vec4 FragColor;
    in vec2 TexCoord;
    uniform sampler2D textureY;
    uniform sampler2D textureU; // Actually UV plane
    
    void main() {
        float y = texture(textureY, TexCoord).r;
        vec2 uv = texture(textureU, TexCoord).rg - 0.5; // NV12 has UV interleaved
        
        float u = uv.x;
        float v = uv.y;
        
        float r = y + 1.402 * v;
        float g = y - 0.344136 * u - 0.714136 * v;
        float b = y + 1.772 * u;
        
        FragColor = vec4(r, g, b, 1.0);
    }
)";

GstRenderer::GstRenderer()
    : player_(nullptr)
    , appsink_(nullptr)
    , width_(0)
    , height_(0)
    , texture_ids_{0, 0, 0}
    , program_id_(0)
    , vao_id_(0)
    , vbo_id_(0)
    , frame_width_(0)
    , frame_height_(0)
    , frame_format_(-1) // Invalid initially
    , gl_initialized_(false)
{
}

GstRenderer::~GstRenderer() {
    cleanup();
}

bool GstRenderer::initialize(GstPlayer* player) {
    if (!player) return false;
    player_ = player;
    appsink_ = player->get_appsink();
    
    if (!appsink_) {
        LOG_ERROR("GstRenderer: appsink is null");
        return false;
    }
    
    // GL resources will be initialized on first render
    
    return true;
}

void GstRenderer::reset_gl() {
    // After an external app (like RetroArch) takes over the EGL context,
    // our GL resources are invalid. Delete them and mark for lazy re-init.
    LOG_DEBUG("GstRenderer: Resetting GL resources after external context takeover");
    
    if (gl_initialized_) {
        // These resources may be invalid but try to delete for cleanliness
        if (vao_id_ != 0) {
            glDeleteVertexArrays(1, &vao_id_);
            vao_id_ = 0;
        }
        if (vbo_id_ != 0) {
            glDeleteBuffers(1, &vbo_id_);
            vbo_id_ = 0;
        }
        if (program_id_ != 0) {
            glDeleteProgram(program_id_);
            program_id_ = 0;
        }
        for (int i = 0; i < 3; i++) {
            if (texture_ids_[i] != 0) {
                glDeleteTextures(1, &texture_ids_[i]);
                texture_ids_[i] = 0;
            }
        }
    }
    
    // Reset state for lazy re-initialization
    gl_initialized_ = false;
    frame_format_ = -1;
    frame_width_ = 0;
    frame_height_ = 0;
    frame_par_num_ = 1;
    frame_par_den_ = 1;
    textures_allocated_ = false;
    allocated_width_ = 0;
    allocated_height_ = 0;
    allocated_format_ = -1;
    frame_dirty_ = false;

    LOG_DEBUG("GstRenderer: GL resources reset complete, will re-init on next render");
}

void GstRenderer::set_viewport_size(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
}

void GstRenderer::set_letterbox_mode(bool enabled) {
    letterbox_mode_ = enabled;
}

void GstRenderer::init_gl_resources() {
    if (gl_initialized_) return;
    
    // Create VBO/VAO for fullscreen quad
    float vertices[] = {
        // positions   // texCoords
        -1.0f, -1.0f,  0.0f, 1.0f, // Bottom-left
         1.0f, -1.0f,  1.0f, 1.0f, // Bottom-right
        -1.0f,  1.0f,  0.0f, 0.0f, // Top-left
         1.0f,  1.0f,  1.0f, 0.0f  // Top-right
    };
    
    glGenVertexArrays(1, &vao_id_);
    glGenBuffers(1, &vbo_id_);
    
    glBindVertexArray(vao_id_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_id_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // Create textures (Y, U, V)
    glGenTextures(3, texture_ids_);
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, texture_ids_[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    
    gl_initialized_ = true;
}

void GstRenderer::update_shader(int format) {
    if (program_id_ != 0) {
        glDeleteProgram(program_id_);
        program_id_ = 0;
    }
    
    const char* fs_source = nullptr;
    if (format == 0) fs_source = fragment_shader_rgba;
    else if (format == 1) fs_source = fragment_shader_i420;
    else if (format == 2) fs_source = fragment_shader_nv12;
    else return; // Unknown format
    
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertex_shader_src, nullptr);
    glCompileShader(vs);
    
    // Check VS compile errors
    GLint success;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(vs, 512, nullptr, infoLog);
        LOG_ERROR("VS Compile Error: {}", infoLog);
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_source, nullptr);
    glCompileShader(fs);

    // Check FS compile errors
    glGetShaderiv(fs, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(fs, 512, nullptr, infoLog);
        LOG_ERROR("FS Compile Error (fmt={}): {}", format, infoLog);
    }

    program_id_ = glCreateProgram();
    glAttachShader(program_id_, vs);
    glAttachShader(program_id_, fs);
    glLinkProgram(program_id_);
    
    glDeleteShader(vs);
    glDeleteShader(fs);
    
    // Set uniforms
    glUseProgram(program_id_);
    glUniform1i(glGetUniformLocation(program_id_, "textureY"), 0);
    if (format == 1) {
        glUniform1i(glGetUniformLocation(program_id_, "textureU"), 1);
        glUniform1i(glGetUniformLocation(program_id_, "textureV"), 2);
    } else if (format == 2) {
        glUniform1i(glGetUniformLocation(program_id_, "textureU"), 1);
    }
    
    frame_format_ = format;
}

void GstRenderer::cleanup() {
    if (gl_initialized_) {
        glDeleteTextures(3, texture_ids_);
        glDeleteVertexArrays(1, &vao_id_);
        glDeleteBuffers(1, &vbo_id_);
        if (program_id_ != 0) glDeleteProgram(program_id_);
        gl_initialized_ = false;
    }
    textures_allocated_ = false;
    allocated_width_ = 0;
    allocated_height_ = 0;
    allocated_format_ = -1;
    frame_dirty_ = false;
}

uint64_t GstRenderer::get_update_flags() const {
    // Always report UPDATE_FRAME when pipeline is active.
    // We can't peek the appsink without consuming the sample, and GStreamer
    // delivers frames asynchronously. The render() method uses try_pull_sample
    // with a 0 timeout which is already non-blocking, so the cost of calling
    // render() when no new frame is available is minimal (just a failed pull).
    if (appsink_) {
        return UPDATE_FRAME;
    }
    return 0;
}

void GstRenderer::render() {
    if (!gl_initialized_) init_gl_resources();

    // Pull sample (non-blocking)
    GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), 0);

    if (sample) {
        upload_frame(sample);
        gst_sample_unref(sample);
    }

    render_quad();
}

void GstRenderer::upload_frame(GstSample* sample) {
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer) return;

    GstCaps* caps = gst_sample_get_caps(sample);
    GstStructure* s = gst_caps_get_structure(caps, 0);
    const gchar* format_str = gst_structure_get_string(s, "format");

    // gst_structure_get_string returns nullptr if the field is absent.
    // Caps renegotiation mid-stream and passthrough elements can produce
    // caps without a "format" field — passing nullptr to spdlog or strcmp
    // below is a segfault. Skip the frame instead.
    if (!format_str) {
        LOG_WARN("GstRenderer: caps missing 'format' field; skipping frame");
        return;
    }

    LOG_DEBUG("GstRenderer: Processing frame format={}", format_str);

    int w, h;
    gst_structure_get_int(s, "width", &w);
    gst_structure_get_int(s, "height", &h);

    // Pixel aspect ratio. Most modern files don't include it (implies 1:1)
    // or include 1/1 explicitly. DVDs and some broadcast captures use
    // non-square pixels — gst_structure_get_fraction returns FALSE if the
    // field is absent, in which case we keep the default 1:1.
    int par_n = 1, par_d = 1;
    gst_structure_get_fraction(s, "pixel-aspect-ratio", &par_n, &par_d);
    if (par_n <= 0 || par_d <= 0) {
        // Defensive: malformed PAR — fall back to square pixels.
        par_n = 1;
        par_d = 1;
    }
    if (par_n != frame_par_num_ || par_d != frame_par_den_) {
        LOG_DEBUG("GstRenderer: pixel-aspect-ratio = {}:{}", par_n, par_d);
    }
    frame_par_num_ = par_n;
    frame_par_den_ = par_d;

    // Get stride information for proper plane alignment
    int y_stride = w;
    int uv_stride = w / 2;
    if (gst_structure_has_field(s, "stride")) {
        const GValue* stride_val = gst_structure_get_value(s, "stride");
        if (G_VALUE_HOLDS_INT(stride_val)) {
            y_stride = g_value_get_int(stride_val);
        } else if (GST_VALUE_HOLDS_ARRAY(stride_val)) {
            // Multiple strides (one per plane)
            GArray* strides = (GArray*)g_value_get_boxed(stride_val);
            if (strides->len >= 1) {
                y_stride = g_array_index(strides, int, 0);
            }
            if (strides->len >= 2) {
                uv_stride = g_array_index(strides, int, 1);
            }
        }
    }
    
    // Determine format
    int format = -1;
    if (strcmp(format_str, "RGBA") == 0) format = 0;
    else if (strcmp(format_str, "I420") == 0) format = 1;
    else if (strcmp(format_str, "NV12") == 0) format = 2;
    else if (strcmp(format_str, "YUY2") == 0) format = 3;
    else if (strcmp(format_str, "UYVY") == 0) format = 4;

    LOG_DEBUG("GstRenderer: Detected format {} ({}) for {}x{}, strides: y={}, uv={}",
              format, format_str, w, h, y_stride, uv_stride);

    if (format == -1) {
        LOG_WARN("Unsupported format: {} - will attempt RGBA conversion", format_str);
        // For unsupported formats, try to force RGBA conversion
        // This is a fallback that may not work perfectly
        format = 0; // Treat as RGBA for now
    }
    
    // Update shader if format changed
    if (format != frame_format_) {
        update_shader(format);
    }
    
    frame_width_ = w;
    frame_height_ = h;
    
    // Check if texture dimensions or format changed (need full reallocation)
    bool size_changed = !textures_allocated_ || w != allocated_width_ || h != allocated_height_ || format != allocated_format_;

    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        if (format == 0) { // RGBA
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture_ids_[0]);
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, map.data);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, map.data);
            }
        }
        else if (format == 1) { // I420 (Y, U, V planar)
            // Calculate plane sizes using strides for proper alignment
            int y_plane_size = y_stride * h;
            int u_plane_size = uv_stride * ((h + 1) / 2);
            int actual_uv_stride = uv_stride;

            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

            // Upload Y plane
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture_ids_[0]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, y_stride);
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, map.data);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, map.data);
            }
            LOG_TRACE("GstRenderer: Uploaded Y plane {}x{} from offset 0", w, h);

            // Upload U plane (handle optional swap)
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, texture_ids_[swap_uv_ ? 2 : 1]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, actual_uv_stride);
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, actual_uv_stride, (h + 1) / 2, 0, GL_RED, GL_UNSIGNED_BYTE, map.data + y_plane_size);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, actual_uv_stride, (h + 1) / 2, GL_RED, GL_UNSIGNED_BYTE, map.data + y_plane_size);
            }
            LOG_TRACE("GstRenderer: Uploaded U plane {}x{} from offset {}", actual_uv_stride, (h + 1) / 2, y_plane_size);

            // Upload V plane
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, texture_ids_[swap_uv_ ? 1 : 2]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, actual_uv_stride);
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, actual_uv_stride, (h + 1) / 2, 0, GL_RED, GL_UNSIGNED_BYTE, map.data + y_plane_size + u_plane_size);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, actual_uv_stride, (h + 1) / 2, GL_RED, GL_UNSIGNED_BYTE, map.data + y_plane_size + u_plane_size);
            }
            LOG_TRACE("GstRenderer: Uploaded V plane {}x{} from offset {}", actual_uv_stride, (h + 1) / 2, y_plane_size + u_plane_size);

            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);  // Reset to default
        }
        else if (format == 2) { // NV12 (Y plane, then UV interleaved)
            int uv_width = (w + 1) / 2;
            int uv_height = (h + 1) / 2;

            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, y_stride);  // Y plane: 1 byte/texel, stride in bytes = texels

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture_ids_[0]);
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, map.data);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, map.data);
            }

            // UV plane: GL_RG = 2 bytes/texel, row has y_stride bytes = y_stride/2 RG texels
            glPixelStorei(GL_UNPACK_ROW_LENGTH, y_stride / 2);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, texture_ids_[1]);
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RG, uv_width, uv_height, 0, GL_RG, GL_UNSIGNED_BYTE,
                             map.data + y_stride * h);  // Use stride for offset
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RG, GL_UNSIGNED_BYTE,
                                map.data + y_stride * h);
            }

            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        }

        // Track allocated dimensions and format for glTexSubImage2D optimization
        if (size_changed) {
            textures_allocated_ = true;
            allocated_width_ = w;
            allocated_height_ = h;
            allocated_format_ = format;
        }

        gst_buffer_unmap(buffer, &map);
    }
}

void GstRenderer::render_quad() {
    if (program_id_ == 0) return;

    // Working canvas — full screen by default, OR the explicit inset
    // rect set via set_render_inset(). All letterbox / aspect-preserve
    // math runs against canvas_w × canvas_h; the final glViewport call
    // adds the inset's (x, y) offset so the video lands at the right
    // position on screen. Marquee playback uses this to push the movie
    // inside a 40 px wood-frame overlay without disturbing the legacy
    // full-screen path used everywhere else (intro video, playlist
    // playback in the main kiosk UI).
    const bool inset_active =
        (render_inset_w_ > 0 && render_inset_h_ > 0);
    const int canvas_w = inset_active
        ? render_inset_w_
        : static_cast<int>(width_);
    const int canvas_h = inset_active
        ? render_inset_h_
        : static_cast<int>(height_);

    // Calculate viewport based on letterbox mode
    int vp_x = 0;
    int vp_y = 0;
    int vp_w = canvas_w;
    int vp_h = canvas_h;

    if (letterbox_mode_) {
        // Calculate 4:3 content area centered in 16:9 frame
        // Height stays the same, width is adjusted
        vp_h = canvas_h;
        vp_w = canvas_h * 4 / 3;  // 4:3 aspect ratio
        vp_x = (canvas_w - vp_w) / 2;  // Center horizontally
        vp_y = 0;

        // If the calculated width is larger than screen (e.g., 4:3 content on 4:3 screen)
        // then pillarbox based on width instead
        if (vp_w > canvas_w) {
            vp_w = canvas_w;
            vp_h = canvas_w * 3 / 4;
            vp_x = 0;
            vp_y = (canvas_h - vp_h) / 2;
        }
    } else if (aspect_preserve_ && frame_width_ > 0 && frame_height_ > 0) {
        // Aspect-preserve mode: keep source video's *display* aspect ratio.
        // Skipped entirely when aspect_preserve_=false (CRT_NATIVE), in which
        // case we fall through to the default fill-the-screen behavior so the
        // attached CRT TV's own 16:9→4:3 HDMI conversion handles aspect
        // correction at its end.
        //
        // Two factors determine display aspect:
        //   1. frame_width_ × frame_height_ — pixel grid dimensions
        //   2. frame_par_num_ : frame_par_den_ — pixel-aspect-ratio
        //      (most modern files have 1:1 = square pixels, but anamorphic
        //      DVDs / broadcast captures use non-square e.g. 32:27)
        //
        // Display aspect = (frame_w × par_n) / (frame_h × par_d)
        //
        // Compare to screen aspect = width_ / height_.
        //   - Source wider than screen  → letterbox (full width, bars top + bottom)
        //   - Source taller than screen → pillarbox (full height, bars left + right)
        //   - Exact match               → fill screen
        //
        // Worked examples on a 1280×720 (16:9 = 1.78:1) screen:
        //
        //   4:3 classic film (1.33:1, e.g. 640×480 PAR 1:1):
        //      vp = 960×720 centered, 160px bars left+right (pillarbox)
        //   16:9 modern TV (1.78:1, e.g. 1920×1080 PAR 1:1):
        //      vp = 1280×720 fill (exact match)
        //   "Flat widescreen" 1.85:1 (e.g. 1920×1040):
        //      vp = 1280×693, ~13px bars top+bottom
        //   70mm Todd-AO 2.20:1 (e.g. 2001):
        //      vp = 1280×582, ~69px bars top+bottom
        //   Standard "Scope" 2.35:1 (e.g. Pulp Fiction):
        //      vp = 1280×545, ~88px bars top+bottom
        //   Modern Scope 2.39:1 (e.g. Mario Galaxy):
        //      vp = 1280×536, ~92px bars top+bottom
        //   Vintage CinemaScope 2.55:1 (e.g. The Robe 1953):
        //      vp = 1280×502, ~109px bars top+bottom
        //   Anamorphic NTSC DVD (720×480 with PAR 32:27, displays 16:9):
        //      effective src = 720×32 : 480×27 = 23040 : 12960 = 1.78:1
        //      vp = 1280×720 fill (correctly identified as 16:9 despite
        //      stored at 720×480 = 1.5:1 raw)
        //   Square 1:1 (e.g. some short-form social content, 1080×1080):
        //      vp = 720×720 centered, 280px bars left+right
        //   Vertical 9:16 portrait (e.g. phone-shot vertical video):
        //      vp = 405×720 centered, 437px bars left+right
        //
        // All math in int64 to avoid overflow on hypothetical large
        // sources, and integer-only to avoid float rounding around the
        // exact-match boundary.
        const int64_t src_w_eff = static_cast<int64_t>(frame_width_) * frame_par_num_;
        const int64_t src_h_eff = static_cast<int64_t>(frame_height_) * frame_par_den_;
        const int64_t screen_w  = static_cast<int64_t>(canvas_w);
        const int64_t screen_h  = static_cast<int64_t>(canvas_h);
        // Cross-multiply: (src_w_eff * screen_h) vs (screen_w * src_h_eff)
        if (src_w_eff * screen_h > screen_w * src_h_eff) {
            // Source wider than screen → letterbox (full width, black bars top+bottom)
            vp_w = static_cast<int>(screen_w);
            vp_h = static_cast<int>(screen_w * src_h_eff / src_w_eff);
            vp_x = 0;
            vp_y = (static_cast<int>(screen_h) - vp_h) / 2;
        } else if (src_w_eff * screen_h < screen_w * src_h_eff) {
            // Source NARROWER than canvas — would normally pillarbox
            // (full height, black bars left+right). When inset_active
            // (Marquee playback inside the wood-frame cabinet) we
            // instead fill WIDTH and let the height overflow above
            // and below the inset rect: the wood-frame asset covers
            // the overflow band cleanly, so the movie reads as
            // completely filling the inside of the cabinet with no
            // left/right black bars. Operator direction — Monte Cristo
            // (1.78:1) was showing pillarbox bars inside the
            // 1.875:1 inset canvas before this change.
            //
            // For 16:9 source in our 1200×640 inset canvas:
            //   vp_w = 1200, vp_h = 1200×9/16 = 675
            //   vp_y = (640−675)/2 = −17  ← intentionally negative
            //   In screen coords (after +40 inset offset): video lives
            //   at y = 23..698, with the 23..40 and 680..698 overlap
            //   bands hidden behind the wood frame's top + bottom
            //   strips. Zero visible bars on any edge.
            //
            // Legacy callers (intro video, playlist playback in the
            // main kiosk UI) never pass inset_active=true, so the
            // standard pillarbox path is preserved for them.
            if (inset_active) {
                vp_w = static_cast<int>(screen_w);
                vp_h = static_cast<int>(screen_w * src_h_eff / src_w_eff);
                vp_x = 0;
                vp_y = (static_cast<int>(screen_h) - vp_h) / 2;
            } else {
                vp_h = static_cast<int>(screen_h);
                vp_w = static_cast<int>(screen_h * src_w_eff / src_h_eff);
                vp_x = (static_cast<int>(screen_w) - vp_w) / 2;
                vp_y = 0;
            }
        }
        // else: exact aspect match — vp = full canvas as initialized.
    }

    // Apply the inset offset so the video lands at the right position.
    // Without inset (the default), render_inset_x_/y_ are both 0 and
    // this is a no-op — legacy callers see the same viewport as before.
    glViewport(vp_x + render_inset_x_, vp_y + render_inset_y_,
               vp_w, vp_h);

    // NOTE: We deliberately do NOT bind the default framebuffer here.
    // The caller chooses the target framebuffer:
    //   - Legacy path: nothing has rebound the FB, so it's already 0.
    //   - Enhanced CRT path (Renderer::begin_scene_fbo): the offscreen
    //     scene FBO is bound, and we want video to render INTO it so
    //     the composite pass can apply scanlines/mask/etc. to a frame
    //     that contains the actual video content. Hard-binding 0 here
    //     would silently break that — video would land on the default
    //     FB which the composite then overwrites, and the operator
    //     would see audio without picture.
    // If a future caller passes garbage GL state, that's its bug to fix
    // before invoking us.

    // Disable depth test to ensure we draw over everything (or background)
    glDisable(GL_DEPTH_TEST);
    
    glUseProgram(program_id_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_ids_[0]);

    if (frame_format_ == 1) { // I420
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, texture_ids_[1]);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, texture_ids_[2]);
    } else if (frame_format_ == 2) { // NV12
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, texture_ids_[1]);
    }

    // Always render full-screen to fill the 4:3 display uniformly
    // This maintains the original behavior where all videos fill the screen

    glBindVertexArray(vao_id_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

} // namespace video
