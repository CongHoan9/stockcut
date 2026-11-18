// ============================================================================
// CoreEngine - SlotStateTracker (Stockfish-Grade Zero-Map Delta Tracker)
// ============================================================================
#pragma once

#include "types.h"
#include "shared_state.h"
#include "data_structures.h"

namespace stockcut {

constexpr u32   SLOT_TRACKER_SUBSLOTS    = 2u;
constexpr u32   SLOT_TRACKER_MASK        = MAX_TIMELINE_SLOTS - 1u;
constexpr float SLOT_POS_MOVE_EPSILON    = 0.5f;
constexpr float SLOT_ROT_MOVE_EPSILON    = 0.01f;

struct alignas(16) SlotDeltaState {
    float x0;
    float y0;
    float x1;
    float y1;
    float opacity;
    float rotDeg;
    float spinDeg;
    u32   extraKey;
    u32   seenFrame;
    u32   stillN;
    bool  active;
};

class SlotStateTracker {
public:
    SlotStateTracker();
    void reset();

    u32 evaluate_delta(
        u32 frameTick,
        float stageW,
        float stageH,
        double* outDirtyBuf,
        u32 maxDirtyRects
    );

    const SlotDeltaState& get_state(u32 slot, u32 subSlot) const {
        return m_states[slot & SLOT_TRACKER_MASK][subSlot & 1u];
    }

    static void push_rect(double* buf, u32& count, u32 maxN, float x0, float y0, float x1, float y1);

private:
    MultiArray<SlotDeltaState, MAX_TIMELINE_SLOTS, SLOT_TRACKER_SUBSLOTS> m_states; // KHONG CO {}
    u64 m_activeMask = 0ULL;
    u32 m_lastFrameTick = 0;
};

extern SlotStateTracker g_slotTracker;

} // namespace stockcut
