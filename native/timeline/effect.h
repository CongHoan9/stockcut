// ============================================================================
// native/timeline/effect.h
// CƠ SỞ TOÁN HỌC & KIẾN TRÚC TỐI ƯU CẤP THẤP:
// TOÁN TỬ ĐẠI SỐ DUNG HỢP PHI RẼ NHÁNH (64-BYTE NON-COMMUTATIVE MONOID MathOp)
// ----------------------------------------------------------------------------
// 1. Cấu trúc dữ liệu tự chủ 64-byte MathOp (1 L1 Cacheline, 0 padding):
//    - Không gian Trạng thái Phân tầng (Stratified State Space) Z_0 < Z_1 < ... < Z_{n-1}.
//    - Biểu diễn đồng nhất mọi layer / hiệu ứng dưới dạng tự đồng cấu (Endomorphism):
//        L_i(C)(x,y) = P_i(x,y) + T_i(x,y) (*) C(x,y)
//    - Không có bất kỳ hàm tương thích ngược hay alias lịch sử nào.
// 2. Thực thi SIMD128 / AVX2 trực tiếp trên thanh ghi vector:
//    - compose_with và evaluate_4pixels_simd khai thác trực tiếp vAABB, vPremul, vTrans.
// ============================================================================
#pragma once

#include <cmath>
#include "types.h"
#if defined(__EMSCRIPTEN__)
#include <wasm_simd128.h>
#elif defined(__AVX2__)
#include <immintrin.h>
#endif

namespace stockcut {

constexpr float MONOID_ASSOC_EPSILON = 1.0e-5f;

// ============================================================================
// CẤU TRÚC TOÁN TỬ ĐẠI SỐ DUNG HỢP 64-BYTE (NON-COMMUTATIVE MONOID MathOp)
// Căn lề 64 bytes = khớp chính xác 1 L1 Cacheline / 4 thanh ghi SIMD 128-bit
// ============================================================================
struct alignas(64) MathOp {
    union {
        struct {
            // 1. Dải không gian hiệu lực AABB [0..15 bytes]
            float minX, minY, maxX, maxY;

            // 2. Thành phần cộng màu đã nhân trước Alpha (Premultiplied Emission P_i) [16..31 bytes]
            float premulR, premulG, premulB, alpha;

            // 3. Thành phần nhân truyền qua (Transmittance / Color Gain T_i) [32..47 bytes]
            float transR, transG, transB, oneMinusAlpha;

            // 4. Tham số biến đổi tọa độ hình học (Affine Spatial Transform) [48..63 bytes]
            float scale, rotateRad, translateX, translateY;
        };
#if defined(__EMSCRIPTEN__)
        struct {
            v128_t vAABB;
            v128_t vPremul;
            v128_t vTrans;
            v128_t vTransform;
        };
#endif
    };

    // ------------------------------------------------------------------------
    // 1. Phần tử đơn vị (Identity Element I): L_id(C) = C
    //    P_id = (0, 0, 0, 0), T_id = (1, 1, 1, 1), AABB = (-inf, -inf, +inf, +inf)
    // ------------------------------------------------------------------------
    [[nodiscard]] static constexpr MathOp identity() noexcept {
        return MathOp{
            { {
                -AABB_INF_BOUND, -AABB_INF_BOUND, AABB_INF_BOUND, AABB_INF_BOUND,
                0.0f, 0.0f, 0.0f, 0.0f,
                1.0f, 1.0f, 1.0f, 1.0f,
                1.0f, 0.0f, 0.0f, 0.0f
            } }
        };
    }

    // ------------------------------------------------------------------------
    // 2. Tạo toán tử Porter-Duff Over từ màu nguồn chuẩn (R, G, B, Alpha)
    //    và khung hình chữ nhật AABB (x0, y0, x1, y1)
    // ------------------------------------------------------------------------
    [[nodiscard]] static constexpr MathOp make_layer(
        float x0, float y0, float x1, float y1,
        float r, float g, float b, float a,
        float gainR = 1.0f, float gainG = 1.0f, float gainB = 1.0f,
        float s = 1.0f, float rot = 0.0f, float tx = 0.0f, float ty = 0.0f
    ) noexcept {
        const float invA = 1.0f - a;
        return MathOp{
            { {
                x0, y0, x1, y1,
                r * a, g * a, b * a, a,
                gainR * invA, gainG * invA, gainB * invA, invA,
                s, rot, tx, ty
            } }
        };
    }

    // ------------------------------------------------------------------------
    // 3. Toán tử hợp thành không giao hoán (Non-Commutative Sequential Composition):
    //    Tính toán tử kết hợp (this (dot) lower) trong đó `this` (Z_j) nằm TRÊN `lower` (Z_i)
    //    Công thức đóng đại số:
    //      P_{j . i} = P_j + (1 - alpha_j) * P_i
    //      T_{j . i} = T_j (*) T_i
    //      alpha_{j . i} = alpha_j + (1 - alpha_j) * alpha_i
    // ------------------------------------------------------------------------
    [[nodiscard]] inline MathOp compose_with(const MathOp& lower) const noexcept {
        MathOp out{};
        out.minX = (minX < lower.minX) ? minX : lower.minX;
        out.minY = (minY < lower.minY) ? minY : lower.minY;
        out.maxX = (maxX > lower.maxX) ? maxX : lower.maxX;
        out.maxY = (maxY > lower.maxY) ? maxY : lower.maxY;

#if defined(__EMSCRIPTEN__)
        out.vPremul = wasm_f32x4_add(vPremul, wasm_f32x4_mul(wasm_f32x4_splat(oneMinusAlpha), lower.vPremul));
        out.vTrans  = wasm_f32x4_mul(vTrans, lower.vTrans);
#else
        out.premulR = premulR + oneMinusAlpha * lower.premulR;
        out.premulG = premulG + oneMinusAlpha * lower.premulG;
        out.premulB = premulB + oneMinusAlpha * lower.premulB;
        out.alpha   = alpha   + oneMinusAlpha * lower.alpha;

        out.transR = transR * lower.transR;
        out.transG = transG * lower.transG;
        out.transB = transB * lower.transB;
        out.oneMinusAlpha = oneMinusAlpha * lower.oneMinusAlpha;
#endif

        out.scale      = scale * lower.scale;
        out.rotateRad  = rotateRad + lower.rotateRad;
        out.translateX = translateX + lower.translateX;
        out.translateY = translateY + lower.translateY;
        return out;
    }

    // ------------------------------------------------------------------------
    // 4. Hàm chỉ thị không gian phi rẽ nhánh (Branchless Spatial Indicator chi_i(x,y))
    //    Trả về 1.0f nếu (x, y) thuộc [minX, maxX] x [minY, maxY], ngược lại 0.0f.
    //    Tuyệt đối 0 lệnh if/else!
    // ------------------------------------------------------------------------
    [[nodiscard]] static constexpr float branchless_step(float edge, float x) noexcept {
        return static_cast<float>(x >= edge);
    }

    [[nodiscard]] constexpr float spatial_weight(float x, float y) const noexcept {
        const float wx = branchless_step(minX, x) * branchless_step(x, maxX);
        const float wy = branchless_step(minY, y) * branchless_step(y, maxY);
        return wx * wy;
    }

    // ------------------------------------------------------------------------
    // 5. Thực thi biến đổi đơn điểm ảnh phi rẽ nhánh (Branchless Reduction Matrix)
    //    Khi w = 0 (ngoài vùng hiệu lực): L_i tự động suy biến về Identity (P=0, T=1)
    //    mà không cần bất kỳ câu lệnh rẽ nhánh (if/else) nào!
    // ------------------------------------------------------------------------
    constexpr void evaluate_branchless_pixel(
        float x, float y,
        float inR, float inG, float inB, float inA,
        float& outR, float& outG, float& outB, float& outA
    ) const noexcept {
        const float w = spatial_weight(x, y);

        const float effPremulR = premulR * w;
        const float effPremulG = premulG * w;
        const float effPremulB = premulB * w;
        const float effAlpha   = alpha   * w;

        const float effTransR = 1.0f - w * (1.0f - transR);
        const float effTransG = 1.0f - w * (1.0f - transG);
        const float effTransB = 1.0f - w * (1.0f - transB);
        const float effOneMinusA = 1.0f - w * (1.0f - oneMinusAlpha);

        outR = effPremulR + effTransR * inR;
        outG = effPremulG + effTransG * inG;
        outB = effPremulB + effTransB * inB;
        outA = effAlpha   + effOneMinusA * inA;
    }

    // ------------------------------------------------------------------------
    // 6. Thực thi SIMD128 / AVX2 song song trên 4 Pixel cùng lúc (0 Branch Divergence)
    // ------------------------------------------------------------------------
    void evaluate_4pixels_simd(
        const float* x4, const float* y4,
        const float* inR4, const float* inG4, const float* inB4, const float* inA4,
        float* outR4, float* outG4, float* outB4, float* outA4
    ) const noexcept;

    // ------------------------------------------------------------------------
    // 7. Thuật toán gập chuỗi toán tử theo thứ tự Z-index bằng con trỏ Stockfish
    // ------------------------------------------------------------------------
    [[nodiscard]] static MathOp fold_chain(const MathOp* ops, u32 count) noexcept;

    // ------------------------------------------------------------------------
    // 8. Kiểm chứng định lý kết hợp đại số: (A . B) . C == A . (B . C)
    // ------------------------------------------------------------------------
    [[nodiscard]] static bool verify_associativity(
        const MathOp& a,
        const MathOp& b,
        const MathOp& c,
        float epsilon = MONOID_ASSOC_EPSILON
    ) noexcept;
};

static_assert(sizeof(MathOp) == 64, "MathOp must be exactly 64 bytes (1 L1 Cacheline)");

} // namespace stockcut
