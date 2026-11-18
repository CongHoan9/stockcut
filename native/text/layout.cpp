// ============================================================================
// CoreEngine - Text Layout & Typography Implementation
// ============================================================================
#include "layout.h"
#include "math_utils.h"
#include "core/hash_primitives.h"
#include "core/string_parse.h"

namespace stockcut {

constexpr u32   MAX_TEXT_INPUT_CHARS      = 4096u;
constexpr u32   MAX_LAYOUT_GLYPHS         = 2048u;
constexpr u32   MAX_LINES                 = 256u;
constexpr u32   MAX_WORD_GLYPHS           = 128u;
constexpr u32   MAX_CUSTOM_METRICS        = 512u;
constexpr u32   CUSTOM_METRIC_PROBES      = 4u;
constexpr u32   LAYOUT_HASH_SAMPLE_CHARS  = 128u;
constexpr float FALLBACK_LAYOUT_LINE_MUL  = 1.2f;
constexpr float FIRST_BASELINE_FONT_RATIO = 0.8f;

alignas(64) static MultiArray<u32, MAX_TEXT_INPUT_CHARS>     g_textInputBuf{};
alignas(64) static MultiArray<LayoutGlyph, MAX_LAYOUT_GLYPHS> g_layoutGlyphs{};
alignas(64) static TextLayoutResult                          g_layoutResult{};

struct LineInfo {
    u32   startGlyph;
    u32   endGlyph;
    float width;
    u32   spaceCount;
};
alignas(64) static MultiArray<LineInfo, MAX_LINES> g_lines{};

struct CustomGlyphEntry {
    u32   fontCat;
    u32   unicode;
    float advance;
    u32   occupied;
};
static MultiArray<CustomGlyphEntry, MAX_CUSTOM_METRICS> g_customMetrics{};

template <FontCategory Category>
static float get_default_ascii_advance(u32 unicode) noexcept {
    if constexpr (Category == FontCategory::Mono) {
        return 0.60f;
    }
    float adv = 0.52f;
    if (unicode == ' ') {
        adv = 0.28f;
    } else if (unicode == '.' || unicode == ',' || unicode == ':' || unicode == ';' || unicode == '!' || unicode == '\'') {
        adv = 0.25f;
    } else if (unicode == 'i' || unicode == 'l' || unicode == 'I' || unicode == '|' || unicode == '1') {
        adv = 0.28f;
    } else if (unicode == 'j' || unicode == 't' || unicode == 'r' || unicode == 'f') {
        adv = 0.35f;
    } else if (unicode == 'm' || unicode == 'w') {
        adv = 0.78f;
    } else if (unicode == 'M' || unicode == 'W') {
        adv = 0.85f;
    } else if (unicode >= 'A' && unicode <= 'Z') {
        adv = 0.65f;
    } else if (unicode >= '0' && unicode <= '9') {
        adv = 0.55f;
    } else if (unicode >= 32u && unicode <= 126u) {
        adv = 0.50f;
    } else {
        adv = 0.55f;
    }
    if constexpr (Category == FontCategory::Condensed) {
        adv *= 0.72f;
    } else if constexpr (Category == FontCategory::Script) {
        adv *= 0.85f;
    }
    return adv;
}

template <FontCategory Category>
static float get_glyph_advance(u32 unicode) noexcept {
    constexpr u32 kCatU32 = static_cast<u32>(Category);
    const u32 hash = ((unicode * 31u) ^ (kCatU32 * 17u)) & (MAX_CUSTOM_METRICS - 1u);
    for (u32 i = 0; i < CUSTOM_METRIC_PROBES; ++i) {
        const u32 idx = (hash + i) & (MAX_CUSTOM_METRICS - 1u);
        if (g_customMetrics[idx].occupied &&
            g_customMetrics[idx].unicode == unicode &&
            g_customMetrics[idx].fontCat == kCatU32) {
            return g_customMetrics[idx].advance;
        }
    }
    return get_default_ascii_advance<Category>(unicode);
}

u32* text_get_input_buf() { return g_textInputBuf.data(); }
LayoutGlyph* text_get_glyphs_ptr() { return g_layoutGlyphs.data(); }
TextLayoutResult* text_get_result_ptr() { return &g_layoutResult; }

void text_register_glyph_metric(u32 fontCategory, u32 unicode, float advance) {
    const u32 hash = ((unicode * 31u) ^ (fontCategory * 17u)) & (MAX_CUSTOM_METRICS - 1u);
    for (u32 i = 0; i < CUSTOM_METRIC_PROBES; ++i) {
        const u32 idx = (hash + i) & (MAX_CUSTOM_METRICS - 1u);
        if (!g_customMetrics[idx].occupied ||
            (g_customMetrics[idx].unicode == unicode && g_customMetrics[idx].fontCat == fontCategory)) {
            g_customMetrics[idx].fontCat  = fontCategory;
            g_customMetrics[idx].unicode  = unicode;
            g_customMetrics[idx].advance  = advance;
            g_customMetrics[idx].occupied = 1u;
            return;
        }
    }
}

template <TextAlignMode Align, FontCategory Category, TextTransformMode Transform>
static void text_layout_T(
    u32 charCount,
    float fontSize,
    float maxWidth,
    float letterSpacing,
    float lineHeight
) {
    const u32 totalChars = min(charCount, MAX_TEXT_INPUT_CHARS);
    const float lineH    = (lineHeight > 0.0f) ? (lineHeight * fontSize) : (fontSize * FALLBACK_LAYOUT_LINE_MUL);
    const float maxW     = (maxWidth > 0.0f) ? maxWidth : TEXT_UNCONSTRAINED_W;

    u32 glyphCount     = 0u;
    u32 lineCount      = 0u;
    u32 wordCount      = 0u;
    float cursorX      = 0.0f;
    float cursorY      = 0.0f;
    float maxLineWidth = 0.0f;

    g_lines[0] = {0u, 0u, 0.0f, 0u};

    struct TempWordGlyph {
        u32   unicode;
        u16   charIndex;
        float adv;
        float w;
        float h;
    };
    MultiArray<TempWordGlyph, MAX_WORD_GLYPHS> wordBuf; // KHONG CO {} (guarded by wordGlyphCount)
    u32 wordGlyphCount = 0u;
    float wordWidth    = 0.0f;

    auto advance_line = [&]() noexcept {
        g_lines[lineCount].endGlyph = glyphCount;
        g_lines[lineCount].width = (cursorX > letterSpacing) ? (cursorX - letterSpacing) : 0.0f;
        if (g_lines[lineCount].width > maxLineWidth) maxLineWidth = g_lines[lineCount].width;
        ++lineCount;
        if (lineCount >= MAX_LINES) lineCount = MAX_LINES - 1u;
        cursorX = 0.0f;
        cursorY += lineH;
        g_lines[lineCount] = {glyphCount, glyphCount, 0.0f, 0u};
    };

    auto flush_word = [&]() noexcept {
        if (wordGlyphCount == 0u) return;
        if (cursorX > 0.0f && (cursorX + wordWidth) > maxW) {
            advance_line();
        }
        for (u32 w = 0; w < wordGlyphCount && glyphCount < MAX_LAYOUT_GLYPHS; ++w) {
            LayoutGlyph& g = g_layoutGlyphs[glyphCount++];
            g.x         = cursorX;
            g.y         = cursorY;
            g.w         = wordBuf[w].w;
            g.h         = wordBuf[w].h;
            g.unicode   = wordBuf[w].unicode;
            g.lineIndex = static_cast<u16>(lineCount);
            g.charIndex = wordBuf[w].charIndex;
            g.advance   = wordBuf[w].adv;
            g.flags     = GLYPH_FLAG_NONE;
            cursorX += wordBuf[w].adv + letterSpacing;
        }
        ++wordCount;
        wordGlyphCount = 0u;
        wordWidth      = 0.0f;
    };

    for (u32 i = 0; i < totalChars && glyphCount < MAX_LAYOUT_GLYPHS; ++i) {
        const u32 ch = transform_unicode_case<Transform>(g_textInputBuf[i]);
        if (ch == '\n') {
            flush_word();
            advance_line();
            continue;
        }
        if (ch == ' ' || ch == '\t') {
            flush_word();
            const float spaceAdv = get_glyph_advance<Category>(' ') * fontSize;
            if (glyphCount < MAX_LAYOUT_GLYPHS) {
                LayoutGlyph& g = g_layoutGlyphs[glyphCount++];
                g.x         = cursorX;
                g.y         = cursorY;
                g.w         = spaceAdv;
                g.h         = fontSize;
                g.unicode   = ' ';
                g.lineIndex = static_cast<u16>(lineCount);
                g.charIndex = static_cast<u16>(i);
                g.advance   = spaceAdv;
                g.flags     = GLYPH_FLAG_SPACE;
                ++g_lines[lineCount].spaceCount;
                cursorX += spaceAdv + letterSpacing;
            }
            continue;
        }
        const float adv = get_glyph_advance<Category>(ch) * fontSize;
        if (wordGlyphCount >= MAX_WORD_GLYPHS) {
            flush_word();
        }
        wordBuf[wordGlyphCount++] = {ch, static_cast<u16>(i), adv, adv, fontSize};
        wordWidth += adv + letterSpacing;
    }

    flush_word();
    g_lines[lineCount].endGlyph = glyphCount;
    g_lines[lineCount].width = (cursorX > letterSpacing) ? (cursorX - letterSpacing) : 0.0f;
    if (g_lines[lineCount].width > maxLineWidth) maxLineWidth = g_lines[lineCount].width;
    ++lineCount;

    const float effMaxWidth = (maxWidth > 0.0f && maxWidth < TEXT_UNCONSTRAINED_W) ? maxWidth : maxLineWidth;
    if constexpr (Align == TextAlignMode::Center || Align == TextAlignMode::Right) {
        for (u32 l = 0; l < lineCount; ++l) {
            const float curW = g_lines[l].width;
            const float diff = (effMaxWidth > curW) ? (effMaxWidth - curW) : 0.0f;
            const float xOffset = (Align == TextAlignMode::Center) ? (diff * 0.5f) : diff;
            if (xOffset > 0.0f) {
                for (u32 g = g_lines[l].startGlyph; g < g_lines[l].endGlyph; ++g) {
                    g_layoutGlyphs[g].x += xOffset;
                }
            }
        }
    } else if constexpr (Align == TextAlignMode::Justify) {
        for (u32 l = 0; l + 1u < lineCount; ++l) {
            if (g_lines[l].spaceCount == 0u) continue;
            const float curW = g_lines[l].width;
            const float excess = (maxWidth > curW) ? (maxWidth - curW) : 0.0f;
            const float extraPerSpace = excess / static_cast<float>(g_lines[l].spaceCount);
            float accumShift = 0.0f;
            for (u32 g = g_lines[l].startGlyph; g < g_lines[l].endGlyph; ++g) {
                g_layoutGlyphs[g].x += accumShift;
                if (g_layoutGlyphs[g].flags == GLYPH_FLAG_SPACE) {
                    accumShift += extraPerSpace;
                }
            }
        }
    }

    g_layoutResult.boundingWidth  = (maxWidth > 0.0f && maxWidth < TEXT_UNCONSTRAINED_W && Align != TextAlignMode::Left)
                                        ? maxWidth
                                        : maxLineWidth;
    g_layoutResult.boundingHeight = static_cast<float>(lineCount) * lineH;
    g_layoutResult.lineCount      = lineCount;
    g_layoutResult.glyphCount     = glyphCount;
    g_layoutResult.wordCount      = wordCount;
    g_layoutResult.firstBaseline  = fontSize * FIRST_BASELINE_FONT_RATIO;
}

using LayoutFn = void (*)(u32, float, float, float, float);

template <TextAlignMode Align, FontCategory Category>
static constexpr MultiArray<LayoutFn, 3> make_transform_row() {
    return {{
        &text_layout_T<Align, Category, TextTransformMode::None>,
        &text_layout_T<Align, Category, TextTransformMode::Uppercase>,
        &text_layout_T<Align, Category, TextTransformMode::Lowercase>
    }};
}

template <TextAlignMode Align>
static constexpr MultiArray<MultiArray<LayoutFn, 3>, 5> make_category_table() {
    return {{
        make_transform_row<Align, FontCategory::Sans>(),
        make_transform_row<Align, FontCategory::Serif>(),
        make_transform_row<Align, FontCategory::Mono>(),
        make_transform_row<Align, FontCategory::Condensed>(),
        make_transform_row<Align, FontCategory::Script>()
    }};
}

static constexpr MultiArray<MultiArray<MultiArray<LayoutFn, 3>, 5>, 4> LAYOUT_LUT = {{
    make_category_table<TextAlignMode::Left>(),
    make_category_table<TextAlignMode::Center>(),
    make_category_table<TextAlignMode::Right>(),
    make_category_table<TextAlignMode::Justify>()
}};

void text_layout(
    u32 charCount,
    float fontSize,
    float maxWidth,
    float letterSpacing,
    float lineHeight,
    u32 textAlign,
    u32 fontCategory,
    u32 textTransform
) {
    if (charCount == 0u || fontSize <= 0.0f) {
        g_layoutResult.boundingWidth  = 0.0f;
        g_layoutResult.boundingHeight = 0.0f;
        g_layoutResult.lineCount      = 0u;
        g_layoutResult.glyphCount     = 0u;
        g_layoutResult.wordCount      = 0u;
        g_layoutResult.firstBaseline  = 0.0f;
        return;
    }
    const u32 a = (textAlign < 4u) ? textAlign : 0u;
    const u32 c = (fontCategory < 5u) ? fontCategory : 0u;
    const u32 t = (textTransform < 3u) ? textTransform : 0u;
    LAYOUT_LUT[a][c][t](charCount, fontSize, maxWidth, letterSpacing, lineHeight);
}

u64 text_layout_hash(
    u32 charCount,
    float fontSize,
    float maxWidth,
    float letterSpacing,
    float lineHeight,
    u32 textAlign
) {
    u64 h = HASH_GOLDEN_64;
    h ^= static_cast<u64>(charCount) * SPLITMIX_MUL1;
    h ^= static_cast<u64>(fontSize * 100.0f) * SPLITMIX_MUL2;
    h ^= static_cast<u64>(maxWidth * 10.0f) * SPLITMIX_MUL1;
    h ^= static_cast<u64>(letterSpacing * 100.0f) * SPLITMIX_MUL2;
    h ^= static_cast<u64>(lineHeight * 100.0f) * SPLITMIX_MUL1;
    h ^= static_cast<u64>(textAlign) * HASH_GOLDEN_64;
    const u32 n = min(charCount, LAYOUT_HASH_SAMPLE_CHARS);
    for (u32 i = 0; i < n; ++i) {
        h = (h ^ g_textInputBuf[i]) * HASH_GOLDEN_64;
    }
    return h ^ (h >> 32);
}

} // namespace stockcut
