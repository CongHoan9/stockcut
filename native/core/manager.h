// ============================================================================
// CoreEngine - Engine Manager & WASM Composite Orchestrator
// ============================================================================
#pragma once
#include "pipeline.h"
#include "data_structures.h"
#include "render_constants.h"
#include "config.h"
#include "core/string_parse.h"
#include "timeline/project/duration.h"
#include "timeline/runtime/runtime.h"
#include "render/command.h"
#include "core/nested_mark.h"

namespace stockcut {
using DurationResult = timeline::project::DurationResult;

enum class CompositeFrameMode : u32 {
    FullRedraw  = 0,
    ReuseScene  = 1,
    DirtyRedraw = 2
};

enum class TimelineTimeMode : u8 {
    StartOrOffset   = 0,
    BoundedDuration = 1
};

static_assert(MAX_DISPLAY_COMMANDS == MAX_DISPLAY_COMMANDS_RENDER);

constexpr u32 COMPOSITE_CSS_COLOR_CAPACITY    = 256u;
constexpr u32 COMPOSITE_EFFECT_CAPACITY       = 64u;
constexpr u32 TIMELINE_SETUP_INPUT_CAPACITY   = 4096u;

struct EffectOrderEntry {
    double zIndex;
    u32 sequence;
    u32 sourceIndex;
    u32 isTransition;
    u32 reserved;
};
static_assert(sizeof(EffectOrderEntry) == 24);

enum TimelineGeometryFlag : u32 {
    TIMELINE_GEOMETRY_HAS_WIDTH  = 1u << 0,
    TIMELINE_GEOMETRY_HAS_HEIGHT = 1u << 1,
    TIMELINE_GEOMETRY_HAS_LEFT   = 1u << 2,
    TIMELINE_GEOMETRY_HAS_TOP    = 1u << 3
};

enum TimelineTierFlag : u32 {
    TIER_FLAG_SPIN          = 1u << 0,
    TIER_FLAG_SILK          = 1u << 1,
    TIER_FLAG_INFINITE_ANIM = 1u << 2,
    TIER_FLAG_BASS_BG       = 1u << 3,
    TIER_FLAG_ANIMATION     = 1u << 4
};

void parseTranslateProperty(StringSpan value, double& anchorX, double& anchorY);
void parseTransformAnchors(StringSpan transform, double& anchorX, double& anchorY);

class EngineManager {
public:
    DurationResult loadProjectJson(
        u32 length,
        double stageW = DEFAULT_STAGE_WIDTH,
        double stageH = DEFAULT_STAGE_HEIGHT
    );
    bool beginTimeline(float stageW, float stageH);
    DurationResult doProjectJsonMove(
        u32 length,
        double stageW = DEFAULT_STAGE_WIDTH,
        double stageH = DEFAULT_STAGE_HEIGHT
    );
    u64 advanceTimeline(i64 nowMs);

    template <bool SceneValid>
    CompositeFrameMode selectCompositeFrameMode(
        double dirtyArea, double fullArea, u32 dirtyCount
    ) const;

    u32 selectStaticCompositePrefix(u32 commandCount) const;
    QuadStream& gpuQuadStream() noexcept { return gpuQuadStream_; }
    const QuadStream& gpuQuadStream() const noexcept { return gpuQuadStream_; }
    QuadCommand* gpuQuadCommandsBuffer() noexcept { return gpuQuadStream_.data(); }
    u32 compositeRenderCulledCount() const;
    EffectOrderEntry* compositeEffectOrderInputBuffer();
    u32* compositeEffectOrderOutputBuffer();
    u32 orderCompositeEffects(u32 count);
    u32 computeCompositeStaticKey(
        u32 commandCount, double red, double green, double blue, double alpha
    ) const;
    char* compositeCssColorBuffer();
    double* compositeCssColorResultBuffer();
    double* parseCompositeCssColor(u32 length, double opacity);
    char* timelineSetupInputBuffer();
    double* computeTimelineGeometry(
        u32 flags, double stageW, double stageH,
        double intrinsicW, double intrinsicH,
        u32 widthLength, u32 heightLength, u32 leftLength, u32 topLength,
        u32 transformLength, u32 translateLength
    );
    double* computeTimelineKeyframeGeometry(
        u32 flags, double stageW, double stageH, double anchorX, double anchorY,
        double baseX, double baseY, double baseW, double baseH,
        u32 widthLength, u32 heightLength, u32 leftLength, u32 topLength
    );
    u32* normalizeTimelineTextStyle(
        u32 fontLength, u32 weightLength, u32 alignLength,
        u32 revealLength, u32 transformLength, u32 easingLength
    );
    u32 parseTimelineColor(u32 length, u32 fallback) const;
    double* timelineTimeResultBuffer();

    template <TimelineTimeMode Mode>
    double* resolveTimelineTime(
        u32 valueLength, double numericValue, double reference
    );

    double* timelineGeometryResultBuffer();
    u32* timelineTextStyleResultBuffer();
    u32 timelineSetupInputCapacity() const;

    template <bool IsPlaying>
    PipelineResult* evaluateFrame(
        u32 W, u32 H,
        u32 styleEpoch, u32 fontsVer, u32 styleVer, u32 mediaVer, u32 bgHash,
        u64 lastFrameHash, u64 occludedMask,
        double nowMs, u32 frameIndex
    );

    template <bool IsPlaying>
    PipelineResult* renderAt(
        u32 W, u32 H,
        u32 styleEpoch, u32 fontsVer, u32 styleVer, u32 mediaVer, u32 bgHash,
        u64 lastFrameHash, u64 occludedMask,
        double nowMs, u32 frameIndex
    );

    DurationResult& projectDurationResult();
    u32 projectCompileStatus() const;
    u32 projectGeneration() const;
    ConfigOptionsMap& config_options() noexcept { return configOptions_; }
    const ConfigOptionsMap& config_options() const noexcept { return configOptions_; }

private:
    void resetPostLoadCaches() noexcept;

    DurationResult pendingDurationResult_{-1.0, 0, 0};
    DurationResult activeDurationResult_{-1.0, 0, 0};
    u32 projectCompileStatus_ = 0;
    QuadStream gpuQuadStream_{};
    MultiArray<EffectOrderEntry, COMPOSITE_EFFECT_CAPACITY> compositeEffectOrderInput_;
    MultiArray<u32, COMPOSITE_EFFECT_CAPACITY> compositeEffectOrderOutput_;
    u32 compositeEffectOrderCount_ = 0;
    MultiArray<char, COMPOSITE_CSS_COLOR_CAPACITY> compositeCssColorBuffer_;
    MultiArray<double, 4> compositeCssColorResult_;
    MultiArray<char, TIMELINE_SETUP_INPUT_CAPACITY> timelineSetupInputBuffer_;
    MultiArray<double, 6> timelineGeometryResult_;
    MultiArray<u32, 6> timelineTextStyleResult_;
    MultiArray<double, 2> timelineTimeResult_;
    u32 projectGeneration_ = 0;
    ConfigOptionsMap configOptions_{};
};

EngineManager& engineManager();
timeline::runtime::Frame<>& engine_get_runtime_frame();
timeline::runtime::Timeline<>& projectRuntime();
timeline::runtime::RuntimeLoadStatus& projectRuntimeStatusRef();
} // namespace stockcut
