// ============================================================================
// native/render/state.h
// HARDWARE GRAPHICS STATE TRACKER & COMPILE-TIME BACKEND POLICY
// Tinh hoa Stockfish / AAA Engine: Compile-Time Template Specialization, Zero Branching
// ============================================================================
#pragma once
#include "data_structures.h"
#include <cstring>

#if defined(__EMSCRIPTEN__)
#include <GLES3/gl3.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#define STOCKCUT_HAS_GLES3 1
#elif defined(STOCKCUT_DESKTOP_GLES3)
#include <GLES3/gl3.h>
#include <chrono>
#define STOCKCUT_HAS_GLES3 1
#else
#include <chrono>
using GLuint = stockcut::u32;
using GLint = stockcut::i32;
using GLenum = stockcut::u32;
using GLsizei = stockcut::i32;
using GLsizeiptr = stockcut::u32;
using GLintptr = stockcut::u32;
#endif

namespace stockcut {
namespace render {

enum class BackendKind : u8 {
    GLES3_Unified = 0, // WebGL2 (WASM) & Desktop Native OpenGL ES 3.0 / Google ANGLE
    Dawn_WebGPU   = 1, // WebGPU Native (Google Dawn D3D12 / Metal / Vulkan)
    Vulkan_Direct = 2  // Desktop Native Vulkan Extreme
};

#if defined(STOCKCUT_HAS_GLES3)
struct alignas(64) StateManager {
    MultiArray<GLuint, 8> currentTex{};
    MultiArray<GLuint, 4> currentUbo{};
    MultiArray<GLintptr, 4> currentUboOffset{};
    MultiArray<GLsizeiptr, 4> currentUboSize{};
    GLuint currentProgram = 0;
    GLuint currentFbo = 0xFFFFFFFF;
    GLuint currentVao = 0;
    u32 currentViewportW = 0;
    u32 currentViewportH = 0;
    GLenum currentActiveTexUnit = GL_TEXTURE0;
    GLenum blendSrcRGB = 0;
    GLenum blendDstRGB = 0;
    GLenum blendSrcAlpha = 0;
    GLenum blendDstAlpha = 0;
    bool blendEnabled = false;
    bool scissorEnabled = false;

    static inline bool init_hardware_context(const char* canvasSelector) noexcept {
#if defined(__EMSCRIPTEN__)
        EmscriptenWebGLContextAttributes attrs;
        emscripten_webgl_init_context_attributes(&attrs);
        attrs.majorVersion = 2;
        attrs.minorVersion = 0;
        attrs.alpha = true;
        attrs.depth = false;
        attrs.stencil = false;
        attrs.antialias = false;
        attrs.premultipliedAlpha = true;
        attrs.preserveDrawingBuffer = true;
        attrs.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
        const char* selector = canvasSelector ? canvasSelector : "#engine-canvas";
        EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context(selector, &attrs);
        if (ctx <= 0 && std::strcmp(selector, "#native-gl-canvas") != 0) {
            ctx = emscripten_webgl_create_context("#native-gl-canvas", &attrs);
        }
        if (ctx <= 0) {
            return false;
        }
        emscripten_webgl_make_context_current(ctx);
        return true;
#else
        (void)canvasSelector;
        return true;
#endif
    }

    static inline double now_ms() noexcept {
#if defined(__EMSCRIPTEN__)
        return emscripten_get_now();
#else
        using Clock = std::chrono::steady_clock;
        return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
#endif
    }

    inline void reset() noexcept {
        currentProgram = 0;
        currentFbo = 0xFFFFFFFF;
        currentVao = 0;
        currentTex.fill(0xFFFFFFFF);
        currentUbo.fill(0);
        currentUboOffset.fill(0);
        currentUboSize.fill(0);
        currentViewportW = 0;
        currentViewportH = 0;
        currentActiveTexUnit = GL_TEXTURE0;
        blendSrcRGB = 0;
        blendDstRGB = 0;
        blendSrcAlpha = 0;
        blendDstAlpha = 0;
        blendEnabled = false;
        scissorEnabled = false;
    }

    inline void use_program(GLuint prog) noexcept {
        if (currentProgram != prog) {
            glUseProgram(prog);
            currentProgram = prog;
        }
    }

    inline void bind_framebuffer(GLuint fbo, u32 w, u32 h) noexcept {
        if (currentFbo != fbo) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            currentFbo = fbo;
        }
        if (currentViewportW != w || currentViewportH != h) {
            glViewport(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
            currentViewportW = w;
            currentViewportH = h;
        }
    }

    // TRỤ CỘT 2: TILE SRAM DISCARD VIA GLINVALIDATEFRAMEBUFFER
    // Báo cho Hardware Tile Engine hủy tile SRAM tại chỗ, triệt tiêu xả VRAM ngoài
    inline void discard_color_attachment(GLenum target = GL_FRAMEBUFFER) noexcept {
        constexpr GLenum attachment = GL_COLOR_ATTACHMENT0;
        glInvalidateFramebuffer(target, 1, &attachment);
    }

    inline void discard_attachments(GLenum target, GLsizei numAttachments, const GLenum* attachments) noexcept {
        if (numAttachments > 0 && attachments != nullptr) {
            glInvalidateFramebuffer(target, numAttachments, attachments);
        }
    }

    inline void bind_vao(GLuint vao) noexcept {
        if (currentVao != vao) {
            glBindVertexArray(vao);
            currentVao = vao;
        }
    }

    template <bool Enable>
    inline void set_blend() noexcept {
        if (blendEnabled != Enable) {
            if constexpr (Enable) {
                glEnable(GL_BLEND);
            } else {
                glDisable(GL_BLEND);
            }
            blendEnabled = Enable;
        }
    }

    // Zero-Redundant Blend Mode State Switching
    inline void set_blend_func_separate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) noexcept {
        if (blendSrcRGB != srcRGB || blendDstRGB != dstRGB ||
            blendSrcAlpha != srcAlpha || blendDstAlpha != dstAlpha) {
            glBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha);
            blendSrcRGB = srcRGB;
            blendDstRGB = dstRGB;
            blendSrcAlpha = srcAlpha;
            blendDstAlpha = dstAlpha;
        }
    }

    template <bool Enable>
    inline void set_scissor() noexcept {
        if (scissorEnabled != Enable) {
            if constexpr (Enable) {
                glEnable(GL_SCISSOR_TEST);
            } else {
                glDisable(GL_SCISSOR_TEST);
            }
            scissorEnabled = Enable;
        }
    }

    // TRỤ CỘT 1: ZERO-REDUNDANT UNIFORM BUFFER OBJECT (UBO) RANGE BINDING
    inline void bind_ubo_range(u32 slot, GLuint ubo, GLintptr offset, GLsizeiptr size) noexcept {
        if (slot < 4) {
            if (currentUbo[slot] != ubo || currentUboOffset[slot] != offset || currentUboSize[slot] != size) {
                glBindBufferRange(GL_UNIFORM_BUFFER, slot, ubo, offset, size);
                currentUbo[slot] = ubo;
                currentUboOffset[slot] = offset;
                currentUboSize[slot] = size;
            }
        } else {
            glBindBufferRange(GL_UNIFORM_BUFFER, slot, ubo, offset, size);
        }
    }

    inline void bind_ubo_base(u32 slot, GLuint ubo) noexcept {
        if (slot < 4) {
            if (currentUbo[slot] != ubo || currentUboOffset[slot] != 0) {
                glBindBufferBase(GL_UNIFORM_BUFFER, slot, ubo);
                currentUbo[slot] = ubo;
                currentUboOffset[slot] = 0;
                currentUboSize[slot] = 0;
            }
        } else {
            glBindBufferBase(GL_UNIFORM_BUFFER, slot, ubo);
        }
    }

    template <u32 Unit = 0>
    inline void bind_texture(GLuint tex) noexcept {
        static_assert(Unit < 8, "Texture unit must be < 8");
        constexpr GLenum glUnit = GL_TEXTURE0 + Unit;
        if (currentActiveTexUnit != glUnit) {
            glActiveTexture(glUnit);
            currentActiveTexUnit = glUnit;
        }
        if (currentTex[Unit] != tex) {
            glBindTexture(GL_TEXTURE_2D, tex);
            currentTex[Unit] = tex;
        }
    }

    inline void bind_texture_dynamic(u32 unitIdx, GLuint tex) noexcept {
        const GLenum unit = GL_TEXTURE0 + unitIdx;
        if (currentActiveTexUnit != unit) {
            glActiveTexture(unit);
            currentActiveTexUnit = unit;
        }
        if (unitIdx < 8) {
            if (currentTex[unitIdx] != tex) {
                glBindTexture(GL_TEXTURE_2D, tex);
                currentTex[unitIdx] = tex;
            }
        } else {
            glBindTexture(GL_TEXTURE_2D, tex);
        }
    }

    template <u32 Unit = 0>
    inline void invalidate_texture() noexcept {
        static_assert(Unit < 8, "Texture unit must be < 8");
        currentTex[Unit] = 0xFFFFFFFF;
    }

    inline void invalidate_fbo() noexcept {
        currentFbo = 0xFFFFFFFF;
        currentViewportW = 0;
        currentViewportH = 0;
    }

    inline void invalidate_vao() noexcept {
        currentVao = 0;
    }

    inline void blit_to_screen(GLuint srcFbo, u32 width, u32 height) noexcept {
        if (srcFbo != 0) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer(
                0, 0, static_cast<GLint>(width), static_cast<GLint>(height),
                0, 0, static_cast<GLint>(width), static_cast<GLint>(height),
                GL_COLOR_BUFFER_BIT, GL_NEAREST
            );
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            currentFbo = 0;
        }
    }

    inline void invalidate_all() noexcept {
        reset();
    }
};

template <BackendKind Kind = BackendKind::GLES3_Unified>
struct alignas(64) GpuBackend : public StateManager {};

extern GpuBackend<BackendKind::GLES3_Unified> g_gpuState;

#endif // defined(STOCKCUT_HAS_GLES3)

} // namespace render
} // namespace stockcut
