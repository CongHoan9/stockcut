// ============================================================================
// STOCKFISH-STYLE DO_EDIT_MOVE(..., DIRTY) & NNUE_CUT PROJECT BRAIN
// ============================================================================
#pragma once
#include "types.h"
#include "position.h"
#include "artistic_intelligence.hpp"
#include "context_memory.hpp"
#include "video_agent_runtime.hpp"
#include "data_structures.h"

namespace stockcut {

constexpr u32   NNUE_PERSPECTIVES      = 2u;
constexpr usize NNUE_USER_INTENT_CAP   = 512;
constexpr usize NNUE_REPORT_JSON_CAP   = 4096;
constexpr usize NNUE_MEMORY_CHUNK_CAP  = 384;
constexpr usize NNUE_FEATURE_DIFF_CAP  = 128;

enum EditMoveType : u8 {
    EDIT_MOVE_NONE          = 0,
    EDIT_MOVE_FULL_SYNC     = 1,
    EDIT_MOVE_ADD_SLOT      = 2,
    EDIT_MOVE_REMOVE_SLOT   = 3,
    EDIT_MOVE_MUTATE_SLOT   = 4,
    EDIT_MOVE_MUTATE_KF     = 5,
    EDIT_MOVE_MUTATE_STYLE  = 6,
    EDIT_MOVE_MUTATE_TIMING = 7
};

enum SlotPropDirtyBits : u32 {
    PROP_DIRTY_NONE      = 0u,
    PROP_DIRTY_TYPE      = 1u << 0,
    PROP_DIRTY_TIMING    = 1u << 1,
    PROP_DIRTY_GEOMETRY  = 1u << 2,
    PROP_DIRTY_ZINDEX    = 1u << 3,
    PROP_DIRTY_RESOURCE  = 1u << 4,
    PROP_DIRTY_ROT_SPIN  = 1u << 5,
    PROP_DIRTY_KEYFRAMES = 1u << 6,
    PROP_DIRTY_TEXT      = 1u << 7,
    PROP_DIRTY_BASS      = 1u << 8,
    PROP_DIRTY_ALL_MASK  = 0x1FFu
};

enum SlotMutationAction : u8 {
    SLOT_ACTION_NONE    = 0,
    SLOT_ACTION_ADDED   = 1,
    SLOT_ACTION_REMOVED = 2,
    SLOT_ACTION_MUTATED = 3
};

struct EditMove {
    u16 raw = 0;
    EditMoveType type = EDIT_MOVE_NONE;
    u8 slot = 0;
    u32 propMask = 0;
    u64 valueHash = 0;
    static constexpr EditMove make(EditMoveType t, u8 s, u32 props, u64 h = 0) noexcept {
        EditMove m{};
        m.raw = static_cast<u16>((static_cast<u16>(t & 0x7u) << 12) | ((props & 0x3Fu) << 6) | (s & 0x3Fu));
        m.type = t;
        m.slot = s;
        m.propMask = props;
        m.valueHash = h;
        return m;
    }
};

struct SlotMutation {
    u8 slot = 0;
    u8 oldType = 0;
    u8 newType = 0;
    u8 action = SLOT_ACTION_NONE;
    u32 changedPropsMask = 0;
    u64 oldSlotHash = 0;
    u64 newSlotHash = 0;
};

// Pha E: Compact 16-byte Stockfish-Style Incremental Slot Delta (replaces 112B SlotSnapshot & 7.2KB ProjectSnapshot)
struct alignas(16) SlotMutationDelta {
    u64 key = 0ULL;       // 0..7: Unique slot digest
    u32 subCheck = 0;     // 8..11: Packed sub-channel 8-bit tags [timing|kf|geom|style]
    u16 featureIdx = 0;   // 12..13: NNUE accumulator feature bucket
    u8  slot = 0;         // 14
    u8  type = 0;         // 15: MediaType
};
static_assert(sizeof(SlotMutationDelta) == 16, "SlotMutationDelta must fit in 16 bytes");

// STRICT COLD PATH ENFORCEMENT (GEMINI.md Rule 7 & 9)
struct HistoryDirties {
    Bitboard addedSlotsBB = 0ULL;
    Bitboard removedSlotsBB = 0ULL;
    Bitboard mutatedSlotsBB = 0ULL;
    u32 configDirtyMask = 0;
    ValueList<SlotMutation, MAX_TIMELINE_SLOTS> mutations;
    ValueList<u32, NNUE_FEATURE_DIFF_CAP> addedFeatures;
    ValueList<u32, NNUE_FEATURE_DIFF_CAP> removedFeatures;
    inline void clear() noexcept {
        addedSlotsBB = 0ULL;
        removedSlotsBB = 0ULL;
        mutatedSlotsBB = 0ULL;
        configDirtyMask = 0;
        mutations.clear();
        addedFeatures.clear();
        removedFeatures.clear();
    }
    inline Bitboard all_dirty_bb() const noexcept {
        return addedSlotsBB | removedSlotsBB | mutatedSlotsBB;
    }
};

struct EditStateInfo {
    Key projectKey = 0ULL;
    u32 editPly = 0;
    EditMove lastMove;          // KHONG CO {}
    HistoryDirties dirty;       // KHONG CO {}
    float nnueScore = 0.0f;
    float artisticScore = 0.0f;
    EditStateInfo* previous = nullptr;
};

struct AestheticBreakdown {
    float overallScore = 0.0f;
    float rhythmSyncScore = 0.0f;
    float compositionScore = 0.0f;
    float temporalPacingScore = 0.0f;
    float typographyScore = 0.0f;
};

class NNUECutProjectBrain {
public:
    static constexpr u32 ACCUMULATOR_DIM   = 256;
    static constexpr u32 FEATURE_BUCKETS   = 1024;
    static constexpr u32 EDIT_HISTORY_RING = 64;

    static NNUECutProjectBrain& instance() noexcept;
    void init() noexcept;
    void on_project_loaded(const Position<>& pos) noexcept;
    void on_delta_move(const Position<>& pos) noexcept;
    void set_user_intent(const char* intentUtf8, u32 len) noexcept;

    [[nodiscard]] float nnue_score() const noexcept { ensure_lazy_scores(); return currentState_ ? currentState_->nnueScore : 0.0f; }
    [[nodiscard]] float artistic_score() const noexcept { ensure_lazy_scores(); return currentState_ ? currentState_->artisticScore : 0.0f; }
    [[nodiscard]] u32 edit_ply() const noexcept { return currentState_ ? currentState_->editPly : 0; }
    [[nodiscard]] Key project_key() const noexcept { return currentState_ ? currentState_->projectKey : 0ULL; }
    [[nodiscard]] Bitboard last_dirty_bb() const noexcept { return currentState_ ? currentState_->dirty.all_dirty_bb() : Bitboard(0); }
    [[nodiscard]] u32 last_mutation_count() const noexcept { return currentState_ ? static_cast<u32>(currentState_->dirty.mutations.size()) : 0u; }
    [[nodiscard]] bool is_lazy_scores_dirty() const noexcept { return scoresDirty_; }
    [[nodiscard]] bool is_lazy_report_dirty() const noexcept { return reportDirty_; }
    [[nodiscard]] const char* report_json_ptr() const noexcept { ensure_lazy_report(); return reportJson_.data(); }
    [[nodiscard]] u32 report_json_length() const noexcept { ensure_lazy_report(); return reportJsonLen_; }
    [[nodiscard]] const AestheticBreakdown& aesthetic_breakdown() const noexcept { ensure_lazy_scores(); return lastBreakdown_; }
    bitctx::ContextMemory& context_memory() noexcept { return memory_; }
    bitagent::VideoAgentPlan& agent_plan() noexcept { return agentPlan_; }
    EditStateInfo* alloc_edit_state() noexcept;

private:
    NNUECutProjectBrain() noexcept { init(); }

    template <bool InitialLoad>
    void sync_project_state(const Position<>& pos) noexcept;

    [[nodiscard]] Key compose_project_key(const Position<>& pos) const noexcept;
    void update_accumulator_incremental(const HistoryDirties& dirty) noexcept;
    [[nodiscard]] float evaluate_accumulator() const noexcept;
    void ensure_lazy_scores() const noexcept;
    void ensure_lazy_report() const noexcept;

    struct alignas(16) ArtisticCountsSnapshot {
        u16 totalItems = 0;
        u16 videoImageCount = 0;
        u16 lyricTextCount = 0;
        u16 audioCount = 0;
        u16 effectCount = 0;
        u16 keyframedCount = 0;
        u16 bassReactiveCount = 0;
        u16 pad = 0;
    };

    bool initialized_ = false;
    mutable bool scoresDirty_ = false;
    mutable bool reportDirty_ = false;
    alignas(64) MultiArray<i16, NNUE_PERSPECTIVES, ACCUMULATOR_DIM> accumulator_;
    alignas(64) MultiArray<i16, NNUE_PERSPECTIVES, ACCUMULATOR_DIM> outputWeights_;
    MultiArray<EditStateInfo, EDIT_HISTORY_RING> stateRing_;
    MultiArray<SlotMutationDelta, MAX_TIMELINE_SLOTS> committedDeltas_; // KHONG CO {}
    Bitboard committedOccBB_{0ULL};
    TimePoint committedDurationMs_ = 0;
    float committedStageW_ = DEFAULT_STAGE_WIDTH_F32;
    float committedStageH_ = DEFAULT_STAGE_HEIGHT_F32;
    u32 stateCursor_ = 0;
    mutable EditStateInfo* currentState_ = nullptr;
    ArtisticCountsSnapshot cachedArtCounts_; // KHONG CO {}
    mutable bitart::ArtisticIntelligence artistic_; // KHONG CO {}
    mutable AestheticBreakdown lastBreakdown_;      // KHONG CO {}
    mutable bitctx::ContextMemory memory_;          // KHONG CO {}
    bitagent::VideoAgentPlan agentPlan_;            // KHONG CO {}
    MultiArray<char, NNUE_USER_INTENT_CAP> userIntent_;
    mutable MultiArray<char, NNUE_REPORT_JSON_CAP> reportJson_;
    mutable u32 reportJsonLen_ = 0;
};

} // namespace stockcut
