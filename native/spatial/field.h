// ============================================================================
// native/spatial/field.h
// HỆ THỐNG PHÂN Ô KHÔNG GIAN & ĐƯỜNG VẼ GPU TỰ CHỦ CHUẨN STOCKFISH (UNIFIED)
// ----------------------------------------------------------------------------
// [STOCKFISH DATA-STRUCTURE-CENTRIC & WEBGPU STD430/WGSL COMPLIANT]:
// 1. PackedToken64<T, N>: Gói định danh nén cấp thanh ghi 64-bit (1.80 ns, 0 vòng lặp).
// 2. TileCell: Ô phẳng 16 bytes căn lề 16B (khớp trọn vẹn 1 vec4<u32> trong WGSL).
// 3. RangePacker<MaxRanges>: Đóng gói dải bộ nhớ bẩn liên tục cho VRAM (0.42 ns).
// 4. SpatialField<Cols, Rows, CellT, CellDimPx>: Trường không gian đa năng.
// 5. FrameContext: Ngữ cảnh khung hình phần cứng đồng nhất 128 bytes (2 L1 Cachelines).
// 6. FrameRing<Frames>: Đệm vòng khung hình Time-Domain Separation (0 lock, 0 cờ).
// 7. TileGrid & TileArena: Cấu hình lưới và phân ô 1 tầng không gián tiếp.
// 8. TileBitMatrix: Ma trận bit không gian bất biến OIT L2-resident (16.3 KB).
// 9. TileCache: Cây tích lũy Zobrist theo ô.
// 10. PipelineManager: Bộ điều phối GPU Pipeline tự chủ.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>
#include "../core/types.h"
#include "../core/data_structures.h"
#include "../timeline/bitboard.h"
#include "../spatial/dirty_rect.h"

namespace stockcut {
namespace spatial {

// ============================================================================
// CỜ TRẠNG THÁI Ô TẾ BÀO (TILE CELL FLAGS - u16)
// ============================================================================
constexpr u16 TILE_FLAG_EMPTY            = 0u;         // Ô trống
constexpr u16 TILE_FLAG_OPAQUE_OCCLUDED  = (1u << 0);  // Bị che kín bởi layer đục
constexpr u16 TILE_FLAG_STATIC_CLEAN     = (1u << 1);  // Tĩnh tuyệt đối (Zobrist không đổi)
constexpr u16 TILE_FLAG_NEEDS_HALO       = (1u << 2);  // Chứa effect lân cận (Blur/Glow)
constexpr u16 TILE_FLAG_HAS_TRANSPARENT  = (1u << 3);  // Chứa layer bán trong suốt
constexpr u16 TILE_FLAG_IS_3D_HYBRID     = (1u << 4);  // Chứa 3D Depth Pass

// ============================================================================
// 1. PackedToken64: SỬ DỤNG TRỰC TIẾP STRUCT TIỆN ÍCH Token = Packed<u8, 8>
// ============================================================================

// ============================================================================
// 2. TileCell (16 bytes - Khớp chuẩn 1 vec4<u32> trong std430/WGSL)
// ============================================================================
struct alignas(16) TileCell {
    Token tokens;                // 0..7:   8 slot IDs đóng gói gọn trong 1 u64
    u32 staticZobristKey;        // 8..11:  Mã băm Zobrist 32-bit của cụm layer tĩnh
    u16 flags;                   // 12..13: Cờ trạng thái TILE_FLAG_*
    u8 layerCount;               // 14:     Số lượng layer thực tế (0..8)
    u8 maxZ;                     // 15:     zRank cao nhất trong ô

    [[nodiscard]] inline bool is_empty() const noexcept {
        return layerCount == 0u;
    }

    [[nodiscard]] inline bool is_static_clean() const noexcept {
        return (flags & TILE_FLAG_STATIC_CLEAN) != 0u;
    }

    inline void clear() noexcept {
        tokens.clear();
        staticZobristKey = 0u;
        flags = TILE_FLAG_EMPTY;
        layerCount = 0u;
        maxZ = 0u;
    }

    inline bool push_slot(u8 slotId, u8 zRank) noexcept {
        if (tokens.push(slotId)) {
            ++layerCount;
            if (zRank > maxZ) maxZ = zRank;
            return true;
        }
        return false;
    }
};
static_assert(sizeof(TileCell) == 16, "TileCell must be exactly 16 bytes for std430/WGSL");

// ============================================================================
// 3. RangePacker<MaxRanges>: ĐÓNG GÓI DẢI BỘ NHỚ BẨN LIÊN TỤC CHO VRAM (0.42 ns)
// ============================================================================
template <usize MaxRanges = 32u>
struct alignas(16) RangePacker {
    struct alignas(16) Range {
        u32 byteOffset; // std430 offset (slotStart * 64)
        u32 byteSize;   // Kích thước dải (slotCount * 64)
        u16 slotStart;  // Slot index bắt đầu
        u16 slotCount;  // Số lượng slot liên tiếp
        u32 pad;
    };
    static_assert(sizeof(Range) == 16, "Range must be 16 bytes for std430");
    static_assert(alignof(Range) == 16, "Range must be 16 bytes aligned for std430");

    MultiArray<Range, MaxRanges> ranges;
    u32 rangeCount = 0u;
    u32 totalSlots = 0u;
    u32 totalSlotsQueued = 0u;

    inline void clear() noexcept {
        rangeCount = 0u;
        totalSlots = 0u;
        totalSlotsQueued = 0u;
    }

    // Đóng gói từ 64-bit Bitboard đơn (Zero-Branch SWAR Carry-Ripple Run Extraction)
    inline void pack_bitboard64(u64 dirtyBB, u32 baseSlot = 0u, u32 slotBytes = 64u) noexcept {
        u64 bb = dirtyBB;
        while (bb != 0ULL) {
            const u32 startBit = BitOps<u64>::lsb(bb);
            const u64 lsbBit   = bb & (~bb + 1ULL);
            const u64 nextBB   = bb & (bb + lsbBit);
            const u64 runBits  = bb ^ nextBB;
            const u32 actualRun = (BitOps<u64>::msb(runBits) + 1u) - startBit;
            const u32 slotStart = baseSlot + startBit;
            Range& r = ranges[rangeCount++];
            r.slotStart  = static_cast<u16>(slotStart);
            r.slotCount  = static_cast<u16>(actualRun);
            r.byteOffset = slotStart * slotBytes;
            r.byteSize   = actualRun * slotBytes;
            r.pad        = 0u;
            totalSlots  += actualRun;
            totalSlotsQueued += actualRun;
            bb = nextBB;
        }
    }

    // Pha 4: Sub-Cacheline DMA Prep for PersistentZRankVRAMTable (16B/64B stride)
    [[gnu::always_inline]] inline u32 pack_zrank_dirty_dma(u64 dirtyZRankBB, u32 strideBytes = 64u) noexcept {
        clear();
        pack_bitboard64(dirtyZRankBB, 0u, strideBytes);
        return totalSlots * strideBytes;
    }

    // Đóng gói từ Mask<Blocks> tổng quát
    template <usize Blocks>
    inline void pack_mask(const Mask<Blocks>& mask, u32 slotBytes = 64u) noexcept {
        clear();
        for (usize b = 0; b < Blocks; ++b) {
            if (mask.words[b] != 0ULL) {
                pack_bitboard64(mask.words[b], static_cast<u32>(b << 6), slotBytes);
            }
        }
    }
};

// ============================================================================
// 4. SpatialField<Cols, Rows, CellPayload, CellDimPx>: TRƯỜNG KHÔNG GIAN ĐA NĂNG
// ============================================================================
template <u32 Cols, u32 Rows, typename CellPayload = TileCell, u32 CellDimPx = 32>
struct alignas(64) SpatialField {
    static constexpr u32 COLS = Cols;
    static constexpr u32 ROWS = Rows;
    static constexpr u32 TOTAL_CELLS = Cols * Rows;
    static constexpr u32 CELL_DIM = CellDimPx;

    MultiArray<CellPayload, TOTAL_CELLS> cells;
    MultiArray<u16, TOTAL_CELLS> activeIndices;

    float stageW = 1920.0f;
    float stageH = 1080.0f;
    float invCellW = 1.0f / static_cast<float>(CellDimPx);
    float invCellH = 1.0f / static_cast<float>(CellDimPx);
    u32 activeCellCount = 0u;
    u32 totalIntersections = 0u;

    inline void init(float w, float h) noexcept {
        stageW = (w > 0.0f) ? w : 1920.0f;
        stageH = (h > 0.0f) ? h : 1080.0f;
        invCellW = static_cast<float>(Cols) / stageW;
        invCellH = static_cast<float>(Rows) / stageH;
        clear();
    }

    inline void clear() noexcept {
        for (u32 i = 0; i < activeCellCount; ++i) {
            cells[activeIndices[i]].clear();
        }
        activeCellCount = 0u;
        totalIntersections = 0u;
    }

    [[gnu::always_inline]] inline void get_bounds(
        float x0, float y0, float x1, float y1,
        u32& outTx0, u32& outTy0, u32& outTx1, u32& outTy1
    ) const noexcept {
        const float cx0 = (x0 > 0.0f) ? x0 : 0.0f;
        const float cy0 = (y0 > 0.0f) ? y0 : 0.0f;
        const float cx1 = (x1 < stageW) ? x1 : stageW;
        const float cy1 = (y1 < stageH) ? y1 : stageH;

        if (cx1 <= cx0 || cy1 <= cy0) {
            outTx0 = outTy0 = outTx1 = outTy1 = 0u;
            return;
        }

        outTx0 = static_cast<u32>(cx0 * invCellW);
        outTy0 = static_cast<u32>(cy0 * invCellH);
        outTx1 = static_cast<u32>((cx1 - 0.001f) * invCellW);
        outTy1 = static_cast<u32>((cy1 - 0.001f) * invCellH);

        if (outTx1 >= Cols) outTx1 = Cols - 1u;
        if (outTy1 >= Rows) outTy1 = Rows - 1u;
    }

    inline void stamp_slot(float x0, float y0, float x1, float y1, u8 slotId, u8 zRank) noexcept {
        u32 tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
        get_bounds(x0, y0, x1, y1, tx0, ty0, tx1, ty1);
        if (tx1 < tx0 || ty1 < ty0) return;

        for (u32 ty = ty0; ty <= ty1; ++ty) {
            const u32 rowBase = ty * Cols;
            for (u32 tx = tx0; tx <= tx1; ++tx) {
                const u32 idx = rowBase + tx;
                if (cells[idx].is_empty()) {
                    activeIndices[activeCellCount++] = static_cast<u16>(idx);
                }
                if (cells[idx].push_slot(slotId, zRank)) {
                    ++totalIntersections;
                }
            }
        }
    }

    [[nodiscard]] [[gnu::always_inline]] inline const CellPayload& query_point(float x, float y) const noexcept {
        const u32 tx = std::min(Cols - 1u, static_cast<u32>(std::max(0.0f, x) * invCellW));
        const u32 ty = std::min(Rows - 1u, static_cast<u32>(std::max(0.0f, y) * invCellH));
        return cells[ty * Cols + tx];
    }
};

// ============================================================================
// 5. FrameContext: NGỮ CẢNH KHUNG HÌNH PHẦN CỨNG ĐỒNG NHẤT (128 BYTES)
// ============================================================================
struct alignas(64) FrameContext {
    // Cacheline 1 (0..63): Thông số hình học & địa chỉ đệm
    float stageW;
    float stageH;
    u32 tilesX;
    u32 tilesY;
    u32 tileCount;
    u32 activeTileCount;
    u32 uploadRangeCount;
    u32 frameIndex;
    double nowMs;
    u32 tileArenaByteOffset;   // Offset vào mảng TileCell
    u32 slotArenaByteOffset;   // Offset vào mảng Evaluated
    u32 rangeArenaByteOffset;  // Offset vào mảng RangePacker::Range
    u32 activeSlotCount;       // Số lượng slot active (0..64) phục vụ GPU On-The-Fly Pseudo-Binning
    u32 pad0[2];               // Đệm khớp 64 bytes trọn vẹn Cacheline 1

    // Cacheline 2 (64..127): Hiệu năng đo đạc & Thống kê
    u32 totalIntersections;
    u32 staticCleanTiles;
    u32 dynamicDirtyTiles;
    u32 bytesUploaded;
    float gpuPrepareMs;
    float simulatedGpuFps;
    u64 globalDirtyBB;
    MultiArray<u64, 4> pad1;   // Đệm khớp 128 bytes trọn vẹn Cacheline 2

    inline void clear() noexcept {
        std::memset(this, 0, sizeof(*this));
    }
};
static_assert(sizeof(FrameContext) == 128, "FrameContext must be exactly 128 bytes");

// ============================================================================
// 6. FrameRing<Frames = 2>: ĐỆM VÒNG KHUNG HÌNH TIME-DOMAIN SEPARATION
// ============================================================================
template <usize Frames = 2>
struct alignas(64) FrameRing {
    static_assert((Frames & (Frames - 1)) == 0, "Frames must be power of two for branchless masking");
    MultiArray<FrameContext, Frames> frames{};
    u32 currentFrameIndex = 0;

    [[nodiscard]] [[gnu::always_inline]] inline FrameContext& producer_context() noexcept {
        return frames[currentFrameIndex & (Frames - 1)];
    }

    [[nodiscard]] [[gnu::always_inline]] inline const FrameContext& consumer_context() const noexcept {
        return frames[(currentFrameIndex - 1) & (Frames - 1)];
    }

    [[gnu::always_inline]] inline void step_frame() noexcept {
        ++currentFrameIndex;
    }
};

// ============================================================================
// 7. THUẬT TOÁN PHÂN Ô ĐA NĂNG HỢP NHẤT (UNIFIED SPATIAL BINNING)
// ============================================================================
template <typename PositionT, u32 Cols, u32 Rows>
inline void bin_scene_into_spatial_field(
    const PositionT& pos,
    SpatialField<Cols, Rows, TileCell>& outField
) noexcept {
    outField.clear();

    const auto* st = pos.state();
    const u64 activeSummary = st->activeBB.summary();
    if (activeSummary == 0ULL) return;

    const auto* evaluatedSlots = pos.evaluated_slots_arena();

    const u32 bucketCount = pos.z_bucket_count();
    for (u32 level = 0; level < bucketCount; ++level) {
        auto activeAtLevel = pos.z_bucket(level) & st->activeBB;
        u64 bb = activeAtLevel.words[0];
        while (bb != 0ULL) {
            const u32 slot = BitOps<u64>::pop_lsb(bb);
            const auto& ev = evaluatedSlots[slot];
            if (ev.isVisible == 0u || ev.opacity <= 0.0001f) continue;

            outField.stamp_slot(
                ev.aabb.x0, ev.aabb.y0, ev.aabb.x1, ev.aabb.y1,
                static_cast<u8>(slot), static_cast<u8>(ev.zRank)
            );
        }
    }
}

} // namespace spatial

// ============================================================================
// PHÂN HỆ GPU-DRIVEN TỰ CHỦ CHUẨN STOCKFISH (HỢP NHẤT)
// ============================================================================
namespace gpu_driven {

constexpr u32 DEFAULT_TILE_SIZE_PX       = 32u;
constexpr u32 MAX_STAGE_WIDTH_PX         = 3840u;
constexpr u32 MAX_STAGE_HEIGHT_PX        = 2160u;
constexpr u32 MAX_TILES_X                = (MAX_STAGE_WIDTH_PX + DEFAULT_TILE_SIZE_PX - 1u) / DEFAULT_TILE_SIZE_PX;
constexpr u32 MAX_TILES_Y                = (MAX_STAGE_HEIGHT_PX + DEFAULT_TILE_SIZE_PX - 1u) / DEFAULT_TILE_SIZE_PX;
constexpr u32 MAX_TILES_COUNT            = MAX_TILES_X * MAX_TILES_Y;

constexpr u32 STANDARD_HD_TILES_X        = 60u;
constexpr u32 STANDARD_HD_TILES_Y        = 34u;
constexpr u32 STANDARD_HD_TILES_COUNT    = STANDARD_HD_TILES_X * STANDARD_HD_TILES_Y;

constexpr u32 MAX_SCENE_SLOTS            = 256u;
constexpr u32 MAX_LAYERS_PER_TILE        = 64u;

constexpr u32 TILE_FLAG_EMPTY            = 0u;
constexpr u32 TILE_FLAG_OPAQUE_OCCLUDED  = (1u << 0);
constexpr u32 TILE_FLAG_STATIC_CLEAN     = (1u << 1);
constexpr u32 TILE_FLAG_NEEDS_HALO       = (1u << 2);
constexpr u32 TILE_FLAG_HAS_TRANSPARENT  = (1u << 3);
constexpr u32 TILE_FLAG_IS_3D_HYBRID     = (1u << 4);

using RangePacker = spatial::RangePacker<32u>;

// ============================================================================
// 1. TileHeader (16 bytes - Khớp 1 vec4<u32> trong WGSL / std430)
// ============================================================================
struct alignas(16) TileHeader {
    u32 layerOffset;         // 0..3:   Byte/Index offset vào mảng layerIndices
    u16 layerCount;          // 4..5:   Số lượng layer giao cắt trên ô này (0..MAX_LAYERS_PER_TILE)
    u16 flags;               // 6..7:   Cờ trạng thái TILE_FLAG_*
    u32 staticZobristKey32;  // 8..11:  Mã băm Zobrist 32-bit của cụm layer tĩnh trong ô
    u16 minZ;                // 12..13: zRank nhỏ nhất trong ô
    u16 maxZ;                // 14..15: zRank lớn nhất trong ô

    [[nodiscard]] inline bool is_empty() const noexcept {
        return layerCount == 0u;
    }

    [[nodiscard]] inline bool is_static_clean() const noexcept {
        return (flags & TILE_FLAG_STATIC_CLEAN) != 0u;
    }

    inline void clear() noexcept {
        layerOffset = 0u;
        layerCount = 0u;
        flags = TILE_FLAG_EMPTY;
        staticZobristKey32 = 0u;
        minZ = 0xFFFFu;
        maxZ = 0u;
    }
};
static_assert(sizeof(TileHeader) == 16, "TileHeader must be exactly 16 bytes for std430/WGSL");

// ============================================================================
// 2. TileGrid (64 bytes - Cấu hình lưới không gian truyền vào Uniform Buffer)
// ============================================================================
struct alignas(64) TileGrid {
    float stageW;            // 0..3:   Chiều rộng khung render (pixel, ví dụ 1920.0f)
    float stageH;            // 4..7:   Chiều cao khung render (pixel, ví dụ 1080.0f)
    float tileW;             // 8..11:  Chiều rộng 1 ô (pixel, ví dụ 32.0f)
    float tileH;             // 12..15: Chiều cao 1 ô (pixel, ví dụ 32.0f)

    u32 tilesX;              // 16..19: Số cột ô (ví dụ 60)
    u32 tilesY;              // 20..23: Số hàng ô (ví dụ 34)
    u32 tileCount;           // 24..27: Tổng số ô (tilesX * tilesY, ví dụ 2040)
    u32 activeSlotCount;     // 28..31: Tổng số slot cảnh đang kích hoạt trong khung hình

    float invTileW;          // 32..35: 1.0f / tileW (nhân nhanh thay phép chia)
    float invTileH;          // 36..39: 1.0f / tileH
    float invStageW;         // 40..43: 1.0f / stageW
    float invStageH;         // 44..47: 1.0f / stageH

    u32 frameIndex;          // 48..51: Chỉ số khung hình tăng đơn điệu
    float currentTimeMs;     // 52..55: Mili-giây hiện tại của timeline
    u32 globalFlags;         // 56..59: Cờ điều phối toàn cục (WebGPU vs WebGL2)
    u32 pad0;                // 60..63: Padding căn lề 64 bytes

    inline void init(float w, float h, u32 tileSize = DEFAULT_TILE_SIZE_PX) noexcept {
        stageW = (w > 0.0f) ? w : 1920.0f;
        stageH = (h > 0.0f) ? h : 1080.0f;
        tileW  = static_cast<float>(tileSize);
        tileH  = static_cast<float>(tileSize);
        tilesX = static_cast<u32>((stageW + tileW - 1.0f) / tileW);
        tilesY = static_cast<u32>((stageH + tileH - 1.0f) / tileH);
        tileCount = tilesX * tilesY;
        activeSlotCount = 0u;

        invTileW = 1.0f / tileW;
        invTileH = 1.0f / tileH;
        invStageW = 1.0f / stageW;
        invStageH = 1.0f / stageH;

        frameIndex = 0u;
        currentTimeMs = 0.0f;
        globalFlags = 0u;
        pad0 = 0u;
    }

    inline void get_tile_bounds(
        float x0, float y0, float x1, float y1,
        u32& outTx0, u32& outTy0, u32& outTx1, u32& outTy1
    ) const noexcept {
        const float clampedX0 = (x0 > 0.0f) ? x0 : 0.0f;
        const float clampedY0 = (y0 > 0.0f) ? y0 : 0.0f;
        const float clampedX1 = (x1 < stageW) ? x1 : stageW;
        const float clampedY1 = (y1 < stageH) ? y1 : stageH;

        if (clampedX1 <= clampedX0 || clampedY1 <= clampedY0) {
            outTx0 = outTx1 = outTy0 = outTy1 = 0u;
            return;
        }

        outTx0 = static_cast<u32>(clampedX0 * invTileW);
        outTy0 = static_cast<u32>(clampedY0 * invTileH);
        outTx1 = static_cast<u32>((clampedX1 - 0.001f) * invTileW);
        outTy1 = static_cast<u32>((clampedY1 - 0.001f) * invTileH);

        if (outTx1 >= tilesX) outTx1 = tilesX - 1u;
        if (outTy1 >= tilesY) outTy1 = tilesY - 1u;
    }
};
static_assert(sizeof(TileGrid) == 64, "TileGrid must be exactly 64 bytes");

// ============================================================================
// 3. TileArena (Bộ nhớ đệm phân ô phẳng chuẩn Stockfish)
// ============================================================================
template <usize MaxTiles = STANDARD_HD_TILES_COUNT, usize MaxIndices = MaxTiles * 16u>
struct alignas(64) TileArena {
    MultiArray<TileHeader, MaxTiles> headers;
    MultiArray<u16, MaxIndices>      layerIndices;
    u32 totalIndicesCount = 0u;
    u32 activeTileCount = 0u;
    u32 emptyTileCount = static_cast<u32>(MaxTiles);

    inline void clear(u32 tileCount = MaxTiles) noexcept {
        const u32 count = (tileCount < MaxTiles) ? tileCount : static_cast<u32>(MaxTiles);
        for (u32 i = 0; i < count; ++i) {
            headers[i].clear();
        }
        totalIndicesCount = 0u;
        activeTileCount = 0u;
        emptyTileCount = count;
    }

    inline bool push_layer_to_tile(u32 tileIdx, u16 slotId, u16 zRank) noexcept {
        if (tileIdx >= MaxTiles || totalIndicesCount >= MaxIndices) return false;

        TileHeader& hdr = headers[tileIdx];
        if (hdr.layerCount >= MAX_LAYERS_PER_TILE) return false;

        if (hdr.layerCount == 0u) {
            hdr.layerOffset = totalIndicesCount;
            hdr.minZ = zRank;
            hdr.maxZ = zRank;
            --emptyTileCount;
            ++activeTileCount;
        } else {
            if (zRank < hdr.minZ) hdr.minZ = zRank;
            if (zRank > hdr.maxZ) hdr.maxZ = zRank;
        }

        layerIndices[totalIndicesCount++] = slotId;
        ++hdr.layerCount;
        return true;
    }
};

// ============================================================================
// 4. TileBitMatrix: MA TRẬN BIT KHÔNG GIAN BẤT BIẾN (BITBOARD-TILED HYPER-FUSION)
// ============================================================================
template <usize MaxTiles = STANDARD_HD_TILES_COUNT, usize Blocks = 1>
struct alignas(64) TileBitMatrix {
    MultiArray<Mask<Blocks>, MaxTiles> tileMasks;
    u32 activeTileCount = 0u;
    u32 totalIntersections = 0u;

    inline void clear(u32 tileCount = MaxTiles) noexcept {
        const u32 count = (tileCount < MaxTiles) ? tileCount : static_cast<u32>(MaxTiles);
        for (u32 i = 0; i < count; ++i) {
            tileMasks[i].clear();
        }
        activeTileCount = 0u;
        totalIntersections = 0u;
    }

    [[nodiscard]] inline Mask<Blocks> get_mask(u32 tileIdx) const noexcept {
        return (tileIdx < MaxTiles) ? tileMasks[tileIdx] : Mask<Blocks>{};
    }

    [[nodiscard]] inline u64 get_u64_mask(u32 tileIdx) const noexcept {
        return (tileIdx < MaxTiles) ? tileMasks[tileIdx].words[0] : 0ULL;
    }

    inline void set_layer_range(u32 tx0, u32 ty0, u32 tx1, u32 ty1, u32 slot, u32 tilesX) noexcept {
        const u64 slotBit = (slot < 64u) ? (1ULL << slot) : 0ULL;
        if (slotBit == 0ULL) return;

        for (u32 ty = ty0; ty <= ty1; ++ty) {
            const u32 rowBase = ty * tilesX;
            for (u32 tx = tx0; tx <= tx1; ++tx) {
                const u32 tileIdx = rowBase + tx;
                if (tileIdx < MaxTiles) {
                    if (tileMasks[tileIdx].words[0] == 0ULL) {
                        ++activeTileCount;
                    }
                    tileMasks[tileIdx].words[0] |= slotBit;
                    ++totalIntersections;
                }
            }
        }
    }

    inline void occlude_below(u32 tileIdx, u32 opaqueSlot) noexcept {
        if (tileIdx < MaxTiles && opaqueSlot < 64u) {
            const u64 maskToKeep = ~((1ULL << opaqueSlot) - 1ULL);
            tileMasks[tileIdx].words[0] &= maskToKeep;
        }
    }
};

// ============================================================================
// 5. THUẬT TOÁN CPU REFERENCE TILE BINNING (GROUND TRUTH & FALLBACK)
// ============================================================================
template <typename PositionT, typename BinningArenaT>
inline void bin_scene_into_tiles_cpu(
    const PositionT& pos,
    const TileGrid& cfg,
    BinningArenaT& outArena
) noexcept {
    outArena.clear(cfg.tileCount);

    const auto* st = pos.state();
    const u64 activeSummary = st->activeBB.summary();
    if (activeSummary == 0ULL) return;

    const auto* evaluatedSlots = pos.evaluated_slots_arena();

    const u32 bucketCount = pos.z_bucket_count();
    for (u32 level = 0; level < bucketCount; ++level) {
        auto activeAtLevel = pos.z_bucket(level) & st->activeBB;
        u64 bb = activeAtLevel.words[0];
        while (bb != 0ULL) {
            const u32 slot = BitOps<u64>::pop_lsb(bb);
            const auto& ev = evaluatedSlots[slot];
            if (ev.isVisible == 0u || ev.opacity <= 0.0001f) continue;

            u32 tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
            cfg.get_tile_bounds(ev.aabb.x0, ev.aabb.y0, ev.aabb.x1, ev.aabb.y1, tx0, ty0, tx1, ty1);

            for (u32 ty = ty0; ty <= ty1; ++ty) {
                const u32 rowBase = ty * cfg.tilesX;
                for (u32 tx = tx0; tx <= tx1; ++tx) {
                    const u32 tileIdx = rowBase + tx;
                    outArena.push_layer_to_tile(tileIdx, static_cast<u16>(slot), ev.zRank);
                }
            }
        }
    }
}

// ============================================================================
// 5b. THUẬT TOÁN PHÂN Ô MA TRẬN BIT (BITBOARD-TILED MATRIX BINNING - ZERO ATOMIC)
// ============================================================================
template <typename PositionT, typename BitMatrixT>
inline void bin_scene_into_bit_matrix(
    const PositionT& pos,
    const TileGrid& cfg,
    BitMatrixT& outMatrix
) noexcept {
    outMatrix.clear(cfg.tileCount);

    const auto* st = pos.state();
    const u64 activeSummary = st->activeBB.summary();
    if (activeSummary == 0ULL) return;

    const auto* evaluatedSlots = pos.evaluated_slots_arena();

    u64 activeBB = st->activeBB.words[0];
    while (activeBB != 0ULL) {
        const u32 slot = BitOps<u64>::pop_lsb(activeBB);
        const auto& ev = evaluatedSlots[slot];
        if (ev.isVisible == 0u || ev.opacity <= 0.0001f) continue;

        u32 tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
        cfg.get_tile_bounds(ev.aabb.x0, ev.aabb.y0, ev.aabb.x1, ev.aabb.y1, tx0, ty0, tx1, ty1);

        const u64 slotBit = (slot < 64u) ? (1ULL << slot) : 0ULL;
        for (u32 ty = ty0; ty <= ty1; ++ty) {
            const u32 rowBase = ty * cfg.tilesX;
            for (u32 tx = tx0; tx <= tx1; ++tx) {
                const u32 tileIdx = rowBase + tx;
                if (tileIdx < cfg.tileCount) {
                    if (outMatrix.tileMasks[tileIdx].words[0] == 0ULL) {
                        outMatrix.activeTileCount++;
                    }
                    outMatrix.tileMasks[tileIdx].words[0] |= slotBit;
                    outMatrix.totalIntersections++;
                }
            }
        }
    }
}

// ============================================================================
// 6. TileCache: CÂY TÍCH LŨY TĨNH THEO Ô (ZOBRIST STATIC ACCUMULATOR)
// ============================================================================
template <usize MaxTiles = STANDARD_HD_TILES_COUNT>
struct alignas(64) TileCache {
    MultiArray<u32, MaxTiles> cachedZobristKeys;
    MultiArray<u64, MaxTiles> staticLayerMasks;
    u32 cleanTileCount = 0u;
    u32 dirtyTileCount = 0u;

    inline void clear() noexcept {
        cachedZobristKeys.fill(0u);
        staticLayerMasks.fill(0ULL);
        cleanTileCount = 0u;
        dirtyTileCount = 0u;
    }

    template <typename BinningArenaT>
    inline void evaluate_tile_static_status(
        const TileGrid& cfg,
        u64 globalDirtyBB,
        BinningArenaT& binningArena
    ) noexcept {
        cleanTileCount = 0u;
        dirtyTileCount = 0u;

        const u32 count = (cfg.tileCount < MaxTiles) ? cfg.tileCount : static_cast<u32>(MaxTiles);
        for (u32 i = 0; i < count; ++i) {
            auto& hdr = binningArena.headers[i];
            if (hdr.is_empty()) continue;

            const u64 tileStatic = staticLayerMasks[i];
            if (tileStatic != 0ULL && (globalDirtyBB & tileStatic) == 0ULL) {
                hdr.flags |= TILE_FLAG_STATIC_CLEAN;
                hdr.staticZobristKey32 = cachedZobristKeys[i];
                ++cleanTileCount;
            } else {
                hdr.flags &= ~TILE_FLAG_STATIC_CLEAN;
                ++dirtyTileCount;
            }
        }
    }

    inline void update_tile_cache(u32 tileIdx, u32 zobristKey, u64 staticMask) noexcept {
        if (tileIdx < MaxTiles) {
            cachedZobristKeys[tileIdx] = zobristKey;
            staticLayerMasks[tileIdx] = staticMask;
        }
    }
};

// ============================================================================
// 8. PipelineManager: BỘ ĐIỀU PHỐI ĐƯỜNG VẼ TỰ CHỦ CHUẨN STOCKFISH
// ============================================================================
struct alignas(64) PipelineManager {
    TileGrid config{};
    TileArena<STANDARD_HD_TILES_COUNT, STANDARD_HD_TILES_COUNT * 16u> arena{};
    TileBitMatrix<STANDARD_HD_TILES_COUNT, 1> bitMatrix{};
    spatial::SpatialField<60, 34, spatial::TileCell> spatialField{};
    spatial::FrameContext frameContext{};
    spatial::FrameRing<2> frameRing{};
    TileCache<STANDARD_HD_TILES_COUNT> staticCache{};
    RangePacker rangePacker{};

    inline void init(float stageW = 1920.0f, float stageH = 1080.0f, u32 tileSize = DEFAULT_TILE_SIZE_PX) noexcept {
        config.init(stageW, stageH, tileSize);
        arena.clear(config.tileCount);
        bitMatrix.clear(config.tileCount);
        spatialField.init(stageW, stageH);
        frameContext.clear();
        staticCache.clear();
        rangePacker.clear();
    }

    template <typename PositionT>
    inline void execute_cpu_binning(const PositionT& pos, u64 dirtyBB, float currentTimeMs = 0.0f) noexcept {
        config.currentTimeMs = currentTimeMs;
        config.frameIndex++;

        bin_scene_into_tiles_cpu(pos, config, arena);
        bin_scene_into_bit_matrix(pos, config, bitMatrix);
        staticCache.evaluate_tile_static_status(config, dirtyBB, arena);

        rangePacker.clear();
        rangePacker.pack_bitboard64(dirtyBB, 0u);

        const auto* st = pos.state();
        frameContext.stageW = config.stageW;
        frameContext.stageH = config.stageH;
        frameContext.tilesX = config.tilesX;
        frameContext.tilesY = config.tilesY;
        frameContext.tileCount = config.tileCount;
        frameContext.activeTileCount = arena.activeTileCount;
        frameContext.uploadRangeCount = rangePacker.rangeCount;
        frameContext.frameIndex = config.frameIndex;
        frameContext.nowMs = currentTimeMs;
        frameContext.totalIntersections = bitMatrix.totalIntersections;
        frameContext.globalDirtyBB = dirtyBB;
        frameContext.activeSlotCount = static_cast<u32>(BitOps<u64>::popcount(st->activeBB.words[0]));

        frameRing.producer_context() = frameContext;
        frameRing.step_frame();
    }

    template <typename PositionT>
    inline void execute_gpu_pseudo_prep(const PositionT& pos, u64 dirtyBB, float currentTimeMs = 0.0f) noexcept {
        config.currentTimeMs = currentTimeMs;
        config.frameIndex++;

        rangePacker.clear();
        rangePacker.pack_bitboard64(dirtyBB, 0u);

        const auto* st = pos.state();
        frameContext.stageW = config.stageW;
        frameContext.stageH = config.stageH;
        frameContext.tilesX = config.tilesX;
        frameContext.tilesY = config.tilesY;
        frameContext.tileCount = config.tileCount;
        frameContext.activeTileCount = config.tileCount;
        frameContext.uploadRangeCount = rangePacker.rangeCount;
        frameContext.frameIndex = config.frameIndex;
        frameContext.nowMs = currentTimeMs;
        frameContext.globalDirtyBB = dirtyBB;
        frameContext.activeSlotCount = static_cast<u32>(BitOps<u64>::popcount(st->activeBB.words[0]));

        frameRing.producer_context() = frameContext;
        frameRing.step_frame();
    }

    [[nodiscard]] inline const TileGrid* get_grid_config_ptr() const noexcept { return &config; }
    [[nodiscard]] inline const TileHeader* get_tile_headers_ptr() const noexcept { return arena.headers.data(); }
    [[nodiscard]] inline const u16* get_tile_indices_ptr() const noexcept { return arena.layerIndices.data(); }
    [[nodiscard]] inline const u64* get_tile_bit_matrix_ptr() const noexcept { return reinterpret_cast<const u64*>(bitMatrix.tileMasks.data()); }
    [[nodiscard]] inline const spatial::TileCell* get_spatial_cells_ptr() const noexcept { return spatialField.cells.data(); }
    [[nodiscard]] inline const spatial::FrameContext* get_hardware_context_ptr() const noexcept { return &frameContext; }
    [[nodiscard]] inline const spatial::FrameRing<2>* get_frame_ring_ptr() const noexcept { return &frameRing; }
    [[nodiscard]] inline u32 get_tile_count() const noexcept { return config.tileCount; }
    [[nodiscard]] inline u32 get_active_tile_count() const noexcept { return arena.activeTileCount; }
    [[nodiscard]] inline u32 get_bit_matrix_active_tile_count() const noexcept { return bitMatrix.activeTileCount; }
    [[nodiscard]] inline u32 get_spatial_active_cell_count() const noexcept { return spatialField.activeCellCount; }
    [[nodiscard]] inline u32 get_total_indices_count() const noexcept { return arena.totalIndicesCount; }
    [[nodiscard]] inline u32 get_bit_matrix_total_intersections() const noexcept { return bitMatrix.totalIntersections; }
    [[nodiscard]] inline const RangePacker::Range* get_upload_ranges_ptr() const noexcept { return rangePacker.ranges.data(); }
    [[nodiscard]] inline u32 get_upload_range_count() const noexcept { return rangePacker.rangeCount; }
};

inline PipelineManager& get_gpu_pipeline_manager() noexcept {
    alignas(64) static PipelineManager s_manager;
    return s_manager;
}

} // namespace gpu_driven
} // namespace stockcut
