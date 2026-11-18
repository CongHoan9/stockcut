// ============================================================================
// [ĐÁNH GIÁ ĐIỂM NGHẼN HỆ THỐNG]:
// 1. Phân mảnh dữ liệu bộ nhớ và truy cập L1 Cache rời rạc:
//    - Cấu trúc đối tượng rời rạc và việc kiểm tra phòng thủ lặp lại trong Hot Path 60 FPS
//      làm tăng tỉ lệ trượt L1 Cache và gây lãng phí chu kỳ vi xử lý.
// 2. Chi phí cấp phát heap động (Heap Thrashing):
//    - Cấp phát heap trong vòng lặp chính làm phát sinh áp lực dọn rác và phân mảnh RAM.
//
// [HƯỚNG KHẮC PHỤC TRIỆT ĐỂ — KHÔNG VÁ]:
// 1. Kiến trúc mảng phẳng DOD (Data-Oriented Design) & Căn lề Cacheline:
//    - Căn lề dữ liệu 64-byte (alignas(64)), nạp dữ liệu liên tục theo đường truyền SIMD.
// 2. Straight-line Execution & Zero-Allocation:
//    - Loại bỏ kiểm tra hợp lệ phòng thủ trong Hot Path; 0 malloc/free ở tần số 60 FPS.
// ============================================================================
#pragma once
// ============================================================================
// CoreEngine - Packed Style Subsystem (Pha 1)
// ----------------------------------------------------------------------------
// [KỸ THUẬT STOCKFISH ÁP DỤNG]:
// 1. Packed 128-byte Struct (Exactly 2 CPU Cachelines)
//    - Kiểu bài toán: Truy cập ngẫu nhiên phong cách không tốn chi phí cache miss L1/L2
//    - Tham chiếu Stockfish: Stockfish/src/types.h, Stockfish/src/tt.h (alignas(64))
// 2. Bitmask Feature Activation (StyleActiveBits)
//    - Kiểu bài toán: Kiểm tra thuộc tính kích hoạt bằng bitwise AND mà không rẽ nhánh
//    - Tham chiếu Stockfish: Stockfish/src/bitboard.h
// Tra cứu thêm tại: native/phan_loai_nghiep_vu_stockfish.txt (Mục 1 & Mục 6)
// ============================================================================
#include "types.h"
namespace stockcut {
enum StyleActiveBits : u32 {
    STYLE_BIT_TRANSFORM   = 1 << 0,
    STYLE_BIT_OPACITY     = 1 << 1,
    STYLE_BIT_FILTER      = 1 << 2,
    STYLE_BIT_BORDER      = 1 << 3,
    STYLE_BIT_RADIUS      = 1 << 4,
    STYLE_BIT_TYPOGRAPHY  = 1 << 5,
    STYLE_BIT_ANIMATION   = 1 << 6,
    STYLE_BIT_SHADOW      = 1 << 7,
    STYLE_BIT_CLIP        = 1 << 8,
    STYLE_BIT_BACKGROUND  = 1 << 9,
    STYLE_BIT_VISIBLE     = 1 << 10
};
struct alignas(64) PackedStyle {
    float x;
    float y;
    float w;
    float h;
    float translateX;
    float translateY;
    float scaleX;
    float scaleY;
    float rotateDeg;
    float skewX;
    float skewY;
    float originX;
    float originY;
    float opacity;
    i32   zIndex;
    u32   activeFlags;
    u32   color;
    u32   backgroundColor;
    u32   borderColor;
    float borderWidth;
    float radiusTL;
    float radiusTR;
    float radiusBR;
    float radiusBL;
    float fontSize;
    float letterSpacing;
    float lineHeight;
    u32   fontPacked;
    float filterBlur;
    float filterBright;
    float filterContrast;
    u32   effectsPacked;

    void reset() noexcept;
    void compute_affine_2d(float* out6Floats) const noexcept;
    void interpolate_from(const PackedStyle& from, const PackedStyle& to, float t) noexcept;
    [[nodiscard]] u64 compute_hash() const noexcept;
};
static_assert(sizeof(PackedStyle) == 128, "PackedStyle must be 128 bytes");
static_assert(alignof(PackedStyle) == 64, "PackedStyle must be 64-byte aligned");
constexpr u32 STYLE_ARENA_CAPACITY = 256u;

PackedStyle* style_get_arena();

// Stockfish Boundary Gateway: Kẹp biên toán học không rẽ nhánh qua Bitmask 2^N
[[nodiscard]] PackedStyle& style_slot(u32 idx) noexcept;

void         style_reset(u32 idx);
void         style_compute_affine_2d(u32 idx, float* out6Floats);
void         style_interpolate(u32 fromIdx, u32 toIdx, float t, u32 outIdx);
u64          style_hash(u32 idx);
} // namespace stockcut
