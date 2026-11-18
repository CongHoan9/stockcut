// ============================================================================
// native/property/text.h
// PURE ANONYMOUS TEXT ONTOLOGY (CANH GIOI VO DANH TUYET DOI)
// ----------------------------------------------------------------------------
// Tuân thủ 100% BIT_AND_REGISTER_DICTIONARY.txt:
// 1. TextBlock = Block<24, 1> (112 bytes)
// 2. Không struct trung gian, không typed wrappers lãng phí.
// 3. Đọc/ghi trực tiếp qua register & bit addresses.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include "../core/types.h"
#include "block.h"
#include "media.h"

namespace stockcut {

// ----------------------------------------------------------------------------
// 1. CANONICAL REGISTER & BIT ADDRESSES CHO TEXTBLOCK = BLOCK<24, 1>
// ----------------------------------------------------------------------------
namespace text_reg {
    constexpr usize X0                  = 0;  // Spatial AABB x0
    constexpr usize Y0                  = 1;  // Spatial AABB y0
    constexpr usize X1                  = 2;  // Spatial AABB x1
    constexpr usize Y1                  = 3;  // Spatial AABB y1
    constexpr usize ROTATE_DEG          = 4;  // Rotation in degrees
    constexpr usize SCALE_X             = 5;  // Scale factor X
    constexpr usize OPACITY             = 6;  // Opacity 0.0 .. 1.0
    constexpr usize ANCHOR_X            = 7;  // Anchor point X (0.0=left, 0.5=center, 1.0=right)
    constexpr usize ANCHOR_Y            = 8;  // Anchor point Y (0.0=top, 0.5=center, 1.0=bottom)
    constexpr usize FONT_SIZE           = 9;  // Font size in pixels (CSC 1920x1080)
    constexpr usize LINE_HEIGHT         = 10; // Line height ratio (e.g. 1.25f)
    constexpr usize LETTER_SPACING      = 11; // Letter spacing in pixels
    constexpr usize STROKE_WIDTH        = 12; // Stroke width in pixels
    constexpr usize SHADOW_BLUR         = 13; // Shadow blur radius
    constexpr usize SHADOW_OFFSET_X     = 14; // Shadow offset X
    constexpr usize SHADOW_OFFSET_Y     = 15; // Shadow offset Y
    constexpr usize COLOR               = 16; // Primary text color (u32 0xAABBGGRR)
    constexpr usize STROKE_COLOR        = 17; // Stroke color (u32 0xAABBGGRR)
    constexpr usize SHADOW_COLOR        = 18; // Shadow / Exterior background color (u32 0xAABBGGRR)
    constexpr usize BG_COLOR            = 18; // Exterior background color for text (u32 0xAABBGGRR)
    constexpr usize FONT_HASH           = 19; // FNV-1a hash of font family string (u32)
    constexpr usize FONT_WEIGHT         = 20; // Font weight 100..900 (float / u32)
    constexpr usize REVEAL_DURATION_MS  = 21; // Karaoke reveal duration in ms (float)
    constexpr usize REVEAL_STAGGER_MS   = 22; // Karaoke reveal stagger in ms (float)
    constexpr usize RESOLVED_FONT_ID    = 23; // Pre-baked font file ID (u32)
}

namespace text_bit {
    constexpr usize VISIBLE             = 0;  // Visibility bit
    constexpr usize IS_ITALIC           = 1;  // Italic flag (0/1)
    constexpr usize DECORATION_UNDERLINE= 2;  // TextDecoration underline
    constexpr usize DECORATION_STRIKE   = 3;  // TextDecoration strikethrough
    constexpr usize DECORATION_SUPER    = 4;  // TextDecoration superscript
    constexpr usize INHERIT_COLOR       = 5;  // 1 = inherit from active Background
    constexpr usize TEXT_ALIGN_LO       = 6;  // 2 bits: 0=Left, 1=Center, 2=Right, 3=Justify
    constexpr usize TEXT_ALIGN_HI       = 7;
    constexpr usize TEXT_TRANSFORM_LO   = 8;  // 2 bits: 0=None, 1=Uppercase, 2=Lowercase, 3=Capitalize
    constexpr usize TEXT_TRANSFORM_HI   = 9;
    constexpr usize REVEAL_MODE_0       = 10; // 4 bits revealMode
    constexpr usize REVEAL_MODE_1       = 11;
    constexpr usize REVEAL_MODE_2       = 12;
    constexpr usize REVEAL_MODE_3       = 13;
    constexpr usize FONT_CATEGORY_LO    = 14; // 3 bits: 0=Sans, 1=Serif, 2=Mono, 3=Condensed, 4=Script
    constexpr usize FONT_CATEGORY_MD    = 15;
    constexpr usize FONT_CATEGORY_HI    = 16;
    constexpr usize LINE_COUNT_BASE     = 17; // 8 bits lineCount (17..24)
}

namespace TextDecoration {
    constexpr u8 None          = 0;
    constexpr u8 Underline     = 1u << 0;
    constexpr u8 Strikethrough = 1u << 1;
    constexpr u8 Superscript   = 1u << 2;
    constexpr u8 Mask          = Underline | Strikethrough | Superscript;
}

} // namespace stockcut
