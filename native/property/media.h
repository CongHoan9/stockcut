// ============================================================================
// CoreEngine - Pure Anonymous Media Ontology (Media<MediaType, Block>)
// ----------------------------------------------------------------------------
// Kiến trúc Vô Danh Tuyệt Đối (Zero Decoders / Zero Symbolic Struct Wrappers):
// 1. get_block<MediaType>:
//    - Hoàn toàn KHÔNG dùng struct trung gian (No MediaTypeBlockTraits).
//    - Suy diễn kích thước Block trực tiếp tại compile-time:
//      + Text / Lyric / Path: Block<24, 1>
//      + Image / Video / Audio / Background / Root: Block<16, 1>
// 2. Media<MediaType, Block>:
//    - Vỏ bọc duy nhất chứa struct Block trực tiếp cho mọi Media Type thực thụ.
//    - Đọc/ghi thuần túy qua raw address: get_value<T, Size>(addr) / set_value(addr, val).
// 3. Autonomous struct Effect (effect.h):
//    - Tách riêng biệt khỏi MediaType, nén toàn bộ metadata/thuộc tính ngoài vào Block,
//      kèm con trỏ hàm dung hợp Monoid (foldFn) O(1) không rẽ nhánh.
// Mọi quy ước địa chỉ thanh ghi reg[] và bit[] nằm duy nhất trong:
// BIT_AND_REGISTER_DICTIONARY.txt
// ============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include "../core/types.h"
#include "block.h"
#include "effect.h"
#include "../effect/monoid_operator.h"

namespace stockcut {

// ============================================================================
// 1. ZERO-STRUCT COMPILE-TIME BLOCK DEDUCTION (0 STRUCT TRUNG GIAN)
// ============================================================================
template <MediaType M>
constexpr usize media_float_count() noexcept {
    if constexpr (M == MediaType::Text || M == MediaType::Lyric || M == MediaType::Path) {
        return 24;
    } else {
        return 16;
    }
}

template <MediaType M>
constexpr usize media_bit_blocks() noexcept {
    return 1;
}

template <MediaType M>
using get_block = Block<media_float_count<M>(), media_bit_blocks<M>()>;

// ============================================================================
// 2. CẤU TRÚC MEDIA VÔ DANH (MEDIA<MEDIATYPE, BLOCK>)
// Kế thừa trực tiếp Block: 0 boilerplate, 0 wrapper, 0 overhead.
// ============================================================================
template <MediaType M, typename B = get_block<M>>
struct alignas(alignof(B)) Media : public B {
    static constexpr MediaType mediaType = M;
};

// ----------------------------------------------------------------------------
// 3. CANONICAL REGISTER & BIT CONSTANTS CHO MEDIA<MEDIATYPE::PATH>
// Tuân thủ 100% BIT_AND_REGISTER_DICTIONARY.txt & Canh Giới Vô Danh Tuyệt Đối
// ----------------------------------------------------------------------------
namespace path_reg {
    constexpr usize STROKE_WIDTH       = 9;  // CSS stroke-width
    constexpr usize STROKE_MITERLIMIT  = 10; // CSS stroke-miterlimit
    constexpr usize STROKE_DASHOFFSET  = 11; // CSS stroke-dashoffset
    constexpr usize EFFECTIVE_STROKE_W = 12; // Runtime resolved stroke width
    constexpr usize TRIM_START         = 13; // Vector path trim start (0.0..1.0)
    constexpr usize BASE_FILL_COLOR    = 14; // Base fill color (u32 RGBA)
    constexpr usize BASE_STROKE_COLOR  = 15; // Base stroke color (u32 RGBA)
    constexpr usize EFFECTIVE_FILL_COL = 16; // Runtime resolved fill color (u32 RGBA)
    constexpr usize EFFECTIVE_STROKE_C = 17; // Runtime resolved stroke color (u32 RGBA)
    constexpr usize DATA_HASH          = 18; // 32-bit FNV-1a hash of SVG d string
    constexpr usize VIEWBOX_MIN_X      = 19; // ViewBox minX
    constexpr usize VIEWBOX_MIN_Y      = 20; // ViewBox minY
    constexpr usize VIEWBOX_MAX_X      = 21; // ViewBox maxX
    constexpr usize VIEWBOX_MAX_Y      = 22; // ViewBox maxY
    constexpr usize DATA_LEN           = 23; // Length of SVG d string
}

namespace path_bit {
    constexpr usize VISIBLE            = 0;  // Visibility bit
    constexpr usize FILL_RULE          = 1;  // 0 = NonZero, 1 = EvenOdd
    constexpr usize LINE_CAP_LO        = 2;  // 2 bits: 0 = Butt, 1 = Round, 2 = Square
    constexpr usize LINE_CAP_HI        = 3;
    constexpr usize LINE_JOIN_LO       = 4;  // 2 bits: 0 = Miter, 1 = Round, 2 = Bevel
    constexpr usize LINE_JOIN_HI       = 5;
    constexpr usize HAS_FILL           = 6;  // 1 if fill is enabled
    constexpr usize HAS_STROKE         = 7;  // 1 if stroke is enabled
    constexpr usize INHERIT_FILL       = 8;  // 1 if fill is inherited from Style/Role
    constexpr usize INHERIT_STROKE     = 9;  // 1 if stroke is inherited from Style/Role
    constexpr usize CLOSED_PATH        = 10; // 1 if path has Z/z closed loop
}

} // namespace stockcut
