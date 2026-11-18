// ============================================================================
// CoreEngine - Math Utilities Implementation
// ============================================================================
#include "math_utils.h"
#include "data_structures.h"

namespace stockcut {
#if !defined(__GNUC__) && !defined(__clang__)
u64 mul_hi64(u64 a, u64 b) {
    const u64 a0 = a & 0xFFFFFFFFULL;
    const u64 a1 = a >> 32;
    const u64 b0 = b & 0xFFFFFFFFULL;
    const u64 b1 = b >> 32;
    const u64 p00 = a0 * b0;
    const u64 p01 = a0 * b1;
    const u64 p10 = a1 * b0;
    const u64 p11 = a1 * b1;
    const u64 mid = p01 + (p00 >> 32);
    u64 mid_val = mid;
    asm volatile("" : "+r"(mid_val));
    const u64 mid1 = mid_val & 0xFFFFFFFFULL;
    const u64 mid2 = mid_val >> 32;
    const u64 mid_total = p10 + mid1;
    return p11 + mid2 + (mid_total >> 32);
}
#endif
u32 lerp_color_rgba(u32 c0, u32 c1, float t) {
    const float r0 = static_cast<float>(c0 & 0xFFu);
    const float g0 = static_cast<float>((c0 >> 8) & 0xFFu);
    const float b0 = static_cast<float>((c0 >> 16) & 0xFFu);
    const float a0 = static_cast<float>((c0 >> 24) & 0xFFu);
    const float r1 = static_cast<float>(c1 & 0xFFu);
    const float g1 = static_cast<float>((c1 >> 8) & 0xFFu);
    const float b1 = static_cast<float>((c1 >> 16) & 0xFFu);
    const float a1 = static_cast<float>((c1 >> 24) & 0xFFu);
    const u32 r = static_cast<u32>(r0 + (r1 - r0) * t) & 0xFFu;
    const u32 g = static_cast<u32>(g0 + (g1 - g0) * t) & 0xFFu;
    const u32 b = static_cast<u32>(b0 + (b1 - b0) * t) & 0xFFu;
    const u32 a = static_cast<u32>(a0 + (a1 - a0) * t) & 0xFFu;
    return r | (g << 8) | (b << 16) | (a << 24);
}
void matrix3x3_mul(const float* A, const float* B, float* outR) {
    MultiArray<float, 9> temp;
    for (usize r = 0; r < 3; ++r) {
        for (usize c = 0; c < 3; ++c) {
            temp[r * 3 + c] = A[r * 3 + 0] * B[0 * 3 + c] +
                              A[r * 3 + 1] * B[1 * 3 + c] +
                              A[r * 3 + 2] * B[2 * 3 + c];
        }
    }
    for (usize i = 0; i < 9; ++i) outR[i] = temp[i];
}
void matrix3x3_mul_vec3(const float* A, const float* V, float* outV) {
    MultiArray<float, 3> temp;
    temp[0] = A[0] * V[0] + A[1] * V[1] + A[2] * V[2];
    temp[1] = A[3] * V[0] + A[4] * V[1] + A[5] * V[2];
    temp[2] = A[6] * V[0] + A[7] * V[1] + A[8] * V[2];
    outV[0] = temp[0]; outV[1] = temp[1]; outV[2] = temp[2];
}
void affine2d_mul(const float* A, const float* B, float* outR) {
    MultiArray<float, 6> temp;
    temp[0] = A[0] * B[0] + A[2] * B[1];
    temp[1] = A[1] * B[0] + A[3] * B[1];
    temp[2] = A[0] * B[2] + A[2] * B[3];
    temp[3] = A[1] * B[2] + A[3] * B[3];
    temp[4] = A[0] * B[4] + A[2] * B[5] + A[4];
    temp[5] = A[1] * B[4] + A[3] * B[5] + A[5];
    for (usize i = 0; i < 6; ++i) outR[i] = temp[i];
}
void affine2d_transform_point(const float* M, float x, float y, float& outX, float& outY) {
    outX = M[0] * x + M[2] * y + M[4];
    outY = M[1] * x + M[3] * y + M[5];
}
void compute_color_matrix_64b(
    float bright, float contrast, float sat, float hueDeg, float inv, float sep, float opacity,
    ColorMatrix64B& out
) {
    MultiArray<float, 9> M = {{ 1, 0, 0,  0, 1, 0,  0, 0, 1 }};
    MultiArray<float, 3> O = {{ 0, 0, 0 }};
    if (bright != 1.0f) {
        for (float* p = M.begin(); p < M.end(); ++p) *p *= bright;
        for (float* p = O.begin(); p < O.end(); ++p) *p *= bright;
    }
    if (contrast != 1.0f) {
        const float cOffset = 0.5f * (1.0f - contrast);
        for (float* p = M.begin(); p < M.end(); ++p) *p *= contrast;
        for (float* p = O.begin(); p < O.end(); ++p) *p = *p * contrast + cOffset;
    }
    if (sat != 1.0f) {
        constexpr float r = 0.2126f;
        constexpr float g = 0.7152f;
        constexpr float b = 0.0722f;
        const MultiArray<float, 9> S = {{
            r + (1.0f - r) * sat, g - g * sat,         b - b * sat,
            r - r * sat,         g + (1.0f - g) * sat, b - b * sat,
            r - r * sat,         g - g * sat,         b + (1.0f - b) * sat
        }};
        matrix3x3_mul(S.data(), M.data(), M.data());
        matrix3x3_mul_vec3(S.data(), O.data(), O.data());
    }
    if (hueDeg != 0.0f) {
        const float rad = hueDeg * DEG_TO_RAD_F32;
        const float c = fast_cos(rad);
        const float s = fast_sin(rad);
        const MultiArray<float, 9> H = {{
            0.213f + c * 0.787f - s * 0.213f, 0.715f - c * 0.715f - s * 0.715f, 0.072f - c * 0.072f + s * 0.928f,
            0.213f - c * 0.213f + s * 0.143f, 0.715f + c * 0.285f + s * 0.140f, 0.072f - c * 0.072f - s * 0.283f,
            0.213f - c * 0.213f - s * 0.787f, 0.715f - c * 0.715f + s * 0.715f, 0.072f + c * 0.928f + s * 0.072f
        }};
        matrix3x3_mul(H.data(), M.data(), M.data());
        matrix3x3_mul_vec3(H.data(), O.data(), O.data());
    }
    if (sep > 0.0f) {
        const float p = min(sep, 1.0f);
        const float invP = 1.0f - p;
        const MultiArray<float, 9> SEP = {{
            invP + p * 0.393f, p * 0.769f,        p * 0.189f,
            p * 0.349f,        invP + p * 0.686f, p * 0.168f,
            p * 0.272f,        p * 0.534f,        invP + p * 0.131f
        }};
        matrix3x3_mul(SEP.data(), M.data(), M.data());
        matrix3x3_mul_vec3(SEP.data(), O.data(), O.data());
    }
    if (inv > 0.0f) {
        const float invC = min(inv, 1.0f);
        const float scale = 1.0f - 2.0f * invC;
        for (float* p = M.begin(); p < M.end(); ++p) *p *= scale;
        for (float* p = O.begin(); p < O.end(); ++p) *p = *p * scale + invC;
    }
    for (usize i = 0; i < 9; ++i) out.M[i] = M[i];
    out.offset[0] = O[0];
    out.offset[1] = O[1];
    out.offset[2] = O[2];
    out.opacity   = opacity;
    out.pad[0] = 0.0f; out.pad[1] = 0.0f; out.pad[2] = 0.0f;
}
} // namespace stockcut
