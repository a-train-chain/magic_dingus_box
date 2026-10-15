// GL half of UI draw-call batching (MDB_BATCH_UI). The CPU half — vertex
// layout, primitive -> triangle conversion, the one-texture rule — is
// ui/ui_batch.h. See Renderer::BatchScope in renderer.h for the contract.

#include "renderer.h"

#include <GLES3/gl3.h>

#include <iostream>

#include "font_manager.h"

namespace ui {

namespace {

// Same transform as the immediate path's vertex shader, plus a per-vertex
// color. Explicit locations so one VAO layout serves the program.
const char* kBatchVertexShader = R"(
#version 300 es
precision mediump float;
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texCoord;
layout(location = 2) in vec4 color;
out vec2 vTexCoord;
out vec4 vColor;
uniform vec2 screenSize;

void main() {
    vec2 normalizedPos = (position / screenSize) * 2.0 - 1.0;
    normalizedPos.y = -normalizedPos.y;  // Flip Y
    gl_Position = vec4(normalizedPos, 0.0, 1.0);
    vTexCoord = texCoord;
    vColor = color;
}
)";

// The immediate shader is `texColor * color` (textured) or `color`
// (solid). Here every draw is textured: solids sample a white texel, so
// texture() == 1.0 and the result is the vertex color, as before.
const char* kBatchFragmentShader = R"(
#version 300 es
precision highp float;
in vec2 vTexCoord;
in vec4 vColor;
out vec4 fragColor;
uniform sampler2D tex;

void main() {
    fragColor = texture(tex, vTexCoord) * vColor;
}
)";

}  // namespace

bool Renderer::init_batch_gl() {
    if (!batch_enabled_) return true;

    const uint32_t vs = compile_shader(kBatchVertexShader, GL_VERTEX_SHADER);
    const uint32_t fs = compile_shader(kBatchFragmentShader, GL_FRAGMENT_SHADER);
    if (vs == 0 || fs == 0) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        std::cerr << "UI Renderer: batch shader failed to compile — "
                     "falling back to immediate drawing" << std::endl;
        batch_enabled_ = false;
        return false;
    }
    batch_program_ = glCreateProgram();
    glAttachShader(batch_program_, vs);
    glAttachShader(batch_program_, fs);
    glLinkProgram(batch_program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(batch_program_, GL_LINK_STATUS, &linked);
    if (!linked) {
        char info[512];
        glGetProgramInfoLog(batch_program_, sizeof(info), nullptr, info);
        std::cerr << "UI Renderer: batch shader link failed: " << info
                  << " — falling back to immediate drawing" << std::endl;
        glDeleteProgram(batch_program_);
        batch_program_ = 0;
        batch_enabled_ = false;
        return false;
    }
    batch_u_screen_size_loc_ = glGetUniformLocation(batch_program_, "screenSize");
    batch_u_tex_loc_ = glGetUniformLocation(batch_program_, "tex");

    glGenVertexArrays(1, &batch_vao_);
    glGenBuffers(1, &batch_vbo_);
    glBindVertexArray(batch_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, batch_vbo_);
    const GLsizei stride = UiBatch::kFloatsPerVertex * sizeof(float);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(4 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    // 1x1 white: solids that cannot join an atlas batch (e.g. right after
    // a poster) sample this. NEAREST + CLAMP: any UV reads exactly 1.0.
    const unsigned char white[4] = {255, 255, 255, 255};
    glGenTextures(1, &white_tex_);
    glBindTexture(GL_TEXTURE_2D, white_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

void Renderer::destroy_batch_gl() {
    // Pending vertices reference textures of the context being torn down.
    batch_.clear();
    if (batch_program_ != 0) {
        glDeleteProgram(batch_program_);
        batch_program_ = 0;
    }
    if (batch_vao_ != 0) {
        glDeleteVertexArrays(1, &batch_vao_);
        batch_vao_ = 0;
    }
    if (batch_vbo_ != 0) {
        glDeleteBuffers(1, &batch_vbo_);
        batch_vbo_ = 0;
    }
    if (white_tex_ != 0) {
        glDeleteTextures(1, &white_tex_);
        white_tex_ = 0;
    }
    batch_u_screen_size_loc_ = -1;
    batch_u_tex_loc_ = -1;
}

bool Renderer::is_atlas_texture(uint32_t tex) const {
    return (title_font_manager_ && title_font_manager_->owns_texture(tex)) ||
           (body_font_manager_ && body_font_manager_->owns_texture(tex));
}

uint32_t Renderer::solid_texture(float& u, float& v) const {
    // Join the pending batch when it samples a glyph-atlas page (every
    // page carries a white block) — solids and text then share one draw.
    if (!batch_.empty() && is_atlas_texture(batch_.texture())) {
        u = FontManager::white_texel_u();
        v = FontManager::white_texel_v();
        return batch_.texture();
    }
    // Starting a batch: prefer the body font's first page, which most of
    // the following text will sample too.
    if (batch_.empty() && body_font_manager_) {
        const uint32_t page = body_font_manager_->first_page_texture();
        if (page != 0) {
            u = FontManager::white_texel_u();
            v = FontManager::white_texel_v();
            return page;
        }
    }
    u = 0.5f;
    v = 0.5f;
    return white_tex_;
}

void Renderer::flush_ui_batch() {
    if (batch_.empty()) return;
    if (batch_program_ == 0) {  // context torn down under us: drop, never draw
        batch_.clear();
        return;
    }

    // The batch draws under the program/texture it needs, then hands back
    // EXACTLY the bindings it found: program, active unit, the unit-0 2D
    // texture, VAO and array buffer. Flushes run in the middle of upload
    // sequences (glBindTexture(tex) → flush → glTexImage2D in the QR,
    // thumbnail and system-logo loaders); leaving texture 0 bound there
    // sent the upload into texture 0 and drew those images as black boxes.
    // Blend, viewport and framebuffer are untouched — every renderer site
    // that changes them flushes first. (glGet* of bindings is client-side
    // state: no GPU sync.)
    GLint prev_program = 0, prev_active = GL_TEXTURE0, prev_tex = 0;
    GLint prev_vao = 0, prev_array_buffer = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_array_buffer);

    // screenSize as the UI program has it NOW — which is what it had when
    // these vertices were appended, because every glUniform2f of it in the
    // renderer is preceded by a flush. (Client-side state query: no GPU
    // sync.)
    GLfloat screen[2] = {static_cast<GLfloat>(width_), static_cast<GLfloat>(height_)};
    if (shader_program_ != 0 && u_screen_size_loc_ >= 0) {
        glGetUniformfv(shader_program_, u_screen_size_loc_, screen);
    }

    glUseProgram(batch_program_);
    if (batch_u_screen_size_loc_ >= 0) {
        glUniform2f(batch_u_screen_size_loc_, screen[0], screen[1]);
    }
    if (batch_u_tex_loc_ >= 0) glUniform1i(batch_u_tex_loc_, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, batch_.texture());

    // One upload per flush. glBufferData with fresh storage each time
    // orphans the previous contents, so the driver never stalls waiting
    // for the GPU to finish reading the last batch.
    glBindBuffer(GL_ARRAY_BUFFER, batch_vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(batch_.byte_size()),
                 batch_.data(), GL_STREAM_DRAW);
    glBindVertexArray(batch_vao_);
    ++ui_draw_calls_;
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(batch_.vertex_count()));
    glBindVertexArray(static_cast<GLuint>(prev_vao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prev_array_buffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
    glActiveTexture(static_cast<GLenum>(prev_active));
    glUseProgram(static_cast<GLuint>(prev_program));

    batch_.clear();
}

}  // namespace ui
