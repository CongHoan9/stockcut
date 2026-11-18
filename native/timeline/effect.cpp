// ============================================================================
// native/timeline/effect.cpp
// CƠ SỞ TOÁN HỌC: TOÁN TỬ HỢP THÀNH KHÔNG GIAO HOÁN (NON-COMMUTATIVE MONOID MathOp)
// ----------------------------------------------------------------------------
// 1. Thực thi SIMD128 / AVX2 song song 4 pixels không rẽ nhánh.
// 2. Gập chuỗi toán tử (fold_chain) bằng con trỏ thuần túy kiểu Stockfish:
//      for (const MathOp* __restrict p = begin; p < end; ++p)
// ============================================================================
#include "effect.h"
#include <cmath>

namespace stockcut {

void MathOp::evaluate_4pixels_simd(
    const float* x4, const float* y4,
    const float* inR4, const float* inG4, const float* inB4, const float* inA4,
    float* outR4, float* outG4, float* outB4, float* outA4
) const noexcept {
#if defined(__EMSCRIPTEN__)
    const v128_t vx = wasm_v128_load(x4);
    const v128_t vy = wasm_v128_load(y4);

    const v128_t vMinX = wasm_f32x4_splat(minX);
    const v128_t vMinY = wasm_f32x4_splat(minY);
    const v128_t vMaxX = wasm_f32x4_splat(maxX);
    const v128_t vMaxY = wasm_f32x4_splat(maxY);

    const v128_t geX = wasm_f32x4_ge(vx, vMinX);
    const v128_t leX = wasm_f32x4_le(vx, vMaxX);
    const v128_t geY = wasm_f32x4_ge(vy, vMinY);
    const v128_t leY = wasm_f32x4_le(vy, vMaxY);

    const v128_t mask = wasm_v128_and(wasm_v128_and(geX, leX), wasm_v128_and(geY, leY));

    const v128_t vOne = wasm_f32x4_splat(1.0f);
    const v128_t vZero = wasm_f32x4_splat(0.0f);
    const v128_t vw = wasm_v128_bitselect(vOne, vZero, mask);

    const v128_t vPremulR = wasm_f32x4_mul(wasm_f32x4_splat(premulR), vw);
    const v128_t vPremulG = wasm_f32x4_mul(wasm_f32x4_splat(premulG), vw);
    const v128_t vPremulB = wasm_f32x4_mul(wasm_f32x4_splat(premulB), vw);
    const v128_t vAlpha   = wasm_f32x4_mul(wasm_f32x4_splat(alpha), vw);

    const v128_t vTransR = wasm_f32x4_sub(vOne, wasm_f32x4_mul(vw, wasm_f32x4_splat(1.0f - transR)));
    const v128_t vTransG = wasm_f32x4_sub(vOne, wasm_f32x4_mul(vw, wasm_f32x4_splat(1.0f - transG)));
    const v128_t vTransB = wasm_f32x4_sub(vOne, wasm_f32x4_mul(vw, wasm_f32x4_splat(1.0f - transB)));
    const v128_t vOneMinusA = wasm_f32x4_sub(vOne, wasm_f32x4_mul(vw, wasm_f32x4_splat(1.0f - oneMinusAlpha)));

    const v128_t vinR = wasm_v128_load(inR4);
    const v128_t vinG = wasm_v128_load(inG4);
    const v128_t vinB = wasm_v128_load(inB4);
    const v128_t vinA = wasm_v128_load(inA4);

    const v128_t voutR = wasm_f32x4_add(vPremulR, wasm_f32x4_mul(vTransR, vinR));
    const v128_t voutG = wasm_f32x4_add(vPremulG, wasm_f32x4_mul(vTransG, vinG));
    const v128_t voutB = wasm_f32x4_add(vPremulB, wasm_f32x4_mul(vTransB, vinB));
    const v128_t voutA = wasm_f32x4_add(vAlpha,   wasm_f32x4_mul(vOneMinusA, vinA));

    wasm_v128_store(outR4, voutR);
    wasm_v128_store(outG4, voutG);
    wasm_v128_store(outB4, voutB);
    wasm_v128_store(outA4, voutA);

#elif defined(__AVX2__)
    const __m128 vx = _mm_loadu_ps(x4);
    const __m128 vy = _mm_loadu_ps(y4);

    const __m128 vMinX = _mm_set1_ps(minX);
    const __m128 vMinY = _mm_set1_ps(minY);
    const __m128 vMaxX = _mm_set1_ps(maxX);
    const __m128 vMaxY = _mm_set1_ps(maxY);

    const __m128 geX = _mm_cmpge_ps(vx, vMinX);
    const __m128 leX = _mm_cmple_ps(vx, vMaxX);
    const __m128 geY = _mm_cmpge_ps(vy, vMinY);
    const __m128 leY = _mm_cmple_ps(vy, vMaxY);

    const __m128 mask = _mm_and_ps(_mm_and_ps(geX, leX), _mm_and_ps(geY, leY));
    const __m128 vOne = _mm_set1_ps(1.0f);
    const __m128 vZero = _mm_setzero_ps();
    const __m128 vw = _mm_blendv_ps(vZero, vOne, mask);

    const __m128 vPremulR = _mm_mul_ps(_mm_set1_ps(premulR), vw);
    const __m128 vPremulG = _mm_mul_ps(_mm_set1_ps(premulG), vw);
    const __m128 vPremulB = _mm_mul_ps(_mm_set1_ps(premulB), vw);
    const __m128 vAlpha   = _mm_mul_ps(_mm_set1_ps(alpha), vw);

    const __m128 vTransR = _mm_sub_ps(vOne, _mm_mul_ps(vw, _mm_set1_ps(1.0f - transR)));
    const __m128 vTransG = _mm_sub_ps(vOne, _mm_mul_ps(vw, _mm_set1_ps(1.0f - transG)));
    const __m128 vTransB = _mm_sub_ps(vOne, _mm_mul_ps(vw, _mm_set1_ps(1.0f - transB)));
    const __m128 vOneMinusA = _mm_sub_ps(vOne, _mm_mul_ps(vw, _mm_set1_ps(1.0f - oneMinusAlpha)));

    const __m128 vinR = _mm_loadu_ps(inR4);
    const __m128 vinG = _mm_loadu_ps(inG4);
    const __m128 vinB = _mm_loadu_ps(inB4);
    const __m128 vinA = _mm_loadu_ps(inA4);

    const __m128 voutR = _mm_fmadd_ps(vTransR, vinR, vPremulR);
    const __m128 voutG = _mm_fmadd_ps(vTransG, vinG, vPremulG);
    const __m128 voutB = _mm_fmadd_ps(vTransB, vinB, vPremulB);
    const __m128 voutA = _mm_fmadd_ps(vOneMinusA, vinA, vAlpha);

    _mm_storeu_ps(outR4, voutR);
    _mm_storeu_ps(outG4, voutG);
    _mm_storeu_ps(outB4, voutB);
    _mm_storeu_ps(outA4, voutA);

#else
    const float* __restrict x = x4;
    const float* __restrict y = y4;
    const float* __restrict inR = inR4;
    const float* __restrict inG = inG4;
    const float* __restrict inB = inB4;
    const float* __restrict inA = inA4;
    float* __restrict outR = outR4;
    float* __restrict outG = outG4;
    float* __restrict outB = outB4;
    float* __restrict outA = outA4;
    for (const float* const __restrict xEnd = x4 + 4; x < xEnd; ++x, ++y, ++inR, ++inG, ++inB, ++inA, ++outR, ++outG, ++outB, ++outA) {
        evaluate_branchless_pixel(*x, *y, *inR, *inG, *inB, *inA, *outR, *outG, *outB, *outA);
    }
#endif
}

MathOp MathOp::fold_chain(const MathOp* ops, u32 count) noexcept {
    if (__builtin_expect(!ops || count == 0, 0)) {
        return MathOp::identity();
    }
    MathOp accumulated = *ops;
    for (const MathOp* __restrict p = ops + 1, * const __restrict end = ops + count; p < end; ++p) {
        accumulated = accumulated.compose_with(*p);
    }
    return accumulated;
}

bool MathOp::verify_associativity(
    const MathOp& a,
    const MathOp& b,
    const MathOp& c,
    float epsilon
) noexcept {
    const MathOp ab = a.compose_with(b);
    const MathOp left = ab.compose_with(c);

    const MathOp bc = b.compose_with(c);
    const MathOp right = a.compose_with(bc);

    const auto diff = [](float x, float y) { return std::abs(x - y); };

    if (diff(left.premulR, right.premulR) > epsilon ||
        diff(left.premulG, right.premulG) > epsilon ||
        diff(left.premulB, right.premulB) > epsilon ||
        diff(left.alpha,   right.alpha)   > epsilon) {
        return false;
    }

    if (diff(left.transR, right.transR) > epsilon ||
        diff(left.transG, right.transG) > epsilon ||
        diff(left.transB, right.transB) > epsilon ||
        diff(left.oneMinusAlpha, right.oneMinusAlpha) > epsilon) {
        return false;
    }

    return true;
}

} // namespace stockcut
