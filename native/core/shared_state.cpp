// ============================================================================
// CoreEngine - Shared State & Master Clock Driver
// ============================================================================
#include "shared_state.h"
#include "position.h"
#include "telemetry.h"
#include "manager.h"

namespace stockcut {

alignas(64) static EngineSharedState g_sharedState = {
    .currentTimeMs       = 0.0,
    .projectDurationMs   = DEFAULT_PROJECT_DURATION_MS,
    .playbackState       = PLAYBACK_STOPPED,
    .activeItemsCount    = 0,
    .activeMask          = 0ULL,
    .frameIndex          = 0,
    .playbackRate        = 1.0f,
    .commandCount        = 0,
    .zobristHashLow      = 0,
    .zobristHashHigh     = 0,
    .renderDecision      = 0,
    .flags               = SHARED_FLAG_NONE,
    .activeBgColorRgba   = 0,
    .activeTextColorRgba = 0,
    .activeBassHitIdx    = 0,
    .stateTransferPolicy = static_cast<u32>(StateTransferPolicy::FullTransfer),
    .padding             = {0}
};

alignas(64) static SharedRenderState g_sharedRenderState{};

EngineSharedState* engine_get_shared_state() {
    return &g_sharedState;
}

SharedRenderState* engine_get_shared_render_state() {
    return &g_sharedRenderState;
}

template <bool EmitDisplayList, bool BumpFrameIndex>
static u32 sync_clock_position(double timeMs) {
    Position<>* pos = timeline_position_get_instance();
    if (!pos) return 0u;
    StateInfo<>* nextSt = timeline_position_advance(static_cast<i64>(timeMs));
    u32 cmdCount = 0u;
    if constexpr (EmitDisplayList) {
        cmdCount = timeline_position_display_list(nextSt);
        g_sharedState.commandCount = cmdCount;
    }
    const u64 mask = pos->active_items();
    g_sharedState.activeMask = mask;
    g_sharedState.activeItemsCount = BitOps<u64>::popcount64(mask);
    if constexpr (BumpFrameIndex) {
        ++g_sharedState.frameIndex;
    }
    return cmdCount;
}

template <bool EmitDisplayList>
static u32 tick_clock(double deltaMs) {
    if (g_sharedState.playbackState == PLAYBACK_PLAYING) {
        g_sharedState.currentTimeMs += deltaMs * static_cast<double>(g_sharedState.playbackRate);
        if (g_sharedState.projectDurationMs > 0.0 && g_sharedState.currentTimeMs >= g_sharedState.projectDurationMs) {
            g_sharedState.currentTimeMs = 0.0;
            g_sharedState.flags |= SHARED_FLAG_LOOPED;
            Position<>* pos = timeline_position_get_instance();
            if (pos) {
                pos->invalidate_history();
            }
            timeline_position_display_list_invalidate();
            engineManager().gpuQuadStream().reset();
        }
    }
    TelemetryBuffer* telem = telemetry_get_ptr();
    if (telem) {
        telem->nowMs = g_sharedState.currentTimeMs;
        telem->flags = (g_sharedState.playbackState == PLAYBACK_PLAYING) ? 1u : 0u;
    }
    return sync_clock_position<EmitDisplayList, true>(g_sharedState.currentTimeMs);
}

} // namespace stockcut

using namespace stockcut;

extern "C" {

WASM_EXPORT(wasm_get_shared_state_ptr) EngineSharedState* wasm_get_shared_state_ptr() {
    return &g_sharedState;
}

WASM_EXPORT(wasm_clock_play) void wasm_clock_play() {
    g_sharedState.playbackState = PLAYBACK_PLAYING;
}

WASM_EXPORT(wasm_clock_pause) void wasm_clock_pause() {
    g_sharedState.playbackState = PLAYBACK_PAUSED;
}

WASM_EXPORT(wasm_clock_stop) void wasm_clock_stop() {
    g_sharedState.playbackState = PLAYBACK_STOPPED;
    g_sharedState.currentTimeMs = 0.0;
}

WASM_EXPORT(wasm_clock_set_rate) void wasm_clock_set_rate(float rate) {
    if (rate > 0.0f && rate <= MAX_PLAYBACK_RATE) {
        g_sharedState.playbackRate = rate;
    }
}

WASM_EXPORT(wasm_clock_set_duration) void wasm_clock_set_duration(double durMs) {
    if (durMs >= 0.0) {
        g_sharedState.projectDurationMs = durMs;
    }
}

WASM_EXPORT(wasm_clock_seek) u32 wasm_clock_seek(double timeMs) {
    if (timeMs < 0.0) timeMs = 0.0;
    if (g_sharedState.projectDurationMs > 0.0 && timeMs > g_sharedState.projectDurationMs) {
        timeMs = g_sharedState.projectDurationMs;
    }
    g_sharedState.currentTimeMs = timeMs;
    g_sharedState.flags |= SHARED_FLAG_SEEKED;
    Position<>* pos = timeline_position_get_instance();
    if (pos) {
        pos->invalidate_history();
    }
    timeline_position_display_list_invalidate();
    engineManager().gpuQuadStream().reset();
    return sync_clock_position<true, false>(timeMs);
}

WASM_EXPORT(wasm_clock_tick) u32 wasm_clock_tick(double deltaMs) { return tick_clock<true>(deltaMs); }
WASM_EXPORT(wasm_clock_tick_preview) void wasm_clock_tick_preview(double deltaMs) { tick_clock<false>(deltaMs); }

} // extern "C"
