// ============================================================================
// CoreEngine - Engine Manager & WASM Composite Orchestrator Implementation
// ============================================================================
#include "manager.h"
#include "string_parse.h"
#include "hash_primitives.h"
#include "nnue_project.h"
#include "dsp.h"
#include "audio/script.h"
#include "computation_cache.h"
#include "display_list.h"
#include "tracker.h"
#include "position.h"
#include "render/renderer.h"
#include "text/font_map.h"
#include "timeline/project/compiler.h"
#include "timeline/project/json_buffer.h"
#include "timeline/runtime/runtime.h"
#include "transposition_table.h"
#include <cmath>
#include <limits>

namespace stockcut {
namespace {
constexpr TimePoint REVERSE_SEEK_UNDO_WINDOW_MS = 25;
constexpr u32       STATIC_PREFIX_MIN_STILL     = 2u;
constexpr double    I64_DOUBLE_LIMIT            = 9223372036854775808.0;

inline double normalizeStageDimension(double val, double fallback) noexcept {
    constexpr double maxStageDimension = static_cast<double>(std::numeric_limits<float>::max());
    return (std::isfinite(val) && val > 0.0 && val <= maxStageDimension) ? val : fallback;
}

inline void ensureFontManifestLoaded() {
    if (font::FontMapRegistry::instance().familyCount() == 0) {
        font::FontMapRegistry::instance().loadManifestFile(DEFAULT_FONT_MANIFEST_PATH);
        font::FontMapRegistry::instance().loadAllManifestFonts();
    }
}

bool parseFullNumber(StringSpan value, double& result) {
    value = trim_span(value);
    if (!value.length) {
        result = 0.0;
        return true;
    }
    if (value.length > 2 && value.data[0] == '0') {
        u32 radix = 0;
        if (value.data[1] == 'x' || value.data[1] == 'X') radix = 16;
        else if (value.data[1] == 'b' || value.data[1] == 'B') radix = 2;
        else if (value.data[1] == 'o' || value.data[1] == 'O') radix = 8;
        if (radix) {
            double parsed = 0.0;
            for (u32 i = 2; i < value.length; ++i) {
                const i32 nib = hex_nibble(value.data[i]);
                if (nib < 0 || static_cast<u32>(nib) >= radix) return false;
                parsed = parsed * radix + static_cast<double>(nib);
            }
            result = parsed;
            return true;
        }
    }
    u32 consumed = 0;
    if (!parse_css_number_prefix(value, result, &consumed)) return false;
    return consumed == value.length;
}

bool parseStrictPercent(StringSpan value, double& result) {
    value = trim_span(value);
    if (!span_ends_with(value, "%", 1)) return false;
    StringSpan number = trim_span({value.data, value.length - 1});
    if (!number.length || number.data[0] == '+') return false;
    u32 pos = number.data[0] == '-' ? 1u : 0u;
    bool hasInteger = false;
    while (pos < number.length && number.data[pos] >= '0' && number.data[pos] <= '9') {
        ++pos;
        hasInteger = true;
    }
    bool hasFraction = false;
    if (pos < number.length && number.data[pos] == '.') {
        ++pos;
        const u32 fractionStart = pos;
        while (pos < number.length && number.data[pos] >= '0' && number.data[pos] <= '9')
            ++pos;
        hasFraction = pos > fractionStart;
        if (!hasFraction) return false;
    }
    if (pos != number.length || (!hasInteger && !hasFraction)) return false;
    return parse_css_number_prefix(number, result);
}

double cssDimension(StringSpan value, double reference, double fallback,
                    double stageW, double stageH) {
    value = trim_span(value);
    if (!value.length) return fallback;
    double number = 0.0;
    if (!parse_css_number_prefix(value, number)) {
        if (span_ends_with(value, "px", 2)) return 0.0;
        if (span_ends_with(value, "%", 1) || span_ends_with(value, "cqw", 3) ||
            span_ends_with(value, "vw", 2) || span_ends_with(value, "cqh", 3) ||
            span_ends_with(value, "vh", 2))
            return __builtin_nan("");
        return fallback;
    }
    if (span_ends_with(value, "%", 1))
        return number * reference / 100.0;
    if (span_ends_with(value, "cqw", 3) || span_ends_with(value, "vw", 2))
        return number * (reference > 0.0 ? reference : (stageW != 0.0 ? stageW : DEFAULT_STAGE_WIDTH)) / 100.0;
    if (span_ends_with(value, "cqh", 3) || span_ends_with(value, "vh", 2))
        return number * (reference > 0.0 ? reference : (stageH != 0.0 ? stageH : DEFAULT_STAGE_HEIGHT)) / 100.0;
    return number;
}

u32 parseTimelineFontCategory(StringSpan family) {
    if (span_contains_icase(family, "condensed", 9) ||
        span_contains_icase(family, "anton", 5) ||
        span_contains_icase(family, "bebas", 5) ||
        span_contains_icase(family, "teko", 4) ||
        span_contains_icase(family, "impact", 6)) return static_cast<u32>(FontCategory::Condensed);
    if (span_contains_icase(family, "serif", 5) ||
        span_contains_icase(family, "playfair", 8) ||
        span_contains_icase(family, "times", 5) ||
        span_contains_icase(family, "merriweather", 12) ||
        span_contains_icase(family, "lora", 4)) return static_cast<u32>(FontCategory::Serif);
    if (span_contains_icase(family, "mono", 4) ||
        span_contains_icase(family, "courier", 7) ||
        span_contains_icase(family, "consolas", 8) ||
        span_contains_icase(family, "code", 4)) return static_cast<u32>(FontCategory::Mono);
    if (span_contains_icase(family, "script", 6) ||
        span_contains_icase(family, "pacifico", 8) ||
        span_contains_icase(family, "cursive", 7) ||
        span_contains_icase(family, "handwriting", 11)) return static_cast<u32>(FontCategory::Script);
    return static_cast<u32>(FontCategory::Sans);
}

u32 parseTimelineFontWeight(StringSpan weight) {
    weight = trim_span(weight);
    if (!weight.length) return 400u;
    if (span_equals_icase_n(weight, "bold", 4)) return 700u;
    if (span_equals_icase_n(weight, "normal", 6)) return 400u;
    if (span_equals_icase_n(weight, "light", 5)) return 300u;
    if (span_equals_icase_n(weight, "black", 5) ||
        span_equals_icase_n(weight, "heavy", 5)) return 900u;
    u32 pos = 0;
    bool negative = false;
    if (weight.data[pos] == '-' || weight.data[pos] == '+')
        negative = weight.data[pos++] == '-';
    u32 parsed = 0;
    bool hasDigit = false;
    while (pos < weight.length && weight.data[pos] >= '0' && weight.data[pos] <= '9') {
        parsed = parsed * 10u + static_cast<u32>(weight.data[pos++] - '0');
        hasDigit = true;
    }
    if (!hasDigit) return 400u;
    return negative ? static_cast<u32>(-static_cast<i32>(parsed)) : parsed;
}

u32 parseHexPrefix(StringSpan value) {
    u32 result = 0;
    u32 digits = 0;
    for (u32 i = 0; i < value.length; ++i) {
        const i32 nibble = hex_nibble(value.data[i]);
        if (nibble < 0) break;
        result = (result << 4) | static_cast<u32>(nibble);
        ++digits;
    }
    return digits ? result : 0;
}

bool parseUnsignedDecimal(StringSpan value, u32& result) {
    if (!value.length) return false;
    u32 parsed = 0;
    for (u32 i = 0; i < value.length; ++i) {
        if (value.data[i] < '0' || value.data[i] > '9') return false;
        parsed = parsed * 10u + static_cast<u32>(value.data[i] - '0');
    }
    result = parsed;
    return true;
}

u32 parseTimelineColorValue(StringSpan color, u32 fallback) {
    color = trim_span(color);
    if (!color.length) return fallback;
    if (span_equals_n(color, "transparent", 11)) return 0u;
    if (color.data[0] == '#') {
        const StringSpan hex{color.data + 1, color.length - 1};
        if (hex.length == 3) {
            const u32 red   = parseHexPrefix({hex.data, 1});
            const u32 green = parseHexPrefix({hex.data + 1, 1});
            const u32 blue  = parseHexPrefix({hex.data + 2, 1});
            return RGBA_OPAQUE_BLACK | ((blue * 17u) << 16) |
                   ((green * 17u) << 8) | (red * 17u);
        }
        if (hex.length == 6 || hex.length == 8) {
            const u32 red   = parseHexPrefix({hex.data, 2});
            const u32 green = parseHexPrefix({hex.data + 2, 2});
            const u32 blue  = parseHexPrefix({hex.data + 4, 2});
            const u32 alpha = hex.length == 8
                ? parseHexPrefix({hex.data + 6, 2})
                : 255u;
            return (alpha << 24) | (blue << 16) | (green << 8) | red;
        }
        return fallback;
    }
    u32 pos = 0;
    if (color.length >= 3 && ascii_lower(color.data[0]) == 'r' &&
        ascii_lower(color.data[1]) == 'g' && ascii_lower(color.data[2]) == 'b') {
        pos = 3;
        const bool hasAlpha = pos < color.length && ascii_lower(color.data[pos]) == 'a';
        if (hasAlpha) ++pos;
        while (pos < color.length && is_ascii_space(color.data[pos])) ++pos;
        if (pos >= color.length || color.data[pos++] != '(') return fallback;
        MultiArray<u32, 3> channels{};
        for (u32 channel = 0; channel < 3; ++channel) {
            while (pos < color.length && is_ascii_space(color.data[pos])) ++pos;
            const u32 start = pos;
            while (pos < color.length && color.data[pos] >= '0' &&
                   color.data[pos] <= '9') ++pos;
            if (!parseUnsignedDecimal({color.data + start, pos - start}, channels[channel]))
                return fallback;
            while (pos < color.length && is_ascii_space(color.data[pos])) ++pos;
            if (channel < 2 && (pos >= color.length || color.data[pos++] != ','))
                return fallback;
        }
        u32 alpha = 255u;
        while (pos < color.length && is_ascii_space(color.data[pos])) ++pos;
        if (pos < color.length && color.data[pos] == ',') {
            ++pos;
            while (pos < color.length && is_ascii_space(color.data[pos])) ++pos;
            const u32 start = pos;
            while (pos < color.length &&
                   ((color.data[pos] >= '0' && color.data[pos] <= '9') ||
                    color.data[pos] == '.')) ++pos;
            double alphaValue = 0.0;
            if (parse_css_number_prefix({color.data + start, pos - start}, alphaValue)) {
                const double rounded = __builtin_floor(alphaValue * 255.0 + 0.5);
                alpha = static_cast<u32>(static_cast<i64>(rounded)) & 0xFFu;
            } else {
                alpha = 0u;
            }
            while (pos < color.length && is_ascii_space(color.data[pos])) ++pos;
        }
        if (pos >= color.length || color.data[pos++] != ')') return fallback;
        while (pos < color.length && is_ascii_space(color.data[pos])) ++pos;
        if (pos != color.length) return fallback;
        return (alpha << 24) | ((channels[2] & 0xFFu) << 16) |
               ((channels[1] & 0xFFu) << 8) | (channels[0] & 0xFFu);
    }
    return fallback;
}

bool equalsAt(StringSpan value, u32 offset, const char* text, u32 length) {
    return offset <= value.length && length <= value.length - offset &&
           __builtin_memcmp(value.data + offset, text, length) == 0;
}

bool findCssFunction(StringSpan value, const char* name, u32 nameLength, u32& openParen) {
    for (u32 i = 0; i + nameLength <= value.length; ++i) {
        if (!equalsAt(value, i, name, nameLength)) continue;
        u32 pos = i + nameLength;
        while (pos < value.length && is_ascii_space(value.data[pos])) ++pos;
        if (pos < value.length && value.data[pos] == '(') {
            openParen = pos;
            return true;
        }
    }
    return false;
}

template <bool Second>
StringSpan functionArgument(StringSpan value, u32 openParen) {
    u32 start = openParen + 1;
    u32 end = start;
    while (end < value.length && value.data[end] != ')' &&
           !(Second && value.data[end] == ',')) ++end;
    if constexpr (!Second) {
        u32 comma = start;
        while (comma < end && value.data[comma] != ',') ++comma;
        end = comma;
    } else {
        if (end >= value.length || value.data[end] != ',') return {};
        start = end + 1;
        end = start;
        while (end < value.length && value.data[end] != ')') ++end;
    }
    return trim_span({value.data + start, end - start});
}

bool percentAnchor(StringSpan value, double& result) {
    return parse_anchor_token(value, result);
}
} // namespace

void parseTranslateProperty(StringSpan value, double& anchorX, double& anchorY) {
    value = trim_span(value);
    u32 firstEnd = 0;
    while (firstEnd < value.length && !is_ascii_space(value.data[firstEnd]) &&
           value.data[firstEnd] != ',') ++firstEnd;
    percentAnchor({value.data, firstEnd}, anchorX);
    u32 secondStart = firstEnd;
    while (secondStart < value.length &&
           (is_ascii_space(value.data[secondStart]) || value.data[secondStart] == ','))
        ++secondStart;
    u32 secondEnd = secondStart;
    while (secondEnd < value.length && !is_ascii_space(value.data[secondEnd]) &&
           value.data[secondEnd] != ',') ++secondEnd;
    if (secondEnd > secondStart)
        percentAnchor({value.data + secondStart, secondEnd - secondStart}, anchorY);
}

void parseTransformAnchors(StringSpan transform, double& anchorX, double& anchorY) {
    u32 openParen = 0;
    if (findCssFunction(transform, "translate", 9, openParen)) {
        const StringSpan x = functionArgument<false>(transform, openParen);
        if (x.length) {
            percentAnchor(x, anchorX);
            const StringSpan y = functionArgument<true>(transform, openParen);
            if (y.length) percentAnchor(y, anchorY);
            return;
        }
    }
    if (findCssFunction(transform, "translateX", 10, openParen))
        percentAnchor(functionArgument<false>(transform, openParen), anchorX);
    if (findCssFunction(transform, "translateY", 10, openParen))
        percentAnchor(functionArgument<false>(transform, openParen), anchorY);
}

EngineManager& engineManager() {
    static EngineManager manager;
    return manager;
}

timeline::runtime::RuntimeLoadStatus& projectRuntimeStatusRef() {
    static timeline::runtime::RuntimeLoadStatus status =
        timeline::runtime::RUNTIME_LOAD_INVALID_COMPILED_RECORD;
    return status;
}

void EngineManager::resetPostLoadCaches() noexcept {
    tt_clear();
    display_list_cache_clear();
    computation_cache_clear();
    gpuQuadStream_.reset();
    activeDurationResult_ = pendingDurationResult_;
    ++projectGeneration_;
}

DurationResult EngineManager::loadProjectJson(
    u32 length,
    double stageW,
    double stageH
) {
    const double normalizedStageW = normalizeStageDimension(stageW, DEFAULT_STAGE_WIDTH);
    const double normalizedStageH = normalizeStageDimension(stageH, DEFAULT_STAGE_HEIGHT);
    if (length > timeline::project::projectJsonCapacity()) {
        projectCompileStatus_ = timeline::project::PROJECT_COMPILE_INPUT_CAPACITY_EXCEEDED;
        pendingDurationResult_ = {-1.0, 0, 0};
        return pendingDurationResult_;
    }
    timeline::project::CompileResult compiled =
        timeline::project::compileProject(
            timeline::project::projectJsonBuffer(), length,
            {normalizedStageW, normalizedStageH});
    projectCompileStatus_ = compiled.status;
    if (compiled.status != timeline::project::PROJECT_COMPILE_OK) {
        pendingDurationResult_ = {-1.0, 0, 0};
        return pendingDurationResult_;
    }
    timeline::project::configureCompiledAudioTracks(compiled.duration.durationMs);
    pendingDurationResult_ = compiled.duration;
    if (!beginTimeline(static_cast<float>(normalizedStageW), static_cast<float>(normalizedStageH))) {
        projectCompileStatus_ = timeline::project::PROJECT_COMPILE_INVALID_TIMELINE;
        pendingDurationResult_ = {-1.0, 0, 0};
    }
    return pendingDurationResult_;
}

bool EngineManager::beginTimeline(float stageW, float stageH) {
    if (pendingDurationResult_.durationMs < 0.0 ||
        !std::isfinite(stageW) || !std::isfinite(stageH) ||
        stageW <= 0.0f || stageH <= 0.0f)
        return false;
    Render::reset_project_state();
    timeline_position_reset();
    if (auto* script = get_active_music_script()) script->reset();
    Position<>* position = timeline_position_get_instance();
    position->set_duration(static_cast<TimePoint>(pendingDurationResult_.durationMs));
    position->set_stage_dimensions(stageW, stageH);
    ensureFontManifestLoaded();
    timeline::project::resetCompiledMerkleTable();
    timeline::project::configureCompiledTimelinePosition(stageW, stageH);
    NNUECutProjectBrain::instance().on_project_loaded(*position);
    resetPostLoadCaches();
    return true;
}

DurationResult EngineManager::doProjectJsonMove(
    u32 length,
    double stageW,
    double stageH
) {
    const double normalizedStageW = normalizeStageDimension(stageW, DEFAULT_STAGE_WIDTH);
    const double normalizedStageH = normalizeStageDimension(stageH, DEFAULT_STAGE_HEIGHT);
    if (length > timeline::project::projectJsonCapacity()) {
        projectCompileStatus_ = timeline::project::PROJECT_COMPILE_INPUT_CAPACITY_EXCEEDED;
        pendingDurationResult_ = {-1.0, 0, 0};
        return pendingDurationResult_;
    }
    ensureFontManifestLoaded();
    Position<>* position = timeline_position_get_instance();
    const TimePoint savedTimeMs = position ? position->time() : 0;
    timeline::project::CompileResult compiled =
        timeline::project::compileProject(
            timeline::project::projectJsonBuffer(), length,
            {normalizedStageW, normalizedStageH});
    projectCompileStatus_ = compiled.status;
    if (compiled.status != timeline::project::PROJECT_COMPILE_OK) {
        pendingDurationResult_ = {-1.0, 0, 0};
        return pendingDurationResult_;
    }
    timeline::project::configureCompiledAudioTracks(compiled.duration.durationMs);
    pendingDurationResult_ = compiled.duration;
    position = timeline_position_get_instance();
    position->set_duration(static_cast<TimePoint>(pendingDurationResult_.durationMs));
    position->set_stage_dimensions(
        static_cast<float>(normalizedStageW),
        static_cast<float>(normalizedStageH));
    // PHA 2: INCREMENTAL MERKLE-AST COLD/WARM PATCH (ZERO NUCLEAR RESET!)
    // - No timeline_position_reset()
    // - No Render::invalidate_all_text_caches() -> only invalidate exact textShapeDirtyBB!
    // - No nuclear tt_clear() -> bump generation so untouched frames remain 100% TT hits!
    const auto patch = timeline::project::configureCompiledTimelinePositionIncremental(
        normalizedStageW, normalizedStageH);
    NNUECutProjectBrain::instance().on_delta_move(*position);
    u64 dirtyTxt = patch.textShapeDirtyBB | patch.removedSlotsBB;
    while (dirtyTxt != 0ULL) {
        Render::invalidate_slot_text_cache(BitOps<u64>::pop_lsb(dirtyTxt));
    }
    tt_next_generation();
    display_list_cache_clear();
    gpuQuadStream_.reset();
    activeDurationResult_ = pendingDurationResult_;
    ++projectGeneration_;
    if (savedTimeMs > 0) {
        timeline_position_advance(savedTimeMs);
    }
    return pendingDurationResult_;
}

u64 EngineManager::advanceTimeline(i64 nowMs) {
    Position<>* position = timeline_position_get_instance();
    const TimePoint target = static_cast<TimePoint>(nowMs);
    const TimePoint current = position->time();
    if (target < current && current - target <= REVERSE_SEEK_UNDO_WINDOW_MS && position->undo_step()) {
        const TimePoint restored = position->time();
        const TimePoint difference = restored < target ? target - restored : restored - target;
        if (difference > 1) {
            timeline_position_advance(target);
        }
    } else {
        timeline_position_advance(target);
    }
    return position->active_items();
}

template <bool SceneValid>
CompositeFrameMode EngineManager::selectCompositeFrameMode(
    double dirtyArea, double fullArea, u32 dirtyCount
) const {
    if constexpr (!SceneValid) {
        return CompositeFrameMode::FullRedraw;
    } else {
        if (dirtyArea > fullArea * DIRTY_AREA_FULL_RATIO)
            return CompositeFrameMode::FullRedraw;
        if (dirtyCount == 0u)
            return CompositeFrameMode::ReuseScene;
        return CompositeFrameMode::DirtyRedraw;
    }
}
template CompositeFrameMode EngineManager::selectCompositeFrameMode<false>(double, double, u32) const;
template CompositeFrameMode EngineManager::selectCompositeFrameMode<true>(double, double, u32) const;

u32 EngineManager::selectStaticCompositePrefix(u32 commandCount) const {
    const u32 available = gpuQuadStream_.count();
    const u32 limit = commandCount < available ? commandCount : available;
    u32 i = 0;
    for (const QuadCommand* cmd = gpuQuadStream_.data(); cmd->opCode != static_cast<u8>(CMD_SENTINEL_END) && i < limit; ++cmd, ++i) {
        const u32 stillN = g_slotTracker.get_state(cmd->slot, 0u).stillN;
        if (stillN < STATIC_PREFIX_MIN_STILL) return i;
    }
    return i;
}

u32 EngineManager::compositeRenderCulledCount() const {
    return gpuQuadStream_.culled_count();
}

EffectOrderEntry* EngineManager::compositeEffectOrderInputBuffer() {
    return compositeEffectOrderInput_.data();
}

u32* EngineManager::compositeEffectOrderOutputBuffer() {
    return compositeEffectOrderOutput_.data();
}

u32 EngineManager::orderCompositeEffects(u32 count) {
    compositeEffectOrderCount_ =
        count < COMPOSITE_EFFECT_CAPACITY ? count : COMPOSITE_EFFECT_CAPACITY;
    for (u32 i = 0; i < compositeEffectOrderCount_; ++i)
        compositeEffectOrderOutput_[i] = i;
    for (u32 i = 1; i < compositeEffectOrderCount_; ++i) {
        const u32 candidate = compositeEffectOrderOutput_[i];
        const EffectOrderEntry& candidateEntry = compositeEffectOrderInput_[candidate];
        u32 insertAt = i;
        while (insertAt > 0) {
            const u32 previous = compositeEffectOrderOutput_[insertAt - 1];
            const EffectOrderEntry& previousEntry = compositeEffectOrderInput_[previous];
            const bool candidateBefore =
                (candidateEntry.isTransition < previousEntry.isTransition) ||
                (candidateEntry.isTransition == previousEntry.isTransition &&
                 (candidateEntry.isTransition
                      ? candidateEntry.sequence < previousEntry.sequence
                      : (candidateEntry.zIndex < previousEntry.zIndex ||
                         (candidateEntry.zIndex == previousEntry.zIndex &&
                          candidateEntry.sequence < previousEntry.sequence))));
            if (!candidateBefore) break;
            compositeEffectOrderOutput_[insertAt] = previous;
            --insertAt;
        }
        compositeEffectOrderOutput_[insertAt] = candidate;
    }
    for (u32 i = 0; i < compositeEffectOrderCount_; ++i) {
        const u32 entryIndex = compositeEffectOrderOutput_[i];
        compositeEffectOrderOutput_[i] = compositeEffectOrderInput_[entryIndex].sourceIndex;
    }
    return compositeEffectOrderCount_;
}

u32 EngineManager::computeCompositeStaticKey(
    u32 commandCount, double red, double green, double blue, double alpha
) const {
    const u32 hash = gpuQuadStream_.compute_static_key(
        commandCount,
        static_cast<float>(red),
        static_cast<float>(green),
        static_cast<float>(blue),
        static_cast<float>(alpha));
    return hash == 0u ? 1u : hash;
}

char* EngineManager::compositeCssColorBuffer() {
    return compositeCssColorBuffer_.data();
}

double* EngineManager::compositeCssColorResultBuffer() {
    return compositeCssColorResult_.data();
}

double* EngineManager::parseCompositeCssColor(u32 length, double opacity) {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double alpha = length == 0 ? 0.0 : 1.0;
    if (length > COMPOSITE_CSS_COLOR_CAPACITY || length == 0) {
        compositeCssColorResult_.clear();
        return compositeCssColorResult_.data();
    }
    const char* const input = compositeCssColorBuffer_.data();
    if (length == 4 && input[0] == '#') {
        const i32 r = hex_nibble(input[1]);
        const i32 g = hex_nibble(input[2]);
        const i32 b = hex_nibble(input[3]);
        if (r >= 0 && g >= 0 && b >= 0) {
            red   = static_cast<double>(r * 17) * INV_255_F64;
            green = static_cast<double>(g * 17) * INV_255_F64;
            blue  = static_cast<double>(b * 17) * INV_255_F64;
            alpha = 1.0;
        } else {
            alpha = 1.0;
        }
    } else if ((length == 7 || length == 9) && input[0] == '#') {
        MultiArray<u32, 4> components{ {0u, 0u, 0u, 255u} };
        bool valid = true;
        const u32 componentCount = length == 9 ? 4u : 3u;
        for (u32 i = 0; i < componentCount; ++i) {
            const i32 high = hex_nibble(input[1 + i * 2]);
            const i32 low  = hex_nibble(input[2 + i * 2]);
            if (high < 0 || low < 0) {
                valid = false;
                break;
            }
            components[i] = static_cast<u32>(high * 16 + low);
        }
        if (valid) {
            red   = static_cast<double>(components[0]) * INV_255_F64;
            green = static_cast<double>(components[1]) * INV_255_F64;
            blue  = static_cast<double>(components[2]) * INV_255_F64;
            alpha = static_cast<double>(components[3]) * INV_255_F64;
        } else {
            alpha = 1.0;
        }
    } else if (span_equals_n({input, length}, "transparent", 11)) {
        alpha = 0.0;
    } else {
        const bool rgbPrefix = length >= 4 && input[0] == 'r' &&
                               input[1] == 'g' && input[2] == 'b';
        const bool hasAlpha = rgbPrefix && length >= 5 &&
                              input[3] == 'a' && input[4] == '(';
        const bool validPrefix = rgbPrefix && (hasAlpha || input[3] == '(');
        u32 pos = hasAlpha ? 5u : 4u;
        MultiArray<u32, 3> parsed{};
        bool valid = validPrefix;
        for (u32 component = 0; valid && component < 3; ++component) {
            while (pos < length && input[pos] == ' ') ++pos;
            u32 value = 0;
            bool hasDigit = false;
            while (pos < length && input[pos] >= '0' && input[pos] <= '9') {
                value = value * 10u + static_cast<u32>(input[pos++] - '0');
                hasDigit = true;
            }
            if (!hasDigit || value > 255u) {
                valid = false;
                break;
            }
            parsed[component] = value;
            while (pos < length && input[pos] == ' ') ++pos;
            if (component < 2u) {
                if (pos >= length || input[pos++] != ',') valid = false;
            }
        }
        if (valid) {
            while (pos < length && input[pos] == ' ') ++pos;
            if (pos < length && input[pos] == ',') {
                ++pos;
                while (pos < length && input[pos] == ' ') ++pos;
                double opacityValue = 0.0;
                double fraction = 0.1;
                bool hasDigit = false;
                while (pos < length && input[pos] >= '0' && input[pos] <= '9') {
                    opacityValue = opacityValue * 10.0 + (input[pos++] - '0');
                    hasDigit = true;
                }
                bool hasDecimal = false;
                if (pos < length && input[pos] == '.') {
                    ++pos;
                    hasDecimal = true;
                    while (pos < length && input[pos] >= '0' && input[pos] <= '9') {
                        opacityValue += (input[pos++] - '0') * fraction;
                        fraction *= 0.1;
                        hasDigit = true;
                    }
                }
                if (!hasDigit || (hasDecimal && input[pos - 1] == '.')) {
                    valid = false;
                } else {
                    alpha = opacityValue;
                }
            } else {
                alpha = 1.0;
            }
        }
        if (valid) {
            red   = static_cast<double>(parsed[0]) * INV_255_F64;
            green = static_cast<double>(parsed[1]) * INV_255_F64;
            blue  = static_cast<double>(parsed[2]) * INV_255_F64;
        } else {
            red = 0.0;
            green = 0.0;
            blue = 0.0;
            alpha = 1.0;
        }
    }
    compositeCssColorResult_[0] = red;
    compositeCssColorResult_[1] = green;
    compositeCssColorResult_[2] = blue;
    compositeCssColorResult_[3] = alpha * opacity;
    return compositeCssColorResult_.data();
}

char* EngineManager::timelineSetupInputBuffer() {
    return timelineSetupInputBuffer_.data();
}

double* EngineManager::computeTimelineGeometry(
    u32 flags, double stageW, double stageH,
    double intrinsicW, double intrinsicH,
    u32 widthLength, u32 heightLength, u32 leftLength, u32 topLength,
    u32 transformLength, u32 translateLength
) {
    const u32 totalLength = widthLength + heightLength + leftLength + topLength +
                            transformLength + translateLength;
    if (totalLength > TIMELINE_SETUP_INPUT_CAPACITY) return nullptr;
    const char* cursor = timelineSetupInputBuffer_.data();
    const StringSpan widthValue{cursor, widthLength};         cursor += widthLength;
    const StringSpan heightValue{cursor, heightLength};       cursor += heightLength;
    const StringSpan leftValue{cursor, leftLength};           cursor += leftLength;
    const StringSpan topValue{cursor, topLength};             cursor += topLength;
    const StringSpan transformValue{cursor, transformLength}; cursor += transformLength;
    const StringSpan translateValue{cursor, translateLength};

    const bool hasWidth  = (flags & TIMELINE_GEOMETRY_HAS_WIDTH) != 0;
    const bool hasHeight = (flags & TIMELINE_GEOMETRY_HAS_HEIGHT) != 0;
    const bool hasLeft   = (flags & TIMELINE_GEOMETRY_HAS_LEFT) != 0;
    const bool hasTop    = (flags & TIMELINE_GEOMETRY_HAS_TOP) != 0;

    double width = intrinsicW;
    double height = intrinsicH;
    if (hasWidth && widthLength > 0) {
        width = cssDimension(widthValue, stageW, intrinsicW, stageW, stageH);
    }
    if (hasHeight && heightLength > 0) {
        height = cssDimension(heightValue, stageH, intrinsicH, stageW, stageH);
    }

    // Chính sách toàn hệ thống: Mọi item mặc định left = 50%, top = 50%
    double x = (hasLeft && leftLength > 0)
        ? cssDimension(leftValue, stageW, 0.5 * stageW, stageW, stageH)
        : (0.5 * stageW);
    double y = (hasTop && topLength > 0)
        ? cssDimension(topValue, stageH, 0.5 * stageH, stageW, stageH)
        : (0.5 * stageH);

    // Mọi item mặc định anchor = (0.5, 0.5), tương đương translate(-50%, -50%)
    double anchorX = 0.5;
    double anchorY = 0.5;
    if (translateLength > 0) {
        parseTranslateProperty(translateValue, anchorX, anchorY);
    }
    if (transformLength > 0) {
        parseTransformAnchors(transformValue, anchorX, anchorY);
    }

    if (anchorX != 0.0) x -= anchorX * width;
    if (anchorY != 0.0) y -= anchorY * height;

    timelineGeometryResult_[0] = x;
    timelineGeometryResult_[1] = y;
    timelineGeometryResult_[2] = width;
    timelineGeometryResult_[3] = height;
    timelineGeometryResult_[4] = anchorX;
    timelineGeometryResult_[5] = anchorY;
    return timelineGeometryResult_.data();
}

double* EngineManager::computeTimelineKeyframeGeometry(
    u32 flags, double stageW, double stageH, double anchorX, double anchorY,
    double baseX, double baseY, double baseW, double baseH,
    u32 widthLength, u32 heightLength, u32 leftLength, u32 topLength
) {
    const u32 totalLength = widthLength + heightLength + leftLength + topLength;
    if (totalLength > TIMELINE_SETUP_INPUT_CAPACITY) return nullptr;
    const char* cursor = timelineSetupInputBuffer_.data();
    const StringSpan widthValue{cursor, widthLength};   cursor += widthLength;
    const StringSpan heightValue{cursor, heightLength}; cursor += heightLength;
    const StringSpan leftValue{cursor, leftLength};     cursor += leftLength;
    const StringSpan topValue{cursor, topLength};

    const bool hasWidth  = (flags & TIMELINE_GEOMETRY_HAS_WIDTH) != 0;
    const bool hasHeight = (flags & TIMELINE_GEOMETRY_HAS_HEIGHT) != 0;
    const bool hasLeft   = (flags & TIMELINE_GEOMETRY_HAS_LEFT) != 0;
    const bool hasTop    = (flags & TIMELINE_GEOMETRY_HAS_TOP) != 0;
    double x = baseX;
    double y = baseY;
    double width = baseW;
    double height = baseH;
    if (hasLeft) {
        x = cssDimension(leftValue, stageW, baseX, stageW, stageH);
        if (anchorX != 0.0) x -= anchorX * width;
    }
    if (hasTop) {
        y = cssDimension(topValue, stageH, baseY, stageW, stageH);
        if (anchorY != 0.0) y -= anchorY * height;
    }
    if (hasWidth)  width  = cssDimension(widthValue, stageW, baseW, stageW, stageH);
    if (hasHeight) height = cssDimension(heightValue, stageH, baseH, stageW, stageH);
    timelineGeometryResult_[0] = x;
    timelineGeometryResult_[1] = y;
    timelineGeometryResult_[2] = width;
    timelineGeometryResult_[3] = height;
    timelineGeometryResult_[4] = anchorX;
    timelineGeometryResult_[5] = anchorY;
    return timelineGeometryResult_.data();
}

u32* EngineManager::normalizeTimelineTextStyle(
    u32 fontLength, u32 weightLength, u32 alignLength,
    u32 revealLength, u32 transformLength, u32 easingLength
) {
    const u32 totalLength = fontLength + weightLength + alignLength +
                            revealLength + transformLength + easingLength;
    if (totalLength > TIMELINE_SETUP_INPUT_CAPACITY) return nullptr;
    const char* cursor = timelineSetupInputBuffer_.data();
    const StringSpan font{cursor, fontLength};           cursor += fontLength;
    const StringSpan weight{cursor, weightLength};       cursor += weightLength;
    const StringSpan align{cursor, alignLength};         cursor += alignLength;
    const StringSpan reveal{cursor, revealLength};       cursor += revealLength;
    const StringSpan transform{cursor, transformLength}; cursor += transformLength;
    const StringSpan easing{cursor, easingLength};

    u32 alignCode = static_cast<u32>(TextAlignMode::Left);
    if (span_equals_icase_n(align, "center", 6)) alignCode = static_cast<u32>(TextAlignMode::Center);
    else if (span_equals_icase_n(align, "right", 5) ||
             span_equals_icase_n(align, "end", 3)) alignCode = static_cast<u32>(TextAlignMode::Right);
    else if (span_equals_icase_n(align, "justify", 7)) alignCode = static_cast<u32>(TextAlignMode::Justify);

    u32 revealCode = 0u;
    if (span_equals_icase_n(reveal, "add", 3)) revealCode = 1u;
    else if (span_equals_icase_n(reveal, "display", 7)) revealCode = 2u;
    else if (span_equals_icase_n(reveal, "sweep", 5) ||
             span_equals_icase_n(reveal, "continuous", 10)) revealCode = 3u;

    u32 transformCode = static_cast<u32>(TextTransformMode::None);
    if (span_equals_icase_n(transform, "uppercase", 9)) transformCode = static_cast<u32>(TextTransformMode::Uppercase);
    else if (span_equals_icase_n(transform, "lowercase", 9)) transformCode = static_cast<u32>(TextTransformMode::Lowercase);

    u32 easingCode = static_cast<u32>(EasingCurve::Linear);
    if (span_equals_n(easing, "ease-in", 7)) easingCode = static_cast<u32>(EasingCurve::EaseIn);
    else if (span_equals_n(easing, "ease-out", 8)) easingCode = static_cast<u32>(EasingCurve::EaseOut);
    else if (span_equals_n(easing, "ease-in-out", 11)) easingCode = static_cast<u32>(EasingCurve::EaseInOut);

    timelineTextStyleResult_[0] = parseTimelineFontWeight(weight);
    timelineTextStyleResult_[1] = alignCode;
    timelineTextStyleResult_[2] = parseTimelineFontCategory(font);
    timelineTextStyleResult_[3] = revealCode;
    timelineTextStyleResult_[4] = transformCode;
    timelineTextStyleResult_[5] = easingCode;
    return timelineTextStyleResult_.data();
}

u32 EngineManager::parseTimelineColor(u32 length, u32 fallback) const {
    if (length > TIMELINE_SETUP_INPUT_CAPACITY) return fallback;
    return parseTimelineColorValue({timelineSetupInputBuffer_.data(), length}, fallback);
}

double* EngineManager::timelineTimeResultBuffer() {
    return timelineTimeResult_.data();
}

template <TimelineTimeMode Mode>
double* EngineManager::resolveTimelineTime(
    u32 valueLength, double numericValue, double reference
) {
    timelineTimeResult_[0] = 0.0;
    timelineTimeResult_[1] = 0.0;
    if (valueLength > TIMELINE_SETUP_INPUT_CAPACITY) return nullptr;
    const StringSpan value{timelineSetupInputBuffer_.data(), valueLength};
    double resolved = 0.0;
    if constexpr (Mode == TimelineTimeMode::BoundedDuration) {
        const StringSpan trimmed = trim_span(value);
        if (span_ends_with(trimmed, "%", 1)) {
            double percentage = 0.0;
            const StringSpan number{trimmed.data, trimmed.length - 1};
            resolved = parse_css_number_prefix(number, percentage)
                ? (percentage * reference / 100.0)
                : 0.0;
        } else {
            resolved = valueLength ? 0.0 : numericValue;
            if (valueLength && !parseFullNumber(value, resolved)) resolved = 0.0;
        }
        if (!__builtin_isfinite(resolved) || resolved < 0.0) resolved = 0.0;
        if (resolved > reference) resolved = reference;
        timelineTimeResult_[0] = resolved;
        timelineTimeResult_[1] = reference > 0.0 ? resolved / reference * 100.0 : 0.0;
        return timelineTimeResult_.data();
    } else {
        double percentage = 0.0;
        if (parseStrictPercent(value, percentage)) {
            const double basis = reference > 0.0 ? reference : FALLBACK_DURATION_1H_MS;
            resolved = percentage * basis / 100.0;
            timelineTimeResult_[0] = resolved;
            timelineTimeResult_[1] = 1.0;
            return timelineTimeResult_.data();
        }
        if (valueLength) {
            if (!parseFullNumber(value, resolved)) return timelineTimeResult_.data();
        } else {
            resolved = numericValue;
        }
        if (__builtin_isfinite(resolved)) {
            timelineTimeResult_[0] = resolved;
            timelineTimeResult_[1] = 1.0;
        }
        return timelineTimeResult_.data();
    }
}
template double* EngineManager::resolveTimelineTime<TimelineTimeMode::StartOrOffset>(u32, double, double);
template double* EngineManager::resolveTimelineTime<TimelineTimeMode::BoundedDuration>(u32, double, double);

double* EngineManager::timelineGeometryResultBuffer() {
    return timelineGeometryResult_.data();
}

u32* EngineManager::timelineTextStyleResultBuffer() {
    return timelineTextStyleResult_.data();
}

u32 EngineManager::timelineSetupInputCapacity() const {
    return TIMELINE_SETUP_INPUT_CAPACITY;
}

template <bool IsPlaying>
PipelineResult* EngineManager::evaluateFrame(
    u32 W, u32 H,
    u32 styleEpoch, u32 fontsVer, u32 styleVer, u32 mediaVer, u32 bgHash,
    u64 lastFrameHash, u64 occludedMask,
    double nowMs, u32 frameIndex
) {
    if (activeDurationResult_.durationMs < 0.0) activeDurationResult_.durationMs = 0.0;
    return pipeline_evaluate_position_T<IsPlaying>(
        W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash,
        lastFrameHash, occludedMask, nowMs, frameIndex);
}
template PipelineResult* EngineManager::evaluateFrame<false>(u32, u32, u32, u32, u32, u32, u32, u64, u64, double, u32);
template PipelineResult* EngineManager::evaluateFrame<true>(u32, u32, u32, u32, u32, u32, u32, u64, u64, double, u32);

template <bool IsPlaying>
PipelineResult* EngineManager::renderAt(
    u32 W, u32 H,
    u32 styleEpoch, u32 fontsVer, u32 styleVer, u32 mediaVer, u32 bgHash,
    u64 lastFrameHash, u64 occludedMask,
    double nowMs, u32 frameIndex
) {
    if (activeDurationResult_.durationMs < 0.0 || !std::isfinite(nowMs) ||
        nowMs < -I64_DOUBLE_LIMIT || nowMs >= I64_DOUBLE_LIMIT) {
        if (!std::isfinite(nowMs) || nowMs < 0.0) nowMs = 0.0;
    }
    return evaluateFrame<IsPlaying>(
        W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash,
        lastFrameHash, occludedMask, nowMs, frameIndex);
}
template PipelineResult* EngineManager::renderAt<false>(u32, u32, u32, u32, u32, u32, u32, u64, u64, double, u32);
template PipelineResult* EngineManager::renderAt<true>(u32, u32, u32, u32, u32, u32, u32, u64, u64, double, u32);

DurationResult& EngineManager::projectDurationResult() {
    return pendingDurationResult_;
}

u32 EngineManager::projectCompileStatus() const {
    return projectCompileStatus_;
}

u32 EngineManager::projectGeneration() const {
    return projectGeneration_;
}

timeline::runtime::Timeline<>& projectRuntime() {
    static timeline::runtime::Timeline<> runtime;
    return runtime;
}
} // namespace stockcut

namespace {
using stockcut::projectRuntime;
using stockcut::projectRuntimeStatusRef;
stockcut::EngineManager& manager() {
    return stockcut::engineManager();
}
stockcut::timeline::runtime::Frame<>& projectRuntimeFrame() {
    static stockcut::timeline::runtime::Frame<> frame{};
    return frame;
}
} // namespace

namespace stockcut {
timeline::runtime::Frame<>& engine_get_runtime_frame() {
    return projectRuntimeFrame();
}
} // namespace stockcut stockcut

WASM_EXPORT(wasm_manager_load_project_json) void
wasm_manager_load_project_json(
    stockcut::u32 length,
    double stageW,
    double stageH
) {
    const stockcut::DurationResult loaded =
        manager().loadProjectJson(length, stageW, stageH);
    if (loaded.durationMs < 0.0) {
        projectRuntimeStatusRef() =
            stockcut::timeline::runtime::RUNTIME_LOAD_COMPILE_FAILED;
    }
}

WASM_EXPORT(wasm_manager_get_project_compile_status) stockcut::u32
wasm_manager_get_project_compile_status() {
    return manager().projectCompileStatus();
}

WASM_EXPORT(wasm_timeline_runtime_get_load_status) stockcut::u32
wasm_timeline_runtime_get_load_status() {
    return static_cast<stockcut::u32>(projectRuntimeStatusRef());
}

WASM_EXPORT(wasm_timeline_runtime_render_at) stockcut::u32
wasm_timeline_runtime_render_at(
    double timeMs, stockcut::u32 width, stockcut::u32 height
) {
    if (projectRuntimeStatusRef() != stockcut::timeline::runtime::RUNTIME_LOAD_OK)
        return 0u;
    return projectRuntime().renderAt(
        timeMs, width, height, projectRuntimeFrame()) ? 1u : 0u;
}

WASM_EXPORT(wasm_timeline_runtime_set_bass_envelope) stockcut::u32
wasm_timeline_runtime_set_bass_envelope(
    stockcut::u32 slot, double hop, stockcut::u32 totalCount,
    stockcut::u32 offset, stockcut::u32 count,
    const float* interleaved
) {
    return projectRuntime().setBassEnvelopeChunk(
        slot, hop, totalCount, offset, count, interleaved) ? 1u : 0u;
}

WASM_EXPORT(wasm_timeline_runtime_get_pass_count) stockcut::u32
wasm_timeline_runtime_get_pass_count() {
    return projectRuntimeFrame().passCount;
}

WASM_EXPORT(wasm_timeline_runtime_get_target_count) stockcut::u32
wasm_timeline_runtime_get_target_count() {
    return stockcut::timeline::runtime::RUNTIME_RENDER_TARGET_COUNT;
}

WASM_EXPORT(wasm_timeline_runtime_get_passes)
stockcut::timeline::runtime::RenderPass*
wasm_timeline_runtime_get_passes() {
    return projectRuntimeFrame().passes.data();
}

WASM_EXPORT(wasm_timeline_runtime_get_kernels)
stockcut::timeline::runtime::BlurKernel*
wasm_timeline_runtime_get_kernels() {
    return projectRuntimeFrame().kernels.data();
}

WASM_EXPORT(wasm_timeline_runtime_get_item_count)
stockcut::u32 wasm_timeline_runtime_get_item_count() {
    return projectRuntimeFrame().itemCount;
}

WASM_EXPORT(wasm_timeline_runtime_get_items)
stockcut::timeline::runtime::FrameItem*
wasm_timeline_runtime_get_items() {
    return projectRuntimeFrame().items.data();
}

WASM_EXPORT(wasm_timeline_runtime_is_supported_render_plan) stockcut::u32
wasm_timeline_runtime_is_supported_render_plan() {
    const auto& frame = projectRuntimeFrame();
    // [FAST-PATH O(1)]: Nếu không có Effect và không có Bass-Reactive item, 100% plan hợp lệ
    if (!frame.hasBassReactiveItem && frame.activeEffectCount == 0) {
        return 1u;
    }
    const auto& rt = projectRuntime();
    const auto* script = stockcut::get_active_music_script();
    const bool hasScript = (script && script->totalSamples > 0);
    // Duyệt qua frame.items để kiểm tra effect hợp lệ nếu activeEffectCount > 0
    if (frame.activeEffectCount > 0) {
        for (stockcut::u32 index = 0; index < frame.itemCount; ++index) {
            const stockcut::u32 itemSlot = frame.items[index].slot;
            const auto* meta = rt.item(itemSlot);
            if (!meta || meta->effect == stockcut::EffectKind::None) continue;
            if (!stockcut::is_valid_effect_kind(meta->effect))
                return 0u;
        }
    }
    if (frame.hasBassReactiveItem && !hasScript) {
        for (stockcut::u32 index = 0; index < frame.itemCount; ++index) {
            const stockcut::u32 itemSlot = frame.items[index].slot;
            const auto* meta = rt.item(itemSlot);
            if (meta && meta->effectBassSource() && rt.bass_sample_count(itemSlot) == 0)
                return 0u;
        }
    }
    return 1u;
}

WASM_EXPORT(wasm_manager_begin_timeline) stockcut::u32
wasm_manager_begin_timeline(float stageW, float stageH) {
    return manager().beginTimeline(stageW, stageH) ? 1u : 0u;
}

WASM_EXPORT(wasm_manager_advance_timeline) stockcut::u64
wasm_manager_advance_timeline(stockcut::i64 nowMs) {
    return manager().advanceTimeline(nowMs);
}

WASM_EXPORT(wasm_manager_select_composite_frame_mode) stockcut::u32
wasm_manager_select_composite_frame_mode(
    stockcut::u32 sceneValid, double dirtyArea, double fullArea,
    stockcut::u32 dirtyCount
) {
    return static_cast<stockcut::u32>(
        sceneValid != 0u
            ? manager().selectCompositeFrameMode<true>(dirtyArea, fullArea, dirtyCount)
            : manager().selectCompositeFrameMode<false>(dirtyArea, fullArea, dirtyCount));
}

WASM_EXPORT(wasm_manager_select_static_composite_prefix) stockcut::u32
wasm_manager_select_static_composite_prefix(stockcut::u32 commandCount) {
    return manager().selectStaticCompositePrefix(commandCount);
}

WASM_EXPORT(wasm_manager_get_gpu_quad_commands_buffer)
stockcut::QuadCommand* wasm_manager_get_gpu_quad_commands_buffer() {
    return manager().gpuQuadCommandsBuffer();
}

WASM_EXPORT(wasm_manager_get_composite_render_culled_count) stockcut::u32
wasm_manager_get_composite_render_culled_count() {
    return manager().compositeRenderCulledCount();
}

WASM_EXPORT(wasm_manager_get_composite_effect_order_input_buffer)
stockcut::EffectOrderEntry*
wasm_manager_get_composite_effect_order_input_buffer() {
    return manager().compositeEffectOrderInputBuffer();
}

WASM_EXPORT(wasm_manager_get_composite_effect_order_output_buffer)
stockcut::u32* wasm_manager_get_composite_effect_order_output_buffer() {
    return manager().compositeEffectOrderOutputBuffer();
}

WASM_EXPORT(wasm_manager_order_composite_effects) stockcut::u32
wasm_manager_order_composite_effects(stockcut::u32 count) {
    return manager().orderCompositeEffects(count);
}

WASM_EXPORT(wasm_manager_compute_composite_static_key) stockcut::u32
wasm_manager_compute_composite_static_key(
    stockcut::u32 commandCount,
    double red, double green, double blue, double alpha
) {
    return manager().computeCompositeStaticKey(commandCount, red, green, blue, alpha);
}

WASM_EXPORT(wasm_manager_get_composite_css_color_buffer) char*
wasm_manager_get_composite_css_color_buffer() {
    return manager().compositeCssColorBuffer();
}

WASM_EXPORT(wasm_manager_get_composite_css_color_result_buffer) double*
wasm_manager_get_composite_css_color_result_buffer() {
    return manager().compositeCssColorResultBuffer();
}

WASM_EXPORT(wasm_manager_parse_composite_css_color) double*
wasm_manager_parse_composite_css_color(stockcut::u32 length, double opacity) {
    return manager().parseCompositeCssColor(length, opacity);
}

WASM_EXPORT(wasm_manager_get_timeline_setup_input_buffer) char*
wasm_manager_get_timeline_setup_input_buffer() {
    return manager().timelineSetupInputBuffer();
}

WASM_EXPORT(wasm_manager_get_timeline_geometry_result_buffer) double*
wasm_manager_get_timeline_geometry_result_buffer() {
    return manager().timelineGeometryResultBuffer();
}

WASM_EXPORT(wasm_manager_get_timeline_setup_input_capacity) stockcut::u32
wasm_manager_get_timeline_setup_input_capacity() {
    return manager().timelineSetupInputCapacity();
}

WASM_EXPORT(wasm_manager_get_timeline_text_style_result_buffer) stockcut::u32*
wasm_manager_get_timeline_text_style_result_buffer() {
    return manager().timelineTextStyleResultBuffer();
}

WASM_EXPORT(wasm_manager_compute_timeline_geometry) double*
wasm_manager_compute_timeline_geometry(
    stockcut::u32 flags, double stageW, double stageH,
    double intrinsicW, double intrinsicH,
    stockcut::u32 widthLength, stockcut::u32 heightLength,
    stockcut::u32 leftLength, stockcut::u32 topLength,
    stockcut::u32 transformLength, stockcut::u32 translateLength
) {
    return manager().computeTimelineGeometry(
        flags, stageW, stageH, intrinsicW, intrinsicH,
        widthLength, heightLength, leftLength, topLength,
        transformLength, translateLength);
}

WASM_EXPORT(wasm_manager_compute_timeline_keyframe_geometry) double*
wasm_manager_compute_timeline_keyframe_geometry(
    stockcut::u32 flags, double stageW, double stageH,
    double anchorX, double anchorY, double baseX, double baseY,
    double baseW, double baseH, stockcut::u32 widthLength,
    stockcut::u32 heightLength, stockcut::u32 leftLength,
    stockcut::u32 topLength
) {
    return manager().computeTimelineKeyframeGeometry(
        flags, stageW, stageH, anchorX, anchorY, baseX, baseY, baseW, baseH,
        widthLength, heightLength, leftLength, topLength);
}

WASM_EXPORT(wasm_manager_normalize_timeline_text_style) stockcut::u32*
wasm_manager_normalize_timeline_text_style(
    stockcut::u32 fontLength, stockcut::u32 weightLength,
    stockcut::u32 alignLength, stockcut::u32 revealLength,
    stockcut::u32 transformLength, stockcut::u32 easingLength
) {
    return manager().normalizeTimelineTextStyle(
        fontLength, weightLength, alignLength, revealLength,
        transformLength, easingLength);
}

WASM_EXPORT(wasm_manager_parse_timeline_color) stockcut::u32
wasm_manager_parse_timeline_color(stockcut::u32 length, stockcut::u32 fallback) {
    return manager().parseTimelineColor(length, fallback);
}

WASM_EXPORT(wasm_manager_get_timeline_time_result_buffer) double*
wasm_manager_get_timeline_time_result_buffer() {
    return manager().timelineTimeResultBuffer();
}

WASM_EXPORT(wasm_manager_resolve_timeline_time) double*
wasm_manager_resolve_timeline_time(
    stockcut::u32 valueLength, double numericValue,
    double reference, stockcut::u32 mode
) {
    return (mode & 1u) != 0u
        ? manager().resolveTimelineTime<stockcut::TimelineTimeMode::BoundedDuration>(
              valueLength, numericValue, reference)
        : manager().resolveTimelineTime<stockcut::TimelineTimeMode::StartOrOffset>(
              valueLength, numericValue, reference);
}

WASM_EXPORT(wasm_manager_classify_timeline_item_tier) stockcut::u32
wasm_manager_classify_timeline_item_tier(
    stockcut::u32 mediaType, stockcut::u32 flags,
    stockcut::u32 keyframeCount, stockcut::u32 hasReveal
) {
    using stockcut::MediaType;
    if (mediaType == static_cast<stockcut::u32>(MediaType::Audio) ||
        mediaType >= stockcut::MEDIA_TYPE_NB)
        return 0u;
    if (mediaType == static_cast<stockcut::u32>(MediaType::Video) ||
        (flags & (stockcut::TIER_FLAG_SPIN | stockcut::TIER_FLAG_SILK | stockcut::TIER_FLAG_INFINITE_ANIM)) != 0u)
        return 3u;
    if ((flags & (stockcut::TIER_FLAG_BASS_BG | stockcut::TIER_FLAG_ANIMATION)) != 0u ||
        keyframeCount != 0u || hasReveal != 0u)
        return 2u;
    return 1u;
}

WASM_EXPORT(wasm_manager_evaluate_frame) stockcut::PipelineResult*
wasm_manager_evaluate_frame(
    stockcut::u32 W, stockcut::u32 H,
    stockcut::u32 styleEpoch, stockcut::u32 fontsVer,
    stockcut::u32 styleVer, stockcut::u32 mediaVer,
    stockcut::u32 bgHash, stockcut::u64 lastFrameHash,
    stockcut::u64 occludedMask, double nowMs,
    stockcut::u32 frameIndex, stockcut::u32 isPlaying
) {
    return (isPlaying != 0u)
        ? manager().evaluateFrame<true>(
              W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash,
              lastFrameHash, occludedMask, nowMs, frameIndex)
        : manager().evaluateFrame<false>(
              W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash,
              lastFrameHash, occludedMask, nowMs, frameIndex);
}

WASM_EXPORT(wasm_manager_render_at) stockcut::PipelineResult*
wasm_manager_render_at(
    stockcut::u32 W, stockcut::u32 H,
    stockcut::u32 styleEpoch, stockcut::u32 fontsVer,
    stockcut::u32 styleVer, stockcut::u32 mediaVer,
    stockcut::u32 bgHash, stockcut::u64 lastFrameHash,
    stockcut::u64 occludedMask, double nowMs,
    stockcut::u32 frameIndex, stockcut::u32 isPlaying
) {
    return (isPlaying != 0u)
        ? manager().renderAt<true>(
              W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash,
              lastFrameHash, occludedMask, nowMs, frameIndex)
        : manager().renderAt<false>(
              W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash,
              lastFrameHash, occludedMask, nowMs, frameIndex);
}

WASM_EXPORT(wasm_manager_get_project_duration_result_ptr)
stockcut::timeline::project::DurationResult*
wasm_manager_get_project_duration_result_ptr() {
    return &manager().projectDurationResult();
}

WASM_EXPORT(wasm_manager_get_compiled_items_json_ptr) const char*
wasm_manager_get_compiled_items_json_ptr() {
    return stockcut::timeline::project::compiledItemsJson();
}

WASM_EXPORT(wasm_manager_get_compiled_items_json_length) stockcut::u32
wasm_manager_get_compiled_items_json_length() {
    return stockcut::timeline::project::compiledItemsJsonLength();
}

WASM_EXPORT(wasm_manager_get_project_json_ptr) char*
wasm_manager_get_project_json_ptr() {
    return stockcut::timeline::project::projectJsonBuffer();
}

WASM_EXPORT(wasm_manager_get_project_json_capacity) stockcut::u32
wasm_manager_get_project_json_capacity() {
    return stockcut::timeline::project::projectJsonCapacity();
}

WASM_EXPORT(wasm_manager_get_project_generation) stockcut::u32
wasm_manager_get_project_generation() {
    return manager().projectGeneration();
}

WASM_EXPORT(wasm_manager_do_json_move) double
wasm_manager_do_json_move(stockcut::u32 length, double stageW, double stageH) {
    const stockcut::DurationResult loaded =
        manager().doProjectJsonMove(length, stageW, stageH);
    if (loaded.durationMs < 0.0) {
        projectRuntimeStatusRef() =
            stockcut::timeline::runtime::RUNTIME_LOAD_COMPILE_FAILED;
        return -1.0;
    }
    return loaded.durationMs;
}

// ============================================================================
// STOCKFISH UCI-STYLE CONCISE WASM EXPORTS (Zero Overhead Aliases)
// ============================================================================
WASM_EXPORT(wasm_buf_setup) char* wasm_buf_setup() {
    return manager().timelineSetupInputBuffer();
}
WASM_EXPORT(wasm_buf_geom) double* wasm_buf_geom() {
    return manager().timelineGeometryResultBuffer();
}
WASM_EXPORT(wasm_buf_text) stockcut::u32* wasm_buf_text() {
    return manager().timelineTextStyleResultBuffer();
}
WASM_EXPORT(wasm_buf_time) double* wasm_buf_time() {
    return manager().timelineTimeResultBuffer();
}
WASM_EXPORT(wasm_quads_ptr) stockcut::QuadCommand* wasm_quads_ptr() {
    return wasm_manager_get_gpu_quad_commands_buffer();
}
WASM_EXPORT(wasm_plan_valid) stockcut::u32 wasm_plan_valid() {
    return wasm_timeline_runtime_is_supported_render_plan();
}
WASM_EXPORT(wasm_compiled_json_ptr) const char* wasm_compiled_json_ptr() {
    return wasm_manager_get_compiled_items_json_ptr();
}
WASM_EXPORT(wasm_compiled_json_len) stockcut::u32 wasm_compiled_json_len() {
    return wasm_manager_get_compiled_items_json_length();
}
