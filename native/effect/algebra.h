// ============================================================================
// native/effect/algebra.h
// ANONYMOUS 64-BIT EFFECT ALGEBRA, 4-INTERFACE ABI & 7-DOMAIN PULLBACK CULLING
// ----------------------------------------------------------------------------
// Implements KIEN_TRUC_VIDEO_OS_KERNEL_VA_HIEU_UNG_VO_DANH_MASTER.md:
// - ZERO named-effect enums (No EffectType, No EffectOpCode).
// - Every effect is described solely by a 64-bit bitmask `algebraWord` +
//   raw register/bit storage in Block<FloatCount, BitBlocks>.
// - 4 Pure Shader ABI Interfaces: ABI_UV | ABI_SAMPLE | ABI_COLOR | ABI_GEOM.
// - 7 Spatial-Temporal Domains: D0..D6 driving 64-bit PullbackTileBitboard.
// - 5 CPU Algebraic Pre-Fold Traits: Affine2D, AffineColor, GaussianConv, Gate, Commutative.
// - 8 Shared Auxiliary Blackboard dependencies.
// ============================================================================
#pragma once

#include "../core/types.h"
#include "../core/math_utils.h"
#include "../spatial/dirty_rect.h"
#include "../timeline/bitboard.h"
#include "catalog.h"
#include "engine.h"
#include <cmath>

namespace stockcut {
namespace effect_algebra {

// ============================================================================
// 1. 64-BIT ANONYMOUS ALGEBRA WORD BIT LAYOUT (IDENTICAL TO kCatalogBlob)
// ============================================================================
constexpr u64 ABI_UV     = 1ULL << 0;  // vec2 fn(vec2 uv, int off, vec4 meta)
constexpr u64 ABI_SAMPLE = 1ULL << 1;  // vec4 fn(sampler2D s, vec2 uv, int off, vec4 meta)
constexpr u64 ABI_COLOR  = 1ULL << 2;  // vec4 fn(vec4 col, vec2 uv, int off, vec4 meta)
constexpr u64 ABI_GEOM   = 1ULL << 3;  // Dedicated multi-pass / geometry / pyramid stage
constexpr u64 ABI_MASK   = 0xFULL;

constexpr u64 DOM_D0_FULL       = 1ULL << 4;  // Full-frame support (all 64 tiles)
constexpr u64 DOM_D1_BOUNDED    = 1ULL << 5;  // Compact geometric support (disk/strip/border/rect)
constexpr u64 DOM_D2_HALO       = 1ULL << 6;  // Neighborhood stencil / halo dilation (Chebyshev 1-ring)
constexpr u64 DOM_D3_SPARSE     = 1ULL << 7;  // Data-dependent sparse (e.g. bright tiles > threshold)
constexpr u64 DOM_D4_MULTISCALE = 1ULL << 8;  // Mip pyramid / separable multi-scale
constexpr u64 DOM_D5_TEMPORAL   = 1ULL << 9;  // Temporal history / time-variant kernel
constexpr u64 DOM_D6_AUDIO_SYNC = 1ULL << 10; // Audio FFT / bass-reactive envelope
constexpr u64 DOM_MASK          = 0x7FULL << 4;

constexpr u64 TRAIT_AFFINE_2D     = 1ULL << 11; // Folds into Transform2D on CPU
constexpr u64 TRAIT_AFFINE_COLOR  = 1ULL << 12; // Folds into ColorTransferOp on CPU
constexpr u64 TRAIT_GAUSSIAN_CONV = 1ULL << 13; // Folds sigma^2 = sum(sigma_i^2) on CPU
constexpr u64 TRAIT_IDENTITY_GATE = 1ULL << 14; // 0-cost bypass when gate <= eps
constexpr u64 TRAIT_COMMUTATIVE   = 1ULL << 15; // Reorderable within same epoch stage
constexpr u64 TRAIT_MASK          = 0x1FULL << 11;

constexpr u64 AUX_NOISE_ATLAS  = 1ULL << 16; // Shared 256x256 AnalyticalNoiseAtlas
constexpr u64 AUX_MIP_PYRAMID  = 1ULL << 17; // Shared scene mip pyramid
constexpr u64 AUX_TILE_LUMA    = 1ULL << 18; // Shared TileSummary64 bright mask
constexpr u64 AUX_EDGE_SOBEL   = 1ULL << 19; // Shared Sobel edge buffer
constexpr u64 AUX_ALPHA_SDF    = 1ULL << 20; // Shared alpha SDF
constexpr u64 AUX_AUDIO_FFT    = 1ULL << 21; // Shared audio FFT spectrum
constexpr u64 AUX_DEPTH_MAP    = 1ULL << 22; // Shared depth buffer
constexpr u64 AUX_HISTORY_RING = 1ULL << 23; // Shared temporal frame ring
constexpr u64 AUX_MASK         = 0xFFULL << 16;

// Spatial shape class values packed in bits [48..55]
constexpr u8 SHAPE_FULL         = 0u;
constexpr u8 SHAPE_RADIAL_DISK  = 1u; // Twirl, Pinch, Lens (radius in reg)
constexpr u8 SHAPE_SCAN_STRIP   = 2u; // Glitch (scan strip + jitter block)
constexpr u8 SHAPE_BORDER_RING  = 3u; // Fire Border / Vignette perimeter
constexpr u8 SHAPE_INTRO_BAR    = 4u; // Intro / Outro transition bar & ring
constexpr u8 SHAPE_OUTRO_BAR    = 5u; // Outro transition bar & ring

[[nodiscard]] constexpr u64 pack_algebra_word(
    u64 abi,
    u64 domain,
    u64 traits,
    u64 aux,
    u8 boundedRegAddr = 0xFFu,
    u8 gateRegAddr = 0xFFu,
    u8 aluWeight = 16u,
    u8 shapeClass = SHAPE_FULL,
    u8 kernelOrdinal = 0u
) noexcept {
    return (abi & ABI_MASK) |
           (domain & DOM_MASK) |
           (traits & TRAIT_MASK) |
           (aux & AUX_MASK) |
           (static_cast<u64>(boundedRegAddr) << 24) |
           (static_cast<u64>(gateRegAddr) << 32) |
           (static_cast<u64>(aluWeight) << 40) |
           (static_cast<u64>(shapeClass) << 48) |
           (static_cast<u64>(kernelOrdinal) << 56);
}

[[nodiscard]] constexpr u64 abi_bits(u64 w) noexcept       { return w & ABI_MASK; }
[[nodiscard]] constexpr u64 domain_bits(u64 w) noexcept    { return w & DOM_MASK; }
[[nodiscard]] constexpr u64 trait_bits(u64 w) noexcept     { return w & TRAIT_MASK; }
[[nodiscard]] constexpr u64 aux_bits(u64 w) noexcept       { return w & AUX_MASK; }
[[nodiscard]] constexpr u8  bounded_reg(u64 w) noexcept    { return static_cast<u8>((w >> 24) & 0xFFu); }
[[nodiscard]] constexpr u8  gate_reg(u64 w) noexcept       { return static_cast<u8>((w >> 32) & 0xFFu); }
[[nodiscard]] constexpr u8  alu_weight(u64 w) noexcept     { return static_cast<u8>((w >> 40) & 0xFFu); }
[[nodiscard]] constexpr u8  shape_class(u64 w) noexcept    { return static_cast<u8>((w >> 48) & 0xFFu); }
[[nodiscard]] constexpr u8  kernel_ordinal(u64 w) noexcept { return static_cast<u8>((w >> 56) & 0xFFu); }

[[nodiscard]] constexpr bool is_pure_uv(u64 w) noexcept     { return (w & ABI_MASK) == ABI_UV; }
[[nodiscard]] constexpr bool has_sample(u64 w) noexcept     { return (w & ABI_SAMPLE) != 0ULL; }
[[nodiscard]] constexpr bool is_pure_color(u64 w) noexcept  { return (w & ABI_MASK) == ABI_COLOR; }
[[nodiscard]] constexpr bool is_geom_barrier(u64 w) noexcept{ return (w & ABI_GEOM) != 0ULL; }
[[nodiscard]] constexpr bool is_cpu_foldable(u64 w) noexcept{
    return (w & (TRAIT_AFFINE_2D | TRAIT_AFFINE_COLOR | TRAIT_GAUSSIAN_CONV)) != 0ULL;
}
[[nodiscard]] constexpr bool is_bounded_domain(u64 w) noexcept {
    return ((w & DOM_D0_FULL) == 0ULL) && ((w & (DOM_D1_BOUNDED | DOM_D3_SPARSE)) != 0ULL);
}

// ============================================================================
// 2. SHARED 8x8 MACRO-TILE SUMMARY (64 BYTES = 1 L1 CACHELINE)
// ============================================================================
struct alignas(64) TileSummary64 {
    u64 brightTileMask     = ~0ULL; // D3: Tiles with max luminance > threshold
    u64 opaqueTileMask     = ~0ULL; // Tiles with non-zero alpha content
    u64 dirtyTileMask      = ~0ULL; // Tiles dirty from upstream scene mutation
    u64 edgeTileMask       = ~0ULL; // Tiles with Sobel gradient > threshold
    u64 activeOutputMask   = ~0ULL; // Requested output viewport tile mask
    float brightThreshold  = 0.70f;
    float audioBassLevel   = 0.0f;
    u32 frameEpoch         = 0u;
    u32 flags              = 0u;
    u64 reserved           = 0ULL;

    [[nodiscard]] static constexpr TileSummary64 full_active() noexcept {
        return TileSummary64{};
    }
};
static_assert(sizeof(TileSummary64) == 64, "TileSummary64 must be exactly 64 bytes");

// ============================================================================
// 3. 8x8 BITBOARD CHEBYSHEV 1-RING HALO DILATION (8 BITWISE INSTRUCTIONS, 0 LOOPS)
// ============================================================================
[[nodiscard]] [[gnu::always_inline]] inline constexpr u64 dilate_tiles_8x8(u64 bb) noexcept {
    constexpr u64 kNotCol0 = 0xFEFEFEFEFEFEFEFEULL;
    constexpr u64 kNotCol7 = 0x7F7F7F7F7F7F7F7FULL;
    const u64 horiz = bb | ((bb & kNotCol0) >> 1) | ((bb & kNotCol7) << 1);
    return horiz | (horiz >> 8) | (horiz << 8);
}

// Convert normalized UV rect [u0,v0,u1,v1] in [0,1] into 8x8 macro-tile bitboard
// ZERO-LOOP, ZERO-BRANCH O(1) 1-IMUL 2D BITBOARD BROADCAST!
[[nodiscard]] [[gnu::always_inline]] inline u64 uv_rect_to_tile_mask(
    float u0, float v0, float u1, float v1
) noexcept {
    const float cu0 = clamp(u0, 0.0f, 1.0f);
    const float cv0 = clamp(v0, 0.0f, 1.0f);
    const float cu1 = clamp(u1, 0.0f, 1.0f);
    const float cv1 = clamp(v1, 0.0f, 1.0f);
    const u64 validMask = branchless_mask<u64>((cu1 > cu0) && (cv1 > cv0));
    const u32 tx0 = min(7u, static_cast<u32>(cu0 * 8.0f));
    const u32 ty0 = min(7u, static_cast<u32>(cv0 * 8.0f));
    const u32 tx1 = max(tx0, min(7u, static_cast<u32>(max(0.0f, cu1 * 8.0f - 1e-5f))));
    const u32 ty1 = max(ty0, min(7u, static_cast<u32>(max(0.0f, cv1 * 8.0f - 1e-5f))));
    const u64 rowBits = ((1ULL << (tx1 - tx0 + 1u)) - 1ULL) << tx0;
    const u64 colStep = (0x0101010101010101ULL >> ((7u - (ty1 - ty0)) << 3)) << (ty0 << 3);
    return (rowBits * colStep) & validMask;
}

// Convert 8x8 tile bitboard into tight pixel scissor Rect<float>
// ZERO-LOOP O(1) BITBOARD BOUNDING BOX (2 TZCNT + 2 LZCNT + 3 SHIFT-OR)
[[nodiscard]] [[gnu::always_inline]] inline Rect<float> tile_mask_to_pixel_rect(u64 tileMask, float W, float H) noexcept {
    if (tileMask == 0ULL) return Rect<float>{0.0f, 0.0f, 0.0f, 0.0f};
    const u32 minY = BitOps<u64>::lsb(tileMask) >> 3;
    const u32 maxY = BitOps<u64>::msb(tileMask) >> 3;
    u64 c = tileMask | (tileMask >> 32);
    c |= (c >> 16);
    c |= (c >> 8);
    const u32 cols = static_cast<u32>(c & 0xFFu);
    const u32 minX = BitOps<u32>::lsb(cols);
    const u32 maxX = BitOps<u32>::msb(cols);
    const float tw = W * 0.125f;
    const float th = H * 0.125f;
    return Rect<float>{
        static_cast<float>(minX) * tw,
        static_cast<float>(minY) * th,
        static_cast<float>(maxX + 1u) * tw,
        static_cast<float>(maxY + 1u) * th
    };
}

// ============================================================================
// 4. 7-DOMAIN PULLBACK TILE BITBOARD COMPUTATION (O(1), ZERO ALLOCATION)
// ============================================================================
[[nodiscard]] inline u64 compute_pullback_tile_mask(
    u64 algebraWord,
    float param0,
    float param1,
    float param2,
    float W,
    float H,
    float timeSec,
    const TileSummary64& summary
) noexcept {
    u64 mask = ~0ULL;

    // D1: Bounded geometric support
    if ((algebraWord & DOM_D1_BOUNDED) != 0ULL && (algebraWord & DOM_D0_FULL) == 0ULL) {
        const u8 sc = shape_class(algebraWord);
        if (sc == SHAPE_RADIAL_DISK) {
            const float rad = clamp(param2 > 0.001f ? param2 : 0.45f, 0.01f, 1.5f);
            const float aspect = (H > 0.0f) ? (W / H) : (16.0f / 9.0f);
            const float rx = rad / max(aspect, 0.1f);
            const float ry = rad;
            mask = uv_rect_to_tile_mask(0.5f - rx, 0.5f - ry, 0.5f + rx, 0.5f + ry);
        } else if (sc == SHAPE_SCAN_STRIP) {
            // Exact Glitch scanline strip + deterministic copy-paste block
            const float spd = (param1 > 0.0f) ? param1 : 2.0f;
            const float effMs = timeSec * 1000.0f * spd;
            const float uScale = W / DEFAULT_STAGE_WIDTH_F32;
            const float scanY = 1.0f - std::fmod(effMs, 2000.0f) / 2000.0f;
            const float stripH = (25.0f * uScale) / max(H, 1.0f);
            mask = uv_rect_to_tile_mask(0.0f, scanY - stripH * 0.5f, 1.0f, scanY + stripH * 0.5f);
            // Add jitter block tile support if active
            const float step = std::floor(effMs / 200.0f);
            auto prng = [](float s) noexcept -> float {
                const float x = std::sin(s) * 10000.0f;
                return x - std::floor(x);
            };
            const bool rest = (prng(std::floor(effMs / 2000.0f)) < 0.3f) ||
                              (prng(step * 1.5f) > 0.5f);
            if (!rest) {
                float s = step;
                auto r = [&s, &prng]() noexcept -> float { const float v = prng(s); s += 1.0f; return v; };
                const float primary = 0.2f + r() * 0.05f;
                const float maxSec = 0.05f - (primary - 0.1f) * 0.05f;
                const float secondary = 0.05f + r() * (maxSec - 0.05f);
                const bool isH = r() > 0.5f;
                const float fw = isH ? primary : secondary;
                const float fh = isH ? secondary : primary;
                (void)r(); (void)r(); // srcX, srcY
                const float dstX = r() * (1.0f - fw);
                const float dstY = r() * (1.0f - fh);
                mask |= uv_rect_to_tile_mask(dstX, 1.0f - dstY - fh, dstX + fw, 1.0f - dstY);
            }
        } else if (sc == SHAPE_BORDER_RING) {
            // Perimeter ring: outer 28 tiles out of 64 (56.25% center tiles skipped!)
            mask = 0xFF818181818181FFULL;
        } else if (sc == SHAPE_INTRO_BAR || sc == SHAPE_OUTRO_BAR) {
            IntroOutroGLState st{};
            const float dur = (param1 > 0.001f) ? param1 : 1.0f;
            const float u = clamp(timeSec / dur, 0.0f, 1.0f);
            if (sc == SHAPE_OUTRO_BAR) {
                effect_compute_intro_outro_state<true>(u, &st);
            } else {
                effect_compute_intro_outro_state<false>(u, &st);
            }
            if (st.bar2[3] > 0.5f && st.ring1[0] < 50.0f && abs_val(st.warpX[0] - 1.0f) < 0.01f) {
                mask = uv_rect_to_tile_mask(0.0f, st.bar1[0] - 0.15f, 1.0f, st.bar1[1] + 0.15f);
            }
        } else {
            // Generic bounded region using param0 as normalized half-extent
            const float ext = clamp(param0 > 0.01f ? param0 : 0.5f, 0.05f, 1.0f);
            mask = uv_rect_to_tile_mask(0.5f - ext * 0.5f, 0.5f - ext * 0.5f,
                                        0.5f + ext * 0.5f, 0.5f + ext * 0.5f);
        }
    }

    // D3: Data-dependent sparse culling (Bloom / Glow / Highlight extract)
    if ((algebraWord & DOM_D3_SPARSE) != 0ULL) {
        mask &= summary.brightTileMask;
    }

    // D2: Halo / neighborhood stencil dilation (Chebyshev 1-ring expansion)
    if ((algebraWord & DOM_D2_HALO) != 0ULL && mask != 0ULL && mask != ~0ULL) {
        mask = dilate_tiles_8x8(mask);
    }

    return mask & summary.activeOutputMask;
}

// ============================================================================
// 5. ANONYMOUS IDENTITY GATE & RoI EVALUATOR (REPLACES LEGACY EffectType)
// ============================================================================
[[nodiscard]] inline bool effect_algebra_is_identity(
    u64 algebraWord,
    double nowMs, double startMs, double endMs,
    float param0, float param1
) noexcept {
    (void)param1;
    if (nowMs < startMs) return true;
    const u8 sc = shape_class(algebraWord);
    if (sc == SHAPE_INTRO_BAR) {
        return nowMs >= endMs;
    }
    if (sc == SHAPE_OUTRO_BAR) {
        return false;
    }
    if ((algebraWord & TRAIT_AFFINE_2D) != 0ULL) {
        return param0 <= IDENTITY_EPSILON;
    }
    if ((algebraWord & TRAIT_AFFINE_COLOR) != 0ULL) {
        return abs_val(param0 - 1.0f) <= OPACITY_EPSILON;
    }
    if ((algebraWord & TRAIT_IDENTITY_GATE) != 0ULL) {
        return abs_val(param0) <= IDENTITY_EPSILON;
    }
    return false;
}

inline void effect_algebra_compute_roi(
    u64 algebraWord,
    double nowMs, double startMs, double endMs,
    float W, float H,
    float inX0, float inY0, float inX1, float inY1,
    Rect<float>& outRoI
) noexcept {
    if (effect_algebra_is_identity(algebraWord, nowMs, startMs, endMs, 1.0f, 0.0f)) {
        outRoI = {inX0, inY0, inX1, inY1};
        return;
    }
    const u8 sc = shape_class(algebraWord);
    if (sc == SHAPE_INTRO_BAR || sc == SHAPE_OUTRO_BAR) {
        const float dur = static_cast<float>(endMs - startMs);
        const float u = dur > OPACITY_EPSILON ? static_cast<float>((nowMs - startMs) / dur) : 0.0f;
        IntroOutroGLState st{};
        if (sc == SHAPE_OUTRO_BAR) {
            effect_compute_intro_outro_state<true>(u, &st);
        } else {
            effect_compute_intro_outro_state<false>(u, &st);
        }
        if (st.bar2[3] > 0.5f && st.ring1[0] < 50.0f && abs_val(st.warpX[0] - 1.0f) < 0.01f) {
            const float y0 = max(0.0f, (st.bar1[0] - 0.15f) * H);
            const float y1 = min(H, (st.bar1[1] + 0.15f) * H);
            outRoI = {0.0f, y0, W, y1};
            return;
        }
        outRoI = {0.0f, 0.0f, W, H};
        return;
    }
    if (is_bounded_domain(algebraWord)) {
        const float timeSec = static_cast<float>(max(0.0, nowMs - startMs) * 0.001);
        const u64 tMask = compute_pullback_tile_mask(
            algebraWord, 0.5f, 2.0f, 0.45f, W, H, timeSec, TileSummary64::full_active()
        );
        Rect<float> bounded = tile_mask_to_pixel_rect(tMask, W, H);
        bounded.intersectWith(Rect<float>{inX0, inY0, inX1, inY1});
        outRoI = bounded;
        return;
    }
    outRoI = {inX0, inY0, inX1, inY1};
}

// ============================================================================
// 6. COMPILE-TIME ANONYMOUS NATIVE KERNEL ORDINALS (OPAQUE INDICES, NO ENUM!)
// Mapping from FNV-1a hash -> 64-bit algebraWord (including builtinKernelIndex)
// ============================================================================
constexpr u8 KERNEL_ORD_NONE        = 0u;
constexpr u8 KERNEL_ORD_AFFINE_2D   = 1u;  // Fused Transform2D UV warp
constexpr u8 KERNEL_ORD_AFFINE_COL  = 2u;  // Fused ColorTransferOp gain+emit
constexpr u8 KERNEL_ORD_TWIRL       = 3u;  // ABI_UV | D1 RadialDisk
constexpr u8 KERNEL_ORD_PINCH       = 4u;  // ABI_UV | D1 RadialDisk
constexpr u8 KERNEL_ORD_LENS        = 5u;  // ABI_UV | D1 RadialDisk
constexpr u8 KERNEL_ORD_GLITCH      = 6u;  // ABI_UV | D1 ScanStrip+Block
constexpr u8 KERNEL_ORD_TURBULENT   = 7u;  // ABI_SAMPLE | D0+D5 | AUX_NOISE_ATLAS
constexpr u8 KERNEL_ORD_CHROMATIC   = 8u;  // ABI_SAMPLE | D2 Halo
constexpr u8 KERNEL_ORD_ZOOM_BLUR   = 9u;  // ABI_SAMPLE | D2 Halo
constexpr u8 KERNEL_ORD_MOTION_BLUR = 10u; // ABI_SAMPLE | D2 Halo
constexpr u8 KERNEL_ORD_GLITCH_RGB  = 11u; // ABI_SAMPLE | D2+D5
constexpr u8 KERNEL_ORD_CYBER_SCAN  = 12u; // ABI_SAMPLE | D0+D5
constexpr u8 KERNEL_ORD_FXAA        = 13u; // ABI_SAMPLE | D2 Edge
constexpr u8 KERNEL_ORD_FILM_BURN   = 14u; // ABI_SAMPLE | D0+D5 | AUX_NOISE_ATLAS
constexpr u8 KERNEL_ORD_INTRO       = 15u; // ABI_SAMPLE | D1 IntroBar
constexpr u8 KERNEL_ORD_OUTRO       = 16u; // ABI_SAMPLE | D1 OutroBar
constexpr u8 KERNEL_ORD_FIRE_FULL   = 17u; // ABI_COLOR  | D0+D5 | AUX_NOISE_ATLAS
constexpr u8 KERNEL_ORD_FIRE_BORDER = 18u; // ABI_COLOR  | D1 BorderRing | AUX_NOISE_ATLAS
constexpr u8 KERNEL_ORD_RAIN_BOKEH  = 19u; // ABI_COLOR  | D0+D5
constexpr u8 KERNEL_ORD_MATRIX      = 20u; // ABI_COLOR  | D0+D5
constexpr u8 KERNEL_ORD_RAINDROP    = 21u; // ABI_GEOM   | D0+D4+D5 (NodeVideo 5-layer)
constexpr u8 KERNEL_ORD_BLOOM       = 22u; // ABI_GEOM   | D3+D4+D2 (Pyramid)
constexpr u8 KERNEL_ORD_GAUSS_BLUR  = 23u; // ABI_GEOM   | D2+D4 | TRAIT_GAUSSIAN_CONV
constexpr u8 KERNEL_ORD_GLOW        = 24u; // ABI_GEOM   | D2+D3+D4
constexpr u32 KERNEL_ORD_COUNT      = 25u;

// O(1) Compile-Time Table mapping opaque kernel ordinal -> canonical 64-bit algebraWord
inline constexpr MultiArray<u64, KERNEL_ORD_COUNT> kKernelAlgebraWords = {{
    0ULL, // 0: None
    pack_algebra_word(ABI_UV,     DOM_D0_FULL,                      TRAIT_AFFINE_2D | TRAIT_IDENTITY_GATE, 0ULL, 0xFF, 3, 4,  SHAPE_FULL,        KERNEL_ORD_AFFINE_2D),
    pack_algebra_word(ABI_COLOR,  DOM_D0_FULL,                      TRAIT_AFFINE_COLOR | TRAIT_IDENTITY_GATE | TRAIT_COMMUTATIVE, 0ULL, 0xFF, 3, 2, SHAPE_FULL, KERNEL_ORD_AFFINE_COL),
    pack_algebra_word(ABI_UV,     DOM_D1_BOUNDED,                   TRAIT_IDENTITY_GATE, 0ULL, 1, 0, 12, SHAPE_RADIAL_DISK, KERNEL_ORD_TWIRL),
    pack_algebra_word(ABI_UV,     DOM_D1_BOUNDED,                   TRAIT_IDENTITY_GATE, 0ULL, 1, 0, 12, SHAPE_RADIAL_DISK, KERNEL_ORD_PINCH),
    pack_algebra_word(ABI_UV,     DOM_D1_BOUNDED,                   TRAIT_IDENTITY_GATE, 0ULL, 1, 0, 14, SHAPE_RADIAL_DISK, KERNEL_ORD_LENS),
    pack_algebra_word(ABI_UV,     DOM_D1_BOUNDED | DOM_D5_TEMPORAL, TRAIT_IDENTITY_GATE, 0ULL, 0, 0, 10, SHAPE_SCAN_STRIP,  KERNEL_ORD_GLITCH),
    pack_algebra_word(ABI_SAMPLE, DOM_D0_FULL | DOM_D5_TEMPORAL,    TRAIT_IDENTITY_GATE, AUX_NOISE_ATLAS, 0xFF, 0, 32, SHAPE_FULL, KERNEL_ORD_TURBULENT),
    pack_algebra_word(ABI_SAMPLE, DOM_D2_HALO,                      TRAIT_IDENTITY_GATE, 0ULL, 0, 0, 36, SHAPE_FULL,        KERNEL_ORD_CHROMATIC),
    pack_algebra_word(ABI_SAMPLE, DOM_D2_HALO,                      TRAIT_IDENTITY_GATE, 0ULL, 0, 0, 44, SHAPE_FULL,        KERNEL_ORD_ZOOM_BLUR),
    pack_algebra_word(ABI_SAMPLE, DOM_D2_HALO,                      TRAIT_IDENTITY_GATE, 0ULL, 0, 0, 44, SHAPE_FULL,        KERNEL_ORD_MOTION_BLUR),
    pack_algebra_word(ABI_SAMPLE, DOM_D2_HALO | DOM_D5_TEMPORAL,    TRAIT_IDENTITY_GATE, 0ULL, 0, 0, 48, SHAPE_FULL,        KERNEL_ORD_GLITCH_RGB),
    pack_algebra_word(ABI_SAMPLE, DOM_D0_FULL | DOM_D5_TEMPORAL,    TRAIT_IDENTITY_GATE, 0ULL, 0xFF, 0, 38, SHAPE_FULL,     KERNEL_ORD_CYBER_SCAN),
    pack_algebra_word(ABI_SAMPLE, DOM_D2_HALO,                      0ULL,                AUX_EDGE_SOBEL, 0xFF, 0xFF, 28, SHAPE_FULL, KERNEL_ORD_FXAA),
    pack_algebra_word(ABI_SAMPLE, DOM_D0_FULL | DOM_D5_TEMPORAL,    TRAIT_IDENTITY_GATE, AUX_NOISE_ATLAS, 0xFF, 0, 42, SHAPE_FULL, KERNEL_ORD_FILM_BURN),
    pack_algebra_word(ABI_SAMPLE, DOM_D1_BOUNDED | DOM_D5_TEMPORAL, 0ULL,                0ULL, 0xFF, 0xFF, 64, SHAPE_INTRO_BAR, KERNEL_ORD_INTRO),
    pack_algebra_word(ABI_SAMPLE, DOM_D1_BOUNDED | DOM_D5_TEMPORAL, 0ULL,                0ULL, 0xFF, 0xFF, 64, SHAPE_OUTRO_BAR, KERNEL_ORD_OUTRO),
    pack_algebra_word(ABI_COLOR,  DOM_D0_FULL | DOM_D5_TEMPORAL,    TRAIT_IDENTITY_GATE, AUX_NOISE_ATLAS, 0xFF, 0, 52, SHAPE_FULL, KERNEL_ORD_FIRE_FULL),
    pack_algebra_word(ABI_COLOR,  DOM_D1_BOUNDED | DOM_D5_TEMPORAL, TRAIT_IDENTITY_GATE, AUX_NOISE_ATLAS, 0, 0, 36, SHAPE_BORDER_RING, KERNEL_ORD_FIRE_BORDER),
    pack_algebra_word(ABI_COLOR,  DOM_D0_FULL | DOM_D5_TEMPORAL,    TRAIT_IDENTITY_GATE, 0ULL, 0xFF, 0, 60, SHAPE_FULL,     KERNEL_ORD_RAIN_BOKEH),
    pack_algebra_word(ABI_COLOR,  DOM_D0_FULL | DOM_D5_TEMPORAL,    TRAIT_IDENTITY_GATE, 0ULL, 0xFF, 0, 44, SHAPE_FULL,     KERNEL_ORD_MATRIX),
    pack_algebra_word(ABI_GEOM,   DOM_D0_FULL | DOM_D4_MULTISCALE | DOM_D5_TEMPORAL, 0ULL, AUX_NOISE_ATLAS, 0xFF, 0xFF, 128, SHAPE_FULL, KERNEL_ORD_RAINDROP),
    pack_algebra_word(ABI_GEOM,   DOM_D3_SPARSE | DOM_D4_MULTISCALE | DOM_D2_HALO, TRAIT_IDENTITY_GATE, AUX_MIP_PYRAMID | AUX_TILE_LUMA, 0, 0, 80, SHAPE_FULL, KERNEL_ORD_BLOOM),
    pack_algebra_word(ABI_GEOM,   DOM_D2_HALO | DOM_D4_MULTISCALE,  TRAIT_GAUSSIAN_CONV | TRAIT_IDENTITY_GATE | TRAIT_COMMUTATIVE, AUX_MIP_PYRAMID, 0, 0, 48, SHAPE_FULL, KERNEL_ORD_GAUSS_BLUR),
    pack_algebra_word(ABI_GEOM,   DOM_D2_HALO | DOM_D3_SPARSE | DOM_D4_MULTISCALE, TRAIT_IDENTITY_GATE, AUX_MIP_PYRAMID | AUX_TILE_LUMA, 0, 0, 56, SHAPE_FULL, KERNEL_ORD_GLOW)
}};

// O(1) Resolve 64-bit algebraWord by FNV-1a hash (checks native kernels first, then kCatalogBlob!)
[[nodiscard]] inline constexpr u64 resolve_algebra_word_by_hash(u32 hash) noexcept {
    using effect_catalog::fnv1a_lower;
    if (hash == 0u) return 0ULL;
    if (hash == fnv1a_lower("zoom") || hash == fnv1a_lower("shake"))
        return kKernelAlgebraWords[KERNEL_ORD_AFFINE_2D];
    if (hash == fnv1a_lower("flicker") || hash == fnv1a_lower("brightness"))
        return kKernelAlgebraWords[KERNEL_ORD_AFFINE_COL];
    if (hash == fnv1a_lower("twirl"))       return kKernelAlgebraWords[KERNEL_ORD_TWIRL];
    if (hash == fnv1a_lower("pinch"))       return kKernelAlgebraWords[KERNEL_ORD_PINCH];
    if (hash == fnv1a_lower("lens"))        return kKernelAlgebraWords[KERNEL_ORD_LENS];
    if (hash == fnv1a_lower("glitch"))      return kKernelAlgebraWords[KERNEL_ORD_GLITCH];
    if (hash == fnv1a_lower("turbulent"))   return kKernelAlgebraWords[KERNEL_ORD_TURBULENT];
    if (hash == fnv1a_lower("chromatic"))   return kKernelAlgebraWords[KERNEL_ORD_CHROMATIC];
    if (hash == fnv1a_lower("zoom-blur"))   return kKernelAlgebraWords[KERNEL_ORD_ZOOM_BLUR];
    if (hash == fnv1a_lower("motion-blur")) return kKernelAlgebraWords[KERNEL_ORD_MOTION_BLUR];
    if (hash == fnv1a_lower("glitch-rgb"))  return kKernelAlgebraWords[KERNEL_ORD_GLITCH_RGB];
    if (hash == fnv1a_lower("cyber-scan"))  return kKernelAlgebraWords[KERNEL_ORD_CYBER_SCAN];
    if (hash == fnv1a_lower("fxaa"))        return kKernelAlgebraWords[KERNEL_ORD_FXAA];
    if (hash == fnv1a_lower("film-burn"))   return kKernelAlgebraWords[KERNEL_ORD_FILM_BURN];
    if (hash == fnv1a_lower("intro"))       return kKernelAlgebraWords[KERNEL_ORD_INTRO];
    if (hash == fnv1a_lower("outro"))       return kKernelAlgebraWords[KERNEL_ORD_OUTRO];
    if (hash == fnv1a_lower("fire"))        return kKernelAlgebraWords[KERNEL_ORD_FIRE_FULL];
    if (hash == fnv1a_lower("fire-border")) return kKernelAlgebraWords[KERNEL_ORD_FIRE_BORDER];
    if (hash == fnv1a_lower("rain-bokeh"))  return kKernelAlgebraWords[KERNEL_ORD_RAIN_BOKEH];
    if (hash == fnv1a_lower("matrix"))      return kKernelAlgebraWords[KERNEL_ORD_MATRIX];
    if (hash == fnv1a_lower("rain-drop") || hash == fnv1a_lower("_raindroppro"))
        return kKernelAlgebraWords[KERNEL_ORD_RAINDROP];
    if (hash == fnv1a_lower("bloom"))       return kKernelAlgebraWords[KERNEL_ORD_BLOOM];
    if (hash == fnv1a_lower("blur"))        return kKernelAlgebraWords[KERNEL_ORD_GAUSS_BLUR];
    if (hash == fnv1a_lower("glow"))        return kKernelAlgebraWords[KERNEL_ORD_GLOW];

    const u32 recOff = effect_catalog::blob_find_effect_offset_by_hash(hash);
    if (recOff != 0u) {
        return effect_catalog::blob_read_algebra_word(recOff);
    }
    return 0ULL;
}

// Compact bridge from EffectKind ordinal -> kernel ordinal (eliminated from all effect/ internals)
[[nodiscard]] inline constexpr u64 algebra_word_for_kind_index(u8 kindIdx) noexcept {
    // Matches compacted EffectKind table in core/types.h (0..24, zero Unused gaps!)
    constexpr MultiArray<u8, 25> kMap = {{
        KERNEL_ORD_NONE,        // 0: None
        KERNEL_ORD_GAUSS_BLUR,  // 1: GaussianBlur
        KERNEL_ORD_GLOW,        // 2: Glow
        KERNEL_ORD_AFFINE_COL,  // 3: Flicker
        KERNEL_ORD_AFFINE_2D,   // 4: Zoom
        KERNEL_ORD_AFFINE_2D,   // 5: Shake
        KERNEL_ORD_BLOOM,       // 6: Bloom
        KERNEL_ORD_INTRO,       // 7: Intro
        KERNEL_ORD_OUTRO,       // 8: Outro
        KERNEL_ORD_GLITCH,      // 9: Glitch
        KERNEL_ORD_MATRIX,      // 10: Matrix
        KERNEL_ORD_RAINDROP,    // 11: RainDrop
        KERNEL_ORD_TURBULENT,   // 12: Turbulent
        KERNEL_ORD_TWIRL,       // 13: Twirl
        KERNEL_ORD_PINCH,       // 14: Pinch
        KERNEL_ORD_LENS,        // 15: Lens
        KERNEL_ORD_CHROMATIC,   // 16: Chromatic
        KERNEL_ORD_ZOOM_BLUR,   // 17: ZoomBlur
        KERNEL_ORD_MOTION_BLUR, // 18: MotionBlur
        KERNEL_ORD_GLITCH_RGB,  // 19: GlitchRgb
        KERNEL_ORD_CYBER_SCAN,  // 20: CyberScan
        KERNEL_ORD_FIRE_FULL,   // 21: Fire
        KERNEL_ORD_RAIN_BOKEH,  // 22: RainBokeh
        KERNEL_ORD_FILM_BURN,   // 23: FilmBurn
        KERNEL_ORD_FXAA         // 24: Fxaa
    }};
    return (kindIdx < kMap.size()) ? kKernelAlgebraWords[kMap[kindIdx]] : 0ULL;
}

} // namespace effect_algebra
} // namespace stockcut
