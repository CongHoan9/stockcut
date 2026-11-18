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
// CoreEngine - Spatial Bitboard (8x8 Grid Spatial Indexing)
// ----------------------------------------------------------------------------
// [KỸ THUẬT STOCKFISH ÁP DỤNG]:
// 1. 64-bit Bitboard Grid Mapping
//    - Kiểu bài toán: Chiếu không gian màn hình vào 64 ô (8x8) dạng bitmask u64
//    - Tham chiếu Stockfish: Stockfish/src/bitboard.h (Bitboard, Square, Rank, File)
// 2. Bitwise Tile Collision Detection & Branchless AABB Projection
//    - Kiểu bài toán: Chuyển đổi tọa độ AABB thực (x0, y0, x1, y1) thành bitmask 64-bit
//      và kiểm tra va chạm vùng bẩn với layer bằng phép tính (maskA & maskB) != 0
//    - Tham chiếu Stockfish: Stockfish/src/bitboard.cpp
// ============================================================================
#include "types.h"
#include "data_structures.h"
#include "quantum/quantum_wave.h"
namespace stockcut {
// Cấu trúc Bitboard 8-byte, memory layout tương đương 100% với u64
struct alignas(8) Bitboard {
    u64 value;
    constexpr Bitboard() noexcept : value(0ULL) {}
    constexpr Bitboard(u64 v) noexcept : value(v) {}
    constexpr Bitboard(const Mask<1>& m) noexcept : value(m.word(0)) {}
    constexpr Bitboard& operator=(u64 v) noexcept { value = v; return *this; }
    constexpr Bitboard& operator=(const Mask<1>& m) noexcept { value = m.word(0); return *this; }
    // Chuyển đổi vô hướng không rẽ nhánh tương thích trực tiếp với u64 & builtins
    constexpr operator u64() const noexcept { return value; }
    constexpr Bitboard operator~() const noexcept { return Bitboard(~value); }
    constexpr Bitboard& operator&=(u64 rhs) noexcept {
        value &= rhs;
        return *this;
    }
    constexpr Bitboard& operator|=(u64 rhs) noexcept {
        value |= rhs;
        return *this;
    }
    constexpr Bitboard& operator^=(u64 rhs) noexcept {
        value ^= rhs;
        return *this;
    }
    constexpr Bitboard operator&(u64 rhs) const noexcept { return Bitboard(value & rhs); }
    constexpr Bitboard operator|(u64 rhs) const noexcept { return Bitboard(value | rhs); }
    constexpr Bitboard operator^(u64 rhs) const noexcept { return Bitboard(value ^ rhs); }
    constexpr Bitboard operator&(Bitboard rhs) const noexcept { return Bitboard(value & rhs.value); }
    constexpr Bitboard operator|(Bitboard rhs) const noexcept { return Bitboard(value | rhs.value); }
    constexpr Bitboard operator^(Bitboard rhs) const noexcept { return Bitboard(value ^ rhs.value); }
    constexpr bool operator==(u64 rhs) const noexcept { return value == rhs; }
    constexpr bool operator!=(u64 rhs) const noexcept { return value != rhs; }
    constexpr bool operator==(Bitboard rhs) const noexcept { return value == rhs.value; }
    constexpr bool operator!=(Bitboard rhs) const noexcept { return value != rhs.value; }
    [[nodiscard]] constexpr bool empty() const noexcept { return value == 0ULL; }
    [[nodiscard]] constexpr bool any() const noexcept { return value != 0ULL; }
    [[nodiscard]] inline i32 popcount() const noexcept { return static_cast<i32>(BitOps<u64>::popcount64(value)); }
    [[nodiscard]] inline i32 lsb() const noexcept {
        const u64 safe = value | 1ULL;
        const i32 tz = static_cast<i32>(BitOps<u64>::lsb(safe));
        const i32 isZero = static_cast<i32>(value == 0ULL);
        return tz | (isZero << 6);
    }
    // Stockfish pop_lsb: Trả về chỉ số bit thấp nhất và xóa bit đó khỏi bitboard (O(1) iteration)
    [[nodiscard]] inline u32 pop_lsb() noexcept {
        return BitOps<u64>::pop_lsb(value);
    }
    template <typename Op>
    [[gnu::always_inline]] inline void for_each_active(Op&& op) const noexcept {
        u64 act = value;
        while (act) {
            op(BitOps<u64>::pop_lsb(act));
        }
    }
    [[nodiscard]] inline i32 msb() const noexcept {
        const u64 safe = value | 1ULL;
        const i32 idx = static_cast<i32>(BitOps<u64>::msb(safe));
        const i32 isZero = static_cast<i32>(value == 0ULL);
        const i32 zeroMask = (isZero << 31) >> 31;
        return (idx & ~zeroMask) | zeroMask;
    }
    [[nodiscard]] inline FastSceneBitboard<64> to_fast_scene() const noexcept {
        FastSceneBitboard<64> s;
        s.bits[0] = value;
        return s;
    }
    [[nodiscard]] inline QuantumSceneWave<64> to_quantum_wave(u8 phase = 0) const noexcept {
        QuantumSceneWave<64> q;
        u64 temp = value;
        while (temp) {
            const u32 bit = BitOps<u64>::pop_lsb(temp);
            q.set_phase(bit, phase, 1);
        }
        return q;
    }
    static inline Bitboard from_fast_scene(const FastSceneBitboard<64>& s) noexcept {
        return Bitboard(s.bits[0]);
    }
    [[nodiscard]] constexpr Bitboard isolate_lsb() const noexcept { return Bitboard(value & (~value + 1ULL)); }
    [[nodiscard]] constexpr Bitboard reset_lsb() const noexcept { return Bitboard(value & (value + ~0ULL)); }
    [[nodiscard]] constexpr bool test_bit(i32 i) const noexcept { return (value >> (i & 63)) & 1ULL; }
    constexpr void set_bit(i32 i) noexcept { value |= (1ULL << (i & 63)); }
    constexpr void clear_bit(i32 i) noexcept { value &= ~(1ULL << (i & 63)); }
    // Hỗ trợ kẹp biên 0-7 bằng bitwise thuần túy (0 lệnh CMOV/JMP, 0 phép toán + - * /)
    [[nodiscard]] static constexpr i32 clamp_0_7(i32 v) noexcept {
        v &= ~(v >> 31);
        i32 h = v >> 3;
        h |= (h >> 16);
        h |= (h >> 8);
        h |= (h >> 4);
        h |= (h >> 2);
        h |= (h >> 1);
        i32 m = (h << 31) >> 31;
        return (v | m) & 7;
    }
    // Factory Method: Khởi tạo Bitboard từ AABB bằng toán tử bit thuần túy không rẽ nhánh
    [[nodiscard]] static constexpr Bitboard FromAABB(
        double x0, double y0, double x1, double y1, double W, double H
    ) noexcept {
        const bool is_valid_input = (x1 > x0) & (y1 > y0) & (W > 0.0) & (H > 0.0) &
                                    (x1 > 0.0) & (y1 > 0.0) & (x0 < W) & (y0 < H);
        const u64 valid_mask = static_cast<u64>((static_cast<i64>(is_valid_input) << 63) >> 63);
        const double invW = (W > 0.0) ? (8.0 / W) : 0.0;
        const double invH = (H > 0.0) ? (8.0 / H) : 0.0;
        i32 c0 = static_cast<i32>(x0 * invW);
        i32 c1 = static_cast<i32>((x1 - 0.001) * invW);
        i32 r0 = static_cast<i32>(y0 * invH);
        i32 r1 = static_cast<i32>((y1 - 0.001) * invH);
        c0 = clamp_0_7(c0);
        c1 = clamp_0_7(c1);
        r0 = clamp_0_7(r0);
        r1 = clamp_0_7(r1);
        const bool is_valid_coords = (c1 >= c0) & (r1 >= r0);
        const u64 coord_mask = static_cast<u64>((static_cast<i64>(is_valid_coords) << 63) >> 63);
        // Sinh mask cột và hàng bằng phép XOR bitwise thay thế phép trừ (7 - c = 7 ^ c) và dịch bit (r * 8 = r << 3)
        const u64 col_mask = (0xFFULL >> (7 ^ c1)) & (0xFFULL << c0);
        const u64 rank_mask = (~0ULL >> ((7 ^ r1) << 3)) & (~0ULL << (r0 << 3));
        // Nhân rộng ma trận dọc bằng toán tử bit (Shifts & OR) thay thế phép nhân số học
        u64 col_spread = col_mask;
        col_spread |= (col_spread << 8);
        col_spread |= (col_spread << 16);
        col_spread |= (col_spread << 32);
        const u64 final_bb = col_spread & rank_mask;
        return Bitboard(final_bb & valid_mask & coord_mask);
    }
    // Factory Method: Khởi tạo Bitboard các ô gạch 8x8 NẰM HOÀN TOÀN BÊN TRONG AABB (Conservative Inner Coverage)
    // Dùng cho Early-Z Occlusion Culling: Đảm bảo 100% diện tích ô gạch được che phủ bởi quad đục
    [[nodiscard]] static constexpr Bitboard FromAABBInner(
        double x0, double y0, double x1, double y1, double W, double H
    ) noexcept {
        const bool is_valid_input = (x1 > x0) & (y1 > y0) & (W > 0.0) & (H > 0.0);
        const u64 valid_mask = static_cast<u64>((static_cast<i64>(is_valid_input) << 63) >> 63);
        const double invW = (W > 0.0) ? (8.0 / W) : 0.0;
        const double invH = (H > 0.0) ? (8.0 / H) : 0.0;
        i32 c0 = (x0 <= 0.0) ? 0 : static_cast<i32>(x0 * invW + 0.99999);
        i32 c1 = (x1 >= W) ? 7 : static_cast<i32>(x1 * invW + 0.00001) - 1;
        i32 r0 = (y0 <= 0.0) ? 0 : static_cast<i32>(y0 * invH + 0.99999);
        i32 r1 = (y1 >= H) ? 7 : static_cast<i32>(y1 * invH + 0.00001) - 1;

        if (c1 < c0 || r1 < r0 || c1 < 0 || r1 < 0 || c0 > 7 || r0 > 7) {
            return Bitboard(0ULL);
        }

        c0 = clamp_0_7(c0);
        c1 = clamp_0_7(c1);
        r0 = clamp_0_7(r0);
        r1 = clamp_0_7(r1);

        const bool is_valid_coords = (c1 >= c0) & (r1 >= r0);
        const u64 coord_mask = static_cast<u64>((static_cast<i64>(is_valid_coords) << 63) >> 63);

        const u64 col_mask = (0xFFULL >> (7 ^ c1)) & (0xFFULL << c0);
        const u64 rank_mask = (~0ULL >> ((7 ^ r1) << 3)) & (~0ULL << (r0 << 3));

        u64 col_spread = col_mask;
        col_spread |= (col_spread << 8);
        col_spread |= (col_spread << 16);
        col_spread |= (col_spread << 32);
        const u64 final_bb = col_spread & rank_mask;
        return Bitboard(final_bb & valid_mask & coord_mask);
    }

    // ------------------------------------------------------------------------
    // [COMPILE-TIME ENUMS & TEMPLATE SPECIALIZATION]:
    // ------------------------------------------------------------------------
    enum class SpatialCoverage : u8 {
        ExactAABB = 0,
        ConservativeInner = 1,
        ConservativeOuter = 2
    };

    enum class IntervalOp : u8 {
        Overlap = 0,
        Contained = 1
    };

    template <SpatialCoverage Coverage = SpatialCoverage::ExactAABB>
    [[nodiscard]] static constexpr Bitboard FromAABBT(
        double x0, double y0, double x1, double y1, double W, double H
    ) noexcept {
        if constexpr (Coverage == SpatialCoverage::ConservativeInner) {
            return FromAABBInner(x0, y0, x1, y1, W, H);
        } else if constexpr (Coverage == SpatialCoverage::ConservativeOuter) {
            return FromAABBConservative(x0, y0, x1, y1, W, H);
        } else {
            return FromAABB(x0, y0, x1, y1, W, H);
        }
    }

    template <IntervalOp Op>
    [[nodiscard]] static constexpr bool EvaluateIntervalRelation(u64 spanA, u64 spanB) noexcept {
        if constexpr (Op == IntervalOp::Overlap) {
            return (spanA & spanB) != 0ULL;
        } else if constexpr (Op == IntervalOp::Contained) {
            return (spanA & spanB) == spanB;
        }
    }

    // Đại số khoảng 1D (Hyperbola Quintessence / borrow propagation)
    [[nodiscard]] static constexpr u64 MakeIntervalSpan64(u32 start, u32 end) noexcept {
        const u32 clampedEnd = end > 64 ? 64 : end;
        const u64 maskEnd = (clampedEnd == 64) ? ~0ULL : ((1ULL << clampedEnd) - 1ULL);
        const u64 maskStart = (1ULL << (start & 63)) - 1ULL;
        return maskEnd ^ maskStart;
    }

    // Dãn nở bitboard bảo toàn biên 1-tile xung quanh bằng phép toán bit thuần túy (0 lệnh if/branch, 0 phép toán số học)
    [[nodiscard]] static constexpr Bitboard FromAABBConservative(
        double x0, double y0, double x1, double y1, double W, double H
    ) noexcept {
        const Bitboard base = FromAABB(x0, y0, x1, y1, W, H);
        const u64 val = base.value;
        // Dãn nở bitboard 8x8 ra các ô lân cận bằng phép toán bit thuần túy (Morphological Dilation):
        // Mặt nạ chặn tràn biên cột File A (cột 0) và File H (cột 7)
        constexpr u64 NOT_FILE_A = 0xFEFEFEFEFEFEFEFEULL;
        constexpr u64 NOT_FILE_H = 0x7F7F7F7F7F7F7F7FULL;
        const u64 horiz = val | ((val & NOT_FILE_A) >> 1) | ((val & NOT_FILE_H) << 1);
        const u64 expanded = horiz | (horiz >> 8) | (horiz << 8);
        return Bitboard(expanded);
    }
    // Chuyển đổi 64-bit spatial tile grid sang toạ độ scissor box OpenGL O(1) bằng toán học bitmask (0 lệnh if, 0 toán tử 3 ngôi)
    inline bool to_scissor_box(
        double W, double H,
        double& outX, double& outY, double& outW, double& outH
    ) const noexcept {
        const u64 hasVal = (value | (~value + 1ULL)) >> 63;
        const double valid_mask = static_cast<double>(hasVal);
        const u64 safeVal = value | (hasVal ^ 1ULL);
        const i32 rMin = static_cast<i32>(BitOps<u64>::lsb(safeVal)) >> 3;
        const i32 rMax = static_cast<i32>(BitOps<u64>::msb(safeVal)) >> 3;
        u64 folded = safeVal | (safeVal >> 32);
        folded |= (folded >> 16);
        folded |= (folded >> 8);
        const u32 colMask = static_cast<u32>(folded & 0xFFULL);
        const i32 cMin = static_cast<i32>(BitOps<u32>::lsb(colMask));
        const i32 cMax = static_cast<i32>(BitOps<u32>::msb(colMask));
        const double tileW = W * 0.125;
        const double tileH = H * 0.125;
        outX = (cMin * tileW) * valid_mask;
        outW = ((cMax - cMin + 1) * tileW) * valid_mask;
        outY = (H - (rMax + 1) * tileH) * valid_mask;
        outH = ((rMax - rMin + 1) * tileH) * valid_mask;
        return hasVal != 0ULL;
    }
};
static_assert(sizeof(Bitboard) == 8, "Bitboard must be 8 bytes");

// Stockfish free function pop_lsb
inline u32 pop_lsb(Bitboard& b) noexcept {
    return b.pop_lsb();
}

// ============================================================================
// [STOCKFISH DATA-STRUCTURE-CENTRIC CORE ENGINE ARCHITECTURE] (GEMINI.md Rule 7)
// 1. TypeBitboardMap: Quản lý phân loại Bitboard theo EnumType (Stockfish byTypeBB[])
//    - Trải phẳng toàn bộ vòng lặp duyệt for thành truy xuất O(1) và pop_lsb().
//    - Zero-Allocation, cấu trúc tự chủ (Self-Contained Template Data Structure).
// 2. EnumBitmask: Mặt nạ bit vô hướng đại số đóng cho EnumType (PassKindMask, EffectMask)
//    - Loại bỏ triệt để việc duyệt mảng kiểm tra kiểu thủ tục bằng phép AND bitwise O(1).
// ============================================================================
template <typename EnumType, usize EnumCount>
class alignas(8) TypeBitboardMap {
private:
    MultiArray<Bitboard, EnumCount> bitboards_;
    Bitboard allOccupied_{0ULL};

public:
    constexpr TypeBitboardMap() noexcept = default;

    inline void clear() noexcept {
        bitboards_.clear();
        allOccupied_ = 0ULL;
    }

    inline void set_direct(EnumType type, Bitboard bb) noexcept {
        const usize t = static_cast<usize>(type);
        if (t < EnumCount) {
            bitboards_[t] = bb;
            allOccupied_ |= bb;
        }
    }

    inline void add(EnumType type, u32 index) noexcept {
        const usize t = static_cast<usize>(type);
        if (t < EnumCount && index < 64u) {
            bitboards_[t].set_bit(static_cast<i32>(index));
            allOccupied_.set_bit(static_cast<i32>(index));
        }
    }

    inline void remove(EnumType type, u32 index) noexcept {
        const usize t = static_cast<usize>(type);
        if (t < EnumCount && index < 64u) {
            bitboards_[t].clear_bit(static_cast<i32>(index));
            recompute_occupied();
        }
    }

    inline void recompute_occupied() noexcept {
        Bitboard occ{0ULL};
        for (usize i = 0; i < EnumCount; ++i) {
            occ |= bitboards_[i];
        }
        allOccupied_ = occ;
    }

    [[nodiscard]] inline Bitboard get(EnumType type) const noexcept {
        const usize t = static_cast<usize>(type);
        return (t < EnumCount) ? bitboards_[t] : Bitboard(0ULL);
    }

    [[nodiscard]] inline Bitboard operator[](EnumType type) const noexcept {
        return get(type);
    }

    [[nodiscard]] inline Bitboard occupied() const noexcept {
        return allOccupied_;
    }

    [[nodiscard]] inline bool has(EnumType type) const noexcept {
        return get(type).any();
    }

    [[nodiscard]] inline u32 count(EnumType type) const noexcept {
        return static_cast<u32>(get(type).popcount());
    }

    template <typename... Rest>
    [[nodiscard]] inline Bitboard match_any(EnumType first, Rest... rest) const noexcept {
        if constexpr (sizeof...(rest) == 0) {
            return get(first);
        } else {
            return get(first) | match_any(rest...);
        }
    }

    template <typename Fn>
    inline void for_each(EnumType type, Fn&& fn) const noexcept {
        Bitboard b = get(type);
        while (b) {
            const u32 idx = b.pop_lsb();
            fn(idx);
        }
    }

    template <typename Fn>
    inline void for_each_occupied(Fn&& fn) const noexcept {
        Bitboard b = allOccupied_;
        while (b) {
            const u32 idx = b.pop_lsb();
            fn(idx);
        }
    }
};

template <typename EnumType, typename StorageType = u32>
class EnumBitmask {
private:
    StorageType mask_{0};

public:
    constexpr EnumBitmask() noexcept : mask_(0) {}
    constexpr explicit EnumBitmask(StorageType m) noexcept : mask_(m) {}

    constexpr void clear() noexcept { mask_ = 0; }

    constexpr void set(EnumType val) noexcept {
        const u32 bit = static_cast<u32>(val);
        mask_ |= (static_cast<StorageType>(1) << bit);
    }

    constexpr void clear_bit(EnumType val) noexcept {
        const u32 bit = static_cast<u32>(val);
        mask_ &= ~(static_cast<StorageType>(1) << bit);
    }

    [[nodiscard]] constexpr bool has(EnumType val) const noexcept {
        const u32 bit = static_cast<u32>(val);
        return (mask_ & (static_cast<StorageType>(1) << bit)) != 0;
    }

    [[nodiscard]] constexpr bool has_any(StorageType queryMask) const noexcept {
        return (mask_ & queryMask) != 0;
    }

    [[nodiscard]] constexpr bool has_all(StorageType queryMask) const noexcept {
        return (mask_ & queryMask) == queryMask;
    }

    [[nodiscard]] constexpr StorageType raw() const noexcept { return mask_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return mask_ == 0; }
    [[nodiscard]] constexpr bool any() const noexcept { return mask_ != 0; }

    [[nodiscard]] inline u32 count() const noexcept {
        return BitOps<StorageType>::popcount(mask_);
    }

    template <typename... Args>
    [[nodiscard]] static constexpr StorageType make_mask(Args... args) noexcept {
        return ((static_cast<StorageType>(1) << static_cast<u32>(args)) | ...);
    }
};

// ============================================================================
// Stockfish-Style Unified Bitboard & Mask Aliases (GEMINI.md Rule 7 & 8)
// Hỗ trợ cả Bitboard 64-bit truyền thống và cấu trúc Nested Mask không giới hạn layer
// ============================================================================
// 1. Phân loại Slot theo Chế độ Động học (Tương đương byColorBB[] của Stockfish)
template <typename Mask = Bitboard>
using ByModeMask = MultiArray<Mask, TRACK_MODE_NB>;
using ByModeBB   = ByModeMask<Bitboard>;

// 2. Phân loại Slot theo Định dạng Tài nguyên (Tương đương byTypeBB[] của Stockfish)
template <typename Mask = Bitboard>
using ByTypeMask = MultiArray<Mask, MEDIA_TYPE_NB>;
using ByTypeBB   = ByTypeMask<Bitboard>;
using MediaTypeBitboardMap = TypeBitboardMap<MediaType, MEDIA_TYPE_NB>;
static_assert(sizeof(MediaTypeBitboardMap) == 72, "MediaTypeBitboardMap must be exactly 72 bytes");

// 3. Phân loại Slot theo Phân tầng Động học (MediaTier)
template <typename Mask = Bitboard>
using ByTierMask = MultiArray<Mask, MEDIA_TIER_NB>;
using ByTierBB   = ByTierMask<Bitboard>;

// 4. Phân loại Slot theo Bản chất Động học Nội tại vs Keyframe (Dual-Axis Dynamic Bitboards)
template <typename Mask = Bitboard>
using ByDynamicNatureMask = MultiArray<Mask, DYNAMIC_NATURE_NB>;
using ByDynamicNatureBB   = ByDynamicNatureMask<Bitboard>;

template <typename Mask = Bitboard>
using ByDynamicAxisMask   = MultiArray<Mask, DYNAMIC_AXIS_NB>;
using ByDynamicAxisBB     = ByDynamicAxisMask<Bitboard>;

// 5. Bitboard Không gian Màn hình (8x8 Grid = 64 Tiles)
using SpatialCoverage = Bitboard::SpatialCoverage;
using IntervalOp      = Bitboard::IntervalOp;
// 6. Dấu chân không gian 64 Slot của Timeline
using LayerTilesBB = MultiArray<Bitboard, MAX_TIMELINE_SLOTS>;
// 7. Cấu trúc Quản lý Vùng bẩn Không gian & Khung hình Tập trung (Unified Frame Spatial Dirties)
template <usize Slots> struct SpatialFrameDirty;
} // namespace stockcut
