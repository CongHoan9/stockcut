#pragma once
#include "types.h"
#include "data_structures.h"
#include "timeline/bitboard.h"
#include "core/differential.h"

namespace stockcut {

// ============================================================================
// CoreEngine - SpatialFrameDirty (Stockfish-Style Differential Hot Path Engine)
// ----------------------------------------------------------------------------
// [KỸ THUẬT STOCKFISH ĐỒNG BỘ]:
// 1. Kế thừa từ Differential<Mask<Blocks>, Bitboard>:
//    - entities : Mask các slot có tọa độ, thuộc tính thực sự thay đổi (dirtySlotsBB).
//    - bounds   : Lưới 8x8 các ô gạch bị bẩn trên màn hình 1920x1080 (dirtyTilesBB).
// 2. Dual-Axis Dynamic Bitboards (ByDynamicNatureBB & ByDynamicAxisBB):
//    - Phân tách rõ Dynamic Bản Chất (IntrinsicDynamic: Video, Audio, Effect, Spin, Bass)
//      và Dynamic Keyframe (KeyframeDynamic: các slot đang có keyframe/reveal biến đổi).
//    - Loại trừ hoàn toàn Audio khỏi dirtyTilesBB thị giác.
// 3. 8x8 Screen Tile Grid (SpatialTileBB): Toàn bộ trạng thái bẩn của màn hình
//    1920x1080 gói gọn trong 1 số nguyên 64-bit (1 ô gạch = 240x135 px).
// 4. O(1) Scissor Box Generation: Builtins __builtin_ctzll / __builtin_clzll
//    chuyển đổi 64-bit bitboard thành glScissor box trong < 10 nanoseconds.
// ============================================================================
template <usize Slots = MAX_TIMELINE_SLOTS>
struct alignas(64) SpatialFrameDirty : public Differential<Mask<((Slots + 63u) / 64u)>, Bitboard> {
    using Base = Differential<Mask<((Slots + 63u) / 64u)>, Bitboard>;
    using SlotMask = Mask<((Slots + 63u) / 64u)>;

    // 1. Phân loại Slot theo Chế độ Động học (Stockfish byColorBB[])
    ByModeBB byModeBB{};
    // 2. Phân loại Slot theo Định dạng Tài nguyên (Stockfish byTypeBB[])
    ByTypeBB byTypeBB{};
    // 3. Phân loại Slot theo Bản chất Nội tại vs Keyframe (Dual-Axis Dynamic Bitboards)
    ByDynamicNatureBB byNatureBB{};
    ByDynamicAxisBB byDynamicAxisBB{};

    // 4. Dấu chân không gian 8x8 (Screen Footprint) của từng slot
    MultiArray<Bitboard, Slots> layerTilesBB{};
    MultiArray<Bitboard, Slots> prevLayerTilesBB{};

    // Toạ độ Scissor Box đã tính toán sẵn cho OpenGL
    double scissorX = 0.0;
    double scissorY = 0.0;
    double scissorW = 0.0;
    double scissorH = 0.0;
    bool hasScissor = false;
    bool isFullRedraw = true;
    bool forceFullRedraw = true;

    [[nodiscard]] inline Bitboard& dirty_tiles_bb() noexcept { return this->bounds; }
    [[nodiscard]] inline const Bitboard& dirty_tiles_bb() const noexcept { return this->bounds; }
    [[nodiscard]] inline SlotMask& dirty_slots_bb() noexcept { return this->entities; }
    [[nodiscard]] inline const SlotMask& dirty_slots_bb() const noexcept { return this->entities; }

    inline void clear_all() noexcept {
        byModeBB.fill(Bitboard(0ULL));
        byTypeBB.fill(Bitboard(0ULL));
        byNatureBB.fill(Bitboard(0ULL));
        byDynamicAxisBB.fill(Bitboard(0ULL));
        this->clear();
        layerTilesBB.fill(Bitboard(0ULL));
        prevLayerTilesBB.fill(Bitboard(0ULL));
        scissorX = scissorY = scissorW = scissorH = 0.0;
        hasScissor = false;
        isFullRedraw = true;
        forceFullRedraw = true;
    }

    inline void clear() noexcept {
        byModeBB.fill(Bitboard(0ULL));
        byTypeBB.fill(Bitboard(0ULL));
        byNatureBB.fill(Bitboard(0ULL));
        byDynamicAxisBB.fill(Bitboard(0ULL));
        Base::clear();
        layerTilesBB.fill(Bitboard(0ULL));
        scissorX = scissorY = scissorW = scissorH = 0.0;
        hasScissor = false;
        isFullRedraw = forceFullRedraw;
    }

    // Kiểm tra xem slot có giao cắt với ô gạch bẩn không bằng toán học bitmask (0 lệnh if, 0 toán tử 3 ngôi)
    [[nodiscard]] inline bool is_slot_dirty(u32 slot) const noexcept {
        const bool outOfBounds = (slot >= Slots);
        const u64 validSlotMask = static_cast<u64>((static_cast<i64>(!outOfBounds) << 63) >> 63);
        const u64 slotMask = (layerTilesBB[slot & (Slots - 1u)].value | prevLayerTilesBB[slot & (Slots - 1u)].value) & validSlotMask;
        return outOfBounds | isFullRedraw | ((slotMask & this->bounds.value) != 0ULL);
    }

    // Kiểm tra xem slot có bản chất tĩnh (IntrinsicStatic) hay không (0 lệnh if, 0 toán tử 3 ngôi)
    [[nodiscard]] inline bool is_slot_intrinsic_static(u32 slot) const noexcept {
        const bool inBounds = (slot < Slots);
        const u64 validMask = static_cast<u64>((static_cast<i64>(inBounds) << 63) >> 63);
        const u64 mask = (1ULL << (slot & 63)) & validMask;
        return (byNatureBB[static_cast<u32>(DynamicNature::IntrinsicStatic)].value & mask) != 0ULL;
    }

    // Kiểm tra xem slot có hoàn toàn tĩnh trong frame này không (0 lệnh if, 0 toán tử 3 ngôi)
    [[nodiscard]] inline bool is_slot_static(u32 slot) const noexcept {
        if (slot >= Slots) return false;
        return !this->is_entity_dirty(slot);
    }

    // Tính toán lại dirtyTilesBB từ đúng các visual slots thực sự biến đổi trong frame này
    // (đã được lọc qua Chunk kfActiveBB và Homomorphic Zobrist Delta trong step_to) bằng đại số bitwise thuần túy (0 lệnh if, 0 toán tử 3 ngôi)
    inline void evaluate_spatial_differential(double W, double H, u64 stepDirtySlots = 0ULL) noexcept {
        // Loại trừ tuyệt đối Audio khỏi dấu chân gạch 2D của layer (Effect là autonomous struct riêng)
        const u64 nonLayerVisualMask = byTypeBB[static_cast<u32>(MediaType::Audio)].value;
        const u64 movingMask = stepDirtySlots & ~nonLayerVisualMask;
        this->entities = SlotMask(movingMask);
        u64 accumulatedTiles = 0ULL;
        Bitboard scanMask = movingMask;
        while (scanMask) {
            const u32 slot = scanMask.pop_lsb();
            accumulatedTiles |= (layerTilesBB[slot].value | prevLayerTilesBB[slot].value);
        }
        // Lưu vết dấu chân khung hình trước cho slot O(1) SIMD copy
        prevLayerTilesBB = layerTilesBB;

        const bool isForce = forceFullRedraw;
        forceFullRedraw = false;

        // Nếu force full redraw: coi như toàn bộ 64 ô bị bẩn
        const u64 forceMask = static_cast<u64>((static_cast<i64>(isForce) << 63) >> 63);
        const u64 effectiveTiles = accumulatedTiles | forceMask;
        const u64 hasVal = (effectiveTiles | (~effectiveTiles + 1ULL)) >> 63;
        const bool hasTiles = hasVal != 0ULL;
        const u64 safeVal = effectiveTiles | (hasVal ^ 1ULL);

        const int rMin = static_cast<int>(BitOps<u64>::lsb(safeVal)) >> 3;
        const int rMax = static_cast<int>(BitOps<u64>::msb(safeVal)) >> 3;
        u64 folded = safeVal | (safeVal >> 32);
        folded |= (folded >> 16);
        folded |= (folded >> 8);
        const u32 colMask = static_cast<u32>(folded & 0xFFULL);
        const int cMin = static_cast<int>(BitOps<u32>::lsb(colMask));
        const int cMax = static_cast<int>(BitOps<u32>::msb(colMask));
        const u64 rectColMask = (0xFFULL >> (7 ^ cMax)) & (0xFFULL << cMin);
        const u64 rectRankMask = (~0ULL >> ((7 ^ rMax) << 3)) & (~0ULL << (rMin << 3));

        u64 spread = rectColMask;
        spread |= (spread << 8);
        spread |= (spread << 16);
        spread |= (spread << 32);
        const u64 tileMask = static_cast<u64>((static_cast<i64>(hasTiles) << 63) >> 63);
        const u64 boundedTiles = spread & rectRankMask & tileMask;
        this->bounds = Bitboard(boundedTiles);

        const int count = static_cast<int>(BitOps<u64>::popcount64(this->bounds.value));
        // Full redraw nếu bị force hoặc diện tích bẩn >= DIRTY_TILE_FULL_THRESHOLD (7/8 màn hình)
        const bool fullRedrawCond = isForce | (count >= DIRTY_TILE_FULL_THRESHOLD);
        const bool scissorCond = hasTiles & (!fullRedrawCond);

        isFullRedraw = fullRedrawCond;
        hasScissor = scissorCond;

        const double tileW = W * 0.125;
        const double tileH = H * 0.125;

        // Đại số trọng số (Algebraic Weights) gán scissor box không cần if:
        const double fFull = static_cast<double>(fullRedrawCond);
        const double fScis = static_cast<double>(scissorCond);

        scissorX = (cMin * tileW) * fScis;
        scissorY = (H - (rMax + 1) * tileH) * fScis;
        scissorW = (W * fFull) + ((cMax - cMin + 1) * tileW) * fScis;
        scissorH = (H * fFull) + ((rMax - rMin + 1) * tileH) * fScis;
        this->epoch++;
    }
};

// Singleton / Global Spatial Frame Dirties Instance
inline SpatialFrameDirty<MAX_TIMELINE_SLOTS>& engine_get_frame_spatial_dirty() noexcept {
    static SpatialFrameDirty<MAX_TIMELINE_SLOTS> instance;
    return instance;
}

} // namespace stockcut
