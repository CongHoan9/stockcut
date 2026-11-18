// ============================================================================
// CoreEngine - Text Layout & Typography Engine
// ============================================================================
#pragma once

#include "types.h"
#include "data_structures.h"

namespace stockcut {

constexpr u32 GLYPH_FLAG_NONE    = 0u;
constexpr u32 GLYPH_FLAG_NEWLINE = 1u;
constexpr u32 GLYPH_FLAG_SPACE   = 2u;
constexpr u32 GLYPH_FLAG_HIDDEN  = 4u;

struct alignas(32) LayoutGlyph {
    float x;
    float y;
    float w;
    float h;
    u32   unicode;
    u16   lineIndex;
    u16   charIndex;
    float advance;
    u32   flags;
};
static_assert(sizeof(LayoutGlyph) == 32, "LayoutGlyph must be exactly 32 bytes");

struct alignas(64) TextLayoutResult {
    float boundingWidth;
    float boundingHeight;
    u32   lineCount;
    u32   glyphCount;
    u32   wordCount;
    float firstBaseline;
    MultiArray<u32, 10> reserved{};
};
static_assert(sizeof(TextLayoutResult) == 64, "TextLayoutResult must be 64 bytes");

u32*              text_get_input_buf();
LayoutGlyph*      text_get_glyphs_ptr();
TextLayoutResult* text_get_result_ptr();

void text_layout(
    u32 charCount,
    float fontSize,
    float maxWidth,
    float letterSpacing,
    float lineHeight,
    u32 textAlign,
    u32 fontCategory,
    u32 textTransform
);

void text_register_glyph_metric(u32 fontCategory, u32 unicode, float advance);

u64 text_layout_hash(
    u32 charCount,
    float fontSize,
    float maxWidth,
    float letterSpacing,
    float lineHeight,
    u32 textAlign
);

} // namespace stockcut
