#include "gst_renderer.h"
#include "gst_player.h"
#include <iostream>
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
        std::cerr << "GstRenderer: appsink is null" << std::endl;
        return false;
    }
    
    // GL resources will be initialized on first render
    
    return true;
}

void GstRenderer::set_viewport_size(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
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
        std::cerr << "VS Compile Error: " << infoLog << std::endl;
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_source, nullptr);
    glCompileShader(fs);
    
    // Check FS compile errors
    glGetShaderiv(fs, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(fs, 512, nullptr, infoLog);
        std::cerr << "FS Compile Error (fmt=" << format << "): " << infoLog << std::endl;
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
}

uint64_t GstRenderer::get_update_flags() const {
    return UPDATE_FRAME; 
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

    std::cout << "GstRenderer: Processing frame format=" << format_str << std::endl;

    int w, h;
    gst_structure_get_int(s, "width", &w);
    gst_structure_get_int(s, "height", &h);

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

    std::cout << "GstRenderer: Detected format " << format << " (" << format_str << ") for " << w << "x" << h
              << ", strides: y=" << y_stride << ", uv=" << uv_stride << std::endl;

    if (format == -1) {
        std::cerr << "Unsupported format: " << format_str << " - will attempt RGBA conversion" << std::endl;
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
    
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        if (format == 0) { // RGBA
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture_ids_[0]);
            // Ideally use PBO for async upload, but simple upload for now
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, map.data);
        } 
        else if (format == 1) { // I420 (Y, U, V planar)
            // Calculate plane sizes using strides for proper alignment
            int y_plane_size = y_stride * h;
            int u_plane_size = uv_stride * ((h + 1) / 2);

            // Use the strides we got from GStreamer caps
            int actual_uv_stride = uv_stride;
            // Note: GStreamer buffer usually has padding, but with appsink and video/x-raw we might get packed or strided.
            // We assume packed for simplicity or handle stride if we can parse it. 
            // video/x-raw usually provides packed data unless we check strides.
            // Let's try strict upload assuming packed first.
            
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            
            // Upload Y plane
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture_ids_[0]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, y_stride);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, map.data);
            std::cout << "GstRenderer: Uploaded Y plane " << w << "x" << h << " from offset 0" << std::endl;

            // Upload U plane
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, texture_ids_[1]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, actual_uv_stride);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, actual_uv_stride, (h + 1) / 2, 0, GL_RED, GL_UNSIGNED_BYTE, map.data + y_plane_size);
            std::cout << "GstRenderer: Uploaded U plane " << actual_uv_stride << "x" << ((h + 1) / 2) << " from offset " << y_plane_size << std::endl;

            // Upload V plane
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, texture_ids_[2]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, actual_uv_stride);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, actual_uv_stride, (h + 1) / 2, 0, GL_RED, GL_UNSIGNED_BYTE, map.data + y_plane_size + u_plane_size);
            std::cout << "GstRenderer: Uploaded V plane " << actual_uv_stride << "x" << ((h + 1) / 2) << " from offset " << (y_plane_size + u_plane_size) << std::endl;

            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);  // Reset to default
        }
        else if (format == 2) { // NV12 (Y plane, then UV interleaved)
            int y_size = w * h;
            int uv_width = (w + 1) / 2;
            int uv_height = (h + 1) / 2;
            
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture_ids_[0]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, map.data);
            
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, texture_ids_[1]);
            // UV plane is w/2 x h/2 but 2 bytes per pixel (interleaved) -> same width as UV in I420 but RG texture
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RG, uv_width, uv_height, 0, GL_RG, GL_UNSIGNED_BYTE, map.data + y_size);
            
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        }
        
        gst_buffer_unmap(buffer, &map);
    }
}

void GstRenderer::render_quad() {
    if (program_id_ == 0) return;

    glViewport(0, 0, width_, height_);
    
    // Ensure we are drawing to the default framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
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
