// ============================================================================
// CoreEngine - Math Utilities & Unified Easing Curves
// ============================================================================
#pragma once
#include "types.h"
#include "data_structures.h"
#include "hash_primitives.h"

namespace stockcut {
[[nodiscard]] constexpr float smoothstep01(float u) noexcept {
    u = clamp(u, 0.0f, 1.0f);
    return u * u * (3.0f - 2.0f * u);
}

// Unified compile-time branchless quadratic easing curve (shared by Position & Runtime)
template<EasingCurve Curve, typename Real = double>
[[nodiscard]] constexpr Real eval_quad_easing(Real t) noexcept {
    if constexpr (Curve == EasingCurve::Linear) {
        return t;
    } else if constexpr (Curve == EasingCurve::EaseIn) {
        return t * t;
    } else if constexpr (Curve == EasingCurve::EaseOut) {
        const Real inv = Real(1) - t;
        return Real(1) - inv * inv;
    } else {
        const Real inv = Real(1) - t;
        const Real r1  = Real(2) * t * t;
        const Real r2  = Real(1) - Real(2) * inv * inv;
        const Real isGeHalf = static_cast<Real>(t >= Real(0.5));
        return r1 + (r2 - r1) * isGeHalf;
    }
}

struct alignas(64) ColorMatrix64B {
    MultiArray<float, 9> M;       // Offset 0..35  (9 floats: 3x3 RGB matrix)
    MultiArray<float, 3> offset;  // Offset 36..47 (3 floats: RGB offset)
    float opacity = 0.0f;           // Offset 48..51 (1 float)
    MultiArray<float, 3> pad;     // Offset 52..63 (12 bytes padding căn tròn đúng 64 bytes)
};
static_assert(sizeof(ColorMatrix64B) == 64, "ColorMatrix64B must be 64 bytes");
static_assert(alignof(ColorMatrix64B) == 64, "ColorMatrix64B must be 64-byte aligned");

[[nodiscard]] [[gnu::always_inline]] inline float sqrt_f32(float x) noexcept {
    return __builtin_sqrtf(x);
}

[[nodiscard]] [[gnu::always_inline]] inline float fast_sin(float x) noexcept {
    x -= __builtin_roundf(x * INV_TWO_PI_F32) * TWO_PI_F32;
    const float absX = __builtin_fabsf(x);
    const float excess = static_cast<float>(absX > 0.5f * PI_VAL);
    const float signX = __builtin_copysignf(1.0f, x);
    x = (1.0f - excess) * x + excess * (signX * PI_VAL - x);
    const float x2 = x * x;
    return x * (1.0f - x2 * (1.0f / 6.0f - x2 * (1.0f / 120.0f - x2 * (1.0f / 5040.0f
           - x2 * (1.0f / 362880.0f - x2 * (1.0f / 39916800.0f))))));
}

[[nodiscard]] [[gnu::always_inline]] inline float fast_cos(float x) noexcept {
    return fast_sin(x + 0.5f * PI_VAL);
}

[[gnu::always_inline]] inline u64 splitmix64(u64& state) noexcept {
    return splitmix64_next(state);
}

#if defined(__GNUC__) || defined(__clang__)
inline u64 mul_hi64(u64 a, u64 b) noexcept {
    return static_cast<u64>((static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b)) >> 64);
}
#else
u64   mul_hi64(u64 a, u64 b);
#endif

[[nodiscard]] [[gnu::always_inline]] inline float lerp_f32(float a, float b, float t) noexcept {
    return a + (b - a) * t;
}
u32   lerp_color_rgba(u32 c0, u32 c1, float t);

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
// WASM SIMD128 SoA Keyframe Parallel Evaluator (Reciprocal Multiply, 0 Hardware Division)
[[nodiscard]] [[gnu::always_inline]] inline v128_t simd_lerp4(
    v128_t t, v128_t t0, v128_t invDt, v128_t v0, v128_t v1
) noexcept {
    const v128_t zero = wasm_f32x4_splat(0.0f);
    const v128_t one  = wasm_f32x4_splat(1.0f);
    const v128_t dt   = wasm_f32x4_sub(t, t0);
    const v128_t prog = wasm_f32x4_pmax(zero, wasm_f32x4_pmin(one, wasm_f32x4_mul(dt, invDt)));
    const v128_t diff = wasm_f32x4_sub(v1, v0);
    return wasm_f32x4_add(v0, wasm_f32x4_mul(diff, prog));
}
#endif

void matrix3x3_mul(const float* A, const float* B, float* outR);
void matrix3x3_mul_vec3(const float* A, const float* V, float* outV);
void affine2d_mul(const float* A, const float* B, float* outR);
void affine2d_transform_point(const float* M, float x, float y, float& outX, float& outY);

void compute_color_matrix_64b(
    float bright, float contrast, float sat, float hueDeg, float inv, float sep, float opacity,
    ColorMatrix64B& out
);
} // namespace stockcut
