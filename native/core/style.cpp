// ============================================================================
// CoreEngine - Packed Style Implementation
// ============================================================================
#include "style.h"
#include "math_utils.h"
#include "hash_primitives.h"
#include "data_structures.h"

namespace stockcut {
alignas(64) static MultiArray<PackedStyle, STYLE_ARENA_CAPACITY> g_styles;

PackedStyle* style_get_arena() {
    return g_styles.data();
}

PackedStyle& style_slot(u32 idx) noexcept {
    return g_styles[idx & (STYLE_ARENA_CAPACITY - 1u)];
}

void PackedStyle::reset() noexcept {
    __builtin_memset(this, 0, sizeof(PackedStyle));
    scaleX = 1.0f;
    scaleY = 1.0f;
    opacity = 1.0f;
    color = RGBA_OPAQUE_WHITE;
    filterBright = 1.0f;
    filterContrast = 1.0f;
    fontSize = DEFAULT_FONT_SIZE;
    lineHeight = DEFAULT_LINE_HEIGHT;
    activeFlags = STYLE_BIT_VISIBLE;
}

void PackedStyle::compute_affine_2d(float* out6Floats) const noexcept {
    if (!out6Floats) return;
    if ((activeFlags & STYLE_BIT_TRANSFORM) == 0u) {
        out6Floats[0] = 1.0f; out6Floats[1] = 0.0f;
        out6Floats[2] = 0.0f; out6Floats[3] = 1.0f;
        out6Floats[4] = x;    out6Floats[5] = y;
        return;
    }
    const float rad = rotateDeg * DEG_TO_RAD_F32;
    const float cosA = fast_cos(rad);
    const float sinA = fast_sin(rad);
    const float a = cosA * scaleX;
    const float b = sinA * scaleX;
    const float c = -sinA * scaleY;
    const float d = cosA * scaleY;
    const float ox = originX;
    const float oy = originY;
    const float tx = x + translateX;
    const float ty = y + translateY;
    out6Floats[0] = a;
    out6Floats[1] = b;
    out6Floats[2] = c;
    out6Floats[3] = d;
    out6Floats[4] = tx + ox - (a * ox + c * oy);
    out6Floats[5] = ty + oy - (b * ox + d * oy);
}

void PackedStyle::interpolate_from(const PackedStyle& f, const PackedStyle& to, float t) noexcept {
    if (t <= 0.0f) { *this = f; return; }
    if (t >= 1.0f) { *this = to; return; }
    x = lerp_f32(f.x, to.x, t);
    y = lerp_f32(f.y, to.y, t);
    w = lerp_f32(f.w, to.w, t);
    h = lerp_f32(f.h, to.h, t);
    translateX = lerp_f32(f.translateX, to.translateX, t);
    translateY = lerp_f32(f.translateY, to.translateY, t);
    scaleX     = lerp_f32(f.scaleX, to.scaleX, t);
    scaleY     = lerp_f32(f.scaleY, to.scaleY, t);
    rotateDeg  = lerp_f32(f.rotateDeg, to.rotateDeg, t);
    skewX      = lerp_f32(f.skewX, to.skewX, t);
    skewY      = lerp_f32(f.skewY, to.skewY, t);
    originX    = lerp_f32(f.originX, to.originX, t);
    originY    = lerp_f32(f.originY, to.originY, t);
    opacity    = lerp_f32(f.opacity, to.opacity, t);
    zIndex     = (t < 0.5f) ? f.zIndex : to.zIndex;
    activeFlags = f.activeFlags | to.activeFlags;
    color           = lerp_color_rgba(f.color, to.color, t);
    backgroundColor = lerp_color_rgba(f.backgroundColor, to.backgroundColor, t);
    borderColor     = lerp_color_rgba(f.borderColor, to.borderColor, t);
    borderWidth     = lerp_f32(f.borderWidth, to.borderWidth, t);
    radiusTL   = lerp_f32(f.radiusTL, to.radiusTL, t);
    radiusTR   = lerp_f32(f.radiusTR, to.radiusTR, t);
    radiusBR   = lerp_f32(f.radiusBR, to.radiusBR, t);
    radiusBL   = lerp_f32(f.radiusBL, to.radiusBL, t);
    fontSize      = lerp_f32(f.fontSize, to.fontSize, t);
    letterSpacing = lerp_f32(f.letterSpacing, to.letterSpacing, t);
    lineHeight    = lerp_f32(f.lineHeight, to.lineHeight, t);
    fontPacked    = (t < 0.5f) ? f.fontPacked : to.fontPacked;
    filterBlur     = lerp_f32(f.filterBlur, to.filterBlur, t);
    filterBright   = lerp_f32(f.filterBright, to.filterBright, t);
    filterContrast = lerp_f32(f.filterContrast, to.filterContrast, t);
    effectsPacked  = (t < 0.5f) ? f.effectsPacked : to.effectsPacked;
}

u64 PackedStyle::compute_hash() const noexcept {
    return hash_struct_words64(*this);
}

void style_reset(u32 idx) {
    style_slot(idx).reset();
}

void style_compute_affine_2d(u32 idx, float* out6Floats) {
    style_slot(idx).compute_affine_2d(out6Floats);
}

void style_interpolate(u32 fromIdx, u32 toIdx, float t, u32 outIdx) {
    style_slot(outIdx).interpolate_from(style_slot(fromIdx), style_slot(toIdx), t);
}

u64 style_hash(u32 idx) {
    return style_slot(idx).compute_hash();
}
} // namespace stockcut
