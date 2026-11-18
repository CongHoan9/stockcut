// ============================================================================
// CoreEngine - Stockfish SharedState & Master Clock Buffer (64-byte Aligned)
// ============================================================================
#pragma once

#include "types.h"
#include <atomic>

namespace stockcut {

enum class StateTransferPolicy : u32 {
    ZeroTransfer    = 0, // ReuseScene / ZOBRIST_SKIP (0 VBO bytes uploaded)
    PartialTransfer = 1, // StaticChunked dirty pages via g_gpuQueue + glScissor
    FullTransfer    = 2  // DynamicDense contiguous VBO upload + full pass chain
};

enum PlaybackState : u32 {
    PLAYBACK_STOPPED = 0u,
    PLAYBACK_PLAYING = 1u,
    PLAYBACK_PAUSED  = 2u
};

enum SharedStateFlag : u32 {
    SHARED_FLAG_NONE       = 0u,
    SHARED_FLAG_LOOPED     = 1u << 0,
    SHARED_FLAG_SEEKED     = 1u << 1,
    SHARED_FLAG_AUDIO_SYNC = 1u << 2
};

constexpr double DEFAULT_PROJECT_DURATION_MS = 30000.0;
constexpr float  MAX_PLAYBACK_RATE           = 16.0f;
constexpr u32    ENGINE_SHARED_PADDING_WORDS = 13u;

// RelaxedAtomic<T> is canonically defined in types.h

#pragma pack(push, 8)
struct alignas(64) EngineSharedState {
    double   currentTimeMs;
    double   projectDurationMs;
    u32      playbackState;
    u32      activeItemsCount;
    u64      activeMask;
    u32      frameIndex;
    float    playbackRate;
    u32      commandCount;
    u32      zobristHashLow;
    u32      zobristHashHigh;
    u32      renderDecision;
    u32      flags;
    u32      activeBgColorRgba;
    u32      activeTextColorRgba;
    u32      activeBassHitIdx;
    u32      stateTransferPolicy;
    u32      padding[ENGINE_SHARED_PADDING_WORDS];
};
#pragma pack(pop)
static_assert(sizeof(EngineSharedState) == 128, "EngineSharedState must be exactly 128 bytes");

EngineSharedState* engine_get_shared_state();

struct alignas(64) SharedRenderState {
    RelaxedAtomic<u64> completedMask{0ULL};
    RelaxedAtomic<u64> dirtySlotsMask{0ULL};
    RelaxedAtomic<u32> transferPolicy{static_cast<u32>(StateTransferPolicy::FullTransfer)};
    RelaxedAtomic<u32> gpuUploadedBytes{0u};
    RelaxedAtomic<u32> gpuTransferCmdCount{0u};
    RelaxedAtomic<u32> dirtyPagesSynced{0u};

    template <StateTransferPolicy Policy>
    inline void record_transfer(u64 dirtyBB, u32 bytes, u32 cmds, u32 pages) noexcept {
        constexpr u32 kPolicyU32 = static_cast<u32>(Policy);
        dirtySlotsMask.store(dirtyBB);
        transferPolicy.store(kPolicyU32);
        gpuUploadedBytes.store(bytes);
        gpuTransferCmdCount.store(cmds);
        dirtyPagesSynced.store(pages);
        EngineSharedState* ss = engine_get_shared_state();
        if (ss) {
            ss->stateTransferPolicy = kPolicyU32;
            ss->padding[0] = bytes;
            ss->padding[1] = cmds;
            ss->padding[2] = pages;
            ss->padding[3] = static_cast<u32>(dirtyBB & INVALID_U32);
            ss->padding[4] = static_cast<u32>((dirtyBB >> 32) & INVALID_U32);
        }
    }

    template <StateTransferPolicy Policy>
    inline void record_transfer(u32 bytes) noexcept {
        record_transfer<Policy>(dirtySlotsMask.load(), bytes, gpuTransferCmdCount.load(), dirtyPagesSynced.load());
    }
};

SharedRenderState* engine_get_shared_render_state();

} // namespace stockcut
