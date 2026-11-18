// ============================================================================
// CoreEngine - FreeType + HarfBuzz + WOFF2 Static Glyph Atlas Engine
// ============================================================================
#include "font_engine.h"
#include "font_map.h"
#include "core/hash_primitives.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <woff2/decode.h>
#include <woff2/output.h>
#include <cstring>

namespace stockcut {
namespace {

constexpr u32   MAX_CACHED_GLYPHS        = 8192u;
constexpr u32   MAX_NATIVE_RENDER_GLYPHS = 512u;
constexpr u32   MAX_NATIVE_RENDER_LINES  = 32u;
constexpr u32   RENDER_GLYPH_FLOATS      = 8u;
constexpr float FT_26_6_INV_SCALE        = 1.0f / 64.0f;

struct CachedGlyph {
    u32 fontKey;
    u32 glyphIndex;
    u32 pixelHeight;
    u32 x;
    u32 y;
    u32 width;
    u32 height;
    i32 bearingX;
    i32 bearingY;
    u32 occupied;
};

alignas(16) MultiArray<u8, MAX_NATIVE_FONT_BYTES>              g_fontInput{};
alignas(16) MultiArray<u8, MAX_NATIVE_FONT_BYTES>              g_activeFontData{};
alignas(16) MultiArray<u8, MAX_DECOMPRESSED_FONT_BYTES>        g_decompressedFont{};
alignas(16) MultiArray<u8, MAX_NATIVE_TEXT_UTF8_BYTES>         g_textInput{};
alignas(16) MultiArray<NativeTextGlyph, MAX_NATIVE_TEXT_GLYPHS> g_glyphs{};
alignas(16) MultiArray<u32, MAX_NATIVE_TEXT_UTF8_BYTES>        g_keyframeMap{};
u32 g_keyframeMapCount = 0;

alignas(16) MultiArray<float, MAX_NATIVE_RENDER_GLYPHS * RENDER_GLYPH_FLOATS> g_renderGlyphData{};
alignas(16) MultiArray<float, MAX_NATIVE_RENDER_LINES>                        g_renderLineWidths{};
u32 g_renderGlyphCount = 0;
u32 g_renderLineCount  = 0;

alignas(64) MultiArray<u8, NATIVE_GLYPH_ATLAS_SIZE * NATIVE_GLYPH_ATLAS_SIZE> g_atlas{};
alignas(16) MultiArray<CachedGlyph, MAX_CACHED_GLYPHS>                        g_cachedGlyphs{};
MultiArray<u32, 4> g_dirtyRect = {{INVALID_U32, INVALID_U32, 0u, 0u}};

FT_Library   g_library     = nullptr;
FT_Face      g_face        = nullptr;
hb_font_t*   g_hbFont      = nullptr;
hb_buffer_t* g_hbBuffer    = nullptr;
u32          g_fontKey     = 0;
u32          g_glyphCount  = 0;
u32          g_status      = FONT_STATUS_OK;
u32          g_shelfX      = 1;
u32          g_shelfY      = 1;
u32          g_shelfHeight = 0;

inline u32 glyphCacheIndex(u32 fontKey, u32 glyphIndex, u32 pixelHeight) noexcept {
    u32 hash = (fontKey * MURMUR3_32_C0) ^ (glyphIndex * MURMUR3_32_M1);
    hash ^= hash >> 16;
    return (hash ^ (pixelHeight * MURMUR3_32_M2)) & (MAX_CACHED_GLYPHS - 1u);
}

void includeDirtyRect(u32 x, u32 y, u32 width, u32 height) noexcept {
    if (width == 0 || height == 0) return;
    if (g_dirtyRect[0] == INVALID_U32) {
        g_dirtyRect[0] = x;
        g_dirtyRect[1] = y;
        g_dirtyRect[2] = x + width;
        g_dirtyRect[3] = y + height;
        return;
    }
    if (x < g_dirtyRect[0]) g_dirtyRect[0] = x;
    if (y < g_dirtyRect[1]) g_dirtyRect[1] = y;
    if (x + width > g_dirtyRect[2]) g_dirtyRect[2] = x + width;
    if (y + height > g_dirtyRect[3]) g_dirtyRect[3] = y + height;
}

template <bool ReserveIfEmpty>
CachedGlyph* probeGlyphSlot(u32 fontKey, u32 glyphIndex, u32 pixelHeight) noexcept {
    u32 index = glyphCacheIndex(fontKey, glyphIndex, pixelHeight);
    for (u32 probe = 0; probe < MAX_CACHED_GLYPHS; ++probe) {
        CachedGlyph& entry = g_cachedGlyphs[index];
        if (!entry.occupied) {
            if constexpr (ReserveIfEmpty) {
                entry.fontKey     = fontKey;
                entry.glyphIndex  = glyphIndex;
                entry.pixelHeight = pixelHeight;
                entry.occupied    = 1u;
                return &entry;
            } else {
                return nullptr;
            }
        }
        if (entry.fontKey == fontKey && entry.glyphIndex == glyphIndex && entry.pixelHeight == pixelHeight) {
            return &entry;
        }
        index = (index + 1u) & (MAX_CACHED_GLYPHS - 1u);
    }
    return nullptr;
}

bool copyGlyphToAtlas(u32 glyphIndex, CachedGlyph& entry) {
    if (FT_Load_Glyph(g_face, glyphIndex, FT_LOAD_DEFAULT) != 0 ||
        FT_Render_Glyph(g_face->glyph, FT_RENDER_MODE_NORMAL) != 0)
        return false;
    const FT_Bitmap& bitmap = g_face->glyph->bitmap;
    const u32 width  = bitmap.width;
    const u32 height = bitmap.rows;
    entry.bearingX = g_face->glyph->bitmap_left;
    entry.bearingY = g_face->glyph->bitmap_top;
    entry.width    = width;
    entry.height   = height;
    if (width == 0 || height == 0) {
        entry.x = 0;
        entry.y = 0;
        return true;
    }
    if (width + 2 >= NATIVE_GLYPH_ATLAS_SIZE || height + 2 >= NATIVE_GLYPH_ATLAS_SIZE) {
        return false;
    }
    if (g_shelfX + width + 1 >= NATIVE_GLYPH_ATLAS_SIZE) {
        g_shelfX = 1;
        g_shelfY += g_shelfHeight + 1;
        g_shelfHeight = 0;
    }
    if (g_shelfY + height + 1 >= NATIVE_GLYPH_ATLAS_SIZE) return false;
    const u32 x = g_shelfX;
    const u32 y = g_shelfY;
    const i32 pitch = bitmap.pitch;
    const usize absPitch = static_cast<usize>(pitch >= 0 ? pitch : -pitch);
    for (u32 row = 0; row < height; ++row) {
        const u32 sourceRow = pitch >= 0 ? row : (height - 1u - row);
        const u8* source = bitmap.buffer + static_cast<usize>(sourceRow) * absPitch;
        u8* destination = g_atlas.data() + static_cast<usize>(y + row) * NATIVE_GLYPH_ATLAS_SIZE + x;
        if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
            const u32 numGrays = bitmap.num_grays > 1 ? static_cast<u32>(bitmap.num_grays - 1) : 255u;
            for (u32 col = 0; col < width; ++col) {
                destination[col] = static_cast<u8>((static_cast<u32>(source[col]) * 255u) / numGrays);
            }
        } else if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
            for (u32 col = 0; col < width; ++col) {
                destination[col] = (source[col >> 3] & (0x80u >> (col & 7u))) ? 255u : 0u;
            }
        } else {
            return false;
        }
    }
    entry.x = x;
    entry.y = y;
    g_shelfX += width + 1;
    if (height > g_shelfHeight) g_shelfHeight = height;
    includeDirtyRect(x, y, width, height);
    return true;
}

bool ensureGlyphCached(u32 glyphIndex, u32 pixelHeight, CachedGlyph*& result) {
    result = probeGlyphSlot<false>(g_fontKey, glyphIndex, pixelHeight);
    if (result) return true;
    result = probeGlyphSlot<true>(g_fontKey, glyphIndex, pixelHeight);
    if (!result) {
        g_status = FONT_STATUS_ATLAS_FULL;
        return false;
    }
    if (!copyGlyphToAtlas(glyphIndex, *result)) {
        result->occupied = 0;
        g_status = (g_face->glyph->bitmap.width >= NATIVE_GLYPH_ATLAS_SIZE ||
                    g_face->glyph->bitmap.rows >= NATIVE_GLYPH_ATLAS_SIZE)
                       ? FONT_STATUS_ATLAS_FULL
                       : FONT_STATUS_GLYPH_FAILED;
        return false;
    }
    return true;
}

bool initializeLibrary() {
    if (g_library) return true;
    if (FT_Init_FreeType(&g_library) != 0) {
        g_status = FONT_STATUS_LIBRARY_INIT_FAILED;
        return false;
    }
    g_hbBuffer = hb_buffer_create();
    if (!g_hbBuffer || hb_buffer_allocation_successful(g_hbBuffer) == 0) {
        g_status = FONT_STATUS_LIBRARY_INIT_FAILED;
        return false;
    }
    return true;
}

} // namespace

u8* text_font_get_input_buffer() { return g_fontInput.data(); }
u32 text_font_get_input_capacity() { return MAX_NATIVE_FONT_BYTES; }
u8* text_font_get_utf8_buffer() { return g_textInput.data(); }
u32 text_font_get_utf8_capacity() { return MAX_NATIVE_TEXT_UTF8_BYTES; }
NativeTextGlyph* text_font_get_glyph_buffer() { return g_glyphs.data(); }
u32 text_font_get_glyph_capacity() { return MAX_NATIVE_TEXT_GLYPHS; }
u32* text_font_get_keyframe_map_buffer() { return g_keyframeMap.data(); }
u32 text_font_get_keyframe_map_capacity() { return MAX_NATIVE_TEXT_UTF8_BYTES; }

void text_font_set_keyframe_map_count(u32 byteCount) {
    g_keyframeMapCount = byteCount <= MAX_NATIVE_TEXT_UTF8_BYTES ? byteCount : 0;
}

void text_font_begin_glyph_batch(u32 lineCount) {
    g_renderGlyphCount = 0;
    g_renderLineCount  = min(lineCount, MAX_NATIVE_RENDER_LINES);
    g_renderLineWidths.clear();
}

u32 text_font_append_shaped_line(u32 lineIndex) {
    if (lineIndex >= g_renderLineCount) return 0;
    const u32 before = g_renderGlyphCount;
    for (u32 i = 0; i < g_glyphCount; ++i) {
        const NativeTextGlyph& glyph = g_glyphs[i];
        g_renderLineWidths[lineIndex] += glyph.advanceX;
        if (glyph.atlasWidth == 0 || glyph.atlasHeight == 0) continue;
        if (g_renderGlyphCount >= MAX_NATIVE_RENDER_GLYPHS) break;
        const u32 target = g_renderGlyphCount * RENDER_GLYPH_FLOATS;
        g_renderGlyphData[target + 0] = glyph.x;
        g_renderGlyphData[target + 1] = glyph.y;
        g_renderGlyphData[target + 2] = static_cast<float>(glyph.cluster);
        g_renderGlyphData[target + 3] = static_cast<float>(lineIndex);
        g_renderGlyphData[target + 4] = static_cast<float>(glyph.atlasX);
        g_renderGlyphData[target + 5] = static_cast<float>(glyph.atlasY);
        g_renderGlyphData[target + 6] = static_cast<float>(glyph.atlasWidth);
        g_renderGlyphData[target + 7] = static_cast<float>(glyph.atlasHeight);
        ++g_renderGlyphCount;
    }
    return g_renderGlyphCount - before;
}

float* text_font_get_render_glyph_data() { return g_renderGlyphData.data(); }
float* text_font_get_render_line_widths() { return g_renderLineWidths.data(); }
u32 text_font_get_render_glyph_count() { return g_renderGlyphCount; }
u8* text_font_get_atlas() { return g_atlas.data(); }
u32* text_font_get_atlas_dirty_rect() { return g_dirtyRect.data(); }

void text_font_reset_atlas_dirty_rect() {
    g_dirtyRect[0] = INVALID_U32;
    g_dirtyRect[1] = INVALID_U32;
    g_dirtyRect[2] = 0u;
    g_dirtyRect[3] = 0u;
}

u32 text_font_load_memory(u32 fontKey, const u8* data, u32 byteCount) {
    g_status = FONT_STATUS_OK;
    if (fontKey == 0 || !data || byteCount == 0 || byteCount > MAX_NATIVE_FONT_BYTES) {
        g_status = byteCount > MAX_NATIVE_FONT_BYTES ? FONT_STATUS_FONT_TOO_LARGE : FONT_STATUS_INVALID_INPUT;
        return 0;
    }
    if (!initializeLibrary()) return 0;
    if (g_fontKey == fontKey && g_face) return 1;
    if (g_hbFont) {
        hb_font_destroy(g_hbFont);
        g_hbFont = nullptr;
    }
    if (g_face) {
        FT_Done_Face(g_face);
        g_face = nullptr;
    }
    g_fontKey = 0;

    const u8* faceData = data;
    u32 faceByteCount  = byteCount;
    if (byteCount >= 4 && data[0] == 'w' && data[1] == 'O' && data[2] == 'F' && data[3] == '2') {
        const usize decompressedSize = woff2::ComputeWOFF2FinalSize(data, static_cast<usize>(byteCount));
        if (decompressedSize == 0) {
            g_status = FONT_STATUS_WOFF2_DECOMPRESSION_FAILED;
            return 0;
        }
        if (decompressedSize > MAX_DECOMPRESSED_FONT_BYTES) {
            g_status = FONT_STATUS_DECOMPRESSED_FONT_TOO_LARGE;
            return 0;
        }
        woff2::WOFF2MemoryOut output(g_decompressedFont.data(), MAX_DECOMPRESSED_FONT_BYTES);
        if (!woff2::ConvertWOFF2ToTTF(data, byteCount, &output) || output.Size() == 0) {
            g_status = decompressedSize > MAX_DECOMPRESSED_FONT_BYTES
                ? FONT_STATUS_DECOMPRESSED_FONT_TOO_LARGE
                : FONT_STATUS_WOFF2_DECOMPRESSION_FAILED;
            return 0;
        }
        faceData      = g_decompressedFont.data();
        faceByteCount = static_cast<u32>(output.Size());
    } else {
        if (data != g_activeFontData.data()) {
            std::memcpy(g_activeFontData.data(), data, byteCount);
        }
        faceData = g_activeFontData.data();
    }

    if (FT_New_Memory_Face(g_library, faceData, static_cast<FT_Long>(faceByteCount), 0, &g_face) != 0) {
        g_status = FONT_STATUS_INVALID_FONT;
        return 0;
    }
    g_hbFont = hb_ft_font_create_referenced(g_face);
    if (!g_hbFont) {
        FT_Done_Face(g_face);
        g_face   = nullptr;
        g_status = FONT_STATUS_LIBRARY_INIT_FAILED;
        return 0;
    }
    g_fontKey = fontKey;
    return 1;
}

u32 text_font_load(u32 fontKey, u32 byteCount) {
    return text_font_load_memory(fontKey, g_fontInput.data(), byteCount);
}

u32 text_font_load_by_file_id(u16 fileId) {
    const auto match = font::FontMapRegistry::instance().lookupByFileId(fileId);
    if (!match.buffer || match.byteCount == 0) return 0;
    return text_font_load_memory(match.fontKey, match.buffer, match.byteCount);
}

u32 text_font_shape(u32 utf8ByteCount, float pixelSize) {
    g_status     = FONT_STATUS_OK;
    g_glyphCount = 0;
    if (!g_face || !g_hbFont || !g_hbBuffer || utf8ByteCount > MAX_NATIVE_TEXT_UTF8_BYTES || !(pixelSize > 0.0f)) {
        g_status = utf8ByteCount > MAX_NATIVE_TEXT_UTF8_BYTES ? FONT_STATUS_TEXT_TOO_LARGE : FONT_STATUS_INVALID_INPUT;
        return 0;
    }
    const u32 pixelHeight = static_cast<u32>(pixelSize + 0.5f);
    if (pixelHeight == 0 || FT_Set_Pixel_Sizes(g_face, 0, pixelHeight) != 0) {
        g_status = FONT_STATUS_INVALID_INPUT;
        return 0;
    }
    hb_ft_font_changed(g_hbFont);
    hb_buffer_clear_contents(g_hbBuffer);
    hb_buffer_add_utf8(g_hbBuffer, reinterpret_cast<const char*>(g_textInput.data()), static_cast<int>(utf8ByteCount), 0, static_cast<int>(utf8ByteCount));
    hb_buffer_guess_segment_properties(g_hbBuffer);
    hb_shape(g_hbFont, g_hbBuffer, nullptr, 0);
    if (!hb_buffer_allocation_successful(g_hbBuffer)) {
        g_status = FONT_STATUS_SHAPING_FAILED;
        return 0;
    }
    unsigned int count = 0;
    const hb_glyph_info_t* infos         = hb_buffer_get_glyph_infos(g_hbBuffer, &count);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(g_hbBuffer, &count);
    if (count > MAX_NATIVE_TEXT_GLYPHS || (count && (!infos || !positions))) {
        g_status = FONT_STATUS_TEXT_TOO_LARGE;
        return 0;
    }
    float penX = 0.0f;
    float penY = 0.0f;
    for (u32 i = 0; i < count; ++i) {
        CachedGlyph* cached = nullptr;
        if (!ensureGlyphCached(infos[i].codepoint, pixelHeight, cached)) return 0;
        const float offsetX = static_cast<float>(positions[i].x_offset) * FT_26_6_INV_SCALE;
        const float offsetY = static_cast<float>(positions[i].y_offset) * FT_26_6_INV_SCALE;
        NativeTextGlyph& output = g_glyphs[i];
        output.glyphIndex  = infos[i].codepoint;
        output.cluster     = (g_keyframeMapCount == utf8ByteCount && infos[i].cluster < g_keyframeMapCount)
                                 ? g_keyframeMap[infos[i].cluster]
                                 : infos[i].cluster;
        output.x           = penX + offsetX + static_cast<float>(cached->bearingX);
        output.y           = -(penY + offsetY) - static_cast<float>(cached->bearingY);
        output.advanceX    = static_cast<float>(positions[i].x_advance) * FT_26_6_INV_SCALE;
        output.advanceY    = -static_cast<float>(positions[i].y_advance) * FT_26_6_INV_SCALE;
        output.atlasX      = cached->x;
        output.atlasY      = cached->y;
        output.atlasWidth  = cached->width;
        output.atlasHeight = cached->height;
        penX += output.advanceX;
        penY += output.advanceY;
    }
    g_glyphCount = count;
    return count;
}

u32 text_font_get_glyph_count() { return g_glyphCount; }
u32 text_font_get_status() { return g_status; }

float text_font_get_baseline_offset() {
    if (!g_face || !g_face->size) return 0.0f;
    return static_cast<float>(g_face->size->metrics.ascender) * FT_26_6_INV_SCALE;
}

WASM_EXPORT(wasm_text_font_get_input_buffer_ptr) u8* wasm_text_font_get_input_buffer_ptr() { return text_font_get_input_buffer(); }
WASM_EXPORT(wasm_text_font_get_input_capacity) u32 wasm_text_font_get_input_capacity() { return text_font_get_input_capacity(); }
WASM_EXPORT(wasm_text_font_get_utf8_buffer_ptr) u8* wasm_text_font_get_utf8_buffer_ptr() { return text_font_get_utf8_buffer(); }
WASM_EXPORT(wasm_text_font_get_utf8_capacity) u32 wasm_text_font_get_utf8_capacity() { return text_font_get_utf8_capacity(); }
WASM_EXPORT(wasm_text_font_get_glyph_buffer_ptr) NativeTextGlyph* wasm_text_font_get_glyph_buffer_ptr() { return text_font_get_glyph_buffer(); }
WASM_EXPORT(wasm_text_font_get_glyph_capacity) u32 wasm_text_font_get_glyph_capacity() { return text_font_get_glyph_capacity(); }
WASM_EXPORT(wasm_text_font_get_keyframe_map_ptr) u32* wasm_text_font_get_keyframe_map_ptr() { return text_font_get_keyframe_map_buffer(); }
WASM_EXPORT(wasm_text_font_get_keyframe_map_capacity) u32 wasm_text_font_get_keyframe_map_capacity() { return text_font_get_keyframe_map_capacity(); }
WASM_EXPORT(wasm_text_font_set_keyframe_map_count) void wasm_text_font_set_keyframe_map_count(u32 byteCount) { text_font_set_keyframe_map_count(byteCount); }
WASM_EXPORT(wasm_text_font_begin_glyph_batch) void wasm_text_font_begin_glyph_batch(u32 lineCount) { text_font_begin_glyph_batch(lineCount); }
WASM_EXPORT(wasm_text_font_append_shaped_line) u32 wasm_text_font_append_shaped_line(u32 lineIndex) { return text_font_append_shaped_line(lineIndex); }
WASM_EXPORT(wasm_text_font_get_render_glyph_data_ptr) float* wasm_text_font_get_render_glyph_data_ptr() { return text_font_get_render_glyph_data(); }
WASM_EXPORT(wasm_text_font_get_render_line_widths_ptr) float* wasm_text_font_get_render_line_widths_ptr() { return text_font_get_render_line_widths(); }
WASM_EXPORT(wasm_text_font_get_render_glyph_count) u32 wasm_text_font_get_render_glyph_count() { return text_font_get_render_glyph_count(); }
WASM_EXPORT(wasm_text_font_get_atlas_ptr) u8* wasm_text_font_get_atlas_ptr() { return text_font_get_atlas(); }
WASM_EXPORT(wasm_text_font_get_atlas_dirty_rect_ptr) u32* wasm_text_font_get_atlas_dirty_rect_ptr() { return text_font_get_atlas_dirty_rect(); }
WASM_EXPORT(wasm_text_font_reset_atlas_dirty_rect) void wasm_text_font_reset_atlas_dirty_rect() { text_font_reset_atlas_dirty_rect(); }
WASM_EXPORT(wasm_text_font_load) u32 wasm_text_font_load(u32 fontKey, u32 byteCount) { return text_font_load(fontKey, byteCount); }
WASM_EXPORT(wasm_text_font_shape) u32 wasm_text_font_shape(u32 utf8ByteCount, float pixelSize) { return text_font_shape(utf8ByteCount, pixelSize); }
WASM_EXPORT(wasm_text_font_get_glyph_count) u32 wasm_text_font_get_glyph_count() { return text_font_get_glyph_count(); }
WASM_EXPORT(wasm_text_font_get_status) u32 wasm_text_font_get_status() { return text_font_get_status(); }
WASM_EXPORT(wasm_text_font_get_baseline_offset) float wasm_text_font_get_baseline_offset() { return text_font_get_baseline_offset(); }

} // namespace stockcut
