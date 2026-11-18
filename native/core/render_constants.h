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
#include "types.h"
namespace stockcut {
constexpr u32 MAX_BATCH_QUADS_CPP = 256;
// MAX_TIMELINE_SLOTS defined in types.h
constexpr u32 FLOATS_PER_VERTEX_CPP = 11;
constexpr u32 VERTICES_PER_QUAD_CPP = 6;
constexpr u32 FLOATS_PER_QUAD_CPP =
    FLOATS_PER_VERTEX_CPP * VERTICES_PER_QUAD_CPP;
constexpr u32 FLOATS_PER_QUAD_INPUT_CPP = 18;
constexpr u32 MAX_TEXTURE_SLOTS = 8;
constexpr u32 FLOAT_BYTES = 4;
constexpr u32 VERTEX_POSITION_COMPONENTS = 2;
constexpr u32 VERTEX_UV_COMPONENTS = 2;
constexpr u32 VERTEX_COLOR_COMPONENTS = 4;
constexpr u32 VERTEX_TEXTURE_ID_COMPONENTS = 1;
constexpr u32 VERTEX_MODE_COMPONENTS = 1;
constexpr u32 VERTEX_SPIN_COMPONENTS = 1;
constexpr u32 VERTEX_POSITION_OFFSET = 0;
constexpr u32 VERTEX_UV_OFFSET = 2;
constexpr u32 VERTEX_COLOR_OFFSET = 4;
constexpr u32 VERTEX_TEXTURE_ID_OFFSET = 8;
constexpr u32 VERTEX_MODE_OFFSET = 9;
constexpr u32 VERTEX_SPIN_OFFSET = 10;
constexpr u32 VBO_INPUT_X = 0;
constexpr u32 VBO_INPUT_Y = 1;
constexpr u32 VBO_INPUT_WIDTH = 2;
constexpr u32 VBO_INPUT_HEIGHT = 3;
constexpr u32 VBO_INPUT_PIVOT_X = 4;
constexpr u32 VBO_INPUT_PIVOT_Y = 5;
constexpr u32 VBO_INPUT_ROTATION_RAD = 6;
constexpr u32 VBO_INPUT_RED = 7;
constexpr u32 VBO_INPUT_GREEN = 8;
constexpr u32 VBO_INPUT_BLUE = 9;
constexpr u32 VBO_INPUT_ALPHA = 10;
constexpr u32 VBO_INPUT_TEXTURE_SLOT = 11;
constexpr u32 VBO_INPUT_MODE = 12;
constexpr u32 VBO_INPUT_SPIN_RAD = 13;
constexpr u32 VBO_INPUT_UV_LEFT = 14;
constexpr u32 VBO_INPUT_UV_TOP = 15;
constexpr u32 VBO_INPUT_UV_RIGHT = 16;
constexpr u32 VBO_INPUT_UV_BOTTOM = 17;
constexpr u32 TOTAL_VBO_FLOATS =
    MAX_BATCH_QUADS_CPP * FLOATS_PER_QUAD_CPP;
constexpr u32 TOTAL_VBO_INPUT_FLOATS =
    MAX_BATCH_QUADS_CPP * FLOATS_PER_QUAD_INPUT_CPP;
enum RenderConstantIndex : u32 {
    RC_MAX_BATCH_QUADS,
    RC_FLOATS_PER_VERTEX,
    RC_VERTICES_PER_QUAD,
    RC_FLOATS_PER_QUAD,
    RC_FLOATS_PER_QUAD_INPUT,
    RC_MAX_TEXTURE_SLOTS,
    RC_FLOAT_BYTES,
    RC_VERTEX_POSITION_COMPONENTS,
    RC_VERTEX_UV_COMPONENTS,
    RC_VERTEX_COLOR_COMPONENTS,
    RC_VERTEX_TEXTURE_ID_COMPONENTS,
    RC_VERTEX_MODE_COMPONENTS,
    RC_VERTEX_SPIN_COMPONENTS,
    RC_VERTEX_POSITION_OFFSET,
    RC_VERTEX_UV_OFFSET,
    RC_VERTEX_COLOR_OFFSET,
    RC_VERTEX_TEXTURE_ID_OFFSET,
    RC_VERTEX_MODE_OFFSET,
    RC_VERTEX_SPIN_OFFSET,
    RC_MAX_DISPLAY_COMMANDS,
    RC_DRAW_COMMAND_WORDS,
    RC_DRAW_COMMAND_FLOATS,
    RC_RENDER_COMMAND_WORDS,
    RC_RENDER_COMMAND_FLOATS,
    RC_VBO_INPUT_X,
    RC_VBO_INPUT_Y,
    RC_VBO_INPUT_WIDTH,
    RC_VBO_INPUT_HEIGHT,
    RC_VBO_INPUT_PIVOT_X,
    RC_VBO_INPUT_PIVOT_Y,
    RC_VBO_INPUT_ROTATION_RAD,
    RC_VBO_INPUT_RED,
    RC_VBO_INPUT_GREEN,
    RC_VBO_INPUT_BLUE,
    RC_VBO_INPUT_ALPHA,
    RC_VBO_INPUT_TEXTURE_SLOT,
    RC_VBO_INPUT_MODE,
    RC_VBO_INPUT_SPIN_RAD,
    RC_VBO_INPUT_UV_LEFT,
    RC_VBO_INPUT_UV_TOP,
    RC_VBO_INPUT_UV_RIGHT,
    RC_VBO_INPUT_UV_BOTTOM,
    RC_COUNT
};
constexpr u32 MAX_DISPLAY_COMMANDS_RENDER = 256;
constexpr u32 DRAW_COMMAND_WORDS = 38;
constexpr u32 DRAW_COMMAND_FLOATS = 19;
constexpr u32 RENDER_COMMAND_WORDS = 66;
constexpr u32 RENDER_COMMAND_FLOATS = 33;
constexpr u32 MAX_TEXT_REVEAL_CHARS = 4096;
constexpr u32 MAX_TEXT_REVEAL_SEGMENTS = 32;
constexpr u32 TEXT_REVEAL_FLAG_NEVER = 1;
constexpr u32 TEXT_REVEAL_FLAG_TILDE = 2;
const u32* render_constants_data();
} // namespace stockcut
