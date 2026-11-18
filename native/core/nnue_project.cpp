// ============================================================================
// STOCKFISH-STYLE DO_EDIT_MOVE(..., DIRTY) & NNUE_CUT PROJECT BRAIN
// ============================================================================
#include "nnue_project.h"
#include "hash_primitives.h"
#include "string_parse.h"
#include "audio/script.h"
#include "effect/param_arena.h"
#include <cstdio>
#include <cstring>

namespace stockcut {
namespace {

constexpr i16   NNUE_ACCUM_BIAS      = 32;
constexpr i32   NNUE_CRELU_MAX       = 127;
constexpr float NNUE_SCORE_BASE      = 50.0f;
constexpr float NNUE_SCORE_INV_SCALE = 1.0f / 512.0f;
constexpr float NNUE_BLEND_ACCUM     = 0.55f;
constexpr float NNUE_BLEND_ARTISTIC  = 0.45f;
constexpr float DELTA_STAGE_EPS      = 0.5f;

inline u64 qf(float v) noexcept {
    return static_cast<u64>(static_cast<i64>(__builtin_llround(static_cast<double>(v) * 16.0)));
}

inline i16 feature_weight(u32 perspective, u32 featureIdx, u32 dim) noexcept {
    u64 seed = (static_cast<u64>(perspective + 1u) * HASH_GOLDEN_64) ^
               (static_cast<u64>(featureIdx + 1u) * SPLITMIX_MUL1) ^
               (static_cast<u64>(dim + 1u) * SPLITMIX_MUL2);
    seed ^= seed >> 33;
    seed *= MURMUR64_MUL1;
    seed ^= seed >> 33;
    return static_cast<i16>(static_cast<i32>(seed & 0x7Fu) - 60);
}

template <i32 Sign>
inline void apply_feature_delta(
    MultiArray<i16, NNUE_PERSPECTIVES, NNUECutProjectBrain::ACCUMULATOR_DIM>& accum,
    u32 featureIdx
) noexcept {
    for (u32 p = 0; p < NNUE_PERSPECTIVES; ++p) {
        i16* row = accum[p].data();
        for (u32 d = 0; d < NNUECutProjectBrain::ACCUMULATOR_DIM; ++d) {
            row[d] = static_cast<i16>(row[d] + Sign * feature_weight(p, featureIdx, d));
        }
    }
}

[[nodiscard]] inline SlotMutationDelta make_slot_delta(const Position<>& pos, u32 slot) noexcept {
    SlotMutationDelta d;
    const TimelineItem& it = pos.item(slot);
    d.slot = static_cast<u8>(slot);
    d.type = static_cast<u8>(it.type);
    if (it.type == MediaType::None) {
        d.key = 0ULL;
        d.subCheck = 0u;
        d.featureIdx = 0u;
        return d;
    }
    pos.compute_slot_commit_digest(slot, d.key, d.subCheck);
    d.featureIdx = static_cast<u16>(
        (d.key ^ (static_cast<u64>(slot) * (ZOBRIST_MUL_A >> 32))) &
        (NNUECutProjectBrain::FEATURE_BUCKETS - 1u));
    return d;
}

} // namespace

NNUECutProjectBrain& NNUECutProjectBrain::instance() noexcept {
    static NNUECutProjectBrain brain;
    return brain;
}

void NNUECutProjectBrain::init() noexcept {
    if (initialized_) return;
    initialized_ = true;
    scoresDirty_ = false;
    reportDirty_ = false;
    committedDeltas_.fill(SlotMutationDelta{});
    cachedArtCounts_ = ArtisticCountsSnapshot{};
    lastBreakdown_ = AestheticBreakdown{};
    artistic_.reset_preferences();
    artistic_.set_intent("cinematic dynamic rhythmic typography");
    memory_.clear();
    memory_.set_scope(1ULL, 1ULL);
    agentPlan_.clear();
    for (u32 p = 0; p < NNUE_PERSPECTIVES; ++p) {
        for (u32 d = 0; d < ACCUMULATOR_DIM; ++d) {
            outputWeights_[p][d] = static_cast<i16>((p == 0 ? 18 : 14) + static_cast<i32>((d * 7u) % 13u) - 6);
        }
    }
    copy_cstr_bounded(userIntent_.data(), NNUE_USER_INTENT_CAP, "StockCut Autonomous C++ Timeline & Artistic Sync");
    reportJson_[0] = '\0';
    reportJsonLen_ = 0u;
}

EditStateInfo* NNUECutProjectBrain::alloc_edit_state() noexcept {
    EditStateInfo& st = stateRing_[stateCursor_ & (EDIT_HISTORY_RING - 1u)];
    ++stateCursor_;
    st.projectKey = 0ULL;
    st.editPly = 0u;
    st.lastMove = EditMove{};
    st.dirty.clear();
    st.nnueScore = 0.0f;
    st.artisticScore = 0.0f;
    st.previous = nullptr;
    return &st;
}

Key NNUECutProjectBrain::compose_project_key(const Position<>& pos) const noexcept {
    u64 pKey = hash_mix64(static_cast<u64>(pos.duration()), (qf(pos.stage_w()) << 32) ^ qf(pos.stage_h()));
    Bitboard occ = committedOccBB_;
    while (occ) {
        const u32 s = occ.pop_lsb();
        pKey = hash_mix64(pKey, committedDeltas_[s].key ^ (static_cast<u64>(s + 1u) * HASH_GOLDEN_64));
    }
    return pKey ? pKey : 1ULL;
}

void NNUECutProjectBrain::update_accumulator_incremental(const HistoryDirties& dirty) noexcept {
    for (const u32* f = dirty.removedFeatures.begin(), *const e = dirty.removedFeatures.end(); f < e; ++f) {
        apply_feature_delta<-1>(accumulator_, *f);
    }
    for (const u32* f = dirty.addedFeatures.begin(), *const e = dirty.addedFeatures.end(); f < e; ++f) {
        apply_feature_delta<+1>(accumulator_, *f);
    }
}

float NNUECutProjectBrain::evaluate_accumulator() const noexcept {
    i32 sum = 0;
    for (u32 p = 0; p < NNUE_PERSPECTIVES; ++p) {
        for (u32 d = 0; d < ACCUMULATOR_DIM; ++d) {
            const i32 crelu = clamp(static_cast<i32>(accumulator_[p][d]), 0, NNUE_CRELU_MAX);
            sum += crelu * static_cast<i32>(outputWeights_[p][d]);
        }
    }
    return clamp(NNUE_SCORE_BASE + static_cast<float>(sum) * NNUE_SCORE_INV_SCALE, 0.0f, 100.0f);
}

void NNUECutProjectBrain::ensure_lazy_scores() const noexcept {
    if (!scoresDirty_ || !currentState_) return;
    scoresDirty_ = false;

    bitart::ArtisticIntelligence::Features f; // KHONG CO {}
    const ArtisticCountsSnapshot& c = cachedArtCounts_;
    const float itemNorm = clamp(static_cast<float>(c.totalItems) / 16.0f, 0.1f, 1.0f);

    f[0]  = 0.55f;
    f[1]  = 0.72f;
    f[2]  = 0.56f;
    f[3]  = 0.52f;
    f[4]  = 0.76f;
    f[5]  = 0.45f;
    f[6]  = clamp(0.68f + 0.20f * itemNorm, 0.0f, 1.0f);
    f[7]  = (c.videoImageCount > 0) ? 0.82f : 0.72f;
    f[8]  = 0.62f;
    f[9]  = 0.18f;
    f[10] = 0.42f;
    f[11] = c.keyframedCount > 0 ? 0.86f : 0.72f;
    f[12] = c.effectCount > 0 ? 0.80f : 0.55f;
    f[13] = 0.58f;
    f[14] = c.bassReactiveCount > 0 ? 0.90f : 0.68f;
    f[15] = c.audioCount > 0 ? 0.78f : 0.52f;
    f[16] = 0.64f;
    f[17] = c.effectCount > 0 ? 0.48f : 0.22f;
    f[18] = 0.86f;
    f[19] = c.lyricTextCount > 0 ? 0.92f : 0.80f;
    f[20] = 0.85f;
    f[21] = 0.68f;
    f[22] = 0.46f;
    f[23] = 0.90f;

    const float overall = artistic_.score(f);
    artistic_.set_candidate(0, edit_ply() + 1u, f.data(), bitart::kFeatureDims);
    lastBreakdown_.overallScore        = overall * 100.0f;
    lastBreakdown_.rhythmSyncScore     = (0.5f * f[14] + 0.5f * f[15]) * 100.0f;
    lastBreakdown_.compositionScore    = (0.5f * f[6] + 0.5f * f[7]) * 100.0f;
    lastBreakdown_.temporalPacingScore = (0.5f * f[11] + 0.5f * f[18]) * 100.0f;
    lastBreakdown_.typographyScore     = f[19] * 100.0f;

    currentState_->artisticScore = lastBreakdown_.overallScore;
    currentState_->nnueScore = NNUE_BLEND_ACCUM * evaluate_accumulator() + NNUE_BLEND_ARTISTIC * currentState_->artisticScore;
}

void NNUECutProjectBrain::ensure_lazy_report() const noexcept {
    if (!reportDirty_) return;
    reportDirty_ = false;
    if (!currentState_) {
        reportJson_[0] = '\0';
        reportJsonLen_ = 0u;
        return;
    }
    ensure_lazy_scores();

    const HistoryDirties& d = currentState_->dirty;
    MultiArray<char, NNUE_MEMORY_CHUNK_CAP> chunkBuf; // KHONG CO {}
    const int n = std::snprintf(
        chunkBuf.data(), NNUE_MEMORY_CHUNK_CAP,
        "do_edit_move ply=%u type=%u slot=%u dirtyBB=0x%llx added=%u removed=%u mutated=%u score=%.1f intent=%s",
        currentState_->editPly,
        static_cast<u32>(currentState_->lastMove.type),
        static_cast<u32>(currentState_->lastMove.slot),
        static_cast<u64>(d.all_dirty_bb()),
        BitOps<u64>::popcount64(d.addedSlotsBB),
        BitOps<u64>::popcount64(d.removedSlotsBB),
        BitOps<u64>::popcount64(d.mutatedSlotsBB),
        currentState_->nnueScore,
        userIntent_.data());
    if (n > 0) {
        memory_.add(chunkBuf.data(), static_cast<usize>(n), 1, 200, currentState_->editPly);
    }

    const int written = std::snprintf(
        reportJson_.data(), NNUE_REPORT_JSON_CAP,
        "{\"projectKey\":\"0x%016llx\",\"editPly\":%u,\"nnueScore\":%.2f,\"artisticScore\":%.2f,"
        "\"rhythmSync\":%.2f,\"composition\":%.2f,\"pacing\":%.2f,\"typography\":%.2f,"
        "\"dirtyMask\":\"0x%016llx\",\"addedSlots\":%u,\"removedSlots\":%u,\"mutatedSlots\":%u,"
        "\"memoryEntries\":%u,\"durationMs\":%lld}",
        static_cast<u64>(currentState_->projectKey),
        currentState_->editPly,
        currentState_->nnueScore,
        currentState_->artisticScore,
        lastBreakdown_.rhythmSyncScore,
        lastBreakdown_.compositionScore,
        lastBreakdown_.temporalPacingScore,
        lastBreakdown_.typographyScore,
        static_cast<u64>(d.all_dirty_bb()),
        BitOps<u64>::popcount64(d.addedSlotsBB),
        BitOps<u64>::popcount64(d.removedSlotsBB),
        BitOps<u64>::popcount64(d.mutatedSlotsBB),
        static_cast<u32>(memory_.size()),
        static_cast<i64>(committedDurationMs_));
    reportJsonLen_ = (written > 0 && static_cast<usize>(written) < NNUE_REPORT_JSON_CAP)
                         ? static_cast<u32>(written)
                         : 0u;
}

template <bool InitialLoad>
void NNUECutProjectBrain::sync_project_state(const Position<>& pos) noexcept {
    init();
    EditStateInfo* st = alloc_edit_state();

    const Bitboard newOcc = pos.populated_slots();
    if constexpr (InitialLoad) {
        st->previous = nullptr;
        st->editPly = 1;
        committedDeltas_.fill(SlotMutationDelta{});
        accumulator_.fill(NNUE_ACCUM_BIAS);
        committedOccBB_ = newOcc;
        st->dirty.addedSlotsBB = newOcc;

        Bitboard scan = newOcc;
        while (scan) {
            const u32 s = scan.pop_lsb();
            const SlotMutationDelta d = make_slot_delta(pos, s);
            committedDeltas_[s] = d;
            apply_feature_delta<+1>(accumulator_, d.featureIdx);
        }
        st->lastMove = EditMove::make(EDIT_MOVE_FULL_SYNC, 0, PROP_DIRTY_ALL_MASK, 0ULL);
    } else {
        st->previous = currentState_;
        st->editPly = currentState_ ? (currentState_->editPly + 1u) : 1u;

        st->dirty.configDirtyMask =
            static_cast<u32>(committedDurationMs_ != pos.duration()) |
            (static_cast<u32>(abs_val(committedStageW_ - pos.stage_w()) > DELTA_STAGE_EPS ||
                              abs_val(committedStageH_ - pos.stage_h()) > DELTA_STAGE_EPS) << 1);

        const Bitboard oldOcc = committedOccBB_;
        u8 primarySlot = 0;
        EditMoveType primaryType = EDIT_MOVE_NONE;
        u32 combinedProps = 0;
        u64 combinedHash = 0ULL;

        Bitboard addedBB = newOcc & ~oldOcc;
        st->dirty.addedSlotsBB = addedBB;
        while (addedBB) {
            const u32 s = addedBB.pop_lsb();
            const SlotMutationDelta d = make_slot_delta(pos, s);
            committedDeltas_[s] = d;
            SlotMutation mut{static_cast<u8>(s), 0, d.type, SLOT_ACTION_ADDED, PROP_DIRTY_ALL_MASK, 0ULL, d.key};
            st->dirty.mutations.push_back(mut);
            st->dirty.addedFeatures.push_back(d.featureIdx);
            if (primaryType == EDIT_MOVE_NONE) {
                primarySlot = static_cast<u8>(s);
                primaryType = EDIT_MOVE_ADD_SLOT;
            }
            combinedProps |= PROP_DIRTY_ALL_MASK;
            combinedHash = hash_mix64(combinedHash, d.key);
        }

        Bitboard removedBB = oldOcc & ~newOcc;
        st->dirty.removedSlotsBB = removedBB;
        while (removedBB) {
            const u32 s = removedBB.pop_lsb();
            const SlotMutationDelta oldD = committedDeltas_[s];
            committedDeltas_[s].key = 0ULL;
            committedDeltas_[s].subCheck = 0u;
            committedDeltas_[s].featureIdx = 0u;
            committedDeltas_[s].type = 0u;
            SlotMutation mut{static_cast<u8>(s), oldD.type, 0, SLOT_ACTION_REMOVED, PROP_DIRTY_ALL_MASK, oldD.key, 0ULL};
            st->dirty.mutations.push_back(mut);
            st->dirty.removedFeatures.push_back(oldD.featureIdx);
            if (primaryType == EDIT_MOVE_NONE) {
                primarySlot = static_cast<u8>(s);
                primaryType = EDIT_MOVE_REMOVE_SLOT;
            }
            combinedProps |= PROP_DIRTY_ALL_MASK;
            combinedHash = hash_mix64(combinedHash, oldD.key);
        }

        Bitboard commonBB = oldOcc & newOcc;
        while (commonBB) {
            const u32 s = commonBB.pop_lsb();
            const SlotMutationDelta oldD = committedDeltas_[s];
            const SlotMutationDelta newD = make_slot_delta(pos, s);
            if (oldD.key == newD.key) continue;

            const u32 subDiff = oldD.subCheck ^ newD.subCheck;
            u32 propMask =
                (-static_cast<u32>(oldD.type != newD.type) & PROP_DIRTY_TYPE) |
                (-static_cast<u32>((subDiff & 0x000000FFu) != 0u) & PROP_DIRTY_TIMING) |
                (-static_cast<u32>((subDiff & 0x0000FF00u) != 0u) & PROP_DIRTY_KEYFRAMES) |
                (-static_cast<u32>((subDiff & 0x00FF0000u) != 0u) & (PROP_DIRTY_GEOMETRY | PROP_DIRTY_ROT_SPIN | PROP_DIRTY_ZINDEX)) |
                (-static_cast<u32>((subDiff & 0xFF000000u) != 0u) & (PROP_DIRTY_TEXT | PROP_DIRTY_BASS | PROP_DIRTY_RESOURCE));
            propMask |= (-static_cast<u32>(propMask == PROP_DIRTY_NONE) & PROP_DIRTY_TEXT);

            committedDeltas_[s] = newD;
            st->dirty.mutatedSlotsBB |= (1ULL << s);
            SlotMutation mut{static_cast<u8>(s), oldD.type, newD.type, SLOT_ACTION_MUTATED, propMask, oldD.key, newD.key};
            st->dirty.mutations.push_back(mut);
            st->dirty.removedFeatures.push_back(oldD.featureIdx);
            st->dirty.addedFeatures.push_back(newD.featureIdx);

            if (primaryType == EDIT_MOVE_NONE) {
                primarySlot = static_cast<u8>(s);
                if (propMask & PROP_DIRTY_KEYFRAMES) primaryType = EDIT_MOVE_MUTATE_KF;
                else if (propMask & PROP_DIRTY_TIMING) primaryType = EDIT_MOVE_MUTATE_TIMING;
                else if (propMask & PROP_DIRTY_TEXT) primaryType = EDIT_MOVE_MUTATE_STYLE;
                else primaryType = EDIT_MOVE_MUTATE_SLOT;
            }
            combinedProps |= propMask;
            combinedHash = hash_mix64(combinedHash, newD.key);
        }

        if (primaryType == EDIT_MOVE_NONE && st->dirty.configDirtyMask != 0u) {
            primaryType = EDIT_MOVE_FULL_SYNC;
        }
        committedOccBB_ = newOcc;
        update_accumulator_incremental(st->dirty);
        st->lastMove = EditMove::make(primaryType, primarySlot, combinedProps, combinedHash);
    }

    committedDurationMs_ = pos.duration();
    committedStageW_ = pos.stage_w();
    committedStageH_ = pos.stage_h();

    // Capture lightweight scalar counts for Lazy Artistic/Report evaluation (0 pointers, 0 snprintf in edit hot path)
    u16 bassReactiveCount = 0;
    Bitboard occ = pos.populated_slots();
    while (occ) {
        const u32 s = occ.pop_lsb();
        bassReactiveCount += static_cast<u16>(pos.get_bass_params(s).isBassActive != 0);
    }
    const auto* script = get_active_music_script();
    if (script && script->totalSamples > 0) bassReactiveCount |= 1u;

    cachedArtCounts_.totalItems        = static_cast<u16>(pos.populated_slots().popcount());
    cachedArtCounts_.videoImageCount   = static_cast<u16>((pos.by_type_bb<MediaType::Video>() | pos.by_type_bb<MediaType::Image>()).popcount());
    cachedArtCounts_.lyricTextCount    = static_cast<u16>((pos.by_type_bb<MediaType::Lyric>() | pos.by_type_bb<MediaType::Text>()).popcount());
    cachedArtCounts_.audioCount        = static_cast<u16>(pos.by_type_bb<MediaType::Audio>().popcount());
    cachedArtCounts_.effectCount       = static_cast<u16>(effect_catalog::global_effect_param_arena().instanceCount);
    cachedArtCounts_.keyframedCount    = static_cast<u16>(pos.by_dynamic_axis_bb<DynamicAxis::Keyframe>().popcount());
    cachedArtCounts_.bassReactiveCount = bassReactiveCount;

    st->projectKey = compose_project_key(pos);
    currentState_ = st;
    scoresDirty_ = true;
    reportDirty_ = true;
}

void NNUECutProjectBrain::on_project_loaded(const Position<>& pos) noexcept {
    sync_project_state<true>(pos);
}

void NNUECutProjectBrain::on_delta_move(const Position<>& pos) noexcept {
    sync_project_state<false>(pos);
}

void NNUECutProjectBrain::set_user_intent(const char* intentUtf8, u32 len) noexcept {
    if (!intentUtf8 || len == 0) return;
    const u32 copyLen = len < (NNUE_USER_INTENT_CAP - 1) ? len : static_cast<u32>(NNUE_USER_INTENT_CAP - 1);
    std::memcpy(userIntent_.data(), intentUtf8, copyLen);
    userIntent_[copyLen] = '\0';
    artistic_.set_intent(StringSpan{userIntent_.data(), copyLen});
}

// ============================================================================
// WASM C-ABI EXPORTS FOR NNUE_CUT & AGENT BRAIN
// ============================================================================
WASM_EXPORT(wasm_nnue_cut_get_score) float wasm_nnue_cut_get_score() {
    return NNUECutProjectBrain::instance().nnue_score();
}
WASM_EXPORT(wasm_nnue_cut_get_artistic_score) float wasm_nnue_cut_get_artistic_score() {
    return NNUECutProjectBrain::instance().artistic_score();
}
WASM_EXPORT(wasm_nnue_cut_get_edit_ply) u32 wasm_nnue_cut_get_edit_ply() {
    return NNUECutProjectBrain::instance().edit_ply();
}
WASM_EXPORT(wasm_nnue_cut_get_mutation_count) u32 wasm_nnue_cut_get_mutation_count() {
    return NNUECutProjectBrain::instance().last_mutation_count();
}
WASM_EXPORT(wasm_nnue_cut_get_dirty_mask_low) u32 wasm_nnue_cut_get_dirty_mask_low() {
    return static_cast<u32>(NNUECutProjectBrain::instance().last_dirty_bb().value);
}
WASM_EXPORT(wasm_nnue_cut_get_dirty_mask_high) u32 wasm_nnue_cut_get_dirty_mask_high() {
    return static_cast<u32>(NNUECutProjectBrain::instance().last_dirty_bb().value >> 32);
}
WASM_EXPORT(wasm_nnue_cut_get_report_json_ptr) u32 wasm_nnue_cut_get_report_json_ptr() {
    return static_cast<u32>(reinterpret_cast<usize>(NNUECutProjectBrain::instance().report_json_ptr()));
}
WASM_EXPORT(wasm_nnue_cut_get_report_json_length) u32 wasm_nnue_cut_get_report_json_length() {
    return NNUECutProjectBrain::instance().report_json_length();
}
WASM_EXPORT(wasm_nnue_cut_set_intent) void wasm_nnue_cut_set_intent(const char* ptr, u32 len) {
    NNUECutProjectBrain::instance().set_user_intent(ptr, len);
}

} // namespace stockcut
