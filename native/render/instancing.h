// ============================================================================
// native/render/instancing.h
// TRU COT 4: HARDWARE INSTANCED QUAD RENDERING (GLDRAWARRAYSINSTANCED)
// ----------------------------------------------------------------------------
// Kien truc do hoa AAA (Stockfish Data-Structure-Centric / id Tech 7):
// 1. Static Unit Quad VBO: 1 bo dem 4 dinh duy nhat (Triangle Strip), nap 1 lan duy nhat.
// 2. 64-byte Cacheline Instance Struct:
//    - Thay vi 66 floats (264 bytes) moi quad qua VBO truyen thong, moi quad chi la
//      1 struct 64 bytes (Dung 1 L1 cacheline / 4 vec4s), giam hon 75% bang thong bus!
// 3. GPU Vertex ALU Transformation:
//    - Tinh toan tam xoay (pivot), sin/cos goc xoay va toa do dinh song song tren GPU.
// 4. Triple-Buffered Instance Ring (RingDepth = 3):
//    - Slice [N % 3]: GPU dang ve qua glDrawArraysInstanced.
//    - Slice [(N + 1) % 3]: CPU dong goi instances cho frame tiep theo.
//    - Slice [(N + 2) % 3]: Prefetched buffer cho frame N + 2.
// ============================================================================
#pragma once

#include "types.h"
#include "data_structures.h"
#include "memory.h"
#include "../spatial/field.h"

#if defined(__EMSCRIPTEN__)
#include <GLES3/gl3.h>
#include <emscripten/html5.h>
#else
using GLuint = stockcut::u32;
using GLint = stockcut::i32;
using GLenum = stockcut::u32;
using GLsizei = stockcut::i32;
using GLsizeiptr = stockcut::u32;
using GLintptr = stockcut::u32;
#endif

namespace stockcut {
namespace render {

constexpr u32 MAX_INSTANCED_QUADS = 2048;
constexpr u32 INSTANCE_RING_DEPTH = 3;

// ============================================================================
// 1. QuadInstance (64 bytes = DUNG 1 L1 CACHELINE / 4 VEC4 / 16 FLOATS)
// ============================================================================
struct alignas(64) QuadInstance {
    float rect[4];      // Offset 0:  [x, y, width, height] (vec4)
    float uv[4];        // Offset 16: [u0, v0, u1, v1] (vec4)
    float style[4];     // Offset 32: [rotRad, pivotX, pivotY, packedMeta(texId*16 + mode)] (vec4)
    float color[4];     // Offset 48: [r, g, b, a] (vec4)
};
static_assert(sizeof(QuadInstance) == 64, "QuadInstance must be exactly 64 bytes (1 L1 cacheline)");

// Vertex Shader ES 3.00: Programmable Vertex Pulling (gl_VertexID) & 64B Cacheline Instance
inline constexpr const char* kInstancedQuadVS = R"(#version 300 es
layout(location = 0) in vec4 a_inst_rect;      // x, y, w, h
layout(location = 1) in vec4 a_inst_uv;        // u0, v0, u1, v1
layout(location = 2) in vec4 a_inst_style;     // rotRad, pivotX, pivotY, packedMeta(texId*16 + mode)
layout(location = 3) in vec4 a_inst_color;     // r, g, b, a
uniform vec2 u_res;

out vec2 v_uv;
out vec4 v_color;
out vec4 v_bgColor;
out float v_texId;
out float v_mode;

void main() {
    vec2 unitPos = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    v_uv = mix(a_inst_uv.xy, a_inst_uv.zw, unitPos);
    vec4 bgByte = floor(a_inst_color * 0.25);
    v_color = a_inst_color - bgByte * 4.0;
    v_bgColor = bgByte * (1.0 / 255.0);

    float packedMeta = a_inst_style.w;
    float texId = floor(packedMeta * 0.0625);
    v_texId = texId;
    v_mode = packedMeta - texId * 16.0;

    vec2 worldCorner = a_inst_rect.xy + unitPos * a_inst_rect.zw;
    vec2 pivot = a_inst_style.yz;
    vec2 localPos = worldCorner - pivot;

    float rot = a_inst_style.x;
    float c = cos(rot);
    float s = sin(rot);
    vec2 rotatedPos = vec2(localPos.x * c - localPos.y * s, localPos.x * s + localPos.y * c);

    vec2 worldPos = rotatedPos + pivot;
    vec2 clip = (worldPos / u_res) * 2.0 - 1.0;
    gl_Position = vec4(clip.x, 1.0 - (worldPos.y / u_res.y) * 2.0, 0.0, 1.0);
}
)";

// ============================================================================
// 2. InstancedBatch: BO DEM QUAN LY TRIPLE-BUFFERING CHO INSTANCED DRAW
// ============================================================================
template <u32 MaxQuads = MAX_INSTANCED_QUADS, u32 RingDepth = INSTANCE_RING_DEPTH>
class alignas(64) InstancedBatch {
public:
    static constexpr u32 MAX_QUADS = MaxQuads;
    static constexpr u32 DEPTH = RingDepth;

    InstancedBatch() noexcept = default;

    bool init() noexcept {
        vaoRing_.reset();
        vboRing_.reset();
        instanceCount_ = 0;
#if defined(__EMSCRIPTEN__)
        if (emscripten_webgl_get_current_context() > 0) {
            glGenVertexArrays(static_cast<GLsizei>(RingDepth), vaoRing_.data());
            glGenBuffers(static_cast<GLsizei>(RingDepth), vboRing_.data());

            constexpr GLsizei stride = static_cast<GLsizei>(sizeof(QuadInstance));
            for (u32 i = 0; i < RingDepth; ++i) {
                glBindVertexArray(vaoRing_[i]);
                glBindBuffer(GL_ARRAY_BUFFER, vboRing_[i]);
                glBufferData(
                    GL_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(MaxQuads * sizeof(QuadInstance)),
                    nullptr,
                    GL_DYNAMIC_DRAW
                );

                // Attrib 0: a_inst_rect (offset 0)
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
                glVertexAttribDivisor(0, 1);

                // Attrib 1: a_inst_uv (offset 16)
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(16));
                glVertexAttribDivisor(1, 1);

                // Attrib 2: a_inst_style (offset 32)
                glEnableVertexAttribArray(2);
                glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(32));
                glVertexAttribDivisor(2, 1);

                // Attrib 3: a_inst_color (offset 48)
                glEnableVertexAttribArray(3);
                glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(48));
                glVertexAttribDivisor(3, 1);
            }
            glBindVertexArray(0);
        }
#endif
        return true;
    }

    void reset() noexcept {
        instanceCount_ = 0;
    }

    void advance_ring() noexcept {
        vaoRing_.advance();
        vboRing_.advance();
        instanceCount_ = 0;
    }

    [[nodiscard]] constexpr u32 current_ring_index() const noexcept {
        return vaoRing_.index();
    }

    [[nodiscard]] constexpr u32 next_ring_index() const noexcept {
        return vaoRing_.next_index();
    }

    [[nodiscard]] constexpr u32 prefetch_ring_index() const noexcept {
        return vaoRing_.offset_index(2);
    }

    [[nodiscard]] constexpr u32 count() const noexcept {
        return instanceCount_;
    }

    [[nodiscard]] GLuint current_vao() const noexcept {
        return vaoRing_.current();
    }

    [[nodiscard]] GLuint current_vbo() const noexcept {
        return vboRing_.current();
    }

    inline bool push_quad(
        float x, float y, float w, float h,
        float pivotX, float pivotY, float rotRad,
        float r, float g, float b, float a,
        float texId, float mode, float spinRad,
        float uv0, float v0, float uv1, float v1
    ) noexcept {
        if (instanceCount_ >= MaxQuads) {
            return false;
        }
        QuadInstance& inst = stagingBuffer_[instanceCount_++];
        inst.rect[0] = x;
        inst.rect[1] = y;
        inst.rect[2] = w;
        inst.rect[3] = h;

        inst.uv[0] = uv0;
        inst.uv[1] = v0;
        inst.uv[2] = uv1;
        inst.uv[3] = v1;

        inst.style[0] = rotRad;
        inst.style[1] = pivotX;
        inst.style[2] = pivotY;
        inst.style[3] = texId * 16.0f + mode;

        const u32 bgPacked = std::bit_cast<u32>(spinRad);
        if (__builtin_expect((bgPacked >> 24) != 0u && mode > 2.5f, 0)) {
            inst.color[0] = static_cast<float>(bgPacked & 0xFFu) * 4.0f + clamp(r, 0.0f, 1.0f);
            inst.color[1] = static_cast<float>((bgPacked >> 8) & 0xFFu) * 4.0f + clamp(g, 0.0f, 1.0f);
            inst.color[2] = static_cast<float>((bgPacked >> 16) & 0xFFu) * 4.0f + clamp(b, 0.0f, 1.0f);
            inst.color[3] = static_cast<float>((bgPacked >> 24) & 0xFFu) * 4.0f + clamp(a, 0.0f, 1.0f);
        } else {
            inst.color[0] = r;
            inst.color[1] = g;
            inst.color[2] = b;
            inst.color[3] = a;
        }
        return true;
    }

    // Flush batch hiện tại xuống GPU bằng glDrawArraysInstanced
    u32 flush() noexcept {
        if (instanceCount_ == 0) {
            return 0;
        }
        const u32 drawn = instanceCount_;
#if defined(__EMSCRIPTEN__)
        const GLuint vbo = vboRing_.current();
        const GLuint vao = vaoRing_.current();
        if (vao != 0 && vbo != 0) {
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferSubData(
                GL_ARRAY_BUFFER,
                0,
                static_cast<GLsizeiptr>(drawn * sizeof(QuadInstance)),
                stagingBuffer_.data()
            );
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(drawn));
        }
#endif
        vaoRing_.advance();
        vboRing_.advance();
        instanceCount_ = 0;
        return drawn;
    }

    // Pha 4: PersistentZRankVRAMTable Sub-Cacheline DMA Flush (Uploads ONLY dirty 64B QuadInstance runs)
    u32 flush_dirty_ranges(u32 totalActiveQuads, const spatial::RangePacker<32>& packer) noexcept {
        if (totalActiveQuads == 0u) return 0u;
#if defined(__EMSCRIPTEN__)
        const GLuint vbo = vboRing_.current();
        const GLuint vao = vaoRing_.current();
        if (vao != 0u && vbo != 0u) {
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            for (u32 i = 0u; i < packer.rangeCount; ++i) {
                const auto& r = packer.ranges[i];
                glBufferSubData(
                    GL_ARRAY_BUFFER,
                    static_cast<GLintptr>(r.byteOffset),
                    static_cast<GLsizeiptr>(r.byteSize),
                    stagingBuffer_.data() + r.slotStart
                );
            }
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(totalActiveQuads));
        }
#else
        (void)packer;
#endif
        return totalActiveQuads;
    }

    void destroy() noexcept {
#if defined(__EMSCRIPTEN__)
        for (u32 i = 0; i < RingDepth; ++i) {
            if (vaoRing_[i] != 0) {
                glDeleteVertexArrays(1, &vaoRing_[i]);
                vaoRing_[i] = 0;
            }
            if (vboRing_[i] != 0) {
                glDeleteBuffers(1, &vboRing_[i]);
                vboRing_[i] = 0;
            }
        }
#endif
        instanceCount_ = 0;
        vaoRing_.reset();
        vboRing_.reset();
    }

    [[nodiscard]] const QuadInstance* data() const noexcept {
        return stagingBuffer_.data();
    }

    // ========================================================================
    // Hardware Instanced Batch: Tích hợp trực tiếp vật chứa Mark vào GPU Batch
    // Luồng render tự động nhận biết số tầng layer mà vật chứa Mark nắm giữ
    // ========================================================================
    template <typename MarkContainer, typename QuadConverter>
    u32 emit_from_mark_container(const MarkContainer& container, QuadConverter&& converter) noexcept {
        u32 pushedCount = 0;
        container.for_each_active([&](const auto& item, u32 slot) {
            if (instanceCount_ >= MaxQuads) return;
            if (converter(item, slot, *this)) {
                ++pushedCount;
            }
        });
        return pushedCount;
    }

private:
    alignas(64) MultiArray<QuadInstance, MaxQuads> stagingBuffer_; // 128 KB L1-cacheline aligned
    Ring<GLuint, RingDepth> vaoRing_{};
    Ring<GLuint, RingDepth> vboRing_{};
    u32 instanceCount_ = 0;
};

extern InstancedBatch<MAX_INSTANCED_QUADS> g_gpuInstancedBatch;

} // namespace render
} // namespace stockcut
