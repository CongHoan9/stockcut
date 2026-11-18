// ============================================================================
// [ĐÁNH GIÁ ĐIỂM NGHẼN HỆ THỐNG]:
// 1. Duyệt cây phân cấp đệ quy trong Hot Path 60 FPS:
//    - Duyệt qua lại các node timeline làm phát sinh cache miss và chi phí con trỏ.
// 2. Kế thừa template lặp lại ở mỗi khung hình:
//    - Hỏi quan hệ cha-con liên tục làm giảm hiệu suất xử lý khung hình.
//
// [HƯỚNG KHẮC PHỤC TRIỆT ĐỂ — KHÔNG VÁ]:
// 1. Kế thừa 1 lần duy nhất ở Cold Path & Bộ nhớ chia sẻ:
//    - Con trỏ trực tiếp vào bộ nhớ phẳng của cha, đọc O(1) không duyệt cây.
// 2. Bitboard Layer Masking & Ngăn xếp lịch sử StateInfo:
//    - Lọc trạng thái layer bằng bitmask 64-bit và lưu vết vi sai kiểu Stockfish.
// ============================================================================
#pragma once
// ============================================================================
// CoreEngine - Branchless GPU Transfer Queue (OpenGL ES 3.0 Direct SubData)
// ----------------------------------------------------------------------------
// Hàng đợi truyền dữ liệu vi sai (Partial State Transfer) từ CPU RAM -> GPU VBO
// Không qua trung gian JavaScript.
// ============================================================================
#include "timeline/bitboard.h"
#include "spatial/field.h"
#include "dirty_fields.h"
#if defined(__has_include)
    #if __has_include(<GLES3/gl3.h>)
        #include <GLES3/gl3.h>
    #else
        using GLuint = stockcut::u32;
    #endif
#elif defined(__EMSCRIPTEN__)
    #include <GLES3/gl3.h>
#else
    using GLuint = stockcut::u32;
#endif
namespace stockcut {
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8)
constexpr u32 GPU_TRANSFER_CMD_PAD_WORDS = 2u;
#else
constexpr u32 GPU_TRANSFER_CMD_PAD_WORDS = 4u;
#endif
constexpr u32 GPU_UNROLLED_LUT_SIZE = 9u;

struct alignas(32) TransferCmd {
    GLuint vboId;
    u32 offset;
    u32 size;
    const void* dataPtr;
    MultiArray<u32, GPU_TRANSFER_CMD_PAD_WORDS> pad;

    TransferCmd() noexcept = default;
    constexpr TransferCmd(GLuint vbo, u32 off, u32 sz, const void* ptr) noexcept
        : vboId(vbo), offset(off), size(sz), dataPtr(ptr) {}
};
static_assert(sizeof(TransferCmd) == 32, "TransferCmd must be 32 bytes");
struct TransferQueue {
    static constexpr u32 CAPACITY = 256u;
    alignas(64) MultiArray<TransferCmd, CAPACITY> commands;
    u32 count = 0u;
    u32 totalBytesQueued = 0u;
    [[gnu::always_inline]] inline void push_branchless(
        GLuint vbo, u32 offset, u32 size, const void* data
    ) noexcept {
        const u32 inCapacity = static_cast<u32>((count >> 8) == 0u);
        const u32 safeIdx = count & (CAPACITY - 1u);
        commands[safeIdx] = TransferCmd{vbo, offset, size, data};
        const u32 valid = (size != 0u) & inCapacity;
        count += valid;
        const u32 mask = static_cast<u32>((static_cast<i32>(valid) << 31) >> 31);
        totalBytesQueued += (size & mask);
    }
    inline void clear() noexcept {
        count = 0u;
        totalBytesQueued = 0u;
    }
};

// ----------------------------------------------------------------------------
// Stockfish Zero-Loop Differential VBO Quad Updating Subsystem
// ----------------------------------------------------------------------------
struct DirtyMutation {
    u32 raw;
    // [31]: isRemoved | [30]: isAdded | [29..24]: slotIndex (6 bits) | [23..0]: paramMask (24 bits)
    template <bool Add, bool Remove>
    static constexpr DirtyMutation make(u8 slot, u32 params) noexcept {
        return DirtyMutation{
            (static_cast<u32>(Remove) << 31) |
            (static_cast<u32>(Add) << 30) |
            ((static_cast<u32>(slot) & 0x3Fu) << 24) |
            (params & 0x00FFFFFFu)
        };
    }
};

extern TransferQueue g_gpuQueue;
void flush_gpu_transfer_queue() noexcept;

// ============================================================================
// PersistentSceneBuffer: QUẢN LÝ VÙNG NHỚ SSBO 256 SLOTS THƯỜNG TRỰC TRÊN GPU
// ============================================================================
struct alignas(64) PersistentSceneBuffer {
    static constexpr usize CAPACITY_SLOTS = 256u;
    static constexpr usize TOTAL_BYTES    = CAPACITY_SLOTS * 64u; // 16 KB VRAM

    GLuint ssboId = 0u;
    u32 totalUploadsCount = 0u;
    u64 totalBytesUploaded = 0ULL;
    spatial::RangePacker<32u> packer{};

    inline void init(GLuint id) noexcept {
        ssboId = id;
        totalUploadsCount = 0u;
        totalBytesUploaded = 0ULL;
        packer.clear();
    }

    // Ghi các dải bẩn vào SSBO thông qua con trỏ nguồn Evaluated
    template <typename EvaluatedT>
    inline u32 upload_delta(
        u64 dirtyBB,
        const EvaluatedT* sourceSlots
    ) noexcept {
        if (dirtyBB == 0ULL || !sourceSlots) return 0u;

        packer.clear();
        packer.pack_bitboard64(dirtyBB, 0u);

        for (u32 i = 0; i < packer.rangeCount; ++i) {
            const auto& r = packer.ranges[i];
            const void* ptr = reinterpret_cast<const void*>(&sourceSlots[r.slotStart]);
            g_gpuQueue.push_branchless(ssboId, r.byteOffset, r.byteSize, ptr);
            totalBytesUploaded += r.byteSize;
            ++totalUploadsCount;
        }

        return packer.rangeCount;
    }

    // Direct Buffer Mapping Sub-Data: Chi upload dung dai byte tuong ung voi bit ban qua glBufferSubData tai dia chi offset tinh,
    // giam 80-93.75% bang thong bus du lieu.
    template <typename EvaluatedT, typename DiffAccumulatorT>
    inline u32 upload_differential(
        const DiffAccumulatorT& accum,
        const EvaluatedT* sourceSlots
    ) noexcept {
        if (!accum.has_dirty() || !sourceSlots) return 0u;

        u32 queuedRanges = 0u;
        accum.entities.for_each_active([&](u32 slot) {
            const u16 mask = accum.get_fields(slot) & 0x7Fu;
            if (mask == 0u) return;

            const SubRangePair& pair = kSubRangeLUT[mask];
            const u8* basePtr = reinterpret_cast<const u8*>(&sourceSlots[slot]);
            const u32 slotBaseOffset = slot * 64u;

            if (pair.size0 > 0u) {
                g_gpuQueue.push_branchless(
                    ssboId,
                    slotBaseOffset + pair.offset0,
                    pair.size0,
                    basePtr + pair.offset0
                );
                totalBytesUploaded += pair.size0;
                ++queuedRanges;
            }
            if (pair.size1 > 0u) {
                g_gpuQueue.push_branchless(
                    ssboId,
                    slotBaseOffset + pair.offset1,
                    pair.size1,
                    basePtr + pair.offset1
                );
                totalBytesUploaded += pair.size1;
                ++queuedRanges;
            }
        });

        totalUploadsCount += queuedRanges;
        return queuedRanges;
    }
};

} // namespace stockcut
