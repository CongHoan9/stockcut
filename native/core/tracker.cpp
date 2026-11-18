// ============================================================================
// CoreEngine - SlotStateTracker Implementation (Direct Evaluated Read)
// ============================================================================
#include "tracker.h"
#include "math_utils.h"
#include "dirty_rect.h"
#include "position.h"

namespace stockcut {

SlotStateTracker g_slotTracker;

SlotStateTracker::SlotStateTracker()
    : m_activeMask(0ULL)
    , m_lastFrameTick(0)
{
    reset();
}

void SlotStateTracker::reset() {
    m_states.clear();
    m_activeMask = 0ULL;
    m_lastFrameTick = 0;
}

void SlotStateTracker::push_rect(double* buf, u32& count, u32 maxN, float x0, float y0, float x1, float y1) {
    if (count >= maxN) return;
    const u32 idx = count << 2;
    buf[idx + 0] = static_cast<double>(x0);
    buf[idx + 1] = static_cast<double>(y0);
    buf[idx + 2] = static_cast<double>(x1);
    buf[idx + 3] = static_cast<double>(y1);
    ++count;
}

u32 SlotStateTracker::evaluate_delta(
    u32 frameTick,
    float stageW,
    float stageH,
    double* outDirtyBuf,
    u32 maxDirtyRects
) {
    u32 dirtyCount = 0;
    u64 activeVisMask = 0ULL;
    Position<>* const pos = timeline_position_get_instance();
    if (pos && pos->has_items()) {
        const float scaleX = (pos->stage_w() > 0.0f) ? (stageW / pos->stage_w()) : 1.0f;
        const float scaleY = (pos->stage_h() > 0.0f) ? (stageH / pos->stage_h()) : 1.0f;
        const StateInfo<>* st = pos->state();
        Mask activeVisual = pos->active_items() & ~pos->by_type_bb<MediaType::Audio>();
        if (st) {
            activeVisual &= ~st->occludedSlotsBB;
        }
        activeVisual &= pos->eval_visible_bb();

        activeVisMask = activeVisual.word(0);

        // 1. Newly entered slots (activeVisMask & ~m_activeMask): 0 delta compare, 1 dirty rect
        u64 enteredMask = activeVisMask & ~m_activeMask;
        while (enteredMask) {
            const u32 slot = BitOps<u64>::pop_lsb(enteredMask);
            const Evaluated& ev = pos->evaluated_slot(slot);
            Rect<float> bounds;
            bool isContinuous = false;
            u32 extraKey = 0u;
            pos->resolve_slot_visual_bounds(
                slot, stageW, stageH, scaleX, scaleY, bounds, isContinuous, extraKey);
            const float rotDeg = (pos->item(slot).type == MediaType::Text || pos->item(slot).type == MediaType::Lyric)
                ? (ev.filter > 0u ? 0.0f : static_cast<float>(ev.flags))
                : ev.rotateDeg;

            SlotDeltaState& dst = m_states[slot & SLOT_TRACKER_MASK][0];
            dst.x0 = bounds.x0; dst.y0 = bounds.y0; dst.x1 = bounds.x1; dst.y1 = bounds.y1;
            dst.opacity = ev.opacity; dst.rotDeg = rotDeg; dst.extraKey = extraKey;
            dst.seenFrame = frameTick; dst.stillN = 0; dst.active = true;
            push_rect(outDirtyBuf, dirtyCount, maxDirtyRects, bounds.x0, bounds.y0, bounds.x1, bounds.y1);
        }

        // 2. Staying slots (activeVisMask & m_activeMask): check delta only
        u64 stayingMask = activeVisMask & m_activeMask;
        while (stayingMask) {
            const u32 slot = BitOps<u64>::pop_lsb(stayingMask);
            const Evaluated& ev = pos->evaluated_slot(slot);
            Rect<float> bounds;
            bool isContinuous = false;
            u32 extraKey = 0u;
            pos->resolve_slot_visual_bounds(
                slot, stageW, stageH, scaleX, scaleY, bounds, isContinuous, extraKey);
            const float rotDeg = (pos->item(slot).type == MediaType::Text || pos->item(slot).type == MediaType::Lyric)
                ? (ev.filter > 0u ? 0.0f : static_cast<float>(ev.flags))
                : ev.rotateDeg;
            const float opacity = ev.opacity;

            SlotDeltaState& dst = m_states[slot & SLOT_TRACKER_MASK][0];
            const bool moved = (abs_val(dst.x0 - bounds.x0) > SLOT_POS_MOVE_EPSILON) |
                               (abs_val(dst.y0 - bounds.y0) > SLOT_POS_MOVE_EPSILON) |
                               (abs_val(dst.x1 - bounds.x1) > SLOT_POS_MOVE_EPSILON) |
                               (abs_val(dst.y1 - bounds.y1) > SLOT_POS_MOVE_EPSILON) |
                               (abs_val(dst.opacity - opacity) > DELTA_EPSILON) |
                               (abs_val(dst.rotDeg - rotDeg) > SLOT_ROT_MOVE_EPSILON) |
                               (dst.extraKey != extraKey) |
                               isContinuous;
            if (moved) {
                push_rect(outDirtyBuf, dirtyCount, maxDirtyRects, dst.x0, dst.y0, dst.x1, dst.y1);
                push_rect(outDirtyBuf, dirtyCount, maxDirtyRects, bounds.x0, bounds.y0, bounds.x1, bounds.y1);
                dst.x0 = bounds.x0; dst.y0 = bounds.y0; dst.x1 = bounds.x1; dst.y1 = bounds.y1;
                dst.opacity = opacity; dst.rotDeg = rotDeg; dst.extraKey = extraKey;
                dst.stillN = 0;
            } else {
                ++dst.stillN;
            }
            dst.seenFrame = frameTick;
        }
    }

    // 3. Exited slots (m_activeMask & ~activeVisMask): O(popcount(exited)) instead of scanning 128 elements
    u64 exitedMask = m_activeMask & ~activeVisMask;
    while (exitedMask) {
        const u32 slot = BitOps<u64>::pop_lsb(exitedMask);
        SlotDeltaState& dst = m_states[slot & SLOT_TRACKER_MASK][0];
        push_rect(outDirtyBuf, dirtyCount, maxDirtyRects, dst.x0, dst.y0, dst.x1, dst.y1);
        dst.active = false;
        dst.stillN = 0;
    }

    m_activeMask = activeVisMask;
    m_lastFrameTick = frameTick;
    return dirtyCount;
}

} // namespace stockcut

extern "C" {

WASM_EXPORT(wasm_slot_tracker_reset) void wasm_slot_tracker_reset() {
    stockcut::g_slotTracker.reset();
}

WASM_EXPORT(wasm_slot_tracker_evaluate) stockcut::u32 wasm_slot_tracker_evaluate(
    stockcut::u32 frameTick, float stageW, float stageH, stockcut::u32 maxDirtyRects
) {
    using namespace stockcut;
    double* dirtyBuf = reinterpret_cast<double*>(dirty_get_buf());
    return g_slotTracker.evaluate_delta(frameTick, stageW, stageH, dirtyBuf, maxDirtyRects);
}

WASM_EXPORT(wasm_slot_tracker_get_still_n) stockcut::u32 wasm_slot_tracker_get_still_n(stockcut::u32 slot, stockcut::u32 subSlot) {
    return stockcut::g_slotTracker.get_state(slot, subSlot).stillN;
}

WASM_EXPORT(wasm_slot_tracker_get_bounds) void wasm_slot_tracker_get_bounds(
    stockcut::u32 slot, stockcut::u32 subSlot, float* out4
) {
    if (!out4) return;
    const auto& st = stockcut::g_slotTracker.get_state(slot, subSlot);
    out4[0] = st.x0;
    out4[1] = st.y0;
    out4[2] = st.x1;
    out4[3] = st.y1;
}

} // extern "C"
