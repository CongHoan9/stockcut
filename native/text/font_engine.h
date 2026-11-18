// ============================================================================
// [ĐÁNH GIÁ ĐIỂM NGHẼN HỆ THỐNG]:
// 1. Chi phí rasterize glyph động lặp lại (Glyph Re-rasterization Bottleneck):
//    - Khởi tạo lại glyph vector ở mỗi frame làm tụt giảm khung hình nghiêm trọng.
// 2. Phân mảnh layout văn bản và ngắt dòng đa ngôn ngữ:
//    - Đo đạc chuỗi text qua DOM/Canvas2D chiếm dụng luồng chính của trình duyệt.
//
// [HƯỚNG KHẮC PHỤC TRIỆT ĐỂ — KHÔNG VÁ]:
// 1. FreeType + HarfBuzz + WOFF2 Glyph Atlas 2048x2048 tĩnh:
//    - Tạo hình và nướng toàn bộ glyph vào Texture Atlas tĩnh 1 lần duy nhất.
// 2. VboQuadInstance Batching:
//    - Mỗi glyph tương ứng 1 Quad 48-byte nạp thẳng xuống GPU qua instanced rendering.
// ============================================================================
#pragma once
#include "types.h"
namespace stockcut {
constexpr u32 MAX_NATIVE_FONT_BYTES = 8u * 1024u * 1024u;
constexpr u32 MAX_DECOMPRESSED_FONT_BYTES = 16u * 1024u * 1024u;
constexpr u32 MAX_NATIVE_TEXT_UTF8_BYTES = 65536;
constexpr u32 MAX_NATIVE_TEXT_GLYPHS = 4096;
constexpr u32 NATIVE_GLYPH_ATLAS_SIZE = 2048;
enum NativeFontStatus : u32 {
    FONT_STATUS_OK = 0,
    FONT_STATUS_INVALID_INPUT = 1,
    FONT_STATUS_LIBRARY_INIT_FAILED = 2,
    FONT_STATUS_INVALID_FONT = 3,
    FONT_STATUS_FONT_TOO_LARGE = 4,
    FONT_STATUS_TEXT_TOO_LARGE = 5,
    FONT_STATUS_SHAPING_FAILED = 6,
    FONT_STATUS_ATLAS_FULL = 7,
    FONT_STATUS_GLYPH_FAILED = 8,
    FONT_STATUS_WOFF2_DECOMPRESSION_FAILED = 9,
    FONT_STATUS_DECOMPRESSED_FONT_TOO_LARGE = 10
};
struct NativeTextGlyph {
    u32 glyphIndex;
    u32 cluster;
    float x;
    float y;
    float advanceX;
    float advanceY;
    u32 atlasX;
    u32 atlasY;
    u32 atlasWidth;
    u32 atlasHeight;
};
u8* text_font_get_input_buffer();
u32 text_font_get_input_capacity();
u8* text_font_get_utf8_buffer();
u32 text_font_get_utf8_capacity();
NativeTextGlyph* text_font_get_glyph_buffer();
u32 text_font_get_glyph_capacity();
// Optional byte-offset -> keyframe-index map consumed by the next shape call.
u32* text_font_get_keyframe_map_buffer();
u32 text_font_get_keyframe_map_capacity();
void text_font_set_keyframe_map_count(u32 byteCount);
void text_font_begin_glyph_batch(u32 lineCount);
u32 text_font_append_shaped_line(u32 lineIndex);
float* text_font_get_render_glyph_data();
float* text_font_get_render_line_widths();
u32 text_font_get_render_glyph_count();
u8* text_font_get_atlas();
u32* text_font_get_atlas_dirty_rect();
void text_font_reset_atlas_dirty_rect();
u32 text_font_load(u32 fontKey, u32 byteCount);
u32 text_font_load_memory(u32 fontKey, const u8* data, u32 byteCount);
u32 text_font_load_by_file_id(u16 fileId);
u32 text_font_shape(u32 utf8ByteCount, float pixelSize);
u32 text_font_get_glyph_count();
u32 text_font_get_status();
float text_font_get_baseline_offset();
} // namespace stockcut
