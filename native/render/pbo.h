// ============================================================================
// native/render/pbo.h
// TRU COT 3: PIXEL BUFFER OBJECT (PBO) TRIPLE-BUFFERING FOR ZERO-STALL STREAMING
// ----------------------------------------------------------------------------
// Kien truc do hoa AAA (Stockfish Data-Structure-Centric / id Tech 7):
// 1. Triple-Buffering PBO Ring (RingDepth = 3):
//    - Slot [N % 3]: GPU DMA copy tu PBO vao texture VRAM.
//    - Slot [(N + 1) % 3]: CPU ghi dirty atlas / video frame bytes.
//    - Slot [(N + 2) % 3]: Prefetched buffer cho frame tiep theo.
// 2. Zero-Stall Texture Upload:
//    - Thay vi truyen con tro RAM CPU vao glTexSubImage2D gay pipeline bubble,
//      truyen offset 0 tu GL_PIXEL_UNPACK_BUFFER de GPU copy DMA bat dong bo.
// 3. Zero-Allocation: MultiArray phang, 0 malloc/free trong hot path.
// ============================================================================
#pragma once

#include <cstring>
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
using GLsizei = stockcut::i32;
using GLsizeiptr = stockcut::u32;
using GLintptr = stockcut::u32;
#endif

namespace stockcut {
namespace render {

constexpr u32 DEFAULT_PBO_STREAM_CAPACITY = 2048 * 2048; // 4 MB atlas buffer
// Mở rộng PBO Ring Depth từ 3 lên 5 (Penta-Buffering) giải quyết triệt để nghẽn hàng đợi bất đồng bộ:
// - Index 0: GPU đang render frame N
// - Index 1: GPU DMA đang Unpack frame N+1 (glTexSubImage2D offset 0)
// - Index 2: Driver/CPU đang Staging upload frame N+2 (glBufferSubData)
// - Index 3: Decoder Worker đang giải mã frame N+3
// - Index 4: Demuxer/Prefetcher đang nạp gói frame N+4 từ mạng/bộ nhớ
// Triệt tiêu 100% tình trạng "0 chờ 1, 3 chờ 4", zero GPU-CPU lock bubble.
constexpr u32 PBO_RING_DEPTH = 5;

template <u32 BufferCapacity = DEFAULT_PBO_STREAM_CAPACITY, u32 RingDepth = PBO_RING_DEPTH>
class alignas(64) PboRing : public Ring<GLuint, RingDepth> {
public:
    static constexpr u32 CAPACITY = BufferCapacity;
    static constexpr u32 DEPTH = RingDepth;

    PboRing() noexcept = default;

    bool init(u32 capacityBytes = BufferCapacity) noexcept {
        capacity_ = (capacityBytes > 0) ? capacityBytes : BufferCapacity;
        this->reset();
#if defined(__EMSCRIPTEN__)
        if (emscripten_webgl_get_current_context() > 0) {
            glGenBuffers(static_cast<GLsizei>(RingDepth), this->data());
            for (u32 i = 0; i < RingDepth; ++i) {
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (*this)[i]);
                glBufferData(
                    GL_PIXEL_UNPACK_BUFFER,
                    static_cast<GLsizeiptr>(capacity_),
                    nullptr,
                    GL_STREAM_DRAW
                );
            }
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
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

    [[nodiscard]] constexpr u32 decode_stage_index() const noexcept {
        return this->offset_index(3);
    }

    [[nodiscard]] constexpr u32 demux_stage_index() const noexcept {
        return this->offset_index(4);
    }

    [[nodiscard]] constexpr u32 stage_index(u32 forwardSteps) const noexcept {
        return this->offset_index(forwardSteps);
    }

    [[nodiscard]] GLuint current_pbo_id() const noexcept {
        return this->current();
    }

    [[nodiscard]] constexpr u32 capacity() const noexcept {
        return capacity_;
    }

    // Nạp dữ liệu bất đồng bộ qua DMA: CPU nạp vào PBO hiện tại, sau đó
    // phát lệnh glTexSubImage2D với offset 0 để GPU tự sao chép qua phần cứng.
    bool stream_sub_image_2d(
        GLuint targetTex,
        GLint xoffset, GLint yoffset,
        GLsizei width, GLsizei height,
        GLenum format, GLenum type,
        const void* pixels, u32 byteSize
    ) noexcept {
        if (!pixels || byteSize == 0 || byteSize > capacity_) {
            return false;
        }
#if defined(__EMSCRIPTEN__)
        const GLuint pbo = this->current();
        if (pbo != 0) {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
            // WebGL2 Buffer Orphaning: cắt đứt phụ thuộc khung hình trước
            glBufferData(
                GL_PIXEL_UNPACK_BUFFER,
                static_cast<GLsizeiptr>(capacity_),
                nullptr,
                GL_STREAM_DRAW
            );
            glBufferSubData(
                GL_PIXEL_UNPACK_BUFFER,
                0,
                static_cast<GLsizeiptr>(byteSize),
                pixels
            );

            glBindTexture(GL_TEXTURE_2D, targetTex);
            // Offset 0 trong PBO thay vì con trỏ CPU client memory (Zero CPU Stall!)
            glTexSubImage2D(
                GL_TEXTURE_2D, 0,
                xoffset, yoffset,
                width, height,
                format, type,
                reinterpret_cast<const void*>(0)
            );
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            return true;
        } else {
            // Fallback khi chạy headless / không có ngữ cảnh WebGL phần cứng
            glBindTexture(GL_TEXTURE_2D, targetTex);
            glTexSubImage2D(
                GL_TEXTURE_2D, 0,
                xoffset, yoffset,
                width, height,
                format, type,
                pixels
            );
            return true;
        }
#else
        (void)targetTex; (void)xoffset; (void)yoffset;
        (void)width; (void)height; (void)format; (void)type;
        (void)byteSize;
        return true;
#endif
    }

    void destroy() noexcept {
#if defined(__EMSCRIPTEN__)
        for (u32 i = 0; i < RingDepth; ++i) {
            if ((*this)[i] != 0) {
                glDeleteBuffers(1, &(*this)[i]);
                (*this)[i] = 0;
            }
        }
#endif
        this->reset();
    }

private:
    u32 capacity_ = BufferCapacity;
};

// ============================================================================
// 2. PboPackRing: BỘ ĐỆM VÒNG PIXEL_PACK_BUFFER KẾ THỪA Ring<GLuint, RingDepth>
// ----------------------------------------------------------------------------
// [NON-BLOCKING GPU-TO-CPU FLYWHEEL READBACK (ZERO-STALL)]:
// - Tránh 100% việc CPU bị khóa 16-30ms khi gọi glReadPixels đồng bộ.
// - Khung hình N: glReadPixels(..., 0) với GL_PIXEL_PACK_BUFFER -> GPU DMA ngầm.
// - Khung hình N+2: Đọc dữ liệu đã hoàn tất từ buffer cách 2 frame -> 0ns chờ đợi!
// ============================================================================
constexpr u32 DEFAULT_PACK_CAPACITY = 1920 * 1080 * 4; // 8.3 MB (1080p RGBA)
constexpr u32 PACK_RING_DEPTH = 3;

template <u32 BufferCapacity = DEFAULT_PACK_CAPACITY, u32 RingDepth = PACK_RING_DEPTH>
class alignas(64) PboPackRing : public Ring<GLuint, RingDepth> {
public:
    static constexpr u32 CAPACITY = BufferCapacity;
    static constexpr u32 DEPTH = RingDepth;

    PboPackRing() noexcept = default;

    bool init(u32 capacityBytes = BufferCapacity) noexcept {
        capacity_ = (capacityBytes > 0) ? capacityBytes : BufferCapacity;
        this->reset();
#if defined(__EMSCRIPTEN__)
        if (emscripten_webgl_get_current_context() > 0) {
            glGenBuffers(static_cast<GLsizei>(RingDepth), this->data());
            for (u32 i = 0; i < RingDepth; ++i) {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, (*this)[i]);
                glBufferData(
                    GL_PIXEL_PACK_BUFFER,
                    static_cast<GLsizeiptr>(capacity_),
                    nullptr,
                    GL_STREAM_READ
                );
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }
#endif
        return true;
    }

    void advance_ring() noexcept {
        this->advance();
    }

    [[nodiscard]] constexpr u32 current_ring_index() const noexcept { return this->index(); }
    [[nodiscard]] constexpr u32 completed_ring_index(u32 delay = 2) const noexcept {
        return this->offset_index(RingDepth - (delay % RingDepth));
    }

    // Phát lệnh đọc bất đồng bộ vào PBO hiện tại qua GPU DMA (Zero CPU Stall)
    bool request_async_readback(
        GLint x, GLint y,
        GLsizei width, GLsizei height,
        GLenum format, GLenum type
    ) noexcept {
#if defined(__EMSCRIPTEN__)
        const GLuint pbo = this->current();
        if (pbo != 0) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
            glReadPixels(x, y, width, height, format, type, reinterpret_cast<void*>(0));
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            return true;
        }
#else
        (void)x; (void)y; (void)width; (void)height; (void)format; (void)type;
#endif
        return false;
    }

    // Thu hồi dữ liệu từ khung hình đã hoàn tất (cách 2 frames)
    bool read_completed_pixels(
        void* outBuffer,
        u32 byteSize,
        u32 delayFrames = 2
    ) noexcept {
        if (!outBuffer || byteSize == 0 || byteSize > capacity_) return false;
#if defined(__EMSCRIPTEN__)
        const u32 readIdx = completed_ring_index(delayFrames);
        const GLuint pbo = (*this)[readIdx];
        if (pbo != 0) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
            const void* src = glMapBufferRange(
                GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(byteSize), GL_MAP_READ_BIT
            );
            if (src) {
                std::memcpy(outBuffer, src, byteSize);
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            return src != nullptr;
        }
#else
        (void)outBuffer; (void)byteSize; (void)delayFrames;
#endif
        return false;
    }

    void destroy() noexcept {
#if defined(__EMSCRIPTEN__)
        for (u32 i = 0; i < RingDepth; ++i) {
            if ((*this)[i] != 0) {
                glDeleteBuffers(1, &(*this)[i]);
                (*this)[i] = 0;
            }
        }
#endif
        this->reset();
    }

private:
    u32 capacity_ = BufferCapacity;
};

extern PboRing<DEFAULT_PBO_STREAM_CAPACITY> g_glyphPboRing;

} // namespace render
} // namespace stockcut
