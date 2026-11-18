// ============================================================================
// [KIẾN TRÚC STOCKFISH — PHA D: ĐƠN ENGINE TIMELINE + POST-FX COMPONENT & RENDER PASS PLANNER]:
// - Không tồn tại Engine Timeline thứ 2 sao chép RuntimeProperties / Keyframes / Intervals.
// - PostFxComponent (128 bytes = 2 L1 Cachelines, Zero Implicit Padding) chỉ giữ thông số
//   Post-Processing FX & Hash Seed được Compiler nạp 1 lần duy nhất cùng với Position<>.
// - RenderPassPlanner (Timeline<Mask>) đọc thẳng trạng thái active từ Position<> để dựng
//   RenderPass DAG + BlurKernel + PlanTT cho renderer.cpp.
// ============================================================================
#pragma once
#include "types.h"
#include "memory.h"
#include "data_structures.h"
#include "timeline/bitboard.h"
#include "cache/zobrist.h"
#include "core/history.h"
#include "core/nested_mark.h"
#include <limits>

namespace stockcut {
template <typename Mask> class Position;
}

namespace stockcut::timeline::project {
class JsonReader;
}

namespace stockcut::timeline::runtime {
constexpr u32 RUNTIME_TIMELINE_CAPACITY = MAX_TIMELINE_SLOTS;
constexpr u32 RUNTIME_INTERVAL_CHUNK_SHIFT = 5u;
constexpr u32 RUNTIME_INTERVAL_CHUNK_COUNT = 256u;
constexpr u8 RUNTIME_FRAME_FLAG_ZOOM_TILT_BASS = 1u;
constexpr u8 RUNTIME_FRAME_FLAG_LINEAR_TRANS   = 2u;
constexpr u8 RUNTIME_FRAME_FLAG_POST_PROCESS   = 4u;
constexpr u8 RUNTIME_ITEM_FX_FLAG_HAS_EFFECT    = 4u;
constexpr u8 RUNTIME_ITEM_FX_FLAG_BASS_REACTIVE = 8u;

enum RuntimeLoadStatus : u32 {
    RUNTIME_LOAD_OK = 0,
    RUNTIME_LOAD_COMPILE_FAILED = 1,
    RUNTIME_LOAD_RECORD_CAPACITY_EXCEEDED = 2,
    RUNTIME_LOAD_INVALID_COMPILED_RECORD = 3,
    RUNTIME_LOAD_UNSUPPORTED_FEATURE = 4
};

constexpr u32 RUNTIME_EFFECT_KIND_COUNT = EFFECT_KIND_NB;

enum class RuntimePassKind : u8 {
    ClearScene = 0,
    DrawLayer,
    Downsample,
    GaussianHorizontal,
    GaussianVertical,
    CompositeBlur,
    CompositeGlow,
    CompositeSceneBlur,
    ApplyBrightness,
    CopyScene,
    ApplyZoom,
    ApplyShake,
    BloomExtract,
    BloomDownsample,
    BloomUpsample,
    BloomComposite,
    ApplyShaderEffect,
    ApplyMonoidFused,
    Count,
    Sentinel = 0xFFu
};
constexpr u32 RUNTIME_PASS_KIND_COUNT =
    static_cast<u32>(RuntimePassKind::Count);
using PassKindBitmask = EnumBitmask<RuntimePassKind, u32>;
using EffectKindBitmask = EnumBitmask<EffectKind, u32>;

struct UniformJob {
    u16 index;
    RuntimePassKind kind;
    u8 pad;
};
static_assert(sizeof(UniformJob) == 4,
              "UniformJob must be 4 bytes with zero padding");

constexpr u32 RUNTIME_MAX_DOWNSAMPLE_LEVELS = 7;
constexpr u32 RUNTIME_BLUR_SCRATCH_TARGET_FIRST = 9;
constexpr u32 RUNTIME_RENDER_TARGET_COUNT =
    RUNTIME_BLUR_SCRATCH_TARGET_FIRST + RUNTIME_MAX_DOWNSAMPLE_LEVELS * 2;
constexpr u32 RUNTIME_MAX_PASSES_PER_ITEM = 24;
constexpr u32 RUNTIME_RENDER_PASS_CAPACITY =
    1 + RUNTIME_TIMELINE_CAPACITY * RUNTIME_MAX_PASSES_PER_ITEM;
constexpr u32 RUNTIME_BLUR_TAP_CAPACITY = 32;

// ============================================================================
// TẦNG RUNTIME: RenderPass (40 bytes — Zero Implicit Padding)
// ============================================================================
struct RenderPass {
    RuntimePassKind kind; // 0     (1B)
    u8 inputTarget;       // 1     (1B)
    u8 outputTarget;      // 2     (1B)
    u8 kernelIndex;       // 3     (1B)
    u32 slot;             // 4..7  (4B)
    u32 width;            // 8..11 (4B)
    u32 height;           // 12..15(4B)
    float radius;         // 16..19(4B)
    float intensity;      // 20..23(4B)
    float parameter0;     // 24..27(4B)
    float parameter1;     // 28..31(4B)
    float parameter2;     // 32..35(4B)
    float parameter3;     // 36..39(4B)
};
static_assert(sizeof(RenderPass) == 40,
              "RenderPass must be 40 bytes with zero padding");

struct BlurKernel {
    u8 tapCount;          // 0     (1B)
    u8 levelCount;        // 1     (1B)
    u16 kernelFlags;      // 2..3  (2B)
    MultiArray<float, RUNTIME_BLUR_TAP_CAPACITY + 1> weights; // 4..135 (132B)
};
static_assert(sizeof(BlurKernel) == 136,
              "BlurKernel must be 136 bytes with zero padding");

struct RenderTarget {
    u32 width;            // 0..3  (4B)
    u32 height;           // 4..7  (4B)
};
static_assert(sizeof(RenderTarget) == 8,
              "RenderTarget must be 8 bytes with zero padding");

// ============================================================================
// TẦNG POST-FX COMPONENT (128 bytes = 2 L1 Cachelines — Zero Implicit Padding)
// Thay thế hoàn toàn RuntimeItem (3,928 bytes) — giảm 96.7% RAM / slot.
// ============================================================================
enum FxBoolFlags : u8 {
    FX_FLAG_NONE              = 0,
    FX_FLAG_SQUARE            = 1u << 0,
    FX_FLAG_MIRROR            = 1u << 1,
    FX_FLAG_BASS_SOURCE       = 1u << 2,
    FX_FLAG_HAS_COLOR         = 1u << 3,
    FX_FLAG_STANDALONE_EFFECT = 1u << 4
};

struct alignas(64) PostFxComponent {
    // --- Nhóm 8-byte (32 bytes) ---
    double startMs = 0.0;                     // 0..7
    double endMs = 0.0;                       // 8..15
    u64 bakedEffectHashSeed = 0ULL;           // 16..23
    u64 bakedEffectHashMask = 0ULL;           // 24..31
    // --- Nhóm 4-byte (92 bytes) ---
    u32 resourceId = 0u;                      // 32..35
    u32 type = 0u;                            // 36..39 (MediaType)
    i32 zIndex = 0;                           // 40..43
    float effectRadius = 0.0f;                // 44..47
    float effectIntensity = 0.0f;             // 48..51
    float effectSpeed = 0.0f;                 // 52..55
    float effectBrightness = 0.0f;            // 56..59
    float effectBase = 0.0f;                  // 60..63
    float effectAmplitude = 0.0f;             // 64..67
    float effectPhase = 0.0f;                 // 68..71
    float effectTilt = 0.0f;                  // 72..75
    float effectFade = 0.0f;                  // 76..79
    float effectThreshold = 0.0f;             // 80..83
    float effectBloomFactor = 0.0f;           // 84..87
    float effectTolerance = 0.0f;             // 88..91
    float effectAa = 0.0f;                    // 92..95
    MultiArray<float, 3> effectColor{};       // 96..107
    ReciprocalFadeEnvelope bakedFadeEnvelope{}; // 108..115
    float bakedPassParam1 = 0.0f;             // 116..119
    float bakedTimeHashScale = 0.0f;          // 120..123
    // --- Nhóm 1-byte (4 bytes) ---
    EffectKind effect = EffectKind::None; // 124
    u8 fxFlags = 0u;                          // 125
    u8 bakedEffectFlags = 0u;                 // 126
    u8 active = 0u;                           // 127

    [[nodiscard]] constexpr bool effectSquare() const noexcept { return (fxFlags & FX_FLAG_SQUARE) != 0u; }
    [[nodiscard]] constexpr bool effectMirror() const noexcept { return (fxFlags & FX_FLAG_MIRROR) != 0u; }
    [[nodiscard]] constexpr bool effectBassSource() const noexcept { return (fxFlags & FX_FLAG_BASS_SOURCE) != 0u; }
    [[nodiscard]] constexpr bool effectHasColor() const noexcept { return (fxFlags & FX_FLAG_HAS_COLOR) != 0u; }
    [[nodiscard]] constexpr bool is_standalone_effect() const noexcept { return (fxFlags & FX_FLAG_STANDALONE_EFFECT) != 0u; }
    inline void set_fx_flag(u8 bit, bool enabled) noexcept {
        if (enabled) fxFlags |= bit;
        else fxFlags &= static_cast<u8>(~bit);
    }
};
static_assert(sizeof(PostFxComponent) == 128,
              "PostFxComponent must be 128 bytes with zero implicit padding");


struct SlotBassEnvelopeMeta {
    double hop = 0.0;
    u32 offset = 0;
    u32 count = 0;
};
static_assert(sizeof(SlotBassEnvelopeMeta) == 16);

// ============================================================================
// TẦNG FRAME FX LIVE STATE (32 bytes — Bỏ 56B RuntimeProperties + 8B Hash)
// ============================================================================
struct FrameItem {
    float effectIntensity; // 0..3
    float effectScale;     // 4..7
    float effectRotation;  // 8..11
    float effectShiftX;    // 12..15
    float effectShiftY;    // 16..19
    float elapsedMsF;      // 20..23
    u32 slot;              // 24..27
    i32 zIndex;            // 28..31
};
static_assert(sizeof(FrameItem) == 32,
              "FrameItem must be 32 bytes with zero implicit padding");

// ============================================================================
// TẦNG RUNTIME FRAME CONTAINER: Frame (Zero Implicit Padding)
// ============================================================================
template <typename Mask = Mask<1>>
struct Frame
{
    // --- Nhóm 8-byte (120 bytes) ---
    double timeMs;                                                       // 0..7   (8B)
    Mask activeMask;                                                     // 8..15  (8B)
    u64 frameZobristKey;                                                 // 16..23 (8B)
    u64 effectDynamicZobrist;                                            // 24..31 (8B)
    Mask activeFusibleEffectsBB;                                         // 32..39 (8B)
    MediaTypeBitboardMap byTypeMap;                                      // 40..111(72B)
    // --- Các mảng dữ liệu tĩnh ---
    MultiArray<FrameItem, Mask::BITS> items;                             // 64 * 32B = 2048B
    MultiArray<RenderPass, RUNTIME_RENDER_PASS_CAPACITY> passes;         // 1537 * 40B = 61480B
    MultiArray<BlurKernel, Mask::BITS> kernels;                          // 64 * 136B = 8704B
    MultiArray<RenderTarget, RUNTIME_RENDER_TARGET_COUNT> targets;       // 23 * 8B = 184B
    MultiArray<u8, Mask::BITS> slotMap;                                  // 64B
    // --- Nhóm trường nhỏ (24 bytes) ---
    u32 itemCount;                                                       // 4B
    u32 passCount;                                                       // 4B
    u32 kernelCount;                                                     // 4B
    PassKindBitmask passKindMask;                                        // 4B
    u16 activeEffectCount;                                               // 2B
    u8 hasBassReactiveItem;                                              // 1B
    u8 hasZoomTiltBass;                                                  // 1B
    u32 padFrame;                                                        // 4B

    inline bool hasLinearTransition() const noexcept { return (hasZoomTiltBass & RUNTIME_FRAME_FLAG_LINEAR_TRANS) != 0u; }
    inline bool hasPostProcessingEffect() const noexcept { return (hasZoomTiltBass & RUNTIME_FRAME_FLAG_POST_PROCESS) != 0u; }
    inline void resetHeader(double t) noexcept
    {
        timeMs = t;
        activeMask.clear();
        frameZobristKey = 0;
        effectDynamicZobrist = 0;
        activeFusibleEffectsBB.clear();
        byTypeMap.clear();
        itemCount = 0;
        passCount = 0;
        passes[0].kind = RuntimePassKind::Sentinel;
        kernelCount = 0;
        passKindMask.clear();
        activeEffectCount = 0;
        hasBassReactiveItem = 0;
        hasZoomTiltBass = 0;
        padFrame = 0;
    }
};
Frame() -> Frame<stockcut::Mask<1>>;
static_assert(
    sizeof(Frame<stockcut::Mask<1>>) ==
        112 + sizeof(FrameItem) * RUNTIME_TIMELINE_CAPACITY +
        sizeof(RenderPass) * RUNTIME_RENDER_PASS_CAPACITY +
        sizeof(BlurKernel) * RUNTIME_TIMELINE_CAPACITY +
        sizeof(RenderTarget) * RUNTIME_RENDER_TARGET_COUNT +
        RUNTIME_TIMELINE_CAPACITY + 24,
    "Frame must have zero implicit compiler padding");

template <RuntimePassKind Kind, typename Mask = stockcut::Mask<1>>
inline bool appendPass(
    Frame<Mask>& frame, u32 slot, u32 inputTarget, u32 outputTarget,
    u32 width, u32 height, float radius = 0.0f, float intensity = 1.0f,
    u32 kernelIndex = 0, float p0 = 0.0f, float p1 = 0.0f,
    float p2 = 0.0f, float p3 = 0.0f
) {
    if (frame.passCount >= RUNTIME_RENDER_PASS_CAPACITY - 1) return false;
    frame.passKindMask.set(Kind);
    frame.passes[frame.passCount++] = {
        Kind,
        static_cast<u8>(inputTarget),
        static_cast<u8>(outputTarget),
        static_cast<u8>(kernelIndex),
        slot,
        width,
        height,
        radius,
        intensity,
        p0,
        p1,
        p2,
        p3
    };
    frame.passes[frame.passCount].kind = RuntimePassKind::Sentinel;
    if constexpr (Kind != RuntimePassKind::ClearScene &&
                  Kind != RuntimePassKind::DrawLayer &&
                  Kind != RuntimePassKind::ApplyBrightness &&
                  Kind != RuntimePassKind::ApplyZoom &&
                  Kind != RuntimePassKind::ApplyShake &&
                  Kind != RuntimePassKind::ApplyMonoidFused) {
        frame.hasZoomTiltBass |= RUNTIME_FRAME_FLAG_POST_PROCESS;
    }
    return true;
}


// ============================================================================
// RENDER PASS PLANNER (Thay thế Engine Timeline thứ 2 — đọc trực tiếp Position<>)
// ============================================================================
template <typename Mask = Mask<1>>
class Timeline {
public:
    static constexpr usize CAPACITY = Mask::BITS;

    // Giao dịch 1 lượt trực tiếp từ Compiler (0 JSON re-parse)
    void begin_slot_stage(double stageWidth, double stageHeight, double durationMs) noexcept;
    RuntimeLoadStatus stage_slot_fx(
        u32 slot, MediaType type, double startMs, double endMs, i32 zIndex, u32 resourceId,
        const project::JsonReader& json, u32 itemStart, u32 itemEnd,
        const char* filterCStr = nullptr,
        bool isStandaloneEffect = false
    ) noexcept;
    // Incremental Merkle-AST fast path: reuse unchanged slot FX in O(1) (0 JSON re-parse)
    [[gnu::always_inline]] inline void stage_slot_reuse(u32 slot) noexcept {
        stagedItems_[slot] = items_[slot];
        stagedRainParams_[slot] = rainParams_[slot];
    }
    RuntimeLoadStatus commit_staged_slots(
        u32 count, RuntimeLoadStatus pendingError = RUNTIME_LOAD_OK
    ) noexcept;

    // Hot Path Evaluation đọc trực tiếp Position<>
    bool evaluate_from_position(
        const Position<Mask>* pos, double timeMs, Frame<Mask>& output
    ) const;
    bool renderFromPosition(
        const Position<Mask>& pos, double timeMs, u32 width, u32 height, Frame<Mask>& output
    ) const;
    bool evaluate(double timeMs, Frame<Mask>& output) const;
    bool renderAt(
        double timeMs, u32 width, u32 height, Frame<Mask>& output
    ) const;

    void clear() noexcept {
        for (u32 s = 0; s < itemCount_; ++s) {
            items_[s].effect = EffectKind::None;
            items_[s].bakedEffectFlags = 0u;
            items_[s].bakedEffectHashMask = 0ULL;
            items_[s].fxFlags = 0u;
            items_[s].active = 0u;
            bassMeta_[s].count = 0u;
        }
        byTypeBB_.clear();
        fusibleSlotsBB_.clear();
        postFxSlotsBB_.clear();
        dynamicFxSlotsBB_.clear();
        itemCount_ = 0;
        bassUsed_ = 0;
        durationMs_ = 0.0;
        currentTimeMs_ = 0.0;
        lastStructKey_ = 0;
        cachedPassCount_ = 0;
        cachedPlanValid_ = 0;
        cachedHasPostProcessing_ = 0;
        lastOutputToken_ = 0;
        activePlanTTIdx_ = INVALID_U32;
        cachedItemCount_ = 0;
        lastEvalEpoch_ = 0;
        cachedMomentEligible_ = 0u;
        cachedStaticFxZobrist_ = 0ULL;
        cachedActiveDynFxBB_.clear();
        momentStartMs_ = 0.0;
        momentEndMs_ = -1.0;
        lastRenderedTimeMs_ = -1.0;
        cachedMomentStaticOk_ = 0;
        planEpoch_ = nextPlanEpoch();
    }

    bool setBassEnvelopeChunk(
        u32 slot, double hop, u32 totalCount, u32 offset,
        u32 count, const float* interleaved
    );

    [[nodiscard]] double durationMs() const noexcept { return durationMs_; }
    [[nodiscard]] double currentTimeMs() const noexcept { return currentTimeMs_; }
    [[nodiscard]] u32 itemCount() const noexcept { return itemCount_; }
    [[nodiscard]] u32 bass_sample_count(u32 slot) const noexcept {
        return (slot < itemCount_) ? bassMeta_[slot].count : 0u;
    }
    [[nodiscard]] const PostFxComponent* item(u32 slot) const noexcept {
        return (slot < itemCount_) ? &items_[slot] : nullptr;
    }
    [[nodiscard]] const RainDropParams* rain_params(u32 slot) const noexcept {
        return (slot < Mask::BITS) ? &rainParams_[slot] : nullptr;
    }
    [[nodiscard]] const RainDropParams* rainParamsArena() const noexcept {
        return rainParams_.data();
    }
    [[nodiscard]] const MultiArray<PostFxComponent, Mask::BITS>& itemsArena() const noexcept {
        return items_;
    }
    [[nodiscard]] u64 lastStructKey() const noexcept { return lastStructKey_; }
    [[nodiscard]] constexpr Mask dynamic_fx_slots_bb() const noexcept { return dynamicFxSlotsBB_; }
    [[nodiscard]] constexpr Mask post_fx_slots_bb() const noexcept { return postFxSlotsBB_; }

private:
    template <bool FromPosition = true>
    bool renderFromMaskAndSource(
        const Position<Mask>* pos, double timeMs, u32 width, u32 height, Frame<Mask>& output
    ) const;

    MultiArray<PostFxComponent, Mask::BITS> items_;         // KHONG CO {}
    MultiArray<PostFxComponent, Mask::BITS> stagedItems_;   // KHONG CO {}
    MultiArray<RainDropParams, Mask::BITS> rainParams_;     // KHONG CO {}
    MultiArray<RainDropParams, Mask::BITS> stagedRainParams_; // KHONG CO {}
    MultiArray<SlotBassEnvelopeMeta, Mask::BITS> bassMeta_; // KHONG CO {}
    AlignedPtr<float[]> bassLevels_;
    AlignedPtr<u16[]> bassHits_;
    u32 bassCap_ = 0;
    u32 bassUsed_ = 0;

    ByTypeBB byTypeBB_;                                    // KHONG CO {}
    Mask fusibleSlotsBB_;                                  // KHONG CO {}
    Mask postFxSlotsBB_;                                   // KHONG CO {}
    Mask dynamicFxSlotsBB_;                                // KHONG CO {} (active Fx varying with time/bass)
    MultiArray<u8, Mask::BITS> zOrderedSlots_;             // KHONG CO {}

    u32 itemCount_ = 0;
    double durationMs_ = 0.0;
    mutable double currentTimeMs_ = 0.0;
    double stageWidth_ = DEFAULT_STAGE_WIDTH;
    double stageHeight_ = DEFAULT_STAGE_HEIGHT;
    double stagedStageW_ = DEFAULT_STAGE_WIDTH;
    double stagedStageH_ = DEFAULT_STAGE_HEIGHT;
    double stagedDurationMs_ = 0.0;

    mutable u64 lastStructKey_ = 0;
    mutable u32 cachedPassCount_ = 0;
    mutable u32 cachedKernelCount_ = 0;
    mutable u8 cachedHasPostProcessing_ = 0;
    mutable u8 cachedMomentEligible_ = 0;
    mutable u8 cachedPlanValid_ = 0;
    mutable u8 padRuntime0_ = 0;
    mutable u32 uniformPassCount_ = 0;
    mutable MultiArray<UniformJob, Mask::BITS + 1> uniformPasses_;           // KHONG CO {}
    mutable MultiArray<RenderPass, RUNTIME_RENDER_PASS_CAPACITY> cachedPasses_; // KHONG CO {}
    mutable MultiArray<RenderTarget, RUNTIME_RENDER_TARGET_COUNT> cachedTargets_; // KHONG CO {}
    mutable MultiArray<BlurKernel, Mask::BITS> cachedKernels_;               // KHONG CO {}
    mutable Mask lastActiveMask_;                          // KHONG CO {}
    mutable u32 lastW_ = 0;
    mutable u32 lastH_ = 0;
    mutable u32 lastOutputToken_ = 0;
    mutable u32 activePlanTTIdx_ = INVALID_U32;
    mutable u32 cachedItemCount_ = 0;
    mutable u32 lastEvalEpoch_ = 0;
    mutable u64 cachedStaticFxZobrist_ = 0ULL;
    mutable Mask cachedActiveDynFxBB_;                     // KHONG CO {}
    mutable MultiArray<double, Mask::BITS> cachedItemStartMs_; // KHONG CO {}
    mutable double momentStartMs_ = 0.0;
    mutable double momentEndMs_ = -1.0;
    mutable double lastRenderedTimeMs_ = -1.0;
    mutable u8 cachedMomentStaticOk_ = 0;
    u32 planEpoch_ = nextPlanEpoch();

    static inline u32 nextPlanEpoch() noexcept {
        static u32 s_epoch = 1;
        return ++s_epoch;
    }
};

template <typename Mask = Mask<1>>
using RenderPassPlanner = Timeline<Mask>;

inline const RainDropParams*& active_rain_params_ptr() noexcept {
    static const RainDropParams* s_ptr = nullptr;
    return s_ptr;
}

// ============================================================================
// CoreEngine - Stockfish-Grade Universal Zobrist Tokenizer & Pyramid
// ============================================================================
[[nodiscard]] inline Key hzobrist_pass_meta(
    RuntimePassKind kind,
    u32 width, u32 height,
    float radius, float intensity,
    float p0, float p1, float p2, float p3,
    u8 kernelIndex = 0) noexcept
{
    const u64 w0 = (static_cast<u64>(kind) << 48) |
                   (static_cast<u64>(kernelIndex) << 40) |
                   (static_cast<u64>(width & 0xFFFFu) << 20) |
                   static_cast<u64>(height & 0xFFFFu);
    const u64 qRad = static_cast<u64>(hzobrist_quantize_u16(radius, 64.0f));
    const u64 qInt = static_cast<u64>(hzobrist_quantize_u16(intensity, 4096.0f));
    const u64 w1 = (qRad << 48) | (qInt << 32);

    u32 b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    std::memcpy(&b0, &p0, sizeof(float));
    std::memcpy(&b1, &p1, sizeof(float));
    std::memcpy(&b2, &p2, sizeof(float));
    std::memcpy(&b3, &p3, sizeof(float));
    const u64 w2 = (static_cast<u64>(b0) << 32) | static_cast<u64>(b1);
    const u64 w3 = (static_cast<u64>(b2) << 32) | static_cast<u64>(b3);

    Key h = hzobrist_mix64(w0 ^ ZOBRIST_MUL_A);
    h ^= hzobrist_mix64(w1 ^ (w2 * ZOBRIST_MUL_B));
    h ^= hzobrist_mix64(w3 ^ ZOBRIST_MUL_C);
    return h;
}

[[nodiscard]] inline Key hzobrist_pass_meta(const RenderPass& pass) noexcept
{
    return hzobrist_pass_meta(
        pass.kind, pass.width, pass.height,
        pass.radius, pass.intensity,
        pass.parameter0, pass.parameter1, pass.parameter2, pass.parameter3,
        pass.kernelIndex);
}

} // namespace stockcut::timeline::runtime

namespace stockcut::cache {

template <>
struct Zobrist<timeline::runtime::RenderPass> {
    [[nodiscard]] static inline Key meta_token(const timeline::runtime::RenderPass& pass) noexcept {
        return timeline::runtime::hzobrist_pass_meta(pass);
    }
    [[nodiscard]] static inline Key pass_token(
        const timeline::runtime::RenderPass& pass, Key slotHash, Key inputTargetHash) noexcept
    {
        return meta_token(pass) ^ slotHash ^ inputTargetHash;
    }
};

template <typename Mask = Mask<1>, usize PassCapacity = timeline::runtime::RUNTIME_RENDER_PASS_CAPACITY>
struct alignas(64) ZobristPyramid {
    static constexpr usize SLOTS = Mask::BITS;
    static constexpr usize TARGETS = timeline::runtime::RUNTIME_RENDER_TARGET_COUNT;

    alignas(64) MultiArray<Key, SLOTS> leaves{};
    alignas(64) MultiArray<Key, PassCapacity> passes{};
    alignas(16) MultiArray<Key, TARGETS> targetKeys{};
    Mask taintedSlotsBB{};
    Key root = 0ULL;
    Key staticDrawPrefixKey = 0ULL;
    u32 passCount = 0;
    u32 staticDrawPrefixCount = 0;
    u8  isRootTainted = 0;

    void clear() noexcept {
        leaves.fill(0ULL);
        targetKeys.fill(0ULL);
        taintedSlotsBB.clear();
        root = 0ULL;
        staticDrawPrefixKey = 0ULL;
        passCount = 0;
        staticDrawPrefixCount = 0;
        isRootTainted = 0;
    }

    inline void set_leaf_hash(u32 slot, Key hash) noexcept {
        if (slot < SLOTS) leaves[slot] = hash;
    }

    inline void bind_slot_resource(u32 slot, Key token, bool isReady = true) noexcept {
        if (slot < SLOTS) {
            leaves[slot] ^= token;
            if (!isReady) taintedSlotsBB.set_bit(slot);
        }
    }

    inline void set_tainted_slots(Mask tainted) noexcept {
        taintedSlotsBB = tainted;
    }

    [[nodiscard]] inline Key leaf_hash(u32 slot) const noexcept {
        return (slot < SLOTS) ? leaves[slot] : 0ULL;
    }

    [[nodiscard]] inline Key pass_hash(usize passIdx) const noexcept {
        return (passIdx < passCount) ? passes[passIdx] : 0ULL;
    }

    [[nodiscard]] inline Key target_hash(u32 targetId) const noexcept {
        return (targetId < TARGETS) ? targetKeys[targetId] : 0ULL;
    }

    [[nodiscard]] inline Key root_hash() const noexcept {
        return root;
    }

    [[nodiscard]] inline Key static_draw_prefix_key() const noexcept {
        return staticDrawPrefixKey;
    }

    [[nodiscard]] inline u32 static_draw_prefix_count() const noexcept {
        return staticDrawPrefixCount;
    }

    [[nodiscard]] inline bool is_root_tainted() const noexcept {
        return isRootTainted != 0u;
    }

    template <typename PositionT, typename StateInfoT>
    void build_leaf_hashes(const PositionT& pos, const StateInfoT& st, u64 activeDynamicFxWord = 0ULL) noexcept {
        leaves.fill(0ULL);
        const u64 taintedWord =
            activeDynamicFxWord |
            (st.activeBB.word(0) &
             (pos.template by_dynamic_axis_bb<DynamicAxis::IntrinsicVisual>().word(0) |
              pos.template by_type_bb<MediaType::Video>().word(0) |
              pos.chunk_kf_active_bb().word(0)));
        taintedSlotsBB = Mask(taintedWord);
        (st.activeBB & ~st.pseudoCulledBB).for_each_active([this, &pos](u32 s) {
            leaves[s] = pos.slot_leaf_hash(s);
        });
    }

    template <typename FramePlanT>
    void build_pyramid(const FramePlanT& plan, Key envKey) noexcept {
        targetKeys.fill(0ULL);
        Key rootXor = 0ULL;
        Key prefixAccum = envKey;
        u32 prefixCnt = 0u;
        bool inStaticPrefix = false;
        u8 taintedTargetsMask = 0u;
        const u32 count = (plan.passCount < PassCapacity) ? plan.passCount : static_cast<u32>(PassCapacity);
        passCount = count;

        for (u32 p = 0; p < count; ++p) {
            const auto& pass = plan.passes[p];
            if (pass.kind == timeline::runtime::RuntimePassKind::Sentinel) {
                passCount = p;
                break;
            }

            const Key metaKey = Zobrist<timeline::runtime::RenderPass>::meta_token(pass);
            const Key slotKey = (pass.slot < SLOTS) ? leaves[pass.slot] : 0ULL;
            const Key inTargetKey = (pass.inputTarget > 0 && pass.inputTarget < TARGETS)
                                        ? targetKeys[pass.inputTarget]
                                        : 0ULL;

            const Key hPass = metaKey ^ slotKey ^ inTargetKey;
            passes[p] = hPass;
            rootXor ^= hPass;

            if (pass.outputTarget > 0 && pass.outputTarget < TARGETS) {
                targetKeys[pass.outputTarget] = hPass;
            }

            const bool slotTainted = (pass.slot < SLOTS) && taintedSlotsBB.test_bit(pass.slot);
            const bool inTargetTainted = (pass.inputTarget < TARGETS) &&
                                         ((taintedTargetsMask & static_cast<u8>(1u << pass.inputTarget)) != 0u);
            const bool isTimeDrivenPass =
                (pass.kind == timeline::runtime::RuntimePassKind::ApplyShaderEffect) ||
                (pass.kind == timeline::runtime::RuntimePassKind::ApplyZoom) ||
                (pass.kind == timeline::runtime::RuntimePassKind::ApplyShake) ||
                (pass.kind == timeline::runtime::RuntimePassKind::ApplyBrightness) ||
                (pass.kind == timeline::runtime::RuntimePassKind::ApplyMonoidFused);
            const bool passTainted = slotTainted || inTargetTainted || isTimeDrivenPass;
            if (passTainted && pass.outputTarget < TARGETS) {
                taintedTargetsMask |= static_cast<u8>(1u << pass.outputTarget);
            }

            if (p == 0u) {
                if (pass.kind == timeline::runtime::RuntimePassKind::ClearScene && pass.outputTarget == 0u) {
                    inStaticPrefix = true;
                    prefixAccum = hzobrist_mix64(envKey ^ hPass);
                }
            } else if (inStaticPrefix) {
                if (pass.kind == timeline::runtime::RuntimePassKind::DrawLayer &&
                    pass.outputTarget == 0u &&
                    !slotTainted)
                {
                    ++prefixCnt;
                    prefixAccum = hzobrist_mix64(
                        prefixAccum ^ (hPass + (static_cast<Key>(prefixCnt) * HASH_GOLDEN_64)));
                } else {
                    inStaticPrefix = false;
                }
            }
        }

        staticDrawPrefixCount = prefixCnt;
        staticDrawPrefixKey = (prefixCnt > 0u) ? (prefixAccum ? prefixAccum : 1ULL) : 0ULL;
        isRootTainted = ((taintedTargetsMask & 1u) != 0u || taintedSlotsBB.any()) ? 1u : 0u;
        root = rootXor ^ envKey;
        if (!root) root = 1ULL;
    }
};

} // namespace stockcut::cache
