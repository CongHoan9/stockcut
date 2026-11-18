// ============================================================================
// native/core/differential.h
// UNIVERSAL DIFFERENTIAL PARENT STRUCT (STOCKFISH / NATRON ARCHITECTURE)
// ----------------------------------------------------------------------------
// [STOCKFISH DATA-STRUCTURE-CENTRIC & ZERO-ALLOCATION]:
// 1. Differential<EntityMaskT, SpatialT>:
//    - entities : Bitmask các thực thể (slots, passes, layers) bị đột biến.
//    - bounds   : Dấu chân không gian hoặc hộp bao hình học (Rect, Bitboard 8x8, v.v.).
//    - epoch    : Thế hệ đột biến đơn điệu O(1) kiểm tra thay đổi.
// 2. Chuyên biệt hóa không rẽ nhánh (Zero-Branch Specialization):
//    - SlotDifferential<Slots>: Vi sai thuộc tính 2 chiều (slot x field) & spatial AABB.
//    - SpatialFrameDirty<Slots>: Vi sai không gian 8x8 Tile Grid & Scissor Box.
// ============================================================================
#pragma once

#include <type_traits>
#include "types.h"
#include "data_structures.h"
#include "nested_mark.h"
#include "../spatial/dirty_rect.h"

namespace stockcut {

template <typename EntityMaskT = Mask<1>, typename SpatialT = Rect<float>>
struct alignas(64) Differential {
    using EntityMask = EntityMaskT;
    using SpatialType = SpatialT;

    EntityMask entities{};   // Bitmask of dirty entities (slots, layers, passes)
    SpatialT bounds{};       // Geometric footprint (Rect, Bitboard 8x8 tile grid, etc.)
    u32 epoch = 0;           // Monotonic mutation generation counter

    constexpr Differential() noexcept = default;

    inline void clear() noexcept {
        entities.clear();
        if constexpr (requires { bounds.clear(); }) {
            bounds.clear();
        } else if constexpr (requires { bounds = SpatialT::identity(); }) {
            bounds = SpatialT::identity();
        } else if constexpr (std::is_arithmetic_v<SpatialT>) {
            bounds = SpatialT(0);
        } else {
            bounds = SpatialT{};
        }
    }

    [[nodiscard]] inline bool has_dirty() const noexcept {
        if constexpr (requires { entities.any(); }) {
            return entities.any();
        } else if constexpr (requires { entities.summary(); }) {
            return entities.summary() != 0ULL;
        } else if constexpr (std::is_integral_v<EntityMaskT>) {
            return entities != 0;
        } else {
            return entities != EntityMaskT{};
        }
    }

    [[nodiscard]] inline bool is_entity_dirty(u32 idx) const noexcept {
        if constexpr (requires { entities.test_bit(idx); }) {
            return entities.test_bit(idx);
        } else if constexpr (std::is_integral_v<EntityMaskT>) {
            return (entities & (EntityMaskT(1) << idx)) != 0;
        } else {
            return false;
        }
    }

    inline void mark_entity(u32 idx) noexcept {
        if constexpr (requires { entities.set_bit(idx); }) {
            entities.set_bit(idx);
        } else if constexpr (std::is_integral_v<EntityMaskT>) {
            entities |= (EntityMaskT(1) << idx);
        }
        ++epoch;
    }

    inline void clear_entity(u32 idx) noexcept {
        if constexpr (requires { entities.clear_bit(idx); }) {
            entities.clear_bit(idx);
        } else if constexpr (std::is_integral_v<EntityMaskT>) {
            entities &= ~(EntityMaskT(1) << idx);
        }
    }

    inline void unite_bounds(const SpatialT& b) noexcept {
        if constexpr (requires { bounds.uniteWith(b); }) {
            bounds.uniteWith(b);
        } else if constexpr (requires { bounds = bounds.union_with(b); }) {
            bounds = bounds.union_with(b);
        } else if constexpr (requires { bounds |= b; }) {
            bounds |= b;
        } else {
            bounds = b;
        }
    }

    inline void set_bounds(const SpatialT& b) noexcept {
        bounds = b;
    }

    [[nodiscard]] inline u32 epoch_count() const noexcept {
        return epoch;
    }
};

// ============================================================================
// MerkleDiffTable<Capacity>: Incremental Merkle-AST Span Diff Utility
// Inherits Differential<Mask<Blocks>, u64> directly (zero redundant tracking).
// Branchless 64-bit XOR span hash comparison for O(delta) Cold/Warm updates.
// ============================================================================
template <usize Capacity = MAX_TIMELINE_SLOTS>
struct alignas(64) MerkleDiffTable : public Differential<Mask<(Capacity + 63) / 64>, u64> {
    static constexpr usize kBlocks = (Capacity + 63) / 64;
    using Base = Differential<Mask<kBlocks>, u64>;

    MultiArray<u64, Capacity> hashes{};
    Mask<kBlocks> removedMask{};
    u32 prevCount = 0u;
    u32 currCount = 0u;
    u64 globalContextHash = 0ULL;

    [[nodiscard]] [[gnu::always_inline]] static inline u64 hash_span_64(
        const char* data, u32 len, u64 seed = 0xCBF29CE484222325ULL
    ) noexcept {
        u64 h = seed;
        const u8* p = reinterpret_cast<const u8*>(data);
        const u8* const pEnd = p + len;
        while (p + 8 <= pEnd) {
            u64 w;
            std::memcpy(&w, p, 8);
            h = (h ^ w) * 0x100000001B3ULL;
            p += 8;
        }
        while (p < pEnd) {
            h = (h ^ static_cast<u64>(*p++)) * 0x100000001B3ULL;
        }
        return h ^ (h >> 33);
    }

    inline void reset_all() noexcept {
        Base::clear();
        hashes = {};
        removedMask.clear();
        prevCount = 0u;
        currCount = 0u;
        globalContextHash = 0ULL;
    }

    inline bool begin_diff(u64 newContextHash) noexcept {
        Base::entities.clear();
        removedMask.clear();
        currCount = 0u;
        const bool contextChanged = (newContextHash != globalContextHash);
        globalContextHash = newContextHash;
        return contextChanged;
    }

    // Branchless XOR diff & commit for slot [0..Capacity-1]
    [[nodiscard]] [[gnu::always_inline]] inline bool update_slot(u32 slot, u64 newHash) noexcept {
        const u64 oldHash = hashes[slot];
        const bool isChanged = (oldHash != newHash) || (slot >= prevCount);
        hashes[slot] = newHash;
        Base::entities.set_bit(slot, isChanged);
        Base::bounds ^= (oldHash ^ newHash);
        return isChanged;
    }

    inline void finalize_count(u32 newCount) noexcept {
        currCount = newCount;
        for (u32 s = newCount; s < prevCount; ++s) {
            Base::bounds ^= hashes[s];
            hashes[s] = 0ULL;
            removedMask.set_bit(s, true);
        }
        prevCount = newCount;
        Base::epoch += static_cast<u32>(Base::entities.any() || removedMask.any());
    }
};

} // namespace stockcut
