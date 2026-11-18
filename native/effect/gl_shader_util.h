// ============================================================================
// [ĐÁNH GIÁ ĐIỂM NGHẼN HỆ THỐNG]:
// 1. Khởi tạo và biên dịch Shader phân mảnh:
//    - Tự viết logic biên dịch shader ở từng file gây trùng lặp và khó kiểm soát lỗi.
// 2. Đồng bộ thuộc tính Quad và Uniform Locations:
//    - Cần quy chuẩn hóa Vertex Shader fullscreen quad chuẩn ES 3.00 để tái sử dụng s_quadVao.
//
// [HƯỚNG KHẮC PHỤC TRIỆT ĐỂ — KHÔNG VÁ]:
// 1. Single Shader Pipeline Utility:
//    - Cung cấp hàm link_effect_program và bind_effect_program_input dùng chung toàn bộ C++.
// 2. Chuẩn hóa Vertex Shader ES 3.00 đồng nhất:
//    - Toàn bộ effect và post-process dùng chung kQuadVS, xuất ra v_uv [0, 1] chính xác.
// ============================================================================
#pragma once
#include "data_structures.h"
#include "render/state.h"
#if defined(__EMSCRIPTEN__)
#include <GLES3/gl3.h>
#include <emscripten.h>
#include <cstdio>
namespace stockcut {
namespace effect {
constexpr u32 SHADER_INFO_LOG_CAP = 1024;
constexpr u32 STATIC_TEXTURE_SLOT_CAP = 8;

struct StaticTextureSlot {
    GLuint texId = 0;
    u32 width = 0;
    u32 height = 0;
};

struct alignas(64) StaticTextureRegistry {
    MultiArray<StaticTextureSlot, STATIC_TEXTURE_SLOT_CAP> slots{};
    MultiArray<GLuint, STATIC_TEXTURE_SLOT_CAP> fallbacks{};

    inline void register_slot(u32 slot, u32 texId, u32 width, u32 height) noexcept {
        if (slot >= STATIC_TEXTURE_SLOT_CAP) return;
        slots[slot].texId = static_cast<GLuint>(texId);
        slots[slot].width = width;
        slots[slot].height = height;
        if (texId != 0) {
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texId));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            // Slot 3 = RainMap_Camera_Trail (clamp), Slots 0/1/2 = noise/streaks/small_medium (repeat)
            const GLint wrapMode = (slot == 3u) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode);
            glGenerateMipmap(GL_TEXTURE_2D);
            render::g_gpuState.invalidate_texture<0>();
        }
    }

    inline GLuint get_or_fallback(u32 slot) noexcept {
        if (slot >= STATIC_TEXTURE_SLOT_CAP) return 0;
        if (slots[slot].texId != 0) return slots[slot].texId;
        if (fallbacks[slot] != 0) return fallbacks[slot];
        GLuint fb = 0;
        glGenTextures(1, &fb);
        if (!fb) return 0;
        glBindTexture(GL_TEXTURE_2D, fb);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        const GLint wrapMode = (slot == 3u) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode);
        u8 rgba[4] = { 128, 128, 128, 255 };
        if (slot == 1u) {
            rgba[0] = 0; rgba[1] = 0; rgba[2] = 0; rgba[3] = 0; // Transparent streaks fallback
        } else if (slot == 2u || slot == 3u) {
            rgba[0] = 128; rgba[1] = 127; rgba[2] = 255; rgba[3] = 255; // Flat normal fallback
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        render::g_gpuState.invalidate_texture<0>();
        fallbacks[slot] = fb;
        return fb;
    }
};

inline StaticTextureRegistry& static_texture_registry() noexcept {
    static StaticTextureRegistry s_reg{};
    return s_reg;
}

inline void register_static_texture(u32 slot, u32 texId, u32 width, u32 height) noexcept {
    static_texture_registry().register_slot(slot, texId, width, height);
}

inline GLuint get_static_texture(u32 slot) noexcept {
    return static_texture_registry().get_or_fallback(slot);
}
inline constexpr const char* kQuadVS = R"(#version 300 es
layout(location = 0) in vec2 a;
out vec2 v_uv;
void main() {
    v_uv = (a + 1.0) * 0.5;
    gl_Position = vec4(a, 0.0, 1.0);
}
)";
inline GLuint compile_shader_stage(GLenum type, const char* src) noexcept {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        MultiArray<char, SHADER_INFO_LOG_CAP> infoLog{};
        glGetShaderInfoLog(shader, static_cast<GLsizei>(SHADER_INFO_LOG_CAP), nullptr, infoLog.data());
        std::printf("[FX Shader Error] Compile failed: %s\n", infoLog.data());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
inline GLuint link_effect_program(const char* vsSrc, const char* fsSrc) noexcept {
    GLuint vs = compile_shader_stage(GL_VERTEX_SHADER, vsSrc ? vsSrc : kQuadVS);
    GLuint fs = compile_shader_stage(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        MultiArray<char, SHADER_INFO_LOG_CAP> infoLog{};
        glGetProgramInfoLog(prog, static_cast<GLsizei>(SHADER_INFO_LOG_CAP), nullptr, infoLog.data());
        std::printf("[FX Program Error] Link failed: %s\n", infoLog.data());
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}
inline void bind_effect_program_input(
    GLuint prog, GLuint inTex, GLint locSrc, GLint locRes, u32 width, u32 height
) noexcept {
    render::g_gpuState.use_program(prog);
    render::g_gpuState.bind_texture<0>(inTex);
    if (locSrc >= 0) {
        glUniform1i(locSrc, 0);
    }
    if (locRes >= 0) {
        glUniform2f(locRes, static_cast<float>(width), static_cast<float>(height));
    }
}
} // namespace effect
} // namespace stockcut
#endif
