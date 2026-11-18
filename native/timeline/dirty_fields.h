// ============================================================================
// native/timeline/dirty_fields.h
// MA TRẬN DIRTY 2 CHIỀU (SLOT X FIELD) & SLOT DIFFERENTIAL ACCUMULATOR
// ----------------------------------------------------------------------------
// [STOCKFISH DATA-STRUCTURE-CENTRIC & ZERO-ALLOCATION]:
// 1. DirtyField: Bitmask các trường dữ liệu vi sai trong Evaluated (64 B).
// 2. SubRangePair: Cặp dải byte tối ưu [offset0, size0] và [offset1, size1].
// 3. kSubRangeLUT: Bảng tra cứu compile-time 128 entries (512 B, 8 L1 Cachelines).
// 4. SlotDifferential<Slots>: Cấu trúc vi sai 2 chiều kế thừa từ Differential.
// ============================================================================
#pragma once

#include "../core/types.h"
#include "../core/data_structures.h"
#include "../core/differential.h"
#include "bitboard.h"

namespace stockcut {

// ============================================================================
// 1. CÁC TRƯỜNG THUỘC TÍNH ĐỘC LẬP TRONG Evaluated (64 BYTES)
// ----------------------------------------------------------------------------
// Byte 00..15 (16 B): aabb [x0, y0, x1, y1] -> TransformAABB
// Byte 16..19 (04 B): rotateDeg             -> TransformRot
// Byte 20..23 (04 B): opacity               -> Opacity
// Byte 24..27 (04 B): resourceId            -> Resource
// Byte 28..39 (12 B): filter, opCode, flags, zRank, slot -> TextLayout
// Byte 40..43 (04 B): isVisible             -> Visibility
// Byte 44..63 (20 B): customParams[5]       -> Uniforms
// ============================================================================
namespace DirtyField {
    constexpr u16 None          = 0u;
    constexpr u16 TransformAABB = 1u << 0; // Bit 0: aabb [x0, y0, x1, y1] (16 bytes, offset 0)
    constexpr u16 TransformRot  = 1u << 1; // Bit 1: rotateDeg (4 bytes, offset 16)
    constexpr u16 Opacity       = 1u << 2; // Bit 2: opacity (4 bytes, offset 20)
    constexpr u16 Resource      = 1u << 3; // Bit 3: resourceId / texture / color (4 bytes, offset 24)
    constexpr u16 TextLayout    = 1u << 4; // Bit 4: filter, opCode, flags, zRank, slot (12 bytes, offset 28)
    constexpr u16 Visibility    = 1u << 5; // Bit 5: isVisible (4 bytes, offset 40)
    constexpr u16 Uniforms      = 1u << 6; // Bit 6: customParams (20 bytes, offset 44)

    // Tổ hợp trường phổ biến:
    constexpr u16 Transform     = TransformAABB | TransformRot; // 20 bytes (offset 0)
    constexpr u16 VisualStyle   = Opacity | Resource;           // 8 bytes (offset 20)
    constexpr u16 TextAndStyle  = Resource | TextLayout;        // 16 bytes (offset 24)
    constexpr u16 All           = 0x007Fu;                      // 64 bytes (toàn bộ slot)
}

// ============================================================================
// 2. SubRangePair: CẶP DẢI BYTE CON CỦA SLOT CHO TRUYỀN DỮ LIỆU VI SAI
// ============================================================================
struct alignas(4) SubRangePair {
    u8 offset0; // Offset dải 1 trong Evaluated (0..63)
    u8 size0;   // Kích thước dải 1 (bytes)
    u8 offset1; // Offset dải 2 trong Evaluated (0..63, 0 nếu không có)
    u8 size1;   // Kích thước dải 2 (bytes, 0 nếu không có)

    [[nodiscard]] constexpr u8 total_bytes() const noexcept {
        return static_cast<u8>(size0 + size1);
    }
};
static_assert(sizeof(SubRangePair) == 4, "SubRangePair must be exactly 4 bytes");

// ============================================================================
// 3. THUẬT TOÁN COMPILE-TIME TÍNH TOÁN DẢI CON TỐI ƯU CHO 128 TỔ HỢP BITMASK
// ============================================================================
namespace detail {
    constexpr SubRangePair compute_sub_range_pair(u8 mask) noexcept {
        if (mask == 0u) return SubRangePair{0, 0, 0, 0};

        constexpr u8 offsets[7] = {0,  16, 20, 24, 28, 40, 44};
        constexpr u8 ends[7]    = {16, 20, 24, 28, 40, 44, 64};

        u8 starts[7] = {};
        u8 endings[7] = {};
        u8 count = 0;

        for (u8 b = 0; b < 7; ++b) {
            if (mask & (1u << b)) {
                starts[count] = offsets[b];
                endings[count] = ends[b];
                ++count;
            }
        }

        u8 mStarts[7] = {};
        u8 mEnds[7] = {};
        u8 mCount = 0;
        u8 curS = starts[0];
        u8 curE = endings[0];

        for (u8 i = 1; i < count; ++i) {
            if (starts[i] == curE) {
                curE = endings[i];
            } else {
                mStarts[mCount] = curS;
                mEnds[mCount] = curE;
                ++mCount;
                curS = starts[i];
                curE = endings[i];
            }
        }
        mStarts[mCount] = curS;
        mEnds[mCount] = curE;
        ++mCount;

        if (mCount == 1) {
            return SubRangePair{mStarts[0], static_cast<u8>(mEnds[0] - mStarts[0]), 0, 0};
        } else if (mCount == 2) {
            return SubRangePair{
                mStarts[0], static_cast<u8>(mEnds[0] - mStarts[0]),
                mStarts[1], static_cast<u8>(mEnds[1] - mStarts[1])
            };
        } else {
            return SubRangePair{mStarts[0], static_cast<u8>(mEnds[mCount - 1] - mStarts[0]), 0, 0};
        }
    }

    template <usize... Is>
    constexpr MultiArray<SubRangePair, sizeof...(Is)> build_sub_range_lut(std::index_sequence<Is...>) noexcept {
        return MultiArray<SubRangePair, sizeof...(Is)>{{ compute_sub_range_pair(static_cast<u8>(Is))... }};
    }
} // namespace detail

// Bảng tra cứu 128 entries (512 bytes, đúng 8 L1 Cachelines)
inline constexpr MultiArray<SubRangePair, 128> kSubRangeLUT =
    detail::build_sub_range_lut(std::make_index_sequence<128>{});

static_assert(kSubRangeLUT[0].total_bytes() == 0, "Empty mask must upload 0 bytes");
static_assert(kSubRangeLUT[DirtyField::TransformAABB].offset0 == 0 && kSubRangeLUT[DirtyField::TransformAABB].size0 == 16, "TransformAABB must be 16 bytes at offset 0");
static_assert(kSubRangeLUT[DirtyField::TransformRot].offset0 == 16 && kSubRangeLUT[DirtyField::TransformRot].size0 == 4, "TransformRot must be 4 bytes at offset 16");
static_assert(kSubRangeLUT[DirtyField::Transform].offset0 == 0 && kSubRangeLUT[DirtyField::Transform].size0 == 20, "Transform must be 20 bytes at offset 0");
static_assert(kSubRangeLUT[DirtyField::Opacity].offset0 == 20 && kSubRangeLUT[DirtyField::Opacity].size0 == 4, "Opacity must be 4 bytes at offset 20");
static_assert(kSubRangeLUT[DirtyField::Resource].offset0 == 24 && kSubRangeLUT[DirtyField::Resource].size0 == 4, "Resource must be 4 bytes at offset 24");
static_assert(kSubRangeLUT[DirtyField::TextLayout].offset0 == 28 && kSubRangeLUT[DirtyField::TextLayout].size0 == 12, "TextLayout must be 12 bytes at offset 28");
static_assert(kSubRangeLUT[DirtyField::Visibility].offset0 == 40 && kSubRangeLUT[DirtyField::Visibility].size0 == 4, "Visibility must be 4 bytes at offset 40");
static_assert(kSubRangeLUT[DirtyField::Uniforms].offset0 == 44 && kSubRangeLUT[DirtyField::Uniforms].size0 == 20, "Uniforms must be 20 bytes at offset 44");
static_assert(kSubRangeLUT[DirtyField::All].offset0 == 0 && kSubRangeLUT[DirtyField::All].size0 == 64, "All must be 64 bytes at offset 0");

// ============================================================================
// 4. SlotDifferential<Slots>: BỘ TÍCH LŨY VI SAI 2 CHIỀU (SLOT X FIELD)
// ----------------------------------------------------------------------------
// Kế thừa trực tiếp từ Differential<Mask<Blocks>, Rect<float>>.
// Mỗi slot sở hữu 1 bitmask u16 ghi nhận chính xác trường bị bẩn.
// Hot Path 60-120 FPS chỉ thao tác trên thanh ghi bitwise, 0 cấp phát bộ nhớ.
// ============================================================================
template <usize Slots = MAX_TIMELINE_SLOTS>
struct alignas(64) SlotDifferential : public Differential<Mask<((Slots + 63u) / 64u)>, Rect<float>> {
    using Base = Differential<Mask<((Slots + 63u) / 64u)>, Rect<float>>;
    using SlotMask = Mask<((Slots + 63u) / 64u)>;

    alignas(64) MultiArray<u16, Slots> dirtyFields{};
    SlotMask transformSlots{};
    SlotMask opacitySlots{};
    SlotMask contentSlots{};

    inline void clear() noexcept {
        Base::clear();
        dirtyFields.fill(0u);
        transformSlots.clear();
        opacitySlots.clear();
        contentSlots.clear();
    }

    inline void record_field_masks(u32 slot, u16 fieldBits) noexcept {
        if (fieldBits & DirtyField::Transform) transformSlots.set_bit(slot);
        if (fieldBits & DirtyField::Opacity)   opacitySlots.set_bit(slot);
        if (fieldBits & (DirtyField::Resource | DirtyField::TextLayout | DirtyField::Uniforms | DirtyField::Visibility))
            contentSlots.set_bit(slot);
    }

    inline void mark_field(u32 slot, u16 fieldBits) noexcept {
        if (__builtin_expect(slot < Slots, 1)) {
            dirtyFields[slot] |= fieldBits;
            this->mark_entity(slot);
            record_field_masks(slot, fieldBits);
        }
    }

    inline void mark_field(u32 slot, u16 fieldBits, const Rect<float>& aabb) noexcept {
        if (__builtin_expect(slot < Slots, 1)) {
            dirtyFields[slot] |= fieldBits;
            this->mark_entity(slot);
            this->unite_bounds(aabb);
            record_field_masks(slot, fieldBits);
        }
    }

    inline void mark_all_fields(u32 slot) noexcept {
        if (__builtin_expect(slot < Slots, 1)) {
            dirtyFields[slot] = DirtyField::All;
            this->mark_entity(slot);
            transformSlots.set_bit(slot);
            opacitySlots.set_bit(slot);
            contentSlots.set_bit(slot);
        }
    }

    inline void mark_all_fields(u32 slot, const Rect<float>& aabb) noexcept {
        if (__builtin_expect(slot < Slots, 1)) {
            dirtyFields[slot] = DirtyField::All;
            this->mark_entity(slot);
            this->unite_bounds(aabb);
            transformSlots.set_bit(slot);
            opacitySlots.set_bit(slot);
            contentSlots.set_bit(slot);
        }
    }

    inline void clear_slot(u32 slot) noexcept {
        if (__builtin_expect(slot < Slots, 1)) {
            dirtyFields[slot] = 0u;
            this->clear_entity(slot);
            transformSlots.clear_bit(slot);
            opacitySlots.clear_bit(slot);
            contentSlots.clear_bit(slot);
        }
    }

    [[nodiscard]] inline u16 get_fields(u32 slot) const noexcept {
        return (slot < Slots) ? dirtyFields[slot] : 0u;
    }

    [[nodiscard]] inline const SlotMask& dirty_slots_mask() const noexcept {
        return this->entities;
    }

    [[nodiscard]] inline const SubRangePair& get_sub_ranges(u32 slot) const noexcept {
        const u16 mask = (slot < Slots) ? (dirtyFields[slot] & 0x7Fu) : 0u;
        return kSubRangeLUT[mask];
    }
};

} // namespace stockcut
