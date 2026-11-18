// ============================================================================
// CoreEngine - Shader Pipeline & CSS Filter/Blend Implementation
// ============================================================================
#include "filter.h"
#include "math_utils.h"
#include "hash_primitives.h"
#include "data_structures.h"

namespace stockcut {
namespace {
constexpr float FILTER_ACTIVE_EPSILON = 0.01f;
constexpr float KAWASE_MIN_RADIUS     = 0.5f;
} // namespace

alignas(64) static MultiArray<FilterUniforms, MAX_FILTER_SLOTS> g_filters;
alignas(64) static MultiArray<KawasePassInfo, MAX_KAWASE_PASSES> g_kawasePasses;

FilterUniforms* filter_get_arena() {
    return g_filters.data();
}

KawasePassInfo* filter_get_kawase_passes_buf() {
    return g_kawasePasses.data();
}

FilterUniforms& filter_slot(u32 idx) noexcept {
    return g_filters[idx & (MAX_FILTER_SLOTS - 1u)];
}

void FilterUniforms::reset() noexcept {
    __builtin_memset(this, 0, sizeof(FilterUniforms));
    brightness = 1.0f;
    contrast   = 1.0f;
    saturate   = 1.0f;
    opacity    = 1.0f;
    blendMode  = BLEND_NORMAL;
    activeFilterBits = 0u;
}

void FilterUniforms::set_blur(float radius) noexcept {
    blurRadius = max(0.0f, radius);
    if (blurRadius > FILTER_ACTIVE_EPSILON) {
        activeFilterBits |= FILTER_BIT_BLUR;
    } else {
        activeFilterBits &= ~FILTER_BIT_BLUR;
    }
}

void FilterUniforms::set_shadow(float ox, float oy, float blur, float spread, u32 colorRgba) noexcept {
    shadowOffsetX = ox;
    shadowOffsetY = oy;
    shadowBlur    = max(0.0f, blur);
    shadowSpread  = spread;
    shadowColor   = colorRgba;
    if (blur > FILTER_ACTIVE_EPSILON || spread > FILTER_ACTIVE_EPSILON || ox != 0.0f || oy != 0.0f) {
        activeFilterBits |= FILTER_BIT_SHADOW;
    } else {
        activeFilterBits &= ~FILTER_BIT_SHADOW;
    }
}

void FilterUniforms::set_blend_mode(u32 mode) noexcept {
    blendMode = (mode > BLEND_EXCLUSION) ? BLEND_NORMAL : mode;
    if (blendMode != BLEND_NORMAL) {
        activeFilterBits |= FILTER_BIT_BLEND_MODE;
    } else {
        activeFilterBits &= ~FILTER_BIT_BLEND_MODE;
    }
}

void FilterUniforms::set_color_adjustments(
    float bright, float cont, float sat, float hueDeg, float inv, float sep
) noexcept {
    brightness   = bright;
    contrast     = cont;
    saturate     = sat;
    hueRotateDeg = hueDeg;
    invert       = inv;
    sepia        = sep;
    u32 bits = activeFilterBits & (FILTER_BIT_BLUR | FILTER_BIT_SHADOW | FILTER_BIT_BLEND_MODE);
    if (bright != 1.0f) bits |= FILTER_BIT_BRIGHTNESS;
    if (cont != 1.0f)   bits |= FILTER_BIT_CONTRAST;
    if (sat != 1.0f)    bits |= FILTER_BIT_SATURATE;
    if (hueDeg != 0.0f) bits |= FILTER_BIT_HUE_ROTATE;
    if (inv > 0.0f)     bits |= FILTER_BIT_INVERT;
    if (sep > 0.0f)     bits |= FILTER_BIT_SEPIA;
    activeFilterBits = bits;
}

void FilterUniforms::compute_color_matrix_64b(ColorMatrix64B& outMat) const noexcept {
    stockcut::compute_color_matrix_64b(
        brightness, contrast, saturate, hueRotateDeg, invert, sepia, opacity,
        outMat
    );
}

void FilterUniforms::compute_color_matrix(float* out16Floats) const noexcept {
    if (!out16Floats) return;
    ColorMatrix64B mat;
    compute_color_matrix_64b(mat);
    for (u32 i = 0; i < 9; ++i) out16Floats[i] = mat.M[i];
    out16Floats[9]  = mat.offset[0];
    out16Floats[10] = mat.offset[1];
    out16Floats[11] = mat.offset[2];
    out16Floats[12] = opacity;
    out16Floats[13] = 0.0f;
    out16Floats[14] = 0.0f;
    out16Floats[15] = 0.0f;
}

u64 FilterUniforms::compute_hash() const noexcept {
    return hash_struct_words64(*this);
}

void filter_reset(u32 idx) {
    filter_slot(idx).reset();
}

void filter_set_blur(u32 idx, float radius) {
    filter_slot(idx).set_blur(radius);
}

void filter_set_shadow(u32 idx, float ox, float oy, float blur, float spread, u32 colorRgba) {
    filter_slot(idx).set_shadow(ox, oy, blur, spread, colorRgba);
}

void filter_set_blend_mode(u32 idx, u32 blendMode) {
    filter_slot(idx).set_blend_mode(blendMode);
}

void filter_set_color_adjustments(
    u32 idx, float bright, float contrast, float sat, float hueDeg, float inv, float sep
) {
    filter_slot(idx).set_color_adjustments(bright, contrast, sat, hueDeg, inv, sep);
}

void filter_compute_color_matrix_64b(u32 idx, ColorMatrix64B& outMat) {
    filter_slot(idx).compute_color_matrix_64b(outMat);
}

void filter_compute_color_matrix(u32 idx, float* out16Floats) {
    filter_slot(idx).compute_color_matrix(out16Floats);
}

u32 filter_compute_kawase_passes_static(float blurRadius, u32 srcW, u32 srcH) {
    return filter_compute_kawase_passes(blurRadius, srcW, srcH, g_kawasePasses.data(), MAX_KAWASE_PASSES);
}

u32 filter_compute_kawase_passes(
    float blurRadius, u32 srcW, u32 srcH, KawasePassInfo* outPasses, u32 maxPasses
) {
    if (blurRadius <= KAWASE_MIN_RADIUS || !outPasses || maxPasses == 0u || srcW == 0u || srcH == 0u) {
        return 0u;
    }
    u32 iterations = 1u + static_cast<u32>(blurRadius > 4.0f)
                        + static_cast<u32>(blurRadius > 12.0f)
                        + static_cast<u32>(blurRadius > 28.0f);
    if (iterations * 2u > maxPasses) iterations = maxPasses >> 1;
    if (iterations == 0u) return 0u;

    const float offsetScale = blurRadius / (static_cast<float>(iterations) * 4.0f + 1.0f);
    u32 passIdx = 0u;
    u32 curW = srcW;
    u32 curH = srcH;
    for (u32 i = 0; i < iterations; ++i) {
        curW = (curW > 2u) ? (curW >> 1) : 1u;
        curH = (curH > 2u) ? (curH >> 1) : 1u;
        KawasePassInfo& p = outPasses[passIdx++];
        p.isDownsample = 1u;
        p.targetWidth  = curW;
        p.targetHeight = curH;
        p.offset       = (static_cast<float>(i) + 0.5f) * offsetScale;
    }
    for (i32 i = static_cast<i32>(iterations) - 1; i >= 0; --i) {
        if (i == 0) {
            curW = srcW;
            curH = srcH;
        } else {
            curW <<= 1;
            curH <<= 1;
        }
        KawasePassInfo& p = outPasses[passIdx++];
        p.isDownsample = 0u;
        p.targetWidth  = curW;
        p.targetHeight = curH;
        p.offset       = (static_cast<float>(i) + 0.5f) * offsetScale;
    }
    return passIdx;
}

u64 filter_hash(u32 idx) {
    return filter_slot(idx).compute_hash();
}
} // namespace stockcut
