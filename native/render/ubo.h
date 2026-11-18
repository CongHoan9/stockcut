// ============================================================================
// native/render/ubo.h
// TRU COT 1: UNIFORM BUFFER OBJECT (UBO) RING-BUFFER & STD140 STRUCT PACKING
// TRU COT 2: TILE-BASED LATENCY HIDING & PIPELINED PREFETCHING (N, N+1, N+2)
// ----------------------------------------------------------------------------
// Kien truc do hoa AAA (Stockfish Data-Structure-Centric / id Tech 7):
// 1. std140 Struct Layout: Can le 16/64/256 bytes chuan GLSL ES 3.00.
// 2. Triple-Buffering Pipelining (RingDepth = 3):
//    - Slice [N % 3]: GPU dang execute render passes.
//    - Slice [(N + 1) % 3]: CPU marshalling du lieu uniform cho frame ke tiep.
//    - Slice [(N + 2) % 3]: CPU prefetching & keyframe interpolation (Zero Bubble).
// 3. Redundant Uniform Binding Elimination: 1 driver call duy nhat thay cho 165+ glUniform*.
// 4. Zero-Allocation: 100% MultiArray phang, 0 malloc/free trong render loop.
// ============================================================================
#pragma once

#include "types.h"
#include "data_structures.h"
#include "memory.h"

#if defined(__EMSCRIPTEN__)
#include <GLES3/gl3.h>
#include <emscripten/html5.h>
#else
using GLuint = stockcut::u32;
using GLint = stockcut::i32;
using GLenum = stockcut::u32;
using GLsizeiptr = stockcut::u32;
using GLintptr = stockcut::u32;
#endif

namespace stockcut {
namespace render {

// Bo dem can le toi thieu theo chuan WebGL2 / GLES3 GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT
constexpr u32 UBO_HARDWARE_ALIGNMENT = 256;
constexpr u32 UBO_RING_DEPTH = 3; // Pipelining: Frame N (GPU), N+1 (CPU Pack), N+2 (Prefetch)

// ============================================================================
// 1. FrameUniformBlock (std140: 64 bytes = DUNG 1 L1 CACHELINE / 4 VEC4)
// ============================================================================
struct alignas(16) FrameUniformBlock {
    float viewportRes[4];   // x: width, y: height, z: 1.0f / width, w: 1.0f / height
    float timeState[4];     // x: nowMs, y: dtMs, z: frameTick, w: isPlaying (1.0f / 0.0f)
    float bassHarmonics[4]; // x: bassEnergy, y: bassRaw, z: bassActive, w: bassAttack
    float bgColor[4];       // r, g, b, a
};
static_assert(sizeof(FrameUniformBlock) == 64, "FrameUniformBlock must be exactly 64 bytes");

// ============================================================================
// 2. PassUniformBlock (std140: 64 bytes = DUNG 1 L1 CACHELINE / 4 VEC4)
// ============================================================================
struct alignas(16) PassUniformBlock {
    float texelSize[4];     // x: 1.0f / srcW, y: 1.0f / srcH, z: srcW, w: srcH
    float params0[4];       // x: radius, y: intensity, z: parameter0, w: parameter1
    float params1[4];       // x: parameter2, y: parameter3, z: kernelIndex, w: flag
    float targetColor[4];   // x: r, y: g, z: b, w: tolerance / threshold
};
static_assert(sizeof(PassUniformBlock) == 64, "PassUniformBlock must be exactly 64 bytes");

// ============================================================================
// 3. MonoidUniformBlock (std140: 128 bytes = DUNG 2 L1 CACHELINES / 8 VEC4)
// ============================================================================
struct alignas(16) MonoidUniformBlock {
    float uberMat[4];       // mat2 (m00, m01, m10, m11)
    float uberTrans[4];     // x: transX, y: transY, z: pad, w: pad
    float uberGain[4];      // gainR, gainG, gainB, gainA
    float uberEmit[4];      // emitR, emitG, emitB, emitA
    float resCountFlags[4]; // x: resX, y: resY, z: count, w: useUber
    float pad0[12];         // 48 bytes dem du 128 bytes
};
static_assert(sizeof(MonoidUniformBlock) == 128, "MonoidUniformBlock must be exactly 128 bytes");

// ============================================================================
// 4. UboRingSlice: Slice bo dem can le 256 bytes tren bo nho vat ly
// ============================================================================
template <typename BlockT>
struct alignas(UBO_HARDWARE_ALIGNMENT) UboRingSlice {
    BlockT data{};
    u8 padding[UBO_HARDWARE_ALIGNMENT - sizeof(BlockT)];
};

// ============================================================================
// 5. UboRing: BỘ ĐỆM VÒNG TRIPLE-BUFFERING KẾ THỪA STRUCT TIỆN ÍCH Ring<T, Depth>
// ============================================================================
template <typename BlockT, u32 RingDepth = UBO_RING_DEPTH>
class alignas(UBO_HARDWARE_ALIGNMENT) UboRing : public Ring<UboRingSlice<BlockT>, RingDepth> {
public:
    static constexpr u32 SLICE_SIZE = sizeof(UboRingSlice<BlockT>);
    static constexpr u32 TOTAL_BUFFER_BYTES = SLICE_SIZE * RingDepth;

    UboRing() noexcept = default;

    bool init(u32 bindingIndex) noexcept {
        bindingIndex_ = bindingIndex;
        this->reset();
#if defined(__EMSCRIPTEN__)
        if (emscripten_webgl_get_current_context() > 0 && uboId_ == 0) {
            glGenBuffers(1, &uboId_);
            glBindBuffer(GL_UNIFORM_BUFFER, uboId_);
            glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(TOTAL_BUFFER_BYTES), nullptr, GL_DYNAMIC_DRAW);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);
        }
#endif
        return true;
    }

    void advance_ring() noexcept {
        this->advance();
    }

    [[nodiscard]] constexpr u32 current_ring_index() const noexcept {
        return this->index();
    }

    [[nodiscard]] constexpr u32 next_ring_index() const noexcept {
        return this->next_index();
    }

    [[nodiscard]] constexpr u32 prefetch_ring_index() const noexcept {
        return this->offset_index(2);
    }

    [[nodiscard]] constexpr u32 current_offset() const noexcept {
        return this->index() * SLICE_SIZE;
    }

    [[nodiscard]] constexpr u32 binding_point() const noexcept {
        return bindingIndex_;
    }

    [[nodiscard]] GLuint ubo_id() const noexcept {
        return uboId_;
    }

    [[nodiscard]] const BlockT& get_current() const noexcept {
        return this->current().data;
    }

    [[nodiscard]] const BlockT& get_slice(u32 ringIdx) const noexcept {
        return (*this)[ringIdx % RingDepth].data;
    }

    // Ghi du lieu truc tiep vao slice hien tai cua vong dem khong can reallocation
    void upload_current(const BlockT& block) noexcept {
        this->current().data = block;
#if defined(__EMSCRIPTEN__)
        if (uboId_ != 0) {
            glBindBuffer(GL_UNIFORM_BUFFER, uboId_);
            glBufferSubData(
                GL_UNIFORM_BUFFER,
                static_cast<GLintptr>(current_offset()),
                static_cast<GLsizeiptr>(sizeof(BlockT)),
                &block
            );
        }
#endif
    }

    // Ghi truoc du lieu cho frame N + 1 (Latency Hiding)
    void upload_staged(u32 targetRingIdx, const BlockT& block) noexcept {
        const u32 safeIdx = targetRingIdx % RingDepth;
        (*this)[safeIdx].data = block;
#if defined(__EMSCRIPTEN__)
        if (uboId_ != 0) {
            glBindBuffer(GL_UNIFORM_BUFFER, uboId_);
            glBufferSubData(
                GL_UNIFORM_BUFFER,
                static_cast<GLintptr>(safeIdx * SLICE_SIZE),
                static_cast<GLsizeiptr>(sizeof(BlockT)),
                &block
            );
        }
#endif
    }

    // Rang buoc range cua slice hien tai vao uniform block binding point tren GPU
    void bind_current() const noexcept {
#if defined(__EMSCRIPTEN__)
        if (uboId_ != 0) {
            glBindBufferRange(
                GL_UNIFORM_BUFFER,
                bindingIndex_,
                uboId_,
                static_cast<GLintptr>(current_offset()),
                static_cast<GLsizeiptr>(sizeof(BlockT))
            );
        }
#endif
    }

    void destroy() noexcept {
#if defined(__EMSCRIPTEN__)
        if (uboId_ != 0) {
            glDeleteBuffers(1, &uboId_);
            uboId_ = 0;
        }
#endif
        this->reset();
    }

private:
    GLuint uboId_ = 0;
    u32 bindingIndex_ = 0;
};

// ============================================================================
// Global UBO Ring-Buffers:
// Binding 0: FrameUniformBlock (Toan bo shader doc chung do phan giai & thoi gian)
// Binding 1: PassUniformBlock  (Tung pass doc thong so rieng ma khong can glUniform)
// Binding 2: MonoidUniformBlock (Dung hop Uber-Shader Monoid Graph)
// ============================================================================
constexpr u32 UBO_BINDING_FRAME  = 0;
constexpr u32 UBO_BINDING_PASS   = 1;
constexpr u32 UBO_BINDING_MONOID = 2;

extern UboRing<FrameUniformBlock>  g_frameUboRing;
extern UboRing<PassUniformBlock>   g_passUboRing;
extern UboRing<MonoidUniformBlock> g_monoidUboRing;

} // namespace render
} // namespace stockcut
