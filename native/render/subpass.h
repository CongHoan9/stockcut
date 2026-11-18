// ============================================================================
// native/render/subpass.h
// TRỤ CỘT 3: TILE-BASED CLUSTERED EXECUTION & BITBOARD DISPATCH
// TRỤ CỘT 4: COMPUTE SHADER SUBPASSES & ON-CHIP TILE MEMORY
// ----------------------------------------------------------------------------
// Kiến trúc đồ họa AAA (id Tech 7 / DOOM Eternal, Apple Metal, Vulkan Subpasses):
// 1. Phân chia không gian màn hình thành lưới 8x8 = 64 Tile trực giao.
// 2. Định danh và lọc vùng tác động bằng 64-bit Bitboard trong 1 chu kỳ CPU.
// 3. Subpass Chaining: Nối chuỗi nhiều bước xử lý trong On-Chip Tile SRAM (L1/SLM),
//    loại bỏ hoàn toàn chi phí flush trung gian ra bộ nhớ ngoài VRAM.
// 4. Memoryless Transient Attachments: Cấp phát bộ đệm ảo 0 byte VRAM.
// 5. Zero-Allocation: 100% MultiArray phẳng, con trỏ Stockfish thuần túy.
// ============================================================================
#pragma once

#include "types.h"
#include "data_structures.h"
#include "spatial/dirty_rect.h"
#include "timeline/bitboard.h"
#include "render/graph.h"

namespace stockcut {

constexpr u32 TILE_GRID_COLS = 8;
constexpr u32 TILE_GRID_ROWS = 8;
constexpr u32 TILE_GRID_COUNT = 64;
constexpr u32 MAX_SUBPASSES_PER_CHAIN = 16;
constexpr u32 MAX_TILE_ATTACHMENTS = 8;

// ============================================================================
// TRỤ CỘT 3: TILE GRID (LƯỚI KHÔNG GIAN 8x8 KHẮC KHẢO TRÊN BITBOARD 64-BIT)
// ============================================================================
class TileGrid {
public:
    // Chuyển đổi tọa độ AABB thực (x0, y0, x1, y1) thành 64-bit Bitboard
    [[nodiscard]] static constexpr Bitboard from_screen_rect(
        float x0, float y0, float x1, float y1, float viewportW, float viewportH
    ) noexcept {
        return Bitboard::FromAABB(
            static_cast<double>(x0), static_cast<double>(y0),
            static_cast<double>(x1), static_cast<double>(y1),
            static_cast<double>(viewportW), static_cast<double>(viewportH)
        );
    }

    // Chuyển đổi ngược lại từ Bitboard các ô gạch thành hình chữ nhật bao quanh chặt (Tight Scissor Rect)
    // Tối ưu hóa Stockfish Bitboard O(1) thuần số học bitwise: 0 vòng lặp while, 0 lệnh rẽ nhánh if
    [[nodiscard]] static inline Rect<float> to_screen_rect(
        Bitboard tiles, float viewportW, float viewportH
    ) noexcept {
        if (tiles.empty()) {
            return Rect<float>{ 0.0f, 0.0f, 0.0f, 0.0f };
        }
        const float tileW = (viewportW > 0.0f) ? (viewportW * 0.125f) : 0.0f;
        const float tileH = (viewportH > 0.0f) ? (viewportH * 0.125f) : 0.0f;

        const u64 val = tiles.value;
        const i32 minRow = static_cast<i32>(__builtin_ctzll(val) >> 3);
        const i32 maxRow = static_cast<i32>((63 - __builtin_clzll(val)) >> 3);

        // Gấp đại số 8 hàng vào 1 byte để trích xuất mặt nạ cột (0 rẽ nhánh)
        u64 c = val | (val >> 32);
        c |= (c >> 16);
        c |= (c >> 8);
        const u32 colMask = static_cast<u32>(c & 0xFFULL);
        const i32 minCol = static_cast<i32>(__builtin_ctz(colMask));
        const i32 maxCol = static_cast<i32>(31 - __builtin_clz(colMask));

        return Rect<float>{
            static_cast<float>(minCol) * tileW,
            static_cast<float>(minRow) * tileH,
            static_cast<float>(maxCol + 1) * tileW,
            static_cast<float>(maxRow + 1) * tileH
        };
    }

    // Đếm số lượng ô gạch đang kích hoạt
    [[nodiscard]] static inline u32 get_active_tile_count(Bitboard tiles) noexcept {
        return static_cast<u32>(tiles.popcount());
    }
};

// ============================================================================
// TRỤ CỘT 4: CHÍNH SÁCH BỘ ĐỆM TILE TRÊN CHIP (LOAD/STORE OPERATIONS)
// ============================================================================
enum class SubpassLoadOp : u8 {
    DontCare = 0, // Dữ liệu đệm rác trên Tile SRAM, không đọc từ VRAM
    Clear,        // Xóa trắng Tile SRAM trên chip bằng màu nền
    Load          // Nạp dữ liệu cũ từ VRAM vào Tile SRAM
};

enum class SubpassStoreOp : u8 {
    DontCare = 0, // Bỏ qua dữ liệu Tile SRAM khi kết thúc (0 ghi VRAM)
    Store         // Ghi dữ liệu Tile SRAM ra VRAM ngoài
};

// ============================================================================
// TileSubpassNode (64 bytes = ĐÚNG 1 L1 CACHELINE / 4 THANH GHI SIMD 128-BIT)
// ============================================================================
struct alignas(64) TileSubpassNode {
    u8 subpassIndex;               // 0     (1B: Chỉ mục subpass 0..MAX-1)
    u8 inputAttachmentIndex;       // 1     (1B: Attachment đọc từ Tile SRAM)
    u8 outputAttachmentIndex;      // 2     (1B: Attachment ghi vào Tile SRAM)
    SubpassLoadOp loadOp;          // 3     (1B: Chính sách nạp On-Chip)
    SubpassStoreOp storeOp;        // 4     (1B: Chính sách ghi On-Chip)
    u8 isMemoryless;               // 5     (1B: 1 = Chỉ tồn tại trong Tile SRAM, 0 Byte VRAM)
    u8 isFramebufferFetch;         // 6     (1B: 1 = Đọc in-place không cần Texture Sampler)
    u8 isBypassed;                 // 7     (1B: 1 = Bỏ qua toàn bộ vì activeTileMask == 0)
    u32 passNodeId;                // 8..11 (4B: Liên kết về RenderPassNode trong RenderGraph)
    u32 pad0;                      // 12..15(4B: Đệm căn lề 16-byte cho Rect<float>)
    Rect<float> scissoredBounds;   // 16..31(16B: Căn lề alignas(16) chuẩn)
    Bitboard activeTileMask;       // 32..39(8B: Mặt nạ 64 ô gạch kích hoạt)
    u64 simulatedVramBytesSaved;   // 40..47(8B: Lượng byte VRAM tiết kiệm được nhờ On-Chip)
    u64 pad1[2];                   // 48..63(16B: Hoàn thiện 64 bytes Cacheline)

    [[nodiscard]] constexpr bool is_active() const noexcept {
        return isBypassed == 0u;
    }
};
static_assert(sizeof(TileSubpassNode) == 64, "TileSubpassNode must be exactly 64 bytes");

// ============================================================================
// TileSubpassChain: BỘ ĐIỀU PHỐI CHUỖI SUBPASS ON-CHIP TILE SRAM
// ============================================================================
struct alignas(64) TileSubpassChain {
    MultiArray<TileSubpassNode, MAX_SUBPASSES_PER_CHAIN> subpasses;
    Bitboard combinedActiveTiles{0ULL};
    u32 subpassCount = 0;
    u32 activeSubpassCount = 0;
    u32 memorylessAttachmentCount = 0;
    u64 totalVramBytesSaved = 0;

    void reset() noexcept {
        subpassCount = 0;
        activeSubpassCount = 0;
        memorylessAttachmentCount = 0;
        totalVramBytesSaved = 0;
        combinedActiveTiles = Bitboard(0ULL);
    }

    [[gnu::always_inline]] inline TileSubpassNode& add_subpass(
        u32 passNodeId,
        Bitboard activeTiles,
        bool memoryless,
        bool fbFetch,
        float viewportW,
        float viewportH
    ) noexcept {
        if (subpassCount >= MAX_SUBPASSES_PER_CHAIN) {
            return subpasses[MAX_SUBPASSES_PER_CHAIN - 1];
        }
        TileSubpassNode& node = subpasses[subpassCount++];
        node.subpassIndex = static_cast<u8>(subpassCount - 1);
        node.passNodeId = passNodeId;
        node.activeTileMask = activeTiles;
        node.isMemoryless = memoryless ? 1u : 0u;
        node.isFramebufferFetch = fbFetch ? 1u : 0u;
        node.isBypassed = activeTiles.empty() ? 1u : 0u;
        node.pad0 = 0;
        node.pad1[0] = 0;
        node.pad1[1] = 0;

        if (node.subpassIndex == 0) {
            node.inputAttachmentIndex = 0;
            node.outputAttachmentIndex = 1;
            node.loadOp = SubpassLoadOp::Load;
            node.storeOp = memoryless ? SubpassStoreOp::DontCare : SubpassStoreOp::Store;
        } else {
            node.inputAttachmentIndex = (fbFetch) ? subpasses[node.subpassIndex - 1].outputAttachmentIndex : 0;
            node.outputAttachmentIndex = (node.inputAttachmentIndex == 0) ? 1 : 0;
            node.loadOp = SubpassLoadOp::DontCare; // Dữ liệu duy trì trong Tile SRAM!
            node.storeOp = memoryless ? SubpassStoreOp::DontCare : SubpassStoreOp::Store;
        }

        if (!node.isBypassed) {
            node.scissoredBounds = TileGrid::to_screen_rect(activeTiles, viewportW, viewportH);
            combinedActiveTiles |= activeTiles;
            ++activeSubpassCount;

            const float bytesPerFrame = viewportW * viewportH * 8.0f;
            const float tileFraction = static_cast<float>(activeTiles.popcount()) * 0.015625f; // / 64.0
            if (memoryless || fbFetch) {
                node.simulatedVramBytesSaved = static_cast<u64>(bytesPerFrame * tileFraction);
                totalVramBytesSaved += node.simulatedVramBytesSaved;
                if (memoryless) {
                    ++memorylessAttachmentCount;
                }
            } else {
                node.simulatedVramBytesSaved = 0;
            }
        } else {
            node.scissoredBounds = Rect<float>{ 0.0f, 0.0f, 0.0f, 0.0f };
            node.simulatedVramBytesSaved = 0;
        }

        return node;
    }

    // Biên dịch chuỗi Subpass tối ưu từ RenderGraph DAG
    void compile_from_render_graph(
        const RenderGraph& graph,
        float viewportW,
        float viewportH
    ) noexcept;
};

// ============================================================================
// TileSramTelemetry: BỘ ĐỆM GIẢ LẬP & ĐO LƯỜNG BĂNG THÔNG ON-CHIP TILE SRAM
// ============================================================================
struct alignas(64) TileSramTelemetry {
    u64 totalPixelsProcessed = 0;
    u64 totalPixelsBypassed = 0;
    u64 totalVramBytesSaved = 0;
    float peakBandwidthReductionRatio = 0.0f;

    void reset() noexcept {
        totalPixelsProcessed = 0;
        totalPixelsBypassed = 0;
        totalVramBytesSaved = 0;
        peakBandwidthReductionRatio = 0.0f;
    }
};

} // namespace stockcut
