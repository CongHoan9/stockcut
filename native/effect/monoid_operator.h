// ============================================================================
// native/effect/monoid_operator.h
// TRỤ CỘT 1: ALGEBRAIC MONOID FUSION (DUNG HỢP TOÁN TỬ ĐẠI SỐ KHÔNG RẼ NHÁNH)
// ----------------------------------------------------------------------------
// 1. Phép biến đổi không gian 2D Affine (Transform2D - 32 bytes):
//    - Đóng gói ma trận 2x2 và vector tịnh tiến (tx, ty) quanh tâm (0.5, 0.5).
//    - Tích chập Monoid không giao hoán: (T_2 o T_1) kết hợp hoàn hảo.
//    - Dung hợp Zoom, Shake, Mirror, Tilt, Pan thành đúng 1 ma trận duy nhất.
// 2. Phép chuyển đổi màu sắc tuyến tính (ColorTransferOp - 32 bytes):
//    - Đóng gói Transmittance Gain G = (gr, gg, gb, ga) và Emission E = (er, eg, eb, ea).
//    - Tích chập Monoid: C_out = G_net * C_in + E_net.
//    - Dung hợp Brightness, Flicker, Color Tint, Fade thành đúng 1 phép toán.
// 3. Toán tử Master Uber Fused Effect (FusedUberEffect - 64 bytes = 1 L1 Cacheline):
//    - Kết hợp cả hai họ toán học để thực thi trong 1 GPU Fragment Shader duy nhất.
// ============================================================================
#pragma once

#include "types.h"
#include "data_structures.h"
#include "math_utils.h"
#include <cmath>
#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

namespace stockcut {

// ============================================================================
// 1. AFFINE TRANSFORM 2D MONOID (32 bytes - 2 thanh ghi SIMD 128-bit)
// Tọa độ UV chuẩn hóa quanh tâm màn hình C = (0.5, 0.5):
//   p = UV - C
//   UV_src = M * p + C + T
// ============================================================================
struct alignas(16) Transform2D {
    float m00, m01, m10, m11; // Ma trận 2x2 biến dạng không gian
    float tx, ty;             // Vector tịnh tiến offset
    float pad0, pad1;         // Căn lề 32 bytes chuẩn

    [[nodiscard]] static constexpr Transform2D identity() noexcept {
        return Transform2D{
            1.0f, 0.0f,
            0.0f, 1.0f,
            0.0f, 0.0f,
            0.0f, 0.0f
        };
    }

    // Pixel-space aspect-preserving UV pullback matching kTransformFS bit-for-bit:
    //   p = (uv - 0.5) * res - shiftPx
    //   rot = R(+rotRad) * p
    //   uv_src = rot / (res * max(scale, 0.05)) + 0.5
    // pad0 = tiled flag (1.0f = tiled wrap, 0.0f = clamp), pad1 = mirror flag (1.0f = mirror-tile)
    [[nodiscard]] [[gnu::always_inline]] static inline Transform2D from_zoom(
        float scale, float rotRad, bool mirror,
        float viewportW = DEFAULT_STAGE_WIDTH_F32, float viewportH = DEFAULT_STAGE_HEIGHT_F32
    ) noexcept {
        const float safeScale = max(scale, 0.05f);
        const float s = 1.0f / safeScale;
        const float aspect = (viewportH > 0.0f) ? (viewportW / viewportH) : (16.0f / 9.0f);
        const float invAspect = (viewportW > 0.0f) ? (viewportH / viewportW) : (9.0f / 16.0f);
        const float cosA = std::cos(rotRad);
        const float sinA = std::sin(rotRad);
        return Transform2D{
            s * cosA,             -s * sinA * invAspect,
            s * sinA * aspect,     s * cosA,
            0.0f,                  0.0f,
            1.0f,                  mirror ? 1.0f : 0.0f
        };
    }

    // dxNorm, dyNorm are width-normalized fractions (identical to ShakeEvalResult::dx / dy
    // and TransformPassPolicy<true>: u_shift = vec2(dxNorm * W, -dyNorm * W)).
    [[nodiscard]] static inline Transform2D from_shake(
        float scaleBoost, float rotDeg, float dxNorm, float dyNorm, float viewportW, float viewportH
    ) noexcept {
        const float safeScale = max(scaleBoost, 0.05f);
        const float s = 1.0f / safeScale;
        const float aspect = (viewportH > 0.0f) ? (viewportW / viewportH) : (16.0f / 9.0f);
        const float invAspect = (viewportW > 0.0f) ? (viewportH / viewportW) : (9.0f / 16.0f);
        const float rad = rotDeg * DEG_TO_RAD_F32;
        const float cosA = std::cos(rad);
        const float sinA = std::sin(rad);
        const float m00 =  s * cosA;
        const float m01 = -s * sinA * invAspect;
        const float m10 =  s * sinA * aspect;
        const float m11 =  s * cosA;
        // u_shift = (dxNorm * W, -dyNorm * W) -> shifted UV offset = (-dxNorm, +dyNorm * aspect)
        const float su = -dxNorm;
        const float sv =  dyNorm * aspect;
        return Transform2D{
            m00, m01,
            m10, m11,
            m00 * su + m01 * sv,
            m10 * su + m11 * sv,
            0.0f, 0.0f
        };
    }

    // Algebraic composition: next(this(UV))
    // M_net = M_next * M_this, T_net = M_next * T_this + T_next
    // Note: For image pass sequence (pass A then pass B), UV pullback is contravariant:
    //   uv_src = T_A(T_B(uv_out)) => call T_B.compose(T_A)!
    [[nodiscard]] [[gnu::always_inline]] constexpr Transform2D compose(const Transform2D& next) const noexcept {
        return Transform2D{
            next.m00 * m00 + next.m01 * m10,
            next.m00 * m01 + next.m01 * m11,
            next.m10 * m00 + next.m11 * m10,
            next.m10 * m01 + next.m11 * m11,
            next.m00 * tx + next.m01 * ty + next.tx,
            next.m10 * tx + next.m11 * ty + next.ty,
            (pad0 > 0.5f || next.pad0 > 0.5f) ? 1.0f : 0.0f,
            (pad1 > 0.5f || next.pad1 > 0.5f) ? 1.0f : 0.0f
        };
    }

    [[nodiscard]] constexpr bool is_identity(float eps = 1e-5f) const noexcept {
        return abs_val(m00 - 1.0f) <= eps && abs_val(m11 - 1.0f) <= eps &&
               abs_val(m01) <= eps && abs_val(m10) <= eps &&
               abs_val(tx) <= eps && abs_val(ty) <= eps;
    }

    constexpr void apply_uv(float u, float v, float& outU, float& outV) const noexcept {
        const float pu = u - 0.5f;
        const float pv = v - 0.5f;
        outU = m00 * pu + m01 * pv + tx + 0.5f;
        outV = m10 * pu + m11 * pv + ty + 0.5f;
    }
};
static_assert(sizeof(Transform2D) == 32, "Transform2D must be exactly 32 bytes");
// ============================================================================
// 2. COLOR TRANSFER MONOID (32 bytes - 2 thanh ghi SIMD 128-bit)
// Chuyển đổi màu sắc: C_out = Gain * C_in + Emission
// ============================================================================
struct alignas(16) ColorTransferOp {
    float gainR, gainG, gainB, gainA; // Thành phần nhân (Transmittance Gain)
    float emitR, emitG, emitB, emitA; // Thành phần cộng (Emission / Offset)

    [[nodiscard]] static constexpr ColorTransferOp identity() noexcept {
        return ColorTransferOp{
            1.0f, 1.0f, 1.0f, 1.0f,
            0.0f, 0.0f, 0.0f, 0.0f
        };
    }

    [[nodiscard]] static constexpr ColorTransferOp from_brightness(float brightness) noexcept {
        return ColorTransferOp{
            brightness, brightness, brightness, 1.0f,
            0.0f, 0.0f, 0.0f, 0.0f
        };
    }

    [[nodiscard]] static constexpr ColorTransferOp from_tint(float r, float g, float b, float a) noexcept {
        return ColorTransferOp{
            r, g, b, a,
            0.0f, 0.0f, 0.0f, 0.0f
        };
    }

    [[nodiscard]] static constexpr ColorTransferOp from_fade(float fade) noexcept {
        return ColorTransferOp{
            fade, fade, fade, fade,
            0.0f, 0.0f, 0.0f, 0.0f
        };
    }

    // Tích chập Monoid: this áp dụng trước, next áp dụng sau: next(this(Color))
    // G_net = G_next * G_this
    // E_net = G_next * E_this + E_next
    [[nodiscard]] [[gnu::always_inline]] inline ColorTransferOp compose(const ColorTransferOp& next) const noexcept {
        return ColorTransferOp{
            next.gainR * gainR,
            next.gainG * gainG,
            next.gainB * gainB,
            next.gainA * gainA,
            next.gainR * emitR + next.emitR,
            next.gainG * emitG + next.emitG,
            next.gainB * emitB + next.emitB,
            next.gainA * emitA + next.emitA
        };
    }

    [[nodiscard]] constexpr bool is_identity(float eps = 1e-5f) const noexcept {
        return abs_val(gainR - 1.0f) <= eps && abs_val(gainG - 1.0f) <= eps &&
               abs_val(gainB - 1.0f) <= eps && abs_val(gainA - 1.0f) <= eps &&
               abs_val(emitR) <= eps && abs_val(emitG) <= eps &&
               abs_val(emitB) <= eps && abs_val(emitA) <= eps;
    }

    constexpr void apply_color(
        float r, float g, float b, float a,
        float& outR, float& outG, float& outB, float& outA
    ) const noexcept {
        outR = gainR * r + emitR;
        outG = gainG * g + emitG;
        outB = gainB * b + emitB;
        outA = gainA * a + emitA;
    }
};
static_assert(sizeof(ColorTransferOp) == 32, "ColorTransferOp must be exactly 32 bytes");

// ============================================================================
// 3. FUSED UBER EFFECT (64 bytes = ĐÚNG 1 L1 CACHELINE / 4 THANH GHI SIMD 128-BIT)
// Dung hợp hoàn chỉnh cả không gian hình học lẫn dải màu vào 1 cấu trúc duy nhất.
// ============================================================================
struct alignas(64) FusedUberEffect {
    Transform2D uvTransform;  // 32 bytes: Ma trận biến dạng tọa độ
    ColorTransferOp   colorTransfer; // 32 bytes: Phép biến đổi phổ màu

    [[nodiscard]] static constexpr FusedUberEffect identity() noexcept {
        return FusedUberEffect{
            Transform2D::identity(),
            ColorTransferOp::identity()
        };
    }

    [[nodiscard]] [[gnu::always_inline]] inline FusedUberEffect compose(const FusedUberEffect& next) const noexcept {
        return FusedUberEffect{
            next.uvTransform.compose(uvTransform), // Contravariant UV pullback: T_this(T_next(uv))
            colorTransfer.compose(next.colorTransfer)
        };
    }

    [[nodiscard]] constexpr bool is_identity(float eps = 1e-5f) const noexcept {
        return uvTransform.is_identity(eps) && colorTransfer.is_identity(eps);
    }
};
static_assert(sizeof(FusedUberEffect) == 64, "FusedUberEffect must be exactly 64 bytes");

// ============================================================================
// SELF-CONTAINED MONOID GROUP & SEQUENCE TYPE ALIASES (Bảo toàn Stockfish & GEMINI.md)
// Đóng gói sẵn toàn bộ kích thước biên dịch, triệt tiêu việc gọi sizeof(T) rải rác.
// ============================================================================
template <>
struct MonoidTraits<Transform2D> {
    static constexpr usize GroupCapacity = 16;
    static constexpr usize MaxNodes      = 16;
    static constexpr usize MaxChildren   = 64;
};

template <>
struct MonoidTraits<ColorTransferOp> {
    static constexpr usize GroupCapacity = 16;
    static constexpr usize MaxNodes      = 16;
    static constexpr usize MaxChildren   = 64;
};

template <>
struct MonoidTraits<FusedUberEffect> {
    static constexpr usize GroupCapacity = 16;
    static constexpr usize MaxNodes      = 64;
    static constexpr usize MaxChildren   = 256;
};

template <>
struct MonoidTraits<WavefrontPhasor> {
    static constexpr usize GroupCapacity = 16;
    static constexpr usize MaxNodes      = 16;
    static constexpr usize MaxChildren   = 1;
};

using EffectGroup       = Group<FusedUberEffect>;
using TransformGroup    = Group<Transform2D>;
using ColorGroup        = Group<ColorTransferOp>;
using WavefrontGroup    = Group<WavefrontPhasor>;

using EffectPipeline    = Pipelines<FusedUberEffect>;
using TransformPipeline = Pipelines<Transform2D>;
using ColorPipeline     = Pipelines<ColorTransferOp>;
using WavefrontPipeline = Pipelines<WavefrontPhasor>;

using EffectSequence    = FlatMonoidSequence<FusedUberEffect>;
using TransformSequence = FlatMonoidSequence<Transform2D>;
using ColorSequence     = FlatMonoidSequence<ColorTransferOp>;
using WavefrontSequence = FlatMonoidSequence<WavefrontPhasor>;

} // namespace stockcut
