// ============================================================================
// CoreEngine - Canonical SIMD/Cacheline-Aligned Rect & Dirty Rect Coalescer
// ----------------------------------------------------------------------------
// [KỸ THUẬT STOCKFISH & NATRON HỢP NHẤT]:
// 1. Rect<T>: Cấu trúc hình chữ nhật chuẩn duy nhất cho toàn bộ Engine
//    - Rect<float>  (16 bytes, alignas(16)) = CanonicalAABB = NatronRect (SIMD 128-bit)
//    - Rect<double> (32 bytes, alignas(32)) = DirtyRect độ chính xác kép
// 2. Đại số không rẽ nhánh (Algebraic Zeroing & FMA Union) cho giao/hợp RoI.
// ============================================================================
#pragma once
#include "types.h"
#include "data_structures.h"

namespace stockcut {

constexpr usize MAX_DIRTY_RECTS_CAP = 256;
constexpr usize MAX_COALESCED_RECTS = 8;

template <typename T = double>
struct alignas(sizeof(T) * 4) Rect {
    T x0 = T(0);
    T y0 = T(0);
    T x1 = T(0);
    T y1 = T(0);

    [[nodiscard]] static constexpr Rect identity() noexcept {
        return Rect{ T(0), T(0), T(0), T(0) };
    }

    [[nodiscard]] inline constexpr bool is_identity() const noexcept {
        return isEmpty();
    }

    [[nodiscard]] inline T width() const noexcept { return x1 - x0; }
    [[nodiscard]] inline T height() const noexcept { return y1 - y0; }
    [[nodiscard]] inline bool isValid() const noexcept { return (x1 > x0) & (y1 > y0); }
    [[nodiscard]] inline bool isEmpty() const noexcept { return (x0 >= x1) | (y0 >= y1); }
    [[nodiscard]] inline T area() const noexcept {
        return width() * height() * static_cast<T>(isValid());
    }

    [[nodiscard]] inline Rect union_with(const Rect& o) const noexcept {
        if (isEmpty()) return o;
        if (o.isEmpty()) return *this;
        return Rect{
            (x0 < o.x0) ? x0 : o.x0,
            (y0 < o.y0) ? y0 : o.y0,
            (x1 > o.x1) ? x1 : o.x1,
            (y1 > o.y1) ? y1 : o.y1
        };
    }

    inline void uniteWith(const Rect& o) noexcept {
        *this = union_with(o);
    }

    [[nodiscard]] inline bool intersects(const Rect& o) const noexcept {
        return (x0 < o.x1) & (o.x0 < x1) & (y0 < o.y1) & (o.y0 < y1);
    }

    // Giao cắt đại số không rẽ nhánh (Natron Algebraic Zeroing)
    inline void intersectWith(const Rect& o) noexcept {
        x0 = (x0 > o.x0) ? x0 : o.x0;
        y0 = (y0 > o.y0) ? y0 : o.y0;
        x1 = (x1 < o.x1) ? x1 : o.x1;
        y1 = (y1 < o.y1) ? y1 : o.y1;
        const T validMask = static_cast<T>((x1 > x0) & (y1 > y0));
        x0 *= validMask;
        y0 *= validMask;
        x1 *= validMask;
        y1 *= validMask;
    }

    // Hợp nhất đại số bỏ qua rect rỗng (Natron FMA Algebraic Weights)
    inline void unionWith(const Rect& o) noexcept {
        const T oValid  = static_cast<T>(!o.isEmpty());
        const T tValid  = static_cast<T>(!isEmpty());
        const T wMinMax = tValid * oValid;
        const T wO      = oValid * (T(1) - tValid);
        const T wT      = T(1) - oValid;
        const T bx0     = (x0 < o.x0) ? x0 : o.x0;
        const T by0     = (y0 < o.y0) ? y0 : o.y0;
        const T bx1     = (x1 > o.x1) ? x1 : o.x1;
        const T by1     = (y1 > o.y1) ? y1 : o.y1;
        x0 = bx0 * wMinMax + o.x0 * wO + x0 * wT;
        y0 = by0 * wMinMax + o.y0 * wO + y0 * wT;
        x1 = bx1 * wMinMax + o.x1 * wO + x1 * wT;
        y1 = by1 * wMinMax + o.y1 * wO + y1 * wT;
    }
};

static_assert(sizeof(Rect<float>) == 16, "Rect<float> must be 16 bytes");
static_assert(sizeof(Rect<double>) == 32, "Rect<double> must be 32 bytes");

template <>
struct MonoidTraits<Rect<float>> {
    static constexpr usize GroupCapacity = 32;
    static constexpr usize MaxNodes      = 32;
    static constexpr usize MaxChildren   = 128;
};

template <>
struct MonoidTraits<Rect<double>> {
    static constexpr usize GroupCapacity = 32;
    static constexpr usize MaxNodes      = 32;
    static constexpr usize MaxChildren   = 128;
};

using BoundsGroup       = Group<Rect<float>>;
using DirtyRectGroup    = Group<Rect<double>>;
using BoundsPipeline    = Pipelines<Rect<float>>;
using DirtyRectPipeline = Pipelines<Rect<double>>;
using BoundsSequence    = FlatMonoidSequence<Rect<float>>;
using DirtyRectSequence = FlatMonoidSequence<Rect<double>>;

Rect<double>* dirty_get_buf();
double*       dirty_get_area_ptr();
usize         dirty_coalesce(usize count, double W, double H);

} // namespace stockcut
