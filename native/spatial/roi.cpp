// ============================================================================
// CoreEngine - Anonymous Algebra Reverse-DAG RoD / RoI Optimizer
// ============================================================================
#include "roi.h"

namespace stockcut {

void natron_optimize_render_plan_algebra(
    u64 algebraWord,
    const Rect<float>& requestedRoI,
    const Rect<float>& inputRoD,
    double nowMs, double startMs, double endMs,
    float param0, float param1,
    float W, float H,
    NatronRenderPlan& outPlan
) noexcept {
    Rect<float> active = requestedRoI;
    active.intersectWith(inputRoD);
    if (active.isEmpty()) {
        outPlan.renderRoI = Rect<float>{0.0f, 0.0f, 0.0f, 0.0f};
        outPlan.tileMask64 = 0ULL;
        outPlan.isIdentity = 1u;
        outPlan.isCulled = 1u;
        outPlan.splitRectCount = 0u;
        return;
    }

    if (effect_algebra::effect_algebra_is_identity(
            algebraWord, nowMs, startMs, endMs, param0, param1)) {
        outPlan.renderRoI = active;
        outPlan.tileMask64 = 0ULL;
        outPlan.isIdentity = 1u;
        outPlan.isCulled = 0u;
        outPlan.splitRectCount = 1u;
        return;
    }

    Rect<float> roi{};
    effect_algebra::effect_algebra_compute_roi(
        algebraWord, nowMs, startMs, endMs, W, H,
        active.x0, active.y0, active.x1, active.y1,
        roi
    );
    const float timeSec = static_cast<float>(max(0.0, nowMs - startMs) * 0.001);
    const u64 tileMask = effect_algebra::compute_pullback_tile_mask(
        algebraWord, param0, param1, 0.45f, W, H, timeSec,
        effect_algebra::TileSummary64::full_active()
    );

    outPlan.renderRoI = roi;
    outPlan.tileMask64 = tileMask;
    outPlan.isIdentity = 0u;
    outPlan.isCulled = (outPlan.renderRoI.isEmpty() || tileMask == 0ULL) ? 1u : 0u;
    outPlan.splitRectCount = (outPlan.isCulled == 0u) ? 1u : 0u;
}

} // namespace stockcut
