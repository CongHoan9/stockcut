// ============================================================================
// native/property/effect.h
// CoreEngine - Autonomous Anonymous Effect Ontology (Self-Contained Effect<Block>)
// ----------------------------------------------------------------------------
// Kiến trúc Vô Danh Tuyệt Đối + 64-bit algebraWord:
// 1. Phân Tầng Bộ Nhớ Trong Block:
//    - reg[0..3] : Common Effect Prefix (std140 vec4 u_effectMeta)
//                  reg[0] = effectHash (u32)
//                  reg[1] = targetSlot (u32, 0xFFFF = toàn cục)
//                  reg[2] = shaderKernelId (u32, GPU shader index)
//                  reg[3] = masterIntensity (float, 0.0 .. 1.0)
//    - reg[4..]  : Specialized Parameters của từng hiệu ứng
//    - bits[0..15] : Common Effect Bits (enabled, isMonoidFusible, dirtyUbo, blendMode, execMode)
//    - bits[16..]  : Specialized Bits / Enums của từng hiệu ứng
// 2. struct Effect<B>:
//    - Kế thừa trực tiếp Block<N, M> + mang trực tiếp u64 algebraWord (0 Enum!)
//    - fold_into(FusedUberEffect& acc): O(1) non-branching Monoid composition
// ============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include "../core/types.h"
#include "block.h"
#include "../effect/monoid_operator.h"
#include "../effect/algebra.h"

namespace stockcut {

// Con trỏ hàm gập Monoid O(1)
template <typename BlockT>
using EffectMonoidFoldFn = void (*)(FusedUberEffect& acc, const BlockT& blk) noexcept;

// ============================================================================
// CẤU TRÚC EFFECT TỰ CHỦ VÔ DANH (EFFECT<BLOCK>)
// ============================================================================
template <typename B = Block<96, 2>>
struct alignas(alignof(B)) Effect : public B {
    u64                   algebraWord = 0ULL; // 4-bit ABI | 7-bit Domain | 5-bit Traits | 8-bit Aux
    EffectMonoidFoldFn<B> foldFn      = nullptr;

    [[gnu::always_inline]] inline void clear() noexcept {
        B::clear();
        algebraWord = 0ULL;
        foldFn = nullptr;
    }

    [[nodiscard]] [[gnu::always_inline]] inline bool is_identity_bypass() const noexcept {
        if (!this->template get_value<bool, 0>()) return true;
        if ((algebraWord & effect_algebra::TRAIT_IDENTITY_GATE) != 0ULL) {
            const u8 gReg = effect_algebra::gate_reg(algebraWord);
            if (gReg < B::kFloatCount && abs_val(this->reg[gReg]) <= IDENTITY_EPSILON) {
                return true;
            }
        }
        return false;
    }

    // DUNG HỢP MONOID PHI RẼ NHÁNH O(1)
    [[gnu::always_inline]] inline void fold_into(FusedUberEffect& acc) const noexcept {
        if (foldFn && this->template get_value<bool, 0>()) {
            foldFn(acc, *this);
        }
    }
};

// ============================================================================
// TOÁN TỬ MONOID DUNG HỢP TỪ THANH GHI CHUYÊN BIỆT (BẮT ĐẦU TỪ reg[4])
// ============================================================================
template <typename BlockT = Block<16, 1>>
inline void fold_zoom(FusedUberEffect& acc, const BlockT& blk) noexcept {
    const float speed = blk.template get_value<4>();
    const float amp   = blk.template get_value<5>();
    const float mix   = blk.template get_value<3>();
    const float sc    = max(0.01f, amp * mix);
    acc.uvTransform = MonoidComposer<Transform2D>::compose(
        acc.uvTransform, Transform2D::from_zoom(sc, speed, false)
    );
}

template <typename BlockT = Block<16, 1>>
inline void fold_shake(FusedUberEffect& acc, const BlockT& blk) noexcept {
    const float speed = blk.template get_value<4>();
    const float amp   = blk.template get_value<5>();
    const float tilt  = blk.template get_value<6>();
    const float fade  = blk.template get_value<7>();
    const float mix   = blk.template get_value<3>();
    const float sc    = max(0.01f, amp * mix);
    acc.uvTransform = MonoidComposer<Transform2D>::compose(
        acc.uvTransform, Transform2D::from_shake(sc, speed, tilt, fade, 1920.0f, 1080.0f)
    );
}

template <typename BlockT = Block<16, 1>>
inline void fold_brightness(FusedUberEffect& acc, const BlockT& blk) noexcept {
    const float intensity = blk.template get_value<4>();
    const float mix       = blk.template get_value<3>();
    acc.colorTransfer = MonoidComposer<ColorTransferOp>::compose(
        acc.colorTransfer, ColorTransferOp::from_brightness(intensity * mix)
    );
}

} // namespace stockcut
