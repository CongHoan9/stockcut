// ============================================================================
// CoreEngine - Frame Pipeline Orchestrator Implementation
// ============================================================================
#include "pipeline.h"
#include "zobrist.h"
#include "transposition_table.h"
#include "engine.h"
#include "display_list.h"
#include "telemetry.h"
#include "position.h"
#include "shared_state.h"

namespace stockcut
{
    namespace
    {
        alignas(16) PipelineResult g_pipelineResult{};

        struct FrameHashCache
        {
            const Position<>* position = nullptr;
            const StateInfo<>*        state    = nullptr;
            u32       stateGeneration    = 0;
            u32       configurationEpoch = 0;
            TimePoint timeMs             = 0;
            Bitboard  active             = 0;
            u32       width              = 0;
            u32       height             = 0;
            u32       styleEpoch         = 0;
            u32       fontsVersion       = 0;
            u32       styleVersion       = 0;
            u32       mediaVersion       = 0;
            u32       backgroundHash     = 0;
            Key       positionKey        = 0;
            Key       hash               = 0;
            bool      valid              = false;
        };

        template <bool HasContinuous>
        FrameHashCache g_timelineFrameHashCache{};

        constexpr u8 PIPELINE_TT_TIER_LEGACY     = 0u;
        constexpr u8 PIPELINE_TT_TIER_DISCRETE   = 1u;
        constexpr u8 PIPELINE_TT_TIER_CONTINUOUS = 3u;

        inline void pipeline_reset_result_header(u64 currentHash, u64 activeMask) noexcept
        {
            g_pipelineResult.currentHash = currentHash;
            g_pipelineResult.culledCount = 0u;
            g_pipelineResult.ttData      = 0u;
            g_pipelineResult.ttTier      = 0u;
            g_pipelineResult.activeMask  = activeMask;
        }

        template <bool IsPlaying>
        inline void pipeline_sync_telemetry(
            TelemetryBuffer* telem, double nowMs, u32 frameIndex,
            u64 currentHash, u32 baseW, u32 baseH, u32 W, u32 H
        ) noexcept
        {
            if (!telem) return;
            telem->nowMs       = nowMs;
            telem->frameIndex  = frameIndex;
            telem->currentHash = currentHash;
            telem->baseW       = baseW;
            telem->baseH       = baseH;
            telem->previewW    = W;
            telem->previewH    = H;
            telem->flags       = (telem->flags & ~1u) | (IsPlaying ? 1u : 0u);
            telem->heartbeat++;
        }

        inline bool pipeline_try_zobrist_cutoff(
            u64 currentHash, u64 lastFrameHash, EngineSharedState* ss, TelemetryBuffer* telem
        ) noexcept
        {
            if (lastFrameHash == 0ULL || currentHash != lastFrameHash) return false;
            g_pipelineResult.decision = PIPELINE_DECISION_ZOBRIST_SKIP;
            if (ss) ss->renderDecision = PIPELINE_DECISION_ZOBRIST_SKIP;
            if (telem) {
                telem->renderMode = PIPELINE_DECISION_ZOBRIST_SKIP;
                telem->zobristBreakers++;
            }
            return true;
        }

        inline bool pipeline_try_tt_restore(
            u64 currentHash, EngineSharedState* ss, TelemetryBuffer* telem
        ) noexcept
        {
            auto* ttEntry = cache::get_global_display_list_tt().probe(currentHash);
            if (ttEntry && ttEntry->is_valid() && display_list_cache_restore(ttEntry->payload.cacheSlot, currentHash)) {
                g_pipelineResult.decision = PIPELINE_DECISION_TT_HIT;
                g_pipelineResult.ttData   = ttEntry->payload.cacheSlot;
                g_pipelineResult.ttTier   = ttEntry->payload.tier;
                if (ss) {
                    ss->renderDecision = PIPELINE_DECISION_TT_HIT;
                    ss->commandCount   = display_list_get_count();
                }
                if (telem) {
                    telem->renderMode = PIPELINE_DECISION_TT_HIT;
                    telem->ttHits++;
                }
                return true;
            }
            if (telem) telem->ttMisses++;
            return false;
        }

        inline PipelineResult* pipeline_commit_dirty(u64 currentHash, u8 tier) noexcept
        {
            const u32 cacheSlot = display_list_cache_save(currentHash, tier);
            if (cacheSlot != DL_CACHE_INVALID) {
                cache::get_global_display_list_tt().write(currentHash, {cacheSlot, tier, {}});
            }
            g_pipelineResult.decision = PIPELINE_DECISION_DIRTY;
            return &g_pipelineResult;
        }

        template <bool HasContinuous, bool IsPlaying>
        PipelineResult* pipeline_evaluate_timeline_impl(
            Position<>* pos,
            StateInfo<>* nextSt,
            Bitboard active,
            u32 W, u32 H,
            u32 styleEpoch, u32 fontsVer, u32 styleVer, u32 mediaVer, u32 bgHash,
            u64 lastFrameHash, u64 occludedMask,
            double nowMs, u32 frameIndex
        )
        {
            FrameHashCache& hashCache = g_timelineFrameHashCache<HasContinuous>;
            bool reusableHash = false;
            if constexpr (!HasContinuous) {
                reusableHash = hashCache.valid &&
                    hashCache.position == pos && hashCache.state == nextSt &&
                    hashCache.stateGeneration == nextSt->generation &&
                    hashCache.configurationEpoch == pos->configuration_epoch() &&
                    hashCache.timeMs == nextSt->timeMs &&
                    hashCache.active == active && hashCache.width == W &&
                    hashCache.height == H && hashCache.styleEpoch == styleEpoch &&
                    hashCache.fontsVersion == fontsVer &&
                    hashCache.styleVersion == styleVer &&
                    hashCache.mediaVersion == mediaVer &&
                    hashCache.backgroundHash == bgHash &&
                    nextSt->key == hashCache.positionKey;
            }

            u64 currentHash = reusableHash ? hashCache.hash : 0ULL;
            if (!reusableHash) {
                const TimePoint nowTp = static_cast<TimePoint>(nowMs);
                const u32 bgSlot = pos->active_background_slot(nowTp);
                if (bgSlot < MAX_TIMELINE_SLOTS) {
                    const u32 bgCol   = pos->resolve_bass_bg_color(bgSlot, nowTp);
                    const u32 textCol = pos->resolve_bass_text_color(bgSlot, nowTp);
                    if (EngineSharedState* ss = engine_get_shared_state()) {
                        ss->activeBgColorRgba   = bgCol;
                        ss->activeTextColorRgba = textCol;
                        if (pos->is_bass_active(bgSlot)) {
                            ss->activeBassHitIdx = pos->resolve_bass_hit_index(bgSlot, nowTp);
                        }
                    }
                }
                currentHash = (nextSt ? nextSt->key : 0ULL) ^
                              hzobrist_viewport_env(W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash);
                if (!currentHash) currentHash = 1ULL;
                hashCache = {
                    pos, nextSt, nextSt ? nextSt->generation : 0u, pos->configuration_epoch(),
                    nextSt ? nextSt->timeMs : 0LL, active, W, H, styleEpoch, fontsVer, styleVer,
                    mediaVer, bgHash, nextSt ? nextSt->key : 0ULL, currentHash, true
                };
            }

            pipeline_reset_result_header(currentHash, active);

            EngineSharedState* ss = engine_get_shared_state();
            if (ss) {
                ss->currentTimeMs    = nowMs;
                ss->activeMask       = active;
                ss->activeItemsCount = BitOps<u64>::popcount64(active);
                ss->frameIndex       = frameIndex;
                ss->zobristHashLow   = static_cast<u32>(currentHash & INVALID_U32);
                ss->zobristHashHigh  = static_cast<u32>((currentHash >> 32) & INVALID_U32);
            }

            const u32 stageW = static_cast<u32>(pos->stage_w() > 0.0f ? pos->stage_w() : static_cast<float>(W));
            const u32 stageH = static_cast<u32>(pos->stage_h() > 0.0f ? pos->stage_h() : static_cast<float>(H));
            TelemetryBuffer* telem = telemetry_get_ptr();
            pipeline_sync_telemetry<IsPlaying>(telem, nowMs, frameIndex, currentHash, stageW, stageH, W, H);

            if constexpr (!HasContinuous) {
                if (pipeline_try_zobrist_cutoff(currentHash, lastFrameHash, ss, telem)) {
                    return &g_pipelineResult;
                }
            }

            if (pipeline_try_tt_restore(currentHash, ss, telem)) {
                return &g_pipelineResult;
            }

            const u32 cmdCount = timeline_position_display_list(nextSt);
            if (ss) {
                ss->renderDecision = PIPELINE_DECISION_DIRTY;
                ss->commandCount   = cmdCount;
            }

            const u64 combinedOcclusion = occludedMask | (nextSt ? nextSt->occludedBB.value : 0ULL);
            if (cmdCount > 0u && combinedOcclusion != 0ULL) {
                const float cStageW = pos->stage_w() > 0.0f ? pos->stage_w() : static_cast<float>(W);
                const float cStageH = pos->stage_h() > 0.0f ? pos->stage_h() : static_cast<float>(H);
                const u32 culled = display_list_cull(combinedOcclusion, cStageW, cStageH);
                g_pipelineResult.culledCount = culled;
                if (telem) telem->culledCount = culled;
            }

            constexpr u8 tier = HasContinuous ? PIPELINE_TT_TIER_CONTINUOUS : PIPELINE_TT_TIER_DISCRETE;
            return pipeline_commit_dirty(currentHash, tier);
        }
    } // namespace

    PipelineResult* pipeline_get_result_ptr()
    {
        return &g_pipelineResult;
    }

    template <bool IsPlaying>
    PipelineResult* pipeline_evaluate_T(
        u32 W, u32 H,
        u32 styleEpoch, u32 fontsVer, u32 styleVer, u32 mediaVer, u32 bgHash,
        u32 count, u64 lastFrameHash, u64 occludedMask,
        double nowMs, u32 frameIndex
    )
    {
        const u64 currentHash = zobrist_compute(W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash, count);
        pipeline_reset_result_header(currentHash, 0ULL);

        Position<>* pos = timeline_position_get_instance();
        const u32 stageW = static_cast<u32>((pos && pos->stage_w() > 0.0f) ? pos->stage_w() : static_cast<float>(W));
        const u32 stageH = static_cast<u32>((pos && pos->stage_h() > 0.0f) ? pos->stage_h() : static_cast<float>(H));
        TelemetryBuffer* telem = telemetry_get_ptr();
        pipeline_sync_telemetry<IsPlaying>(telem, nowMs, frameIndex, currentHash, stageW, stageH, W, H);

        if (pipeline_try_zobrist_cutoff(currentHash, lastFrameHash, nullptr, telem)) {
            return &g_pipelineResult;
        }
        if (pipeline_try_tt_restore(currentHash, nullptr, telem)) {
            return &g_pipelineResult;
        }

        if (display_list_get_count() > 0u && occludedMask != 0ULL) {
            const u32 culled = display_list_cull(occludedMask, W, H);
            g_pipelineResult.culledCount = culled;
            if (telem) telem->culledCount = culled;
        }

        return pipeline_commit_dirty(currentHash, PIPELINE_TT_TIER_LEGACY);
    }

    template <bool IsPlaying>
    PipelineResult* pipeline_evaluate_position_T(
        u32 W, u32 H,
        u32 styleEpoch, u32 fontsVer, u32 styleVer, u32 mediaVer, u32 bgHash,
        u64 lastFrameHash, u64 occludedMask,
        double nowMs, u32 frameIndex
    )
    {
        Position<>* pos = timeline_position_get_instance();
        if (!pos) {
            g_pipelineResult.decision    = PIPELINE_DECISION_DIRTY;
            g_pipelineResult.currentHash = 0ULL;
            g_pipelineResult.activeMask  = 0ULL;
            return &g_pipelineResult;
        }

        StateInfo<>* nextSt = timeline_position_advance(static_cast<TimePoint>(nowMs));
        const Bitboard active = nextSt->activeBB.word(0);
        const bool hasContinuous = (nextSt->activeBB & pos->continuous_tracks_bb()).any();

        return hasContinuous
            ? pipeline_evaluate_timeline_impl<true, IsPlaying>(
                  pos, nextSt, active, W, H,
                  styleEpoch, fontsVer, styleVer, mediaVer, bgHash,
                  lastFrameHash, occludedMask, nowMs, frameIndex)
            : pipeline_evaluate_timeline_impl<false, IsPlaying>(
                  pos, nextSt, active, W, H,
                  styleEpoch, fontsVer, styleVer, mediaVer, bgHash,
                  lastFrameHash, occludedMask, nowMs, frameIndex);
    }

    template PipelineResult* pipeline_evaluate_T<false>(u32, u32, u32, u32, u32, u32, u32, u32, u64, u64, double, u32);
    template PipelineResult* pipeline_evaluate_T<true>(u32, u32, u32, u32, u32, u32, u32, u32, u64, u64, double, u32);
    template PipelineResult* pipeline_evaluate_position_T<false>(u32, u32, u32, u32, u32, u32, u32, u64, u64, double, u32);
    template PipelineResult* pipeline_evaluate_position_T<true>(u32, u32, u32, u32, u32, u32, u32, u64, u64, double, u32);

} // namespace stockcut
