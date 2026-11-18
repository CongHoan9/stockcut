// ============================================================================
// CoreEngine - Reverse-DAG RoD / RoI & Pullback Tile Optimization Engine
// ----------------------------------------------------------------------------
// Refactored (Zero-Enum): Operates directly on 64-bit anonymous algebraWord
// and 8x8 PullbackTileBitboard (deleted legacy EffectType & NATRON_PLAN_TABLE).
// ============================================================================
#pragma once
#include "types.h"
#include "dirty_rect.h"
#include "../effect/algebra.h"

namespace stockcut {

struct NatronRenderPlan {
    Rect<float> renderRoI{};
    u64         tileMask64     = ~0ULL; // Exact 8x8 active macro-tile bitboard
    u32         isIdentity     = 0;     // 1: Pass-through (0-cost), 0: Render needed
    u32         isCulled       = 0;     // 1: Completely outside screen / 0 tiles
    u32         splitRectCount = 0;     // Active tile/rect count
    u32         pad0           = 0;
};

void natron_optimize_render_plan_algebra(
    u64 algebraWord,
    const Rect<float>& requestedRoI,
    const Rect<float>& inputRoD,
    double nowMs, double startMs, double endMs,
    float param0, float param1,
    float W, float H,
    NatronRenderPlan& outPlan
) noexcept;

} // namespace stockcut
