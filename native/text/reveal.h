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
#include "render_constants.h"
#include "data_structures.h"
namespace stockcut {
struct TextRevealBuffer {
    MultiArray<double, MAX_TEXT_REVEAL_CHARS> t0;
    MultiArray<double, MAX_TEXT_REVEAL_CHARS> fade;
    MultiArray<double, MAX_TEXT_REVEAL_CHARS> dieT;
    MultiArray<double, MAX_TEXT_REVEAL_CHARS> dieFade;
    MultiArray<double, MAX_TEXT_REVEAL_CHARS> alpha;
    MultiArray<u32, MAX_TEXT_REVEAL_CHARS> flags;
    MultiArray<u32, MAX_TEXT_REVEAL_CHARS> visible;
    MultiArray<u32, MAX_TEXT_REVEAL_CHARS> layoutCodepoints;
    MultiArray<u32, MAX_TEXT_REVEAL_CHARS> segmentCodepoints;
    MultiArray<u32, MAX_TEXT_REVEAL_SEGMENTS> segmentOffsets;
    MultiArray<u32, MAX_TEXT_REVEAL_SEGMENTS> segmentLengths;
    MultiArray<double, MAX_TEXT_REVEAL_SEGMENTS> segmentAt;
    MultiArray<i32, MAX_TEXT_REVEAL_CHARS> owner;
    MultiArray<u32, MAX_TEXT_REVEAL_CHARS> jpos;
};
TextRevealBuffer* text_reveal_get_buffer();
u32 text_reveal_get_capacity();
u32 text_reveal_get_segment_capacity();
u32 text_reveal_get_never_flag();
u32 text_reveal_get_tilde_flag();
u32 text_reveal_evaluate(u32 count, double now);
u32 text_reveal_map_owners(u32 layoutCount, u32 segmentCount, u32 segmentCodepointCount);
} // namespace stockcut
