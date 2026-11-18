// ============================================================================
// [ĐÁNH GIÁ ĐIỂM NGHẼN ĐÃ XỬ LÝ — PHA D]:
// - Đã xóa bỏ hoàn toàn Engine Timeline thứ 2 (RuntimeItem 3.9 KB, readProperties,
//   readKeyframes, bakedStatic/Start/EndFrameItems, intervalTimes_, layersAt, 3.1 MB
//   static bass arrays).
// - Nạp PostFxComponent (128 B/slot) 1 lần trực tiếp từ compiler.cpp.
// - RenderPassPlanner đọc thẳng activeBB từ Position<> và dựng RenderPass DAG / PlanTT.
// ============================================================================
#include "runtime.h"
#include "hash_primitives.h"
#include "string_parse.h"
#include "math_utils.h"
#include "fx_math.h"
#include "json_reader.h"
#include "audio/script.h"
#include "render/graph.h"
#include "timeline/position.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace stockcut::timeline::runtime {
namespace {
using JsonSpan = project::JsonValueSpan;

constexpr u8     PASS_NO_TARGET           = INVALID_U8;
constexpr double MAX_BLUR_RADIUS          = 1024.0;
constexpr double MAX_EFFECT_FADE_MS       = 30000.0;
constexpr double SRGB_LINEAR_CUTOFF       = 0.04045;
constexpr double SRGB_LINEAR_DIVISOR      = 12.92;
constexpr double SRGB_GAMMA_OFFSET        = 0.055;
constexpr double SRGB_GAMMA_DIVISOR       = 1.055;
constexpr double SRGB_GAMMA_EXPONENT      = 2.4;
constexpr float  MS_TO_SEC_F32            = 0.001f;
constexpr double MS_TO_SEC_F64            = 0.001;
constexpr double SEC_TO_MS_F64            = 1000.0;
constexpr float  NON_WHITE_SAT_MIN        = 0.05f;
constexpr float  NON_WHITE_CHANNEL_MAX    = 0.95f;
constexpr float  U16_MAX_F32              = 65535.0f;

constexpr u8  FX_FLAG_MASK_HEADER       = RUNTIME_FRAME_FLAG_ZOOM_TILT_BASS |
                                          RUNTIME_FRAME_FLAG_LINEAR_TRANS |
                                          RUNTIME_FRAME_FLAG_POST_PROCESS;
constexpr u64 FX_HASH_MASK_NONE         = 0ULL;
constexpr u64 FX_HASH_MASK_STATIC_BLOOM = 1ULL;
constexpr u64 FX_HASH_MASK_FLICKER      = 2ULL;
constexpr u64 FX_HASH_MASK_ZOOM         = 3ULL;
constexpr u64 FX_HASH_MASK_TIME_ONLY    = 4ULL;
constexpr u64 FX_HASH_MASK_FULL_4D      = 5ULL;
constexpr float FX_QUANT_BLOOM_FACTOR   = 32.0f;
constexpr float FX_QUANT_INTENSITY      = 1024.0f;
constexpr float FX_QUANT_SCALE          = 64.0f;
constexpr float FX_QUANT_TIME           = 1.0f;
constexpr float ZOOM_TILT_EPS           = 0.001f;
constexpr float DEFAULT_TRANS_DUR_SEC   = 2.0f;

bool readProperty(
    const project::JsonReader& json, u32 objectStart, u32 objectEnd,
    const char* name, JsonSpan& result
) {
    result = {};
    if (json.field(objectStart, name, result.start, result.end)) {
        result.found = true;
        return true;
    }
    u32 nestedStart = 0;
    u32 nestedEnd = 0;
    if (json.field(objectStart, "css", nestedStart, nestedEnd) &&
        json.field(nestedStart, name, result.start, result.end)) {
        result.found = true;
        return true;
    }
    if (json.field(objectStart, "style", nestedStart, nestedEnd) &&
        json.field(nestedStart, name, result.start, result.end)) {
        result.found = true;
        return true;
    }
    u32 cursor = objectStart + 1;
    while (cursor < objectEnd) {
        u32 keyStart = 0;
        u32 keyEnd = 0;
        u32 valueStart = 0;
        u32 valueEnd = 0;
        if (!json.nextObjectField(cursor, objectEnd, keyStart, keyEnd,
                                  valueStart, valueEnd))
            break;
        MultiArray<char, 64> key;
        if (!json.readStringValue(keyStart, keyEnd, key.data(), static_cast<u32>(key.size())))
            return false;
        if (cstr_equal(key.data(), name)) {
            result = {valueStart, valueEnd, true};
            return true;
        }
    }
    return true;
}

bool readText(
    const project::JsonReader& json, const JsonSpan& value,
    char* output, u32 capacity
) {
    if (!value.found || !capacity) return false;
    return json.readStringValue(value.start, value.end, output, capacity);
}

bool readScalar(
    const project::JsonReader& json, const JsonSpan& value, double& result
) {
    if (!value.found) return false;
    if (json.valueNumber(value.start, value.end, result))
        return std::isfinite(result);
    MultiArray<char, 128> text;
    if (!readText(json, value, text.data(), static_cast<u32>(text.size()))) return false;
    char* end = nullptr;
    result = std::strtod(text.data(), &end);
    return end != text.data() && std::isfinite(result);
}

bool readEffectField(
    const project::JsonReader& json, u32 itemStart, const JsonSpan& params,
    const char* name, JsonSpan& outField
) {
    outField = {};
    if (params.found &&
        !readProperty(json, params.start, params.end, name, outField))
        return false;
    if (!outField.found)
        outField.found = json.field(itemStart, name, outField.start, outField.end);
    return true;
}

bool readEffectParameter(
    const project::JsonReader& json, u32 itemStart, const JsonSpan& params,
    const char* name, double& value
) {
    JsonSpan field{};
    if (!readEffectField(json, itemStart, params, name, field))
        return false;
    return !field.found || readScalar(json, field, value);
}

bool readEffectBassSource(
    const project::JsonReader& json, u32 itemStart, const JsonSpan& params,
    bool& outBassSource
) {
    JsonSpan source{};
    if (!readEffectField(json, itemStart, params, "source", source))
        return false;
    if (!source.found && params.found)
        source.found = json.field(params.start, "sourse", source.start, source.end);
    if (!source.found)
        source.found = json.field(itemStart, "sourse", source.start, source.end);
    if (!source.found) return true;
    MultiArray<char, 16> sourceName;
    if (!readText(json, source, sourceName.data(), static_cast<u32>(sourceName.size())))
        return false;
    outBassSource = cstr_iequal(sourceName.data(), "bass");
    return outBassSource || cstr_iequal(sourceName.data(), "cycle");
}

bool parseEffectColor(const char* text, float* output) {
    if (!text || text[0] != '#') return false;
    const char* hex = text + 1;
    const u32 digits = static_cast<u32>(std::strlen(hex));
    MultiArray<u32, 3> rgb;
    if (digits == 3u) {
        for (u32 channel = 0; channel < 3u; ++channel) {
            const i32 nibble = hex_nibble(hex[channel]);
            if (nibble < 0) return false;
            rgb[channel] = static_cast<u32>(nibble * 17);
        }
    } else if (digits == 6u) {
        for (u32 channel = 0; channel < 3u; ++channel) {
            const i32 h = hex_nibble(hex[channel * 2u]);
            const i32 l = hex_nibble(hex[channel * 2u + 1u]);
            if (h < 0 || l < 0) return false;
            rgb[channel] = static_cast<u32>(h * 16 + l);
        }
    } else {
        return false;
    }
    for (u32 channel = 0; channel < 3u; ++channel) {
        const double srgb = static_cast<double>(rgb[channel]) * INV_255_F64;
        output[channel] = static_cast<float>(srgb <= SRGB_LINEAR_CUTOFF
            ? srgb / SRGB_LINEAR_DIVISOR
            : std::pow((srgb + SRGB_GAMMA_OFFSET) / SRGB_GAMMA_DIVISOR, SRGB_GAMMA_EXPONENT));
    }
    return true;
}

[[nodiscard]] inline bool isNonWhiteLinearRgb(const MultiArray<float, 3>& color) noexcept {
    const float r = color[0], g = color[1], b = color[2];
    const float maxC = std::max(r, std::max(g, b));
    const float minC = std::min(r, std::min(g, b));
    return (maxC - minC > NON_WHITE_SAT_MIN) ||
           (r < NON_WHITE_CHANNEL_MAX || g < NON_WHITE_CHANNEL_MAX || b < NON_WHITE_CHANNEL_MAX);
}

template <usize Count>
bool findFirstEffectField(
    const project::JsonReader& json, u32 itemStart, const JsonSpan& params,
    const MultiArray<const char*, Count>& names, JsonSpan& outSpan
) {
    outSpan = {};
    for (const char* const* p = names.begin(), *const* const end = names.end(); p < end; ++p) {
        if (!readEffectField(json, itemStart, params, *p, outSpan))
            return false;
        if (outSpan.found) break;
    }
    return true;
}

bool parseBlurFunction(const char* text, double& radius) {
    if (!text || cstr_equal(text, "none") || text[0] == '\0') {
        radius = 0.0;
        return true;
    }
    if (std::strncmp(text, "blur(", 5) != 0) return false;
    const char* start = text + 5;
    char* end = nullptr;
    radius = std::strtod(start, &end);
    if (end == start || !std::isfinite(radius) || radius < 0.0)
        return false;
    if (std::strncmp(end, "px", 2) == 0) end += 2;
    while (*end == ' ' || *end == '\t') ++end;
    return *end++ == ')' && *end == '\0';
}

void readRainVec2(
    const project::JsonReader& json, u32 itemStart, const JsonSpan& params,
    const char* name, float& outX, float& outY
) {
    JsonSpan span{};
    if (!readEffectField(json, itemStart, params, name, span) || !span.found) return;
    if (json.isArray(span.start, span.end)) {
        u32 pos = span.start + 1;
        u32 vStart = 0, vEnd = 0;
        double d0 = outX, d1 = outY;
        if (json.nextArrayValue(pos, span.end, vStart, vEnd) && json.valueNumber(vStart, vEnd, d0)) {
            outX = static_cast<float>(d0);
            if (json.nextArrayValue(pos, span.end, vStart, vEnd) && json.valueNumber(vStart, vEnd, d1)) {
                outY = static_cast<float>(d1);
            } else {
                outY = outX;
            }
        }
        return;
    }
    double scalar = 0.0;
    if (readScalar(json, span, scalar)) {
        outX = static_cast<float>(scalar);
        outY = outX;
    }
}

void readRainBool(
    const project::JsonReader& json, u32 itemStart, const JsonSpan& params,
    const char* name, bool& outVal
) {
    JsonSpan span{};
    if (!readEffectField(json, itemStart, params, name, span) || !span.found) return;
    MultiArray<char, 16> buf;
    buf[0] = '\0';
    u32 len = 0;
    if (json.copySpan(span.start, span.end, buf.data(), static_cast<u32>(buf.size()), len)) {
        buf[len] = '\0';
        if (cstr_iequal(buf.data(), "true") || cstr_iequal(buf.data(), "\"true\"") || cstr_equal(buf.data(), "1")) {
            outVal = true;
            return;
        }
        if (cstr_iequal(buf.data(), "false") || cstr_iequal(buf.data(), "\"false\"") || cstr_equal(buf.data(), "0")) {
            outVal = false;
            return;
        }
    }
    double num = 0.0;
    if (readScalar(json, span, num)) outVal = (num >= 0.5);
}

void readRainBlurType(
    const project::JsonReader& json, u32 itemStart, const JsonSpan& params,
    u32& outBlurType
) {
    static constexpr MultiArray<const char*, 3> kBlurTypeKeys = {{
        "_blur_type", "blur_type", "blurType"
    }};
    JsonSpan span{};
    if (!findFirstEffectField(json, itemStart, params, kBlurTypeKeys, span) || !span.found) return;
    MultiArray<char, 24> text;
    text[0] = '\0';
    if (readText(json, span, text.data(), static_cast<u32>(text.size()))) {
        if (cstr_iequal(text.data(), "_gaussian") || cstr_iequal(text.data(), "gaussian") || cstr_equal(text.data(), "0")) {
            outBlurType = 0u;
            return;
        }
        if (cstr_iequal(text.data(), "_glass") || cstr_iequal(text.data(), "glass") || cstr_equal(text.data(), "1")) {
            outBlurType = 1u;
            return;
        }
    }
    double num = 1.0;
    if (readScalar(json, span, num)) {
        outBlurType = (num < 0.5) ? 0u : 1u;
    }
}

void readRainTint(
    const project::JsonReader& json, u32 itemStart, const JsonSpan& params,
    RainDropParams& rp
) {
    JsonSpan span{};
    if (!readEffectField(json, itemStart, params, "_tint", span) || !span.found) {
        if (!readEffectField(json, itemStart, params, "tint", span) || !span.found) return;
    }
    if (json.isArray(span.start, span.end)) {
        u32 pos = span.start + 1;
        u32 vStart = 0, vEnd = 0;
        double ch[4] = { rp.trail2TintR, rp.trail2TintG, rp.trail2TintB, rp.trail2TintA };
        for (int k = 0; k < 4; ++k) {
            if (!json.nextArrayValue(pos, span.end, vStart, vEnd) || !json.valueNumber(vStart, vEnd, ch[k])) break;
        }
        rp.trail2TintR = static_cast<float>(ch[0]);
        rp.trail2TintG = static_cast<float>(ch[1]);
        rp.trail2TintB = static_cast<float>(ch[2]);
        rp.trail2TintA = static_cast<float>(ch[3]);
    } else {
        MultiArray<char, 24> colorText;
        colorText[0] = '\0';
        float rgb[3] = {0.0f, 0.0f, 0.0f};
        if (readText(json, span, colorText.data(), static_cast<u32>(colorText.size())) &&
            parseEffectColor(colorText.data(), rgb)) {
            rp.trail2TintR = rgb[0];
            rp.trail2TintG = rgb[1];
            rp.trail2TintB = rgb[2];
            rp.trail2TintA = 1.0f;
        }
    }
    if (rp.trail2TintR > 0.002f || rp.trail2TintG > 0.002f || rp.trail2TintB > 0.002f) {
        rp.layerMask |= RAIN_FLAG_CUSTOM_TINT;
    } else {
        rp.layerMask &= ~RAIN_FLAG_CUSTOM_TINT;
    }
}

[[nodiscard]] inline bool parseBoolSpan(
    const project::JsonReader& json, const JsonSpan& span, bool defaultVal = true
) noexcept {
    if (!span.found) return defaultVal;
    MultiArray<char, 16> buf;
    buf[0] = '\0';
    u32 len = 0;
    if (json.copySpan(span.start, span.end, buf.data(), static_cast<u32>(buf.size()), len)) {
        buf[len] = '\0';
        if (cstr_iequal(buf.data(), "true") || cstr_iequal(buf.data(), "\"true\"") || cstr_equal(buf.data(), "1")) return true;
        if (cstr_iequal(buf.data(), "false") || cstr_iequal(buf.data(), "\"false\"") || cstr_equal(buf.data(), "0")) return false;
    }
    double num = defaultVal ? 1.0 : 0.0;
    if (readScalar(json, span, num)) return (num >= 0.5);
    return defaultVal;
}

[[nodiscard]] inline u32 matchRainLayerName(const char* name) noexcept {
    if (!name || !name[0]) return RAIN_LAYER_NONE;
    if (cstr_iequal(name, "rain-static-1") || cstr_iequal(name, "rain_static_1") ||
        cstr_iequal(name, "static1") || cstr_iequal(name, "_static1") ||
        cstr_iequal(name, "_enable_static1")) return RAIN_LAYER_STATIC1;
    if (cstr_iequal(name, "rain-static-2") || cstr_iequal(name, "rain_static_2") ||
        cstr_iequal(name, "static2") || cstr_iequal(name, "_static2") ||
        cstr_iequal(name, "_enable_static2")) return RAIN_LAYER_STATIC2;
    if (cstr_iequal(name, "rain-dynamic") || cstr_iequal(name, "rain_dynamic") ||
        cstr_iequal(name, "dynamic") || cstr_iequal(name, "_dynamic") ||
        cstr_iequal(name, "_enable_dynamic")) return RAIN_LAYER_DYNAMIC;
    if (cstr_iequal(name, "rain-trail") || cstr_iequal(name, "rain_trail") ||
        cstr_iequal(name, "trail") || cstr_iequal(name, "_trail") ||
        cstr_iequal(name, "_enable_trail")) return RAIN_LAYER_TRAIL;
    if (cstr_iequal(name, "rain-trail-2") || cstr_iequal(name, "rain_trail_2") ||
        cstr_iequal(name, "trail2") || cstr_iequal(name, "_trail2") ||
        cstr_iequal(name, "_enable_trail2")) return RAIN_LAYER_TRAIL2;
    return RAIN_LAYER_NONE;
}

inline void readChildParam(
    const project::JsonReader& json, u32 objStart, const JsonSpan& noParams,
    const char* keyA, const char* keyB, float& target, double lo, double hi
) {
    double v = static_cast<double>(target);
    if (keyA) readEffectParameter(json, objStart, noParams, keyA, v);
    if (keyB) readEffectParameter(json, objStart, noParams, keyB, v);
    target = static_cast<float>(std::clamp(v, lo, hi));
}

void parseRainLayerFields(
    const project::JsonReader& json, u32 objStart, const JsonSpan& objParams,
    u32 layerBit, RainDropParams& rp
) {
    if (layerBit == RAIN_LAYER_STATIC1) {
        readChildParam(json, objStart, objParams, "amount", nullptr, rp.amount1, 0.0, 3.0);
        readChildParam(json, objStart, objParams, "_amount1", "amount1", rp.amount1, 0.0, 3.0);
        readChildParam(json, objStart, objParams, "intensity", nullptr, rp.intensity1, 0.0, 3.0);
        readChildParam(json, objStart, objParams, "_intensity1", "intensity1", rp.intensity1, 0.0, 3.0);
        readChildParam(json, objStart, objParams, "scale", nullptr, rp.scale1, 0.05, 8.0);
        readChildParam(json, objStart, objParams, "_scale1", "scale1", rp.scale1, 0.05, 8.0);
        readChildParam(json, objStart, objParams, "noise", nullptr, rp.noise1, 0.0, 10.0);
        readChildParam(json, objStart, objParams, "_noise1", "noise1", rp.noise1, 0.0, 10.0);
        readChildParam(json, objStart, objParams, "speed", nullptr, rp.speed1, 0.0, 3.0);
        readChildParam(json, objStart, objParams, "_speed1", "speed1", rp.speed1, 0.0, 3.0);
    } else if (layerBit == RAIN_LAYER_STATIC2) {
        readChildParam(json, objStart, objParams, "amount", nullptr, rp.amount2, 0.0, 3.0);
        readChildParam(json, objStart, objParams, "_amount2", "amount2", rp.amount2, 0.0, 3.0);
        readChildParam(json, objStart, objParams, "intensity", nullptr, rp.intensity2, 0.0, 3.0);
        readChildParam(json, objStart, objParams, "_intensity2", "intensity2", rp.intensity2, 0.0, 3.0);
        readChildParam(json, objStart, objParams, "scale", nullptr, rp.scale2, 0.05, 8.0);
        readChildParam(json, objStart, objParams, "_scale2", "scale2", rp.scale2, 0.05, 8.0);
        readChildParam(json, objStart, objParams, "noise", nullptr, rp.noise2, 0.0, 10.0);
        readChildParam(json, objStart, objParams, "_noise2", "noise2", rp.noise2, 0.0, 10.0);
        readChildParam(json, objStart, objParams, "speed", nullptr, rp.speed2, 0.0, 3.0);
        readChildParam(json, objStart, objParams, "_speed2", "speed2", rp.speed2, 0.0, 3.0);
    } else if (layerBit == RAIN_LAYER_DYNAMIC) {
        readChildParam(json, objStart, objParams, "amount", nullptr, rp.amount3, 0.0, 1.0);
        readChildParam(json, objStart, objParams, "_amount3", "amount3", rp.amount3, 0.0, 1.0);
        readChildParam(json, objStart, objParams, "intensity", nullptr, rp.intensity3, 0.0, 3.0);
        readChildParam(json, objStart, objParams, "_intensity3", "intensity3", rp.intensity3, 0.0, 3.0);
        readChildParam(json, objStart, objParams, "scale", nullptr, rp.scale3, 0.05, 8.0);
        readChildParam(json, objStart, objParams, "_scale3", "scale3", rp.scale3, 0.05, 8.0);
        readChildParam(json, objStart, objParams, "speed", nullptr, rp.speed3, 0.0, 3.0);
        readChildParam(json, objStart, objParams, "_speed3", "speed3", rp.speed3, 0.0, 3.0);
    } else if (layerBit == RAIN_LAYER_TRAIL) {
        double seedVal = static_cast<double>(rp.trailSeed);
        readEffectParameter(json, objStart, objParams, "_seed", seedVal);
        readEffectParameter(json, objStart, objParams, "seed", seedVal);
        rp.trailSeed = std::clamp(static_cast<i32>(std::lround(seedVal)), 0, 1000);
        if (objParams.found) {
            // Flat _RaindropPro fallback uses _speed for trail
            readChildParam(json, objStart, objParams, "_speed", nullptr, rp.trailSpeed, 0.0, 3.0);
        } else {
            // Inside childs[] object: accept speed / _speed / _speed3
            readChildParam(json, objStart, objParams, "speed", "_speed3", rp.trailSpeed, 0.0, 3.0);
            readChildParam(json, objStart, objParams, "_speed", nullptr, rp.trailSpeed, 0.0, 3.0);
        }
        readChildParam(json, objStart, objParams, "_amount", "amount", rp.trailAmount, 0.0, 100.0);
        readChildParam(json, objStart, objParams, "_intensity", "intensity", rp.trailIntensity, 0.0, 3.0);
        readChildParam(json, objStart, objParams, "_scale", "scale", rp.trailScale, 0.01, 5.0);
        readRainVec2(json, objStart, objParams, "start_size", rp.trailStartSizeMin, rp.trailStartSizeMax);
        readRainVec2(json, objStart, objParams, "_start_size", rp.trailStartSizeMin, rp.trailStartSizeMax);
        rp.trailStartSizeMin = std::clamp(rp.trailStartSizeMin, 0.0f, 2.0f);
        rp.trailStartSizeMax = std::clamp(rp.trailStartSizeMax, 0.0f, 2.0f);
        readChildParam(json, objStart, objParams, "_spread", "spread", rp.trailSpread, 0.0, 1.0);
        readRainVec2(json, objStart, objParams, "start_lifetime", rp.trailLifeMin, rp.trailLifeMax);
        readRainVec2(json, objStart, objParams, "_start_lifetime", rp.trailLifeMin, rp.trailLifeMax);
        rp.trailLifeMin = std::clamp(rp.trailLifeMin, 0.1f, 30.0f);
        rp.trailLifeMax = std::clamp(rp.trailLifeMax, 0.1f, 30.0f);
        readRainVec2(json, objStart, objParams, "start_speed", rp.trailSpeedMin, rp.trailSpeedMax);
        readRainVec2(json, objStart, objParams, "_start_speed", rp.trailSpeedMin, rp.trailSpeedMax);
        rp.trailSpeedMin = std::clamp(rp.trailSpeedMin, 0.0f, 30.0f);
        rp.trailSpeedMax = std::clamp(rp.trailSpeedMax, 0.0f, 30.0f);
        readChildParam(json, objStart, objParams, "_direction", "direction", rp.trailDirection, -5.0, 5.0);
        readChildParam(json, objStart, objParams, "_radial", "radial", rp.trailRadial, 0.0, 2.0);
        readChildParam(json, objStart, objParams, "_offset", "offset", rp.trailOffset, -5.0, 5.0);
        readChildParam(json, objStart, objParams, "_noise", "noise", rp.trailNoise, 0.0, 2.0);
        bool prewarm = (rp.layerMask & RAIN_FLAG_PREWARM) != 0u;
        readRainBool(json, objStart, objParams, "prewarm", prewarm);
        readRainBool(json, objStart, objParams, "_prewarm", prewarm);
        if (prewarm) rp.layerMask |= RAIN_FLAG_PREWARM;
        else rp.layerMask &= ~RAIN_FLAG_PREWARM;
    } else if (layerBit == RAIN_LAYER_TRAIL2) {
        double seed2Val = static_cast<double>(rp.trail2Seed);
        readEffectParameter(json, objStart, objParams, "_seed2", seed2Val);
        if (!objParams.found) readEffectParameter(json, objStart, objParams, "seed", seed2Val);
        rp.trail2Seed = std::clamp(static_cast<i32>(std::lround(seed2Val)), 0, 1000);
        readChildParam(json, objStart, objParams, "strength", nullptr, rp.trail2Strength, 0.0, 2.0);
        readChildParam(json, objStart, objParams, "_strength2", "strength2", rp.trail2Strength, 0.0, 2.0);
        readChildParam(json, objStart, objParams, "amount", nullptr, rp.trail2Amount, 0.0, 100.0);
        readChildParam(json, objStart, objParams, "_amount4", "amount4", rp.trail2Amount, 0.0, 100.0);
        if (!objParams.found) {
            readRainVec2(json, objStart, objParams, "speed", rp.trail2SpeedMin, rp.trail2SpeedMax);
            readRainVec2(json, objStart, objParams, "scale", rp.trail2ScaleMin, rp.trail2ScaleMax);
            readRainVec2(json, objStart, objParams, "intensity", rp.trail2IntensityMin, rp.trail2IntensityMax);
        }
        readRainVec2(json, objStart, objParams, "speed4", rp.trail2SpeedMin, rp.trail2SpeedMax);
        readRainVec2(json, objStart, objParams, "_speed4", rp.trail2SpeedMin, rp.trail2SpeedMax);
        readRainVec2(json, objStart, objParams, "scale4", rp.trail2ScaleMin, rp.trail2ScaleMax);
        readRainVec2(json, objStart, objParams, "_scale4", rp.trail2ScaleMin, rp.trail2ScaleMax);
        readRainVec2(json, objStart, objParams, "intensity4", rp.trail2IntensityMin, rp.trail2IntensityMax);
        readRainVec2(json, objStart, objParams, "_intensity4", rp.trail2IntensityMin, rp.trail2IntensityMax);
        rp.trail2SpeedMin     = std::clamp(rp.trail2SpeedMin, 0.0f, 20.0f);
        rp.trail2SpeedMax     = std::clamp(rp.trail2SpeedMax, 0.0f, 20.0f);
        rp.trail2ScaleMin     = std::clamp(rp.trail2ScaleMin, 0.05f, 8.0f);
        rp.trail2ScaleMax     = std::clamp(rp.trail2ScaleMax, 0.05f, 8.0f);
        rp.trail2IntensityMin = std::clamp(rp.trail2IntensityMin, 0.0f, 2.0f);
        rp.trail2IntensityMax = std::clamp(rp.trail2IntensityMax, 0.0f, 2.0f);
        readChildParam(json, objStart, objParams, "_rotation", "rotation", rp.trail2Rotation, 0.0, 360.0);
        readChildParam(json, objStart, objParams, "_random_rotation", "random_rotation", rp.trail2RandRotation, 0.0, 1.0);
        readChildParam(json, objStart, objParams, "_random_pos", "random_pos", rp.trail2RandPos, 0.0, 2.0);
        readRainTint(json, objStart, objParams, rp);
    }
}

inline u64 computeRainHash64(const RainDropParams& rp) noexcept {
    const u32* words = reinterpret_cast<const u32*>(&rp);
    u64 h = 0x517CC1B727220A95ULL;
    // Hash fields 0..49 (200 bytes = 50 uint32 words before paramHash64)
    for (u32 i = 0; i < 50u; ++i) {
        h ^= hzobrist_mix64(static_cast<u64>(words[i]) + (static_cast<u64>(i + 1u) * 0x9E3779B97F4A7C15ULL));
    }
    return hzobrist_mix64(h);
}

bool parseSlotPostFxConfig(
    const project::JsonReader& json,
    u32 itemStart,
    MediaType mediaType,
    const char* filterCStr,
    PostFxComponent& item,
    RainDropParams& rainParams
) {
    (void)mediaType;
    rainParams = {};
    if (!item.is_standalone_effect()) {
        if (!filterCStr || !filterCStr[0]) return true;
        double radius = 0.0;
        if (!parseBlurFunction(filterCStr, radius) || radius > MAX_BLUR_RADIUS)
            return false;
        if (radius > 0.0) {
            item.effect = EffectKind::GaussianBlur;
            item.effectRadius = static_cast<float>(radius);
        }
        return true;
    }

    JsonSpan effect{};
    if (!json.field(itemStart, "effect", effect.start, effect.end))
        return false;
    effect.found = true;
    MultiArray<char, 64> effectName;
    if (!readText(json, effect, effectName.data(), static_cast<u32>(effectName.size())))
        return false;
    lower_ascii_inplace(effectName.data());
    item.effect = make_effect_kind(effectName.data());
    if (item.effect == EffectKind::None)
        return false;

    JsonSpan params{};
    u32 paramsStart = 0;
    u32 paramsEnd = 0;
    if (json.field(itemStart, "params", paramsStart, paramsEnd) &&
        json.isObject(paramsStart, paramsEnd))
        params = {paramsStart, paramsEnd, true};

    double defaultAmount = 1.0;
    readEffectParameter(json, itemStart, params, "amount", defaultAmount);
    readEffectParameter(json, itemStart, params, "intensity", defaultAmount);
    readEffectParameter(json, itemStart, params, "speed", defaultAmount);
    item.effectIntensity = static_cast<float>(defaultAmount);

    if (item.effect == EffectKind::Bloom) {
        double intensity = 1.0, threshold = 0.2, radius = 1.5;
        double factor = 0.8, tolerance = 0.4, fade = 200.0, aa = 1.0;
        if (!readEffectParameter(json, itemStart, params, "intensity", intensity) ||
            !readEffectParameter(json, itemStart, params, "threshold", threshold) ||
            !readEffectParameter(json, itemStart, params, "radius", radius) ||
            !readEffectParameter(json, itemStart, params, "bloomFactor", factor) ||
            !readEffectParameter(json, itemStart, params, "tolerance", tolerance) ||
            !readEffectParameter(json, itemStart, params, "fade", fade) ||
            !readEffectParameter(json, itemStart, params, "aa", aa))
            return false;
        item.effectIntensity   = static_cast<float>(std::clamp(intensity, 0.0, 10.0));
        item.effectThreshold   = static_cast<float>(std::clamp(threshold, 0.0, 1.0));
        item.effectRadius      = static_cast<float>(std::clamp(radius, 0.1, 10.0));
        item.effectBloomFactor = static_cast<float>(std::clamp(factor, 0.0, 1.0));
        item.effectTolerance   = static_cast<float>(std::clamp(tolerance, 0.05, 1.0));
        item.effectFade        = static_cast<float>(std::clamp(fade, 0.0, MAX_EFFECT_FADE_MS));
        item.effectAa          = static_cast<float>(std::clamp(aa, 0.0, 1.0));

        static constexpr MultiArray<const char*, 4> kBloomColorNames = {{
            "color", "targetColor", "keyColor", "tint"
        }};
        JsonSpan color{};
        if (!findFirstEffectField(json, itemStart, params, kBloomColorNames, color))
            return false;
        if (color.found) {
            MultiArray<char, 16> colorText;
            if (!readText(json, color, colorText.data(), static_cast<u32>(colorText.size())) ||
                !parseEffectColor(colorText.data(), item.effectColor.data()))
                return false;
            const bool isNonWhite = isNonWhiteLinearRgb(item.effectColor);
            JsonSpan modeSpan{};
            readEffectField(json, itemStart, params, "mode", modeSpan);
            MultiArray<char, 16> modeBuf;
            modeBuf[0] = '\0';
            if (modeSpan.found) readText(json, modeSpan, modeBuf.data(), static_cast<u32>(modeBuf.size()));
            if (cstr_equal(modeBuf.data(), "color")) {
                item.set_fx_flag(FX_FLAG_HAS_COLOR, true);
            } else if (cstr_equal(modeBuf.data(), "bright") || cstr_equal(modeBuf.data(), "luminance")) {
                item.set_fx_flag(FX_FLAG_HAS_COLOR, false);
            } else {
                item.set_fx_flag(FX_FLAG_HAS_COLOR, isNonWhite);
            }
        }
        return true;
    }

    if (item.effect == EffectKind::Flicker) {
        bool bassSource = false;
        if (!readEffectBassSource(json, itemStart, params, bassSource))
            return false;
        item.set_fx_flag(FX_FLAG_BASS_SOURCE, bassSource);
        JsonSpan shape{};
        if (!readEffectField(json, itemStart, params, "shape", shape))
            return false;
        if (shape.found) {
            MultiArray<char, 16> shapeName;
            if (!readText(json, shape, shapeName.data(), static_cast<u32>(shapeName.size())))
                return false;
            item.set_fx_flag(FX_FLAG_SQUARE, cstr_iequal(shapeName.data(), "square"));
        }
        double speed = 2.0;
        double brightness = bassSource ? 0.25 : 0.5;
        double base = 1.0;
        if (!readEffectParameter(json, itemStart, params, "speed", speed))
            return false;
        static constexpr MultiArray<const char*, 2> kFlickerBrightnessNames = {{
            "brightness", "intensity"
        }};
        JsonSpan value{};
        if (!findFirstEffectField(json, itemStart, params, kFlickerBrightnessNames, value))
            return false;
        if (value.found && !readScalar(json, value, brightness)) return false;
        if (!readEffectParameter(json, itemStart, params, "base", base))
            return false;
        item.effectSpeed      = static_cast<float>(std::clamp(speed, 0.1, 60.0));
        item.effectBrightness = static_cast<float>(std::max(brightness, 0.0));
        item.effectBase       = static_cast<float>(std::clamp(base, 0.0, 4.0));
        return true;
    }

    if (item.effect == EffectKind::Zoom ||
        item.effect == EffectKind::Shake) {
        u32 keyframesStart = 0;
        u32 keyframesEnd = 0;
        if (item.effect == EffectKind::Zoom &&
            json.field(itemStart, "keyframes", keyframesStart, keyframesEnd))
            return false;
        bool bassSource = false;
        if (!readEffectBassSource(json, itemStart, params, bassSource))
            return false;
        item.set_fx_flag(FX_FLAG_BASS_SOURCE, bassSource);
        double speed = item.effect == EffectKind::Zoom ? 0.616 : 0.5;
        double amplitude = item.effect == EffectKind::Zoom
            ? (bassSource ? 0.05 : 0.25) : 0.006;
        double base = item.effect == EffectKind::Zoom
            ? (bassSource ? 1.0 : 1.0 - amplitude) : 1.0;
        double phase = 0.578;
        double tilt = item.effect == EffectKind::Zoom ? 2.5 : 0.4;
        double fade = 0.0;
        if (!readEffectParameter(json, itemStart, params, "speed", speed) ||
            !readEffectParameter(json, itemStart, params, "amplitude", amplitude) ||
            !readEffectParameter(json, itemStart, params, "tilt", tilt))
            return false;
        if (item.effect == EffectKind::Zoom &&
            (!readEffectParameter(json, itemStart, params, "phase", phase) ||
             !readEffectParameter(json, itemStart, params, "base", base)))
            return false;
        if (item.effect == EffectKind::Shake &&
            !readEffectParameter(json, itemStart, params, "fade", fade))
            return false;
        item.effectSpeed = static_cast<float>(item.effect == EffectKind::Zoom
            ? std::clamp(speed, 0.05, 20.0)
            : std::clamp(speed, 0.1, 10.0));
        item.effectAmplitude = static_cast<float>(item.effect == EffectKind::Zoom
            ? std::clamp(amplitude, 0.0, 1.0)
            : std::clamp(amplitude, 0.0, 0.2));
        item.effectBase = static_cast<float>(item.effect == EffectKind::Zoom
            ? std::clamp(base, 0.05, 4.0) : 1.0);
        item.effectPhase = static_cast<float>(phase);
        item.effectTilt = static_cast<float>(item.effect == EffectKind::Zoom
            ? tilt : std::clamp(tilt, 0.0, 20.0));
        item.effectFade = static_cast<float>(std::max(fade, 0.0));
        if (item.effect == EffectKind::Zoom) {
            JsonSpan mirror{};
            if (!readEffectField(json, itemStart, params, "mirror", mirror))
                return false;
            if (mirror.found) {
                MultiArray<char, 8> mirrorText;
                u32 mirrorLength = 0;
                if (!json.copySpan(
                        mirror.start, mirror.end, mirrorText.data(),
                        static_cast<u32>(mirrorText.size()), mirrorLength) ||
                    mirrorLength >= mirrorText.size())
                    return false;
                mirrorText[mirrorLength] = '\0';
                if (cstr_equal(mirrorText.data(), "true") ||
                    cstr_equal(mirrorText.data(), "\"true\""))
                    item.set_fx_flag(FX_FLAG_MIRROR, true);
                else if (!cstr_equal(mirrorText.data(), "false") &&
                         !cstr_equal(mirrorText.data(), "\"false\""))
                    return false;
            }
        }
        return true;
    }

    if (item.effect == EffectKind::Turbulent) {
        double speed = 1.0, strength = 1.0, frequency = 1.0, octaves = 3.0, fade = 300.0;
        readEffectParameter(json, itemStart, params, "speed", speed);
        readEffectParameter(json, itemStart, params, "rate", speed);
        readEffectParameter(json, itemStart, params, "strength", strength);
        readEffectParameter(json, itemStart, params, "amplitude", strength);
        readEffectParameter(json, itemStart, params, "amount", strength);
        readEffectParameter(json, itemStart, params, "intensity", strength);
        readEffectParameter(json, itemStart, params, "frequency", frequency);
        readEffectParameter(json, itemStart, params, "scale", frequency);
        readEffectParameter(json, itemStart, params, "octaves", octaves);
        readEffectParameter(json, itemStart, params, "fade", fade);
        readEffectParameter(json, itemStart, params, "fadeMs", fade);
        item.effectSpeed      = static_cast<float>(std::clamp(speed, 0.0, 20.0));
        item.effectIntensity  = static_cast<float>(std::clamp(strength, 0.0, 10.0));
        item.effectRadius     = static_cast<float>(std::clamp(frequency, 0.1, 10.0));
        item.effectBrightness = static_cast<float>(std::clamp(std::floor(octaves), 1.0, 4.0));
        item.effectFade       = static_cast<float>(std::clamp(fade, 0.0, MAX_EFFECT_FADE_MS));
        return true;
    }

    if (item.effect == EffectKind::RainDrop) {
        double fade = 0.0;
        bool hasParentFade = false;
        {
            JsonSpan fadeSpan{};
            readEffectField(json, itemStart, params, "fade", fadeSpan);
            if (!fadeSpan.found) readEffectField(json, itemStart, params, "fadeMs", fadeSpan);
            if (fadeSpan.found && readScalar(json, fadeSpan, fade)) hasParentFade = true;
        }

        // Initialize standard NodeVideo _RaindropPro defaults
        rainParams = RainDropParams{};

        double strength = 1.0;
        double blur = 0.0;
        u32 blurType = 1u;
        readEffectParameter(json, itemStart, params, "_strength", strength);
        readEffectParameter(json, itemStart, params, "strength", strength);
        readRainBlurType(json, itemStart, params, blurType);
        readEffectParameter(json, itemStart, params, "_blur", blur);
        readEffectParameter(json, itemStart, params, "blur", blur);
        rainParams.strength = static_cast<float>(std::clamp(strength, 0.0, 2.0));
        rainParams.blurType = blurType;
        rainParams.blur     = static_cast<float>(std::clamp(blur, 0.0, 2.0));

        static constexpr MultiArray<const char*, 3> kChildsKeys = {{
            "childs", "layers", "children"
        }};
        JsonSpan childsSpan{};
        findFirstEffectField(json, itemStart, params, kChildsKeys, childsSpan);

        if (childsSpan.found && json.isArray(childsSpan.start, childsSpan.end)) {
            // Only enable sub-layers listed & enabled in "childs"
            rainParams.layerMask &= ~RAIN_LAYER_ALL;
            u32 pos = childsSpan.start + 1;
            u32 cStart = 0, cEnd = 0;
            static constexpr MultiArray<const char*, 5> kToggleKeysStatic1 = {{"rain-static-1", "rain_static_1", "static1", "_static1", "_enable_static1"}};
            static constexpr MultiArray<const char*, 5> kToggleKeysStatic2 = {{"rain-static-2", "rain_static_2", "static2", "_static2", "_enable_static2"}};
            static constexpr MultiArray<const char*, 5> kToggleKeysDynamic = {{"rain-dynamic",  "rain_dynamic",  "dynamic", "_dynamic", "_enable_dynamic"}};
            static constexpr MultiArray<const char*, 5> kToggleKeysTrail   = {{"rain-trail",    "rain_trail",    "trail",   "_trail",   "_enable_trail"}};
            static constexpr MultiArray<const char*, 5> kToggleKeysTrail2  = {{"rain-trail-2",  "rain_trail_2",  "trail2",  "_trail2",  "_enable_trail2"}};
            static constexpr MultiArray<const char*, 6> kLayerNameKeys     = {{"layer", "mode", "name", "id", "effect", "type"}};

            while (json.nextArrayValue(pos, childsSpan.end, cStart, cEnd)) {
                if (!json.isObject(cStart, cEnd)) continue;
                const JsonSpan noParams{};
                u32 layerBit = RAIN_LAYER_NONE;
                bool layerEnabled = true;

                JsonSpan toggleSpan{};
                auto checkToggle = [&](const MultiArray<const char*, 5>& keys, u32 bit) -> bool {
                    if (findFirstEffectField(json, cStart, noParams, keys, toggleSpan) && toggleSpan.found) {
                        layerBit = bit;
                        layerEnabled = parseBoolSpan(json, toggleSpan, true);
                        return true;
                    }
                    return false;
                };
                if (!checkToggle(kToggleKeysTrail2,  RAIN_LAYER_TRAIL2)  &&
                    !checkToggle(kToggleKeysTrail,   RAIN_LAYER_TRAIL)   &&
                    !checkToggle(kToggleKeysDynamic, RAIN_LAYER_DYNAMIC) &&
                    !checkToggle(kToggleKeysStatic2, RAIN_LAYER_STATIC2) &&
                    !checkToggle(kToggleKeysStatic1, RAIN_LAYER_STATIC1)) {
                    JsonSpan nameSpan{};
                    if (findFirstEffectField(json, cStart, noParams, kLayerNameKeys, nameSpan) && nameSpan.found) {
                        MultiArray<char, 32> nameBuf;
                        nameBuf[0] = '\0';
                        if (readText(json, nameSpan, nameBuf.data(), static_cast<u32>(nameBuf.size()))) {
                            layerBit = matchRainLayerName(nameBuf.data());
                        }
                    }
                    JsonSpan enSpan{};
                    if (readEffectField(json, cStart, noParams, "enabled", enSpan) && enSpan.found) {
                        layerEnabled = parseBoolSpan(json, enSpan, true);
                    } else if (readEffectField(json, cStart, noParams, "enable", enSpan) && enSpan.found) {
                        layerEnabled = parseBoolSpan(json, enSpan, true);
                    }
                }

                if (layerBit == RAIN_LAYER_NONE) continue;
                if (layerEnabled) rainParams.layerMask |= layerBit;
                else {
                    rainParams.layerMask &= ~layerBit;
                    continue;
                }

                if (!hasParentFade) {
                    double childFade = 0.0;
                    if (readEffectParameter(json, cStart, noParams, "fade", childFade) && childFade > 0.0) {
                        fade = std::max(fade, childFade);
                    }
                }
                parseRainLayerFields(json, cStart, noParams, layerBit, rainParams);
            }
        } else {
            // Flat / omitted childs: any unwritten child layer defaults to false unless _enable_* is explicitly true
            bool enS1 = false, enS2 = false, enDyn = false, enTr = false, enTr2 = false;
            readRainBool(json, itemStart, params, "_enable_static1", enS1);
            readRainBool(json, itemStart, params, "_enable_static2", enS2);
            readRainBool(json, itemStart, params, "_enable_dynamic", enDyn);
            readRainBool(json, itemStart, params, "_enable_trail",   enTr);
            readRainBool(json, itemStart, params, "_enable_trail2",  enTr2);
            rainParams.layerMask &= ~RAIN_LAYER_ALL;
            if (enS1)  rainParams.layerMask |= RAIN_LAYER_STATIC1;
            if (enS2)  rainParams.layerMask |= RAIN_LAYER_STATIC2;
            if (enDyn) rainParams.layerMask |= RAIN_LAYER_DYNAMIC;
            if (enTr)  rainParams.layerMask |= RAIN_LAYER_TRAIL;
            if (enTr2) rainParams.layerMask |= RAIN_LAYER_TRAIL2;

            // Mark params found flag for flat disambiguation
            const JsonSpan flatParams = params.found ? params : JsonSpan{itemStart, itemStart, true};
            parseRainLayerFields(json, itemStart, flatParams, RAIN_LAYER_STATIC1, rainParams);
            parseRainLayerFields(json, itemStart, flatParams, RAIN_LAYER_STATIC2, rainParams);
            parseRainLayerFields(json, itemStart, flatParams, RAIN_LAYER_DYNAMIC, rainParams);
            parseRainLayerFields(json, itemStart, flatParams, RAIN_LAYER_TRAIL,   rainParams);
            parseRainLayerFields(json, itemStart, flatParams, RAIN_LAYER_TRAIL2,  rainParams);
        }

        item.effectFade = static_cast<float>(std::clamp(fade, 0.0, MAX_EFFECT_FADE_MS));
        rainParams.paramHash64 = computeRainHash64(rainParams);
        item.effectSpeed      = rainParams.trailSpeed;
        item.effectIntensity  = rainParams.strength;
        item.effectBrightness = rainParams.trailScale;
        item.effectRadius     = rainParams.trailAmount;
        return true;
    }

    if (item.effect >= EffectKind::Intro) {
        double speed = 1.0, intensity = 1.0, fade = 200.0, density = 1.0, threshold = 0.0, radius = 1.0;
        readEffectParameter(json, itemStart, params, "speed", speed);
        readEffectParameter(json, itemStart, params, "rate", speed);
        readEffectParameter(json, itemStart, params, "intensity", intensity);
        readEffectParameter(json, itemStart, params, "amount", intensity);
        readEffectParameter(json, itemStart, params, "fade", fade);
        readEffectParameter(json, itemStart, params, "fadeMs", fade);
        readEffectParameter(json, itemStart, params, "density", density);
        readEffectParameter(json, itemStart, params, "lines", density);
        readEffectParameter(json, itemStart, params, "scanlines", density);
        readEffectParameter(json, itemStart, params, "scale", density);
        readEffectParameter(json, itemStart, params, "distortion", density);
        readEffectParameter(json, itemStart, params, "radius", radius);
        readEffectParameter(json, itemStart, params, "distance", radius);
        readEffectParameter(json, itemStart, params, "threshold", threshold);
        item.effectSpeed      = static_cast<float>(std::clamp(speed, 0.01, 100.0));
        item.effectIntensity  = static_cast<float>(std::max(0.0, intensity));
        item.effectFade       = static_cast<float>(std::clamp(fade, 0.0, MAX_EFFECT_FADE_MS));
        item.effectRadius     = static_cast<float>(radius);
        item.effectThreshold  = static_cast<float>(threshold);
        item.effectBrightness = static_cast<float>(density);
        static constexpr MultiArray<const char*, 3> kShaderColorNames = {{
            "color", "targetColor", "tint"
        }};
        JsonSpan color{};
        if (!findFirstEffectField(json, itemStart, params, kShaderColorNames, color))
            return false;
        if (color.found) {
            MultiArray<char, 16> colorText;
            if (readText(json, color, colorText.data(), static_cast<u32>(colorText.size())) &&
                parseEffectColor(colorText.data(), item.effectColor.data())) {
                item.set_fx_flag(FX_FLAG_HAS_COLOR, isNonWhiteLinearRgb(item.effectColor));
            }
        }
        return true;
    }

    double radius = 0.0;
    JsonSpan radiusValue{};
    radiusValue.found = json.field(itemStart, "radius", radiusValue.start, radiusValue.end);
    if (!radiusValue.found)
        radiusValue.found = json.field(itemStart, "blurRadius", radiusValue.start, radiusValue.end);
    if (params.found && !radiusValue.found) {
        radiusValue.found = json.field(params.start, "radius", radiusValue.start, radiusValue.end);
        if (!radiusValue.found)
            radiusValue.found = json.field(params.start, "blurRadius", radiusValue.start, radiusValue.end);
    }
    if (!radiusValue.found || !readScalar(json, radiusValue, radius) ||
        radius <= 0.0 || radius > MAX_BLUR_RADIUS)
        return false;
    item.effectRadius = static_cast<float>(radius);

    double intensity = 1.0;
    JsonSpan intensityValue{};
    intensityValue.found = json.field(itemStart, "intensity", intensityValue.start, intensityValue.end);
    if (params.found && !intensityValue.found)
        intensityValue.found = json.field(params.start, "intensity", intensityValue.start, intensityValue.end);
    if (intensityValue.found && (!readScalar(json, intensityValue, intensity) || intensity < 0.0 || intensity > 16.0))
        return false;
    item.effectIntensity = static_cast<float>(intensity);
    return true;
}

void bakeSlotPostFxMetadata(u32 slot, PostFxComponent& it, const RainDropParams& rp) noexcept {
    it.bakedFadeEnvelope = ReciprocalFadeEnvelope::bake(it.effectFade);
    it.bakedPassParam1 = (it.effect == EffectKind::Intro || it.effect == EffectKind::Outro)
        ? ((it.endMs > it.startMs) ? static_cast<float>((it.endMs - it.startMs) / SEC_TO_MS_F64) : DEFAULT_TRANS_DUR_SEC)
        : it.effectSpeed;
    const bool hasFx = (it.effect != EffectKind::None);
    const bool isZoomTiltBass = hasFx && it.effectBassSource() &&
        (it.effect == EffectKind::Zoom) && (std::abs(it.effectTilt) > ZOOM_TILT_EPS);
    const bool isLinearTrans = (it.effect == EffectKind::Intro || it.effect == EffectKind::Outro);
    const bool isTimeDrivenFx = isLinearTrans ||
        (it.effect == EffectKind::Shake) ||
        (it.effect >= EffectKind::Intro);
    const bool hasDynamicFxHash = (it.effect == EffectKind::Flicker) ||
        (it.effect == EffectKind::Zoom) ||
        (it.effect == EffectKind::Bloom) ||
        isTimeDrivenFx;
    it.bakedTimeHashScale = isTimeDrivenFx ? FX_QUANT_TIME : 0.0f;
    it.bakedEffectFlags = static_cast<u8>(
        (isZoomTiltBass ? RUNTIME_FRAME_FLAG_ZOOM_TILT_BASS : 0u) |
        (isLinearTrans ? RUNTIME_FRAME_FLAG_LINEAR_TRANS : 0u) |
        (hasFx ? RUNTIME_ITEM_FX_FLAG_HAS_EFFECT : 0u) |
        ((hasFx && it.effectBassSource()) ? RUNTIME_ITEM_FX_FLAG_BASS_REACTIVE : 0u));

    const u64 slotMult = HASH_GOLDEN_64 ^ (static_cast<u64>(slot + 1u) * ZOBRIST_MUL_A);
    const u64 bloomBias = (it.effect == EffectKind::Bloom)
        ? static_cast<u64>(static_cast<u32>(hzobrist_quantize_midtread(it.effectBloomFactor, FX_QUANT_BLOOM_FACTOR)))
        : 0ULL;
    u32 fxRadBits = 0u, fxSpdBits = 0u, fxBrtBits = 0u;
    std::memcpy(&fxRadBits, &it.effectRadius, sizeof(u32));
    std::memcpy(&fxSpdBits, &it.effectSpeed, sizeof(u32));
    std::memcpy(&fxBrtBits, &it.effectBrightness, sizeof(u32));
    const u64 fxParamMix =
        (static_cast<u64>(fxRadBits) * 0x9E3779B185EBCA87ULL) ^
        (static_cast<u64>(fxSpdBits) * 0xC2B2AE3D27D4EB4FULL) ^
        (static_cast<u64>(fxBrtBits) * 0x165667B19E3779F9ULL);
    const u64 rainParamMix = (it.effect == EffectKind::RainDrop)
        ? rp.paramHash64 : 0ULL;
    const u64 rawSeed = slotMult ^ bloomBias ^ fxParamMix ^ rainParamMix ^ (static_cast<u64>(it.effect) * ZOBRIST_MUL_C);
    const u64 qIntConst   = static_cast<u64>(static_cast<u16>(hzobrist_quantize_midtread(it.effectIntensity, FX_QUANT_INTENSITY)));
    const u64 qScaleConst = static_cast<u64>(static_cast<u16>(hzobrist_quantize_midtread(1.0f, FX_QUANT_SCALE)));
    const u64 qRotConst   = static_cast<u64>(hzobrist_pack_rot16(0.0f));
    const u64 qTimeConst  = static_cast<u64>(static_cast<u16>(hzobrist_quantize_midtread(0.0f, FX_QUANT_TIME)));
    if (!hasDynamicFxHash) {
        it.bakedEffectHashSeed = 0ULL;
        it.bakedEffectHashMask = FX_HASH_MASK_NONE;
    } else if (it.effect == EffectKind::Bloom && !it.effectBassSource()) {
        const u64 fxPackConst = qIntConst | (qScaleConst << 16) | (qRotConst << 32) | (qTimeConst << 48);
        it.bakedEffectHashSeed = hzobrist_mix64(fxPackConst ^ rawSeed);
        it.bakedEffectHashMask = FX_HASH_MASK_STATIC_BLOOM;
    } else if (it.effect == EffectKind::Flicker) {
        it.bakedEffectHashSeed = rawSeed ^ ((qScaleConst << 16) | (qRotConst << 32) | (qTimeConst << 48));
        it.bakedEffectHashMask = FX_HASH_MASK_FLICKER;
    } else if (it.effect == EffectKind::Zoom) {
        it.bakedEffectHashSeed = rawSeed ^ (qIntConst | (qTimeConst << 48));
        it.bakedEffectHashMask = FX_HASH_MASK_ZOOM;
    } else if (isTimeDrivenFx && !it.effectBassSource()) {
        it.bakedEffectHashSeed = rawSeed ^ (qIntConst | (qScaleConst << 16) | (qRotConst << 32));
        it.bakedEffectHashMask = FX_HASH_MASK_TIME_ONLY;
    } else {
        it.bakedEffectHashSeed = rawSeed;
        it.bakedEffectHashMask = FX_HASH_MASK_FULL_4D;
    }
}

// appendPass is now in runtime.h as inline template


template <typename Mask = Mask<1>>
bool buildGaussianKernel(
    Frame<Mask>& frame, u32 slot, double radius, u32& kernelIndex
) {
    if (frame.kernelCount == RUNTIME_TIMELINE_CAPACITY) return false;
    const double sigma = std::max(radius * 0.5, 0.25);
    const u32 tapCount = std::min(
        RUNTIME_BLUR_TAP_CAPACITY,
        std::max<u32>(1, static_cast<u32>(std::ceil(sigma * 3.0))));
    kernelIndex = frame.kernelCount++;
    BlurKernel& kernel = frame.kernels[kernelIndex];
    kernel.levelCount = 0u;
    kernel.kernelFlags = 0u;
    kernel.tapCount = static_cast<u8>(tapCount);
    double totalWeight = 1.0;
    for (u32 tap = 1; tap <= tapCount; ++tap) {
        const double normalized = static_cast<double>(tap) / sigma;
        const double weight = std::exp(-0.5 * normalized * normalized);
        kernel.weights[tap] = static_cast<float>(weight);
        totalWeight += 2.0 * weight;
    }
    kernel.weights[0] = static_cast<float>(1.0 / totalWeight);
    for (u32 tap = 1; tap <= tapCount; ++tap)
        kernel.weights[tap] = static_cast<float>(
            static_cast<double>(kernel.weights[tap]) / totalWeight);
    (void)slot;
    return true;
}

template <RuntimePassKind CompositeKind, typename Mask = Mask<1>>
bool appendBlurPasses(
    Frame<Mask>& frame, const PostFxComponent& meta, const FrameItem& item,
    u32 sourceTarget, u32 width, u32 height, double stageWidth, double stageHeight
) {
    const double radius = static_cast<double>(meta.effectRadius) *
        std::max(width / stageWidth, height / stageHeight);
    if (!std::isfinite(radius) || radius <= 0.0 || radius > MAX_BLUR_RADIUS)
        return false;
    u32 currentTarget = sourceTarget;
    u32 currentWidth = width;
    u32 currentHeight = height;
    u32 levels = 0;
    double effectiveRadius = radius;
    while ((effectiveRadius > 6.0 || levels == 0) &&
           levels < RUNTIME_MAX_DOWNSAMPLE_LEVELS &&
           (currentWidth > 1 || currentHeight > 1)) {
        const u32 nextWidth = currentWidth > 1 ? (currentWidth + 1) >> 1 : 1;
        const u32 nextHeight = currentHeight > 1 ? (currentHeight + 1) >> 1 : 1;
        if (nextWidth == currentWidth && nextHeight == currentHeight)
            break;
        const u32 target = 2 + levels;
        frame.targets[target] = {nextWidth, nextHeight};
        if (!appendPass<RuntimePassKind::Downsample>(
                frame, item.slot, currentTarget, target, nextWidth, nextHeight,
                static_cast<float>(effectiveRadius)))
            return false;
        currentTarget = target;
        currentWidth = nextWidth;
        currentHeight = nextHeight;
        effectiveRadius *= 0.5;
        ++levels;
    }
    if (currentWidth == 1 && currentHeight == 1)
        effectiveRadius = 1.0;
    const u32 scratchLevel = levels ? levels - 1 : 0;
    const u32 horizontalTarget =
        RUNTIME_BLUR_SCRATCH_TARGET_FIRST + scratchLevel * 2;
    const u32 verticalTarget = horizontalTarget + 1;
    u32 kernelIndex = 0;
    if (!buildGaussianKernel(frame, item.slot, effectiveRadius, kernelIndex))
        return false;
    frame.kernels[kernelIndex].levelCount = static_cast<u8>(levels);
    frame.targets[horizontalTarget] = {currentWidth, currentHeight};
    frame.targets[verticalTarget] = {currentWidth, currentHeight};
    if (!appendPass<RuntimePassKind::GaussianHorizontal>(
            frame, item.slot, currentTarget, horizontalTarget,
            currentWidth, currentHeight,
            static_cast<float>(effectiveRadius), 1.0f, kernelIndex) ||
        !appendPass<RuntimePassKind::GaussianVertical>(
            frame, item.slot, horizontalTarget, verticalTarget,
            currentWidth, currentHeight,
            static_cast<float>(effectiveRadius), 1.0f, kernelIndex))
        return false;
    return appendPass<CompositeKind>(
        frame, item.slot, verticalTarget,
        0, width, height, static_cast<float>(radius),
        item.effectIntensity);
}

template <typename Mask = Mask<1>>
bool appendBloomPasses(
    Frame<Mask>& frame, const PostFxComponent& meta, const FrameItem& item,
    u32 sourceTarget, u32 width, u32 height, double stageHeight
) {
    if (!appendPass<RuntimePassKind::CopyScene>(
            frame, item.slot, sourceTarget, 1, width, height))
        return false;
    MultiArray<u32, RUNTIME_MAX_DOWNSAMPLE_LEVELS> levelWidths;
    MultiArray<u32, RUNTIME_MAX_DOWNSAMPLE_LEVELS> levelHeights;
    u32 levelCount = 1;
    levelWidths[0] = std::max(1u, width >> 1);
    levelHeights[0] = std::max(1u, height >> 1);
    while (levelCount < RUNTIME_MAX_DOWNSAMPLE_LEVELS &&
           std::min(levelWidths[levelCount - 1],
                    levelHeights[levelCount - 1]) >= 8) {
        levelWidths[levelCount] = std::max(1u, levelWidths[levelCount - 1] >> 1);
        levelHeights[levelCount] = std::max(1u, levelHeights[levelCount - 1] >> 1);
        ++levelCount;
    }
    const double effectiveRadius =
        meta.effectRadius * height / std::max(stageHeight, 1.0);
    const double centerA = std::log2(5.24 * effectiveRadius);
    const double centerB = std::log2(14.3 * effectiveRadius);
    const double inverseTwoSigmaSquared = 1.0 / (2.0 * 0.85 * 0.85);
    MultiArray<double, RUNTIME_MAX_DOWNSAMPLE_LEVELS> weights;
    double sumA = 0.0;
    double sumB = 0.0;
    for (u32 level = 0; level < levelCount; ++level) {
        const double deltaA = level - centerA;
        const double deltaB = level - centerB;
        sumA += std::exp(-deltaA * deltaA * inverseTwoSigmaSquared);
        sumB += std::exp(-deltaB * deltaB * inverseTwoSigmaSquared);
    }
    double totalWeight = 0.0;
    for (u32 level = 0; level < levelCount; ++level) {
        const double deltaA = level - centerA;
        const double deltaB = level - centerB;
        weights[level] =
            0.55 * std::exp(-deltaA * deltaA * inverseTwoSigmaSquared) / sumA +
            0.45 * std::exp(-deltaB * deltaB * inverseTwoSigmaSquared) / sumB;
        totalWeight += weights[level];
    }
    for (u32 level = 0; level < levelCount; ++level)
        weights[level] /= totalWeight;
    const u32 extractTarget = 2;
    frame.targets[extractTarget] = {levelWidths[0], levelHeights[0]};
    if (!appendPass<RuntimePassKind::BloomExtract>(
            frame, item.slot, 1,
            extractTarget, levelWidths[0], levelHeights[0],
            meta.effectHasColor() ? 1.0f : 0.0f, meta.effectThreshold, 0,
            meta.effectTolerance,
            meta.effectColor[0], meta.effectColor[1], meta.effectColor[2]))
        return false;
    for (u32 level = 1; level < levelCount; ++level) {
        const u32 target = extractTarget + level;
        frame.targets[target] = {levelWidths[level], levelHeights[level]};
        if (!appendPass<RuntimePassKind::BloomDownsample>(
                frame, item.slot,
                target - 1, target, levelWidths[level], levelHeights[level],
                meta.effectRadius))
            return false;
    }
    u32 currentTarget = extractTarget + levelCount - 1;
    for (u32 level = levelCount - 1; level > 0; --level) {
        const u32 outputTarget = RUNTIME_BLUR_SCRATCH_TARGET_FIRST + level - 1;
        frame.targets[outputTarget] = {
            levelWidths[level - 1], levelHeights[level - 1]};
        const float wLow = static_cast<float>(level == levelCount - 1
            ? weights[levelCount - 1] : 1.0);
        const float wHi = static_cast<float>(weights[level - 1]);
        if (!appendPass<RuntimePassKind::BloomUpsample>(
                frame, item.slot,
                currentTarget, outputTarget, levelWidths[level - 1],
                levelHeights[level - 1], meta.effectRadius,
                0.0f, 0,
                wLow, wHi, 0.0f,
                static_cast<float>(extractTarget + level - 1)))
            return false;
        currentTarget = outputTarget;
    }
    const float fade = meta.bakedFadeEnvelope.evaluate_f64(
        frame.timeMs - meta.startMs, meta.endMs - frame.timeMs, meta.effectFade);
    return appendPass<RuntimePassKind::BloomComposite>(
        frame, item.slot, currentTarget, 0,
        width, height, meta.effectBloomFactor,
        item.effectIntensity, 0,
        meta.effectAa, 1.0f, fade);
}

using FlickerPeriodicFn = float (*)(float speed, float brightness, float baseVal, double tMs);
alignas(16) static constexpr MultiArray<FlickerPeriodicFn, 2> kFlickerPeriodicLut = {{
    &flicker_evaluate_periodic<false>,
    &flicker_evaluate_periodic<true>
}};

using ZoomPeriodicFn = void (*)(float speed, float amplitude, float phase, float tilt, double tMs, ZoomEvalResult* outResult);
alignas(16) static constexpr MultiArray<ZoomPeriodicFn, 2> kZoomPeriodicLut = {{
    &zoom_evaluate_periodic<false>,
    &zoom_evaluate_periodic<true>
}};

template <typename Mask = Mask<1>>
bool buildRenderPlan(
    Frame<Mask>& frame,
    const MultiArray<PostFxComponent, Mask::BITS>& metaItems,
    double stageWidth, double stageHeight,
    u32 width, u32 height
) {
    if (!width || !height || !std::isfinite(stageWidth) ||
        !std::isfinite(stageHeight) || stageWidth <= 0.0 ||
        stageHeight <= 0.0)
        return false;
    frame.targets[0] = {width, height};
    frame.targets[1] = {width, height};
    for (RenderTarget* t = frame.targets.data() + 2, *const tEnd = frame.targets.data() + RUNTIME_RENDER_TARGET_COUNT; t < tEnd; ++t)
        *t = {0, 0};
    if (!appendPass<RuntimePassKind::ClearScene>(
            frame, 0, PASS_NO_TARGET, 0, width, height))
        return false;

    if (frame.activeEffectCount == 0) {
        for (const FrameItem* itemPtr = frame.items.data(), *const itemEnd = itemPtr + frame.itemCount; itemPtr < itemEnd; ++itemPtr) {
            if (!appendPass<RuntimePassKind::DrawLayer>(frame, itemPtr->slot, PASS_NO_TARGET, 0, width, height))
                return false;
        }
        return true;
    }

    u8 currentSceneTarget = 0;
    for (const FrameItem* itemPtr = frame.items.data(), *const itemEnd = itemPtr + frame.itemCount; itemPtr < itemEnd; ++itemPtr) {
        const FrameItem& item = *itemPtr;
        const PostFxComponent& meta = metaItems[item.slot < RUNTIME_TIMELINE_CAPACITY ? item.slot : 0];
        if (meta.is_standalone_effect()) {
            if (meta.effect == EffectKind::None) return false;
            if (meta.effect == EffectKind::Flicker) {
                const u8 inTarget = currentSceneTarget;
                const u8 outTarget = 1 - currentSceneTarget;
                if (!appendPass<RuntimePassKind::ApplyBrightness>(
                        frame, item.slot, inTarget, outTarget, width, height, 0.0f, item.effectIntensity))
                    return false;
                currentSceneTarget = outTarget;
                continue;
            }
            if (meta.effect == EffectKind::Zoom ||
                meta.effect == EffectKind::Shake) {
                const float elapsed = item.elapsedMsF;
                const u8 inTarget = currentSceneTarget;
                const u8 outTarget = 1 - currentSceneTarget;
                if (meta.effect == EffectKind::Zoom) {
                    ZoomEvalResult result{};
                    if (meta.effectBassSource()) {
                        result.scale = item.effectScale;
                        result.rotateDeg = item.effectRotation;
                    } else {
                        kZoomPeriodicLut[meta.effectMirror() ? 1u : 0u](
                            meta.effectSpeed, meta.effectAmplitude,
                            meta.effectPhase, meta.effectTilt, elapsed, &result);
                    }
                    if (!appendPass<RuntimePassKind::ApplyZoom>(
                            frame, item.slot, inTarget, outTarget, width, height, result.scale,
                            result.rotateDeg, meta.effectMirror() ? 1u : 0u))
                        return false;
                } else {
                    ShakeEvalResult result{};
                    shake_evaluate<true>(
                        meta.effectSpeed, meta.effectAmplitude,
                        meta.effectTilt, meta.effectFade, elapsed,
                        meta.endMs - meta.startMs,
                        static_cast<float>(width), static_cast<float>(height),
                        &result);
                    if (!appendPass<RuntimePassKind::ApplyShake>(
                            frame, item.slot, inTarget, outTarget, width, height, result.scaleBoost,
                            result.rotDeg, 0, result.dx, result.dy))
                        return false;
                }
                currentSceneTarget = outTarget;
                continue;
            }
            if (meta.effect == EffectKind::Bloom) {
                if (currentSceneTarget != 0) {
                    if (!appendPass<RuntimePassKind::CopyScene>(frame, item.slot, currentSceneTarget, 0, width, height))
                        return false;
                    currentSceneTarget = 0;
                }
                if (!appendBloomPasses(frame, meta, item, 0, width, height, stageHeight))
                    return false;
                currentSceneTarget = 0;
                continue;
            }
            if (meta.effect >= EffectKind::Intro) {
                const u8 inTarget = currentSceneTarget;
                const u8 outTarget = 1 - currentSceneTarget;
                const float elapsedSec = static_cast<float>(static_cast<double>(item.elapsedMsF) * MS_TO_SEC_F64);
                const float fadeFactor = meta.bakedFadeEnvelope.evaluate_f64(
                    frame.timeMs - meta.startMs, meta.endMs - frame.timeMs, meta.effectFade);
                if (!appendPass<RuntimePassKind::ApplyShaderEffect>(
                        frame, item.slot, inTarget, outTarget, width, height, elapsedSec,
                        fadeFactor, static_cast<u8>(meta.effect),
                        item.effectIntensity, meta.bakedPassParam1, meta.effectBrightness, meta.effectRadius))
                    return false;
                currentSceneTarget = outTarget;
                continue;
            }
            if (currentSceneTarget != 0) {
                if (!appendPass<RuntimePassKind::CopyScene>(frame, item.slot, currentSceneTarget, 0, width, height))
                    return false;
                currentSceneTarget = 0;
            }
            const bool ok = (meta.effect == EffectKind::Glow)
                ? appendBlurPasses<RuntimePassKind::CompositeGlow>(
                    frame, meta, item, 0, width, height, stageWidth, stageHeight)
                : appendBlurPasses<RuntimePassKind::CompositeSceneBlur>(
                    frame, meta, item, 0, width, height, stageWidth, stageHeight);
            if (!ok) return false;
            currentSceneTarget = 0;
            continue;
        }
        if (currentSceneTarget != 0) {
            if (!appendPass<RuntimePassKind::CopyScene>(frame, item.slot, currentSceneTarget, 0, width, height))
                return false;
            currentSceneTarget = 0;
        }
        if (meta.effect == EffectKind::None ||
            meta.effect == EffectKind::Flicker ||
            meta.effect == EffectKind::Zoom ||
            meta.effect == EffectKind::Shake) {
            if (!appendPass<RuntimePassKind::DrawLayer>(
                    frame, item.slot, PASS_NO_TARGET, 0, width, height))
                return false;
            continue;
        }
        if (meta.effect == EffectKind::Bloom) {
            if (!appendPass<RuntimePassKind::DrawLayer>(
                    frame, item.slot, PASS_NO_TARGET, 0, width, height) ||
                !appendBloomPasses(
                    frame, meta, item, 0, width, height, stageHeight))
                return false;
            continue;
        }
        if (meta.effect >= EffectKind::Intro) {
            if (!appendPass<RuntimePassKind::DrawLayer>(
                    frame, item.slot, PASS_NO_TARGET, 0, width, height))
                return false;
            const u8 inTarget = currentSceneTarget;
            const u8 outTarget = 1 - currentSceneTarget;
            const float elapsedSec = static_cast<float>(static_cast<double>(item.elapsedMsF) * MS_TO_SEC_F64);
            const float fadeFactor = meta.bakedFadeEnvelope.evaluate_f64(
                frame.timeMs - meta.startMs, meta.endMs - frame.timeMs, meta.effectFade);
            if (!appendPass<RuntimePassKind::ApplyShaderEffect>(
                    frame, item.slot, inTarget, outTarget, width, height, elapsedSec,
                    fadeFactor, static_cast<u8>(meta.effect),
                    item.effectIntensity, meta.bakedPassParam1, meta.effectBrightness, meta.effectRadius))
                return false;
            currentSceneTarget = outTarget;
            continue;
        }
        if (meta.effect == EffectKind::Glow &&
            !appendPass<RuntimePassKind::DrawLayer>(
                frame, item.slot, PASS_NO_TARGET, 0, width, height))
            return false;
        if (!appendPass<RuntimePassKind::DrawLayer>(
                frame, item.slot, PASS_NO_TARGET, 1, width, height))
            return false;
        const bool ok = (meta.effect == EffectKind::Glow)
            ? appendBlurPasses<RuntimePassKind::CompositeGlow>(
                frame, meta, item, 1, width, height, stageWidth, stageHeight)
            : appendBlurPasses<RuntimePassKind::CompositeBlur>(
                frame, meta, item, 1, width, height, stageWidth, stageHeight);
        if (!ok) return false;
    }
    if (currentSceneTarget != 0) {
        if (!appendPass<RuntimePassKind::CopyScene>(frame, 0, currentSceneTarget, 0, width, height))
            return false;
        currentSceneTarget = 0;
    }
    return true;
}

template <RuntimePassKind Kind, bool IsIntervalEntry = false>
struct PassUniformSpec {
    static inline void update(
        RenderPass &pass,
        const PostFxComponent &meta,
        const FrameItem &item,
        double timeMs,
        float elapsedF,
        float widthF,
        float heightF) noexcept
    {
        if constexpr (Kind == RuntimePassKind::ApplyShaderEffect)
        {
            if constexpr (IsIntervalEntry)
            {
                pass.radius = static_cast<float>(static_cast<double>(elapsedF) * MS_TO_SEC_F64);
                pass.intensity = meta.bakedFadeEnvelope.evaluate_f64(
                    timeMs - meta.startMs, meta.endMs - timeMs, meta.effectFade);
            }
            else
            {
                const float remF = static_cast<float>(meta.endMs - timeMs);
                const float fadeFactor = meta.bakedFadeEnvelope.evaluate(elapsedF, remF);
                pass.radius = elapsedF * MS_TO_SEC_F32;
                pass.intensity = fadeFactor;
            }
            pass.parameter0 = item.effectIntensity;
            pass.parameter1 = meta.bakedPassParam1;
        }
        else if constexpr (Kind == RuntimePassKind::ApplyBrightness)
        {
            pass.intensity = item.effectIntensity;
            if constexpr (IsIntervalEntry)
                pass.parameter0 = 0.0f;
            else
                pass.parameter0 = item.effectIntensity;
            (void)meta; (void)timeMs; (void)elapsedF; (void)widthF; (void)heightF;
        }
        else if constexpr (Kind == RuntimePassKind::ApplyZoom)
        {
            pass.radius = item.effectScale;
            pass.intensity = item.effectRotation;
            (void)meta; (void)timeMs; (void)elapsedF; (void)widthF; (void)heightF;
        }
        else if constexpr (Kind == RuntimePassKind::ApplyShake)
        {
            ShakeEvalResult sRes{};
            shake_evaluate<true>(
                meta.effectSpeed, meta.effectAmplitude, meta.effectTilt, meta.effectFade,
                elapsedF, meta.endMs - meta.startMs, widthF, heightF, &sRes);
            pass.radius = sRes.scaleBoost;
            pass.intensity = sRes.rotDeg;
            pass.parameter0 = sRes.dx;
            pass.parameter1 = sRes.dy;
            (void)item; (void)timeMs;
        }
        else if constexpr (Kind == RuntimePassKind::BloomComposite)
        {
            if constexpr (IsIntervalEntry)
            {
                pass.parameter2 = meta.bakedFadeEnvelope.evaluate_f64(
                    timeMs - meta.startMs, meta.endMs - timeMs, meta.effectFade);
            }
            else
            {
                const float remF = static_cast<float>(meta.endMs - timeMs);
                const float fadeFactor = meta.bakedFadeEnvelope.evaluate(elapsedF, remF);
                pass.parameter2 = smoothstep01(fadeFactor);
            }
            (void)item; (void)widthF; (void)heightF;
        }
        else
        {
            (void)pass; (void)meta; (void)item; (void)timeMs; (void)elapsedF; (void)widthF; (void)heightF;
        }
    }
};

struct PassUniformTable {
    static constexpr u32 kUniformKindMask =
        (1u << static_cast<u8>(RuntimePassKind::ApplyBrightness)) |
        (1u << static_cast<u8>(RuntimePassKind::ApplyZoom)) |
        (1u << static_cast<u8>(RuntimePassKind::ApplyShake)) |
        (1u << static_cast<u8>(RuntimePassKind::BloomComposite)) |
        (1u << static_cast<u8>(RuntimePassKind::ApplyShaderEffect));

    static inline bool hasUniformUpdate(RuntimePassKind kind) noexcept {
        return ((1u << static_cast<u8>(kind)) & kUniformKindMask) != 0;
    }

    template <bool IsIntervalEntry = false, usize Capacity = RUNTIME_TIMELINE_CAPACITY>
    static inline void update(
        RuntimePassKind kind,
        RenderPass &pass,
        const MultiArray<PostFxComponent, Capacity> &items,
        const FrameItem &item,
        double timeMs,
        float elapsedF,
        float widthF,
        float heightF) noexcept
    {
        const PostFxComponent &meta = items[pass.slot];
        switch (kind) {
            case RuntimePassKind::ApplyBrightness:
                PassUniformSpec<RuntimePassKind::ApplyBrightness, IsIntervalEntry>::update(
                    pass, meta, item, timeMs, elapsedF, widthF, heightF);
                break;
            case RuntimePassKind::ApplyZoom:
                PassUniformSpec<RuntimePassKind::ApplyZoom, IsIntervalEntry>::update(
                    pass, meta, item, timeMs, elapsedF, widthF, heightF);
                break;
            case RuntimePassKind::ApplyShake:
                PassUniformSpec<RuntimePassKind::ApplyShake, IsIntervalEntry>::update(
                    pass, meta, item, timeMs, elapsedF, widthF, heightF);
                break;
            case RuntimePassKind::BloomComposite:
                PassUniformSpec<RuntimePassKind::BloomComposite, IsIntervalEntry>::update(
                    pass, meta, item, timeMs, elapsedF, widthF, heightF);
                break;
            case RuntimePassKind::ApplyShaderEffect:
                PassUniformSpec<RuntimePassKind::ApplyShaderEffect, IsIntervalEntry>::update(
                    pass, meta, item, timeMs, elapsedF, widthF, heightF);
                break;
            default:
                break;
        }
    }
};

constexpr u32 PLAN_TT_SETS = 128;
constexpr u32 PLAN_TT_WAYS = 2;
constexpr u32 PLAN_TT_TOTAL = PLAN_TT_SETS * PLAN_TT_WAYS;
constexpr u32 PLAN_TT_MAX_PASSES = 64;
constexpr u32 PLAN_TT_MAX_KERNELS = 8;

struct alignas(64) PlanTTEntry {
    u32 epoch = INVALID_U32;
    u32 width = 0;
    u32 height = 0;
    u32 passCount = 0;
    u32 kernelCount = 0;
    u32 uniformPassCount = 0;
    u64 activeMask = 0ULL;
    PassKindBitmask passKindMask;
    u16 itemCount = 0;
    u16 activeEffectCount = 0;
    u8 hasPostProcessing = 0;
    u8 hasZoomTiltBass = 0;
    u8 hasBassReactiveItem = 0;
    u8 momentStaticOk = 0;
    u8 allZeroStartMs = 0;
    u8 pad[3]{};
    u64 activeFusibleEffectsBB = 0ULL;
    u64 activeDynFxBB = 0ULL;
    u64 staticFxZobrist = 0ULL;
    double momentStartMs = 0.0;
    double momentEndMs = -1.0;
    MultiArray<u8, RUNTIME_TIMELINE_CAPACITY> activeSlots;
    MultiArray<u8, RUNTIME_TIMELINE_CAPACITY> slotMap;
    MultiArray<u64, MEDIA_TYPE_NB> byTypeSummary;
    MultiArray<UniformJob, RUNTIME_TIMELINE_CAPACITY + 1> uniformPasses;
    MultiArray<RenderPass, PLAN_TT_MAX_PASSES + 1> passes;
    MultiArray<RenderTarget, RUNTIME_RENDER_TARGET_COUNT> targets;
    MultiArray<BlurKernel, PLAN_TT_MAX_KERNELS> kernels;
};

alignas(64) static MultiArray<PlanTTEntry, PLAN_TT_TOTAL> s_planTT;
static const void* s_lastGlobalOutputPtr = nullptr;
static u32 s_lastGlobalToken = 0;
static u32 s_planVictimToggle = 0;

inline void copyPlanBuffers(
    RenderPass* dstPasses,
    RenderTarget* dstTargets,
    BlurKernel* dstKernels,
    const RenderPass* srcPasses,
    const RenderTarget* srcTargets,
    const BlurKernel* srcKernels,
    u32 passCount,
    u32 kernelCount
) noexcept {
    std::memcpy(dstPasses, srcPasses, passCount * sizeof(RenderPass));
    dstPasses[passCount].kind = RuntimePassKind::Sentinel;
    std::memcpy(dstTargets, srcTargets, RUNTIME_RENDER_TARGET_COUNT * sizeof(RenderTarget));
    if (kernelCount > 0) {
        std::memcpy(dstKernels, srcKernels, kernelCount * sizeof(BlurKernel));
    }
}

template <bool IsIntervalEntry, typename Mask = Mask<1>>
inline void dispatchUniformJobs(
    const UniformJob* jobs,
    u32 jobCount,
    Frame<Mask>& output,
    const MultiArray<PostFxComponent, Mask::BITS>& items,
    float widthF,
    float heightF
) noexcept {
    for (const UniformJob* job = jobs, *const jobEnd = jobs + jobCount; job < jobEnd; ++job)
    {
        RenderPass &pass = output.passes[job->index];
        const u8 itemIdx = output.slotMap[pass.slot];
        const FrameItem &item = output.items[itemIdx];
        PassUniformTable::update<IsIntervalEntry>(
            job->kind, pass, items, item, output.timeMs, item.elapsedMsF, widthF, heightF);
    }
}

struct DynFxFrameMemo {
    const PostFxComponent* lastFlicker = nullptr;
    const PostFxComponent* lastZoom = nullptr;
    float flickerIntensity = 0.0f;
    u64 flickerQInt = 0ULL;
    float zoomScale = 1.0f;
    float zoomRot = 0.0f;
    u64 zoomQPack = 0ULL;
    float lastTimeOnlyElapsedF = -1.0f;
    u64 lastQTimeShifted = 0ULL;
};

inline u64 evaluate_dynamic_fx_item(
    const PostFxComponent &item,
    FrameItem &active,
    double timeMs,
    float elapsedF,
    const MusicAnalysisScript *script,
    bool hasScript,
    const SlotBassEnvelopeMeta &bm,
    const float *bassLevels,
    const u16 *bassHits,
    DynFxFrameMemo &memo
) noexcept {
    if (item.bakedEffectFlags == 0u) return 0ULL;

    if (!item.effectBassSource()) {
        if (item.effect == EffectKind::Flicker) {
            if (memo.lastFlicker &&
                memo.lastFlicker->startMs == item.startMs &&
                memo.lastFlicker->effectSpeed == item.effectSpeed &&
                memo.lastFlicker->effectBrightness == item.effectBrightness &&
                memo.lastFlicker->effectBase == item.effectBase &&
                memo.lastFlicker->effectSquare() == item.effectSquare())
            {
                active.effectIntensity = memo.flickerIntensity;
                if (item.bakedEffectHashMask == FX_HASH_MASK_FLICKER) {
                    return hzobrist_mix64(memo.flickerQInt ^ item.bakedEffectHashSeed);
                }
            } else {
                active.effectIntensity = kFlickerPeriodicLut[item.effectSquare() ? 1u : 0u](
                    item.effectSpeed, item.effectBrightness, item.effectBase, timeMs - item.startMs);
                memo.lastFlicker = &item;
                memo.flickerIntensity = active.effectIntensity;
                if (item.bakedEffectHashMask == FX_HASH_MASK_FLICKER) {
                    const u64 qInt = static_cast<u64>(static_cast<u16>(hzobrist_quantize_midtread(active.effectIntensity, FX_QUANT_INTENSITY)));
                    memo.flickerQInt = qInt;
                    return hzobrist_mix64(qInt ^ item.bakedEffectHashSeed);
                }
            }
        } else if (item.effect == EffectKind::Zoom) {
            if (memo.lastZoom &&
                memo.lastZoom->startMs == item.startMs &&
                memo.lastZoom->effectSpeed == item.effectSpeed &&
                memo.lastZoom->effectAmplitude == item.effectAmplitude &&
                memo.lastZoom->effectPhase == item.effectPhase &&
                memo.lastZoom->effectTilt == item.effectTilt &&
                memo.lastZoom->effectMirror() == item.effectMirror())
            {
                active.effectScale = memo.zoomScale;
                active.effectRotation = memo.zoomRot;
                if (item.bakedEffectHashMask == FX_HASH_MASK_ZOOM) {
                    return hzobrist_mix64(memo.zoomQPack ^ item.bakedEffectHashSeed);
                }
            } else {
                ZoomEvalResult zRes; // KHONG CO {}
                kZoomPeriodicLut[item.effectMirror() ? 1u : 0u](
                    item.effectSpeed, item.effectAmplitude, item.effectPhase, item.effectTilt, elapsedF, &zRes);
                active.effectScale = zRes.scale;
                active.effectRotation = zRes.rotateDeg;
                memo.lastZoom = &item;
                memo.zoomScale = zRes.scale;
                memo.zoomRot = zRes.rotateDeg;
                if (item.bakedEffectHashMask == FX_HASH_MASK_ZOOM) {
                    const u64 qScale = static_cast<u64>(static_cast<u16>(hzobrist_quantize_midtread(active.effectScale, FX_QUANT_SCALE)));
                    const u64 qRot   = static_cast<u64>(hzobrist_pack_rot16(active.effectRotation));
                    const u64 qPack  = (qScale << 16) | (qRot << 32);
                    memo.zoomQPack = qPack;
                    return hzobrist_mix64(qPack ^ item.bakedEffectHashSeed);
                }
            }
        }
    } else {
        float level = 0.0f;
        u32 hit = 0;
        if (hasScript) {
            level = script->sample_level(timeMs);
            hit = script->sample_hit_index(timeMs);
        } else if (bm.count && bassLevels && bassHits) {
            const double hopMs = SEC_TO_MS_F64 * bm.hop;
            const double maxBassMs = static_cast<double>(bm.count) * hopMs;
            const double effTimeMs = (maxBassMs > 0.0 && timeMs >= maxBassMs) ? std::fmod(timeMs, maxBassMs) : timeMs;
            const double samplePosition = effTimeMs / hopMs;
            const u32 posInt = static_cast<u32>(samplePosition);
            const u32 bounded = posInt % bm.count;
            const u32 nextSample = (bounded + 1) - bm.count * static_cast<u32>(bounded + 1 >= bm.count);
            const float fraction = static_cast<float>(samplePosition - static_cast<double>(posInt));
            const float first = bassLevels[bm.offset + bounded];
            const float second = bassLevels[bm.offset + nextSample];
            level = first + (second - first) * fraction;
            const u32 loopCount = (maxBassMs > 0.0) ? static_cast<u32>(timeMs / maxBassMs) : 0;
            hit = loopCount * bassHits[bm.offset + bm.count - 1] + bassHits[bm.offset + bounded];
        }
        if (item.effect == EffectKind::Flicker) {
            active.effectIntensity = std::max(0.0f, item.effectBase + item.effectBrightness * level);
        } else if (item.effect == EffectKind::Zoom) {
            active.effectScale = item.effectBase + std::max(0.0f, 1.0f + item.effectAmplitude - item.effectBase) * level;
            active.effectRotation = item.effectTilt * level * MusicAnalysisScript::rotation_sign(hit);
        }
    }

    if (item.bakedEffectHashMask != FX_HASH_MASK_NONE) {
        if (item.bakedEffectHashMask == FX_HASH_MASK_STATIC_BLOOM) {
            return item.bakedEffectHashSeed;
        } else if (item.bakedEffectHashMask == FX_HASH_MASK_TIME_ONLY) {
            if (elapsedF != memo.lastTimeOnlyElapsedF) {
                memo.lastTimeOnlyElapsedF = elapsedF;
                const u32 qTime32 = static_cast<u32>(hzobrist_quantize_midtread(elapsedF, FX_QUANT_TIME));
                memo.lastQTimeShifted =
                    (static_cast<u64>(qTime32 & INVALID_U16) << 48) ^
                    (static_cast<u64>(qTime32 >> 16) * ZOBRIST_MUL_D);
            }
            return hzobrist_mix64(memo.lastQTimeShifted ^ item.bakedEffectHashSeed);
        } else if (item.bakedEffectHashMask == FX_HASH_MASK_FLICKER) {
            const u64 qInt = static_cast<u64>(static_cast<u16>(hzobrist_quantize_midtread(active.effectIntensity, FX_QUANT_INTENSITY)));
            return hzobrist_mix64(qInt ^ item.bakedEffectHashSeed);
        } else if (item.bakedEffectHashMask == FX_HASH_MASK_ZOOM) {
            const u64 qScale = static_cast<u64>(static_cast<u16>(hzobrist_quantize_midtread(active.effectScale, FX_QUANT_SCALE)));
            const u64 qRot   = static_cast<u64>(hzobrist_pack_rot16(active.effectRotation));
            return hzobrist_mix64(((qScale << 16) | (qRot << 32)) ^ item.bakedEffectHashSeed);
        } else {
            const u64 qInt   = static_cast<u64>(static_cast<u16>(hzobrist_quantize_midtread(active.effectIntensity, FX_QUANT_INTENSITY)));
            const u64 qScale = static_cast<u64>(static_cast<u16>(hzobrist_quantize_midtread(active.effectScale, FX_QUANT_SCALE)));
            const u64 qRot   = static_cast<u64>(hzobrist_pack_rot16(active.effectRotation));
            const u32 qTime32 = static_cast<u32>(hzobrist_quantize_midtread(elapsedF * item.bakedTimeHashScale, FX_QUANT_TIME));
            const u64 qTime  = static_cast<u64>(qTime32 & INVALID_U16);
            const u64 fxPack = qInt | (qScale << 16) | (qRot << 32) | (qTime << 48) ^
                               (static_cast<u64>(qTime32 >> 16) * ZOBRIST_MUL_D);
            return hzobrist_mix64(fxPack ^ item.bakedEffectHashSeed);
        }
    }
    return 0ULL;
}
} // namespace

template <typename Mask>
void Timeline<Mask>::begin_slot_stage(double stageWidth, double stageHeight, double durationMs) noexcept {
    stagedStageW_ = stageWidth;
    stagedStageH_ = stageHeight;
    stagedDurationMs_ = durationMs;
}

template <typename Mask>
RuntimeLoadStatus Timeline<Mask>::stage_slot_fx(
    u32 slot, MediaType type, double startMs, double endMs, i32 zIndex, u32 resourceId,
    const project::JsonReader& json, u32 itemStart, u32 itemEnd,
    const char* filterCStr,
    bool isStandaloneEffect
) noexcept {
    (void)itemEnd;
    if (slot >= Mask::BITS) return RUNTIME_LOAD_RECORD_CAPACITY_EXCEEDED;
    PostFxComponent& it = stagedItems_[slot];
    it = {};
    it.startMs = startMs;
    it.endMs = endMs;
    it.resourceId = resourceId;
    it.type = static_cast<u32>(type);
    it.zIndex = zIndex;
    it.active = 1u;
    it.set_fx_flag(FX_FLAG_STANDALONE_EFFECT, isStandaloneEffect);

    RainDropParams& rp = stagedRainParams_[slot];
    rp = {};
    if (!parseSlotPostFxConfig(json, itemStart, type, filterCStr, it, rp)) {
        it = {};
        rp = {};
        return RUNTIME_LOAD_UNSUPPORTED_FEATURE;
    }
    bakeSlotPostFxMetadata(slot, it, rp);
    return RUNTIME_LOAD_OK;
}

template <typename Mask>
RuntimeLoadStatus Timeline<Mask>::commit_staged_slots(
    u32 count, RuntimeLoadStatus pendingError
) noexcept {
    if (pendingError != RUNTIME_LOAD_OK) {
        return pendingError;
    }
    if (count > Mask::BITS) count = static_cast<u32>(Mask::BITS);

    byTypeBB_.clear();
    fusibleSlotsBB_.clear();
    postFxSlotsBB_.clear();
    dynamicFxSlotsBB_.clear();
    bassUsed_ = 0;

    for (u32 slot = 0; slot < count; ++slot) {
        items_[slot] = stagedItems_[slot];
        rainParams_[slot] = stagedRainParams_[slot];
        bassMeta_[slot].count = 0u;
        bassMeta_[slot].offset = 0u;
        bassMeta_[slot].hop = 0.0;
        zOrderedSlots_[slot] = static_cast<u8>(slot);
        const PostFxComponent& it = items_[slot];
        if (!it.is_standalone_effect()) {
            const MediaType mType = make_media_type(it.type);
            if (slot < 64) {
                byTypeBB_[static_cast<usize>(mType)] |= (1ULL << slot);
            }
        }
        if (it.effect != EffectKind::None) {
            postFxSlotsBB_.set_bit(slot);
        }
        if (it.effect == EffectKind::Zoom ||
            it.effect == EffectKind::Shake ||
            it.effect == EffectKind::Flicker) {
            fusibleSlotsBB_.set_bit(slot);
        }
        if (it.bakedEffectFlags != 0u &&
            (it.effect == EffectKind::Flicker ||
             it.effect == EffectKind::Zoom ||
             it.effectBassSource() ||
             (it.bakedEffectHashMask != FX_HASH_MASK_NONE &&
              it.bakedEffectHashMask != FX_HASH_MASK_STATIC_BLOOM))) {
            dynamicFxSlotsBB_.set_bit(slot);
        }
    }
    for (u32 slot = count; slot < Mask::BITS; ++slot) {
        items_[slot].effect = EffectKind::None;
        items_[slot].bakedEffectFlags = 0u;
        items_[slot].bakedEffectHashMask = FX_HASH_MASK_NONE;
        bassMeta_[slot].count = 0u;
    }

    // Sắp xếp zOrderedSlots theo zIndex tăng dần (hòa giữ slot nhỏ trước) tại Cold Path
    for (u32 i = 1; i < count; ++i) {
        const u8 curSlot = zOrderedSlots_[i];
        const i32 curZ = items_[curSlot].zIndex;
        u32 j = i;
        while (j > 0 && items_[zOrderedSlots_[j - 1]].zIndex > curZ) {
            zOrderedSlots_[j] = zOrderedSlots_[j - 1];
            --j;
        }
        zOrderedSlots_[j] = curSlot;
    }

    itemCount_ = count;
    durationMs_ = stagedDurationMs_;
    currentTimeMs_ = 0.0;
    stageWidth_ = stagedStageW_;
    stageHeight_ = stagedStageH_;
    lastStructKey_ = 0;
    cachedPassCount_ = 0;
    cachedPlanValid_ = 0;
    cachedKernelCount_ = 0;
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
    return RUNTIME_LOAD_OK;
}

template <typename Mask>
bool Timeline<Mask>::setBassEnvelopeChunk(
    u32 slot, double hop, u32 totalCount, u32 offset,
    u32 count, const float* interleaved
) {
    if (slot >= itemCount_ || !interleaved ||
        !std::isfinite(hop) || hop <= 0.0 ||
        totalCount == 0 ||
        offset > totalCount || count > totalCount - offset)
        return false;
    const PostFxComponent& item = items_[slot];
    SlotBassEnvelopeMeta& meta = bassMeta_[slot];
    if (!item.effectBassSource() ||
        (item.effect != EffectKind::Flicker &&
         item.effect != EffectKind::Zoom))
        return false;
    if (meta.count == 0) {
        const u32 needed = bassUsed_ + totalCount;
        if (needed > bassCap_) {
            u32 newCap = (bassCap_ == 0u) ? 4096u : bassCap_;
            while (newCap < needed) newCap *= 2u;
            AlignedPtr<float[]> nextLvl = make_unique_aligned<float[]>(newCap);
            AlignedPtr<u16[]> nextHit = make_unique_aligned<u16[]>(newCap);
            if (!nextLvl || !nextHit) return false;
            if (bassUsed_ > 0u && bassLevels_ && bassHits_) {
                std::memcpy(nextLvl.get(), bassLevels_.get(), static_cast<usize>(bassUsed_) * sizeof(float));
                std::memcpy(nextHit.get(), bassHits_.get(), static_cast<usize>(bassUsed_) * sizeof(u16));
            }
            bassLevels_ = std::move(nextLvl);
            bassHits_ = std::move(nextHit);
            bassCap_ = newCap;
        }
        meta.offset = bassUsed_;
        meta.count = totalCount;
        meta.hop = hop;
        bassUsed_ += totalCount;
    } else if (meta.count != totalCount || meta.hop != hop) {
        return false;
    }
    for (u32 index = 0; index < count; ++index) {
        const float level = interleaved[index * 2];
        const float hit = interleaved[index * 2 + 1];
        if (!std::isfinite(level) || !std::isfinite(hit)) return false;
        const u32 sample = meta.offset + offset + index;
        bassLevels_[sample] = std::clamp(level, 0.0f, 1.0f);
        bassHits_[sample] = static_cast<u16>(std::clamp(hit, 0.0f, U16_MAX_F32));
    }
    return true;
}

template <typename Mask>
bool Timeline<Mask>::evaluate_from_position(
    const Position<Mask>* pos, double timeMs, Frame<Mask>& output
) const {
    if (!std::isfinite(timeMs) || timeMs < 0.0)
        return false;
    output.resetHeader(timeMs);
    if (itemCount_ == 0) return true;

    const bool usePosMask = (pos != nullptr) && (pos->time() == static_cast<TimePoint>(timeMs));
    const Mask posActiveMask = usePosMask ? pos->active_items() : Mask{};
    if (usePosMask && !posActiveMask) {
        return true;
    }

    const auto *script = stockcut::get_active_music_script();
    const bool hasScript = (script && script->totalSamples > 0);
    DynFxFrameMemo fxMemo;

    for (u32 rank = 0; rank < itemCount_; ++rank) {
        const u32 slot = zOrderedSlots_[rank];
        const PostFxComponent &item = items_[slot];
        if (usePosMask) {
            if (!posActiveMask.test_bit(slot)) continue;
        } else {
            if (timeMs < item.startMs || timeMs >= item.endMs) continue;
        }

        const u32 outIdx = output.itemCount++;
        output.slotMap[slot] = static_cast<u8>(outIdx);
        output.activeMask.set_bit(slot);
        FrameItem &active = output.items[outIdx];
        const float elapsedF = static_cast<float>(timeMs - item.startMs);

        active.effectIntensity = (item.effect == EffectKind::Flicker) ? item.effectBase : item.effectIntensity;
        active.effectScale = 1.0f;
        active.effectRotation = 0.0f;
        active.effectShiftX = 0.0f;
        active.effectShiftY = 0.0f;
        active.elapsedMsF = elapsedF;
        active.slot = slot;
        active.zIndex = item.zIndex;

        if (item.bakedEffectFlags == 0u) continue;

        output.effectDynamicZobrist ^= evaluate_dynamic_fx_item(
            item, active, timeMs, elapsedF, script, hasScript,
            bassMeta_[slot], bassLevels_.get(), bassHits_.get(), fxMemo);

        output.activeEffectCount += static_cast<u16>((item.bakedEffectFlags & RUNTIME_ITEM_FX_FLAG_HAS_EFFECT) != 0u);
        output.hasZoomTiltBass |= static_cast<u8>(item.bakedEffectFlags & FX_FLAG_MASK_HEADER);
        output.hasBassReactiveItem |= static_cast<u8>((item.bakedEffectFlags & RUNTIME_ITEM_FX_FLAG_BASS_REACTIVE) != 0u);
    }

    output.activeFusibleEffectsBB = output.activeMask & fusibleSlotsBB_;
    for (usize t = 0; t < MEDIA_TYPE_NB; ++t) {
        const u64 tBB = byTypeBB_[t];
        const u64 summary = tBB ? (output.activeMask & Mask(tBB)).summary() : 0ULL;
        output.byTypeMap.set_direct(static_cast<MediaType>(t), summary);
    }
    return true;
}

template <typename Mask>
bool Timeline<Mask>::evaluate(double timeMs, Frame<Mask>& output) const {
    return evaluate_from_position(nullptr, timeMs, output);
}

template <typename Mask>
template <bool FromPosition>
bool Timeline<Mask>::renderFromMaskAndSource(
    const Position<Mask>* pos, double timeMs, u32 width, u32 height, Frame<Mask>& output
) const {
    active_rain_params_ptr() = rainParams_.data();
    if (__builtin_expect(width == 0 || height == 0, 0)) {
        if constexpr (FromPosition) {
            assert(pos != nullptr);
            return evaluate_from_position(pos, timeMs, output);
        } else {
            return evaluate_from_position(nullptr, timeMs, output);
        }
    }

    bool posCondition = false;
    if constexpr (FromPosition) {
        assert(pos != nullptr);
        posCondition = (pos->time() == static_cast<TimePoint>(timeMs)) &&
                       (pos->active_items() == lastActiveMask_);
    }

    const bool sameOutputFrame =
        cachedPlanValid_ != 0u &&
        std::isfinite(timeMs) && timeMs >= 0.0 &&
        width == lastW_ && height == lastH_ &&
        lastEvalEpoch_ == planEpoch_ &&
        static_cast<const void*>(&output) == s_lastGlobalOutputPtr &&
        lastOutputToken_ != 0u && lastOutputToken_ == s_lastGlobalToken &&
        posCondition &&
        output.activeMask == lastActiveMask_ &&
        output.passCount == cachedPassCount_ &&
        output.itemCount == cachedItemCount_ &&
        output.timeMs == lastRenderedTimeMs_;

    const auto updateMomentHorizon = [&]() noexcept {
        cachedItemCount_ = output.itemCount;
        lastRenderedTimeMs_ = timeMs;
        if (uniformPassCount_ == 0u && cachedActiveDynFxBB_.empty()) {
            double lo = 0.0;
            double hi = 1e15;
            output.activeMask.for_each_active([&](u32 slot) {
                if (slot < itemCount_) {
                    const PostFxComponent& it = items_[slot];
                    if (static_cast<double>(it.startMs) > lo) lo = static_cast<double>(it.startMs);
                    if (static_cast<double>(it.endMs) < hi) hi = static_cast<double>(it.endMs);
                }
            });
            const Mask inactiveMask = ~output.activeMask;
            inactiveMask.for_each_active([&](u32 slot) {
                if (slot < itemCount_) {
                    const PostFxComponent& it = items_[slot];
                    if (static_cast<double>(it.endMs) <= timeMs && static_cast<double>(it.endMs) > lo) {
                        lo = static_cast<double>(it.endMs);
                    }
                    if (static_cast<double>(it.startMs) > timeMs && static_cast<double>(it.startMs) < hi) {
                        hi = static_cast<double>(it.startMs);
                    }
                }
            });
            momentStartMs_ = lo;
            momentEndMs_ = hi;
            cachedMomentStaticOk_ = (lo < hi) ? 1u : 0u;
        } else {
            cachedMomentStaticOk_ = 0u;
        }
    };

    if (__builtin_expect(sameOutputFrame, 1)) {
        output.timeMs = timeMs;
        const u32 itemCnt = cachedItemCount_;
        if (padRuntime0_ != 0u) {
            const float commonElapsedF = static_cast<float>(timeMs);
            for (u32 k = 0; k < itemCnt; ++k) {
                output.items[k].elapsedMsF = commonElapsedF;
            }
        } else {
            for (u32 k = 0; k < itemCnt; ++k) {
                output.items[k].elapsedMsF = static_cast<float>(timeMs - cachedItemStartMs_[k]);
            }
        }
        if (__builtin_expect(cachedMomentStaticOk_ != 0u && timeMs >= momentStartMs_ && timeMs < momentEndMs_, 0)) {
            lastRenderedTimeMs_ = timeMs;
            return true;
        }
        u64 dynZ = cachedStaticFxZobrist_;
        if (cachedActiveDynFxBB_.any()) {
            const auto *script = stockcut::get_active_music_script();
            const bool hasScript = (script && script->totalSamples > 0);
            const float *bassLevels = bassLevels_.get();
            const u16 *bassHits = bassHits_.get();
            DynFxFrameMemo fxMemo;
            cachedActiveDynFxBB_.for_each_active([&](u32 slot) noexcept {
                const PostFxComponent &item = items_[slot];
                FrameItem &active = output.items[output.slotMap[slot]];
                const float elapsedF = active.elapsedMsF;
                active.effectIntensity = (item.effect == EffectKind::Flicker) ? item.effectBase : item.effectIntensity;
                active.effectScale = 1.0f;
                active.effectRotation = 0.0f;
                dynZ ^= evaluate_dynamic_fx_item(
                    item, active, timeMs, elapsedF, script, hasScript,
                    bassMeta_[slot], bassLevels, bassHits, fxMemo);
            });
        }
        output.effectDynamicZobrist = dynZ;
        if (uniformPassCount_ > 0u) {
            const float widthF = static_cast<float>(width);
            const float heightF = static_cast<float>(height);
            const UniformJob *jobs = (activePlanTTIdx_ < PLAN_TT_TOTAL)
                ? s_planTT[activePlanTTIdx_].uniformPasses.data()
                : uniformPasses_.data();
            dispatchUniformJobs<false>(jobs, uniformPassCount_, output, items_, widthF, heightF);
        }
        if (__builtin_expect(cachedMomentEligible_ != 0u, 0)) {
            updateMomentHorizon();
        } else {
            lastRenderedTimeMs_ = timeMs;
        }
        return true;
    }

    const float widthF = static_cast<float>(width);
    const float heightF = static_cast<float>(height);

    if constexpr (FromPosition) {
        if (__builtin_expect(pos->time() == static_cast<TimePoint>(timeMs) && std::isfinite(timeMs) && timeMs >= 0.0, 1)) {
            const Mask posActive = pos->active_items();
            const u64 activeVal = posActive.summary();
            const u64 planHash = hzobrist_mix64(
                activeVal ^
                (static_cast<u64>(width) << 32) ^
                static_cast<u64>(height) ^
                (static_cast<u64>(planEpoch_) * HASH_GOLDEN_64));
            const u32 setBase = (static_cast<u32>(planHash ^ (planHash >> 32)) & (PLAN_TT_SETS - 1)) * PLAN_TT_WAYS;
            for (u32 w = 0; w < PLAN_TT_WAYS; ++w) {
                const u32 ttIdx = setBase + w;
                PlanTTEntry &way = s_planTT[ttIdx];
                if (way.epoch == planEpoch_ &&
                    way.activeMask == activeVal &&
                    way.width == width &&
                    way.height == height)
                {
                    output.timeMs = timeMs;
                    output.itemCount = way.itemCount;
                    output.activeEffectCount = way.activeEffectCount;
                    output.hasZoomTiltBass = way.hasZoomTiltBass | way.hasPostProcessing;
                    output.hasBassReactiveItem = way.hasBassReactiveItem;
                    output.activeMask = posActive;
                    output.activeFusibleEffectsBB = Mask(way.activeFusibleEffectsBB);
                    output.passCount = way.passCount;
                    output.kernelCount = way.kernelCount;
                    output.passKindMask = way.passKindMask;
                    std::memcpy(output.slotMap.data(), way.slotMap.data(), sizeof(output.slotMap));
                    for (usize t = 0; t < MEDIA_TYPE_NB; ++t) {
                        output.byTypeMap.set_direct(static_cast<MediaType>(t), way.byTypeSummary[t]);
                    }
                    const u32 itemCnt = way.itemCount;
                    for (u32 k = 0; k < itemCnt; ++k) {
                        const u32 slot = way.activeSlots[k];
                        const PostFxComponent &it = items_[slot];
                        cachedItemStartMs_[k] = it.startMs;
                        FrameItem &active = output.items[k];
                        active.effectIntensity = (it.effect == EffectKind::Flicker) ? it.effectBase : it.effectIntensity;
                        active.effectScale = 1.0f;
                        active.effectRotation = 0.0f;
                        active.effectShiftX = 0.0f;
                        active.effectShiftY = 0.0f;
                        active.elapsedMsF = static_cast<float>(timeMs - it.startMs);
                        active.slot = slot;
                        active.zIndex = it.zIndex;
                    }
                    u64 dynZ = way.staticFxZobrist;
                    if (way.activeDynFxBB != 0ULL) {
                        const auto *script = stockcut::get_active_music_script();
                        const bool hasScript = (script && script->totalSamples > 0);
                        const float *bassLevels = bassLevels_.get();
                        const u16 *bassHits = bassHits_.get();
                        DynFxFrameMemo fxMemo;
                        Mask(way.activeDynFxBB).for_each_active([&](u32 slot) noexcept {
                            const PostFxComponent &it = items_[slot];
                            FrameItem &active = output.items[output.slotMap[slot]];
                            dynZ ^= evaluate_dynamic_fx_item(
                                it, active, timeMs, active.elapsedMsF, script, hasScript,
                                bassMeta_[slot], bassLevels, bassHits, fxMemo);
                        });
                    }
                    output.effectDynamicZobrist = dynZ;
                    copyPlanBuffers(
                        output.passes.data(), output.targets.data(), output.kernels.data(),
                        way.passes.data(), way.targets.data(), way.kernels.data(),
                        way.passCount, way.kernelCount);
                    if (way.uniformPassCount > 0) {
                        dispatchUniformJobs<true>(way.uniformPasses.data(), way.uniformPassCount, output, items_, widthF, heightF);
                    }
                    lastActiveMask_ = posActive;
                    lastW_ = width;
                    lastH_ = height;
                    cachedPassCount_ = way.passCount;
                    cachedPlanValid_ = 1;
                    cachedKernelCount_ = way.kernelCount;
                    cachedHasPostProcessing_ = way.hasPostProcessing;
                    uniformPassCount_ = way.uniformPassCount;
                    activePlanTTIdx_ = ttIdx;
                    cachedItemCount_ = itemCnt;
                    lastRenderedTimeMs_ = timeMs;
                    lastEvalEpoch_ = planEpoch_;
                    cachedActiveDynFxBB_ = Mask(way.activeDynFxBB);
                    cachedStaticFxZobrist_ = way.staticFxZobrist;
                    padRuntime0_ = way.allZeroStartMs;
                    cachedMomentEligible_ = (way.uniformPassCount == 0u && way.activeDynFxBB == 0ULL) ? 1u : 0u;
                    lastOutputToken_ = ++s_lastGlobalToken;
                    s_lastGlobalOutputPtr = static_cast<const void*>(&output);
                    if (cachedMomentEligible_ != 0u) {
                        if (way.momentStaticOk != 0u && timeMs >= way.momentStartMs && timeMs < way.momentEndMs) {
                            momentStartMs_ = way.momentStartMs;
                            momentEndMs_ = way.momentEndMs;
                            cachedMomentStaticOk_ = 1u;
                        } else {
                            updateMomentHorizon();
                            way.momentStartMs = momentStartMs_;
                            way.momentEndMs = momentEndMs_;
                            way.momentStaticOk = cachedMomentStaticOk_;
                        }
                    } else {
                        cachedMomentStaticOk_ = 0u;
                    }
                    return true;
                }
            }
        }
    }

    if constexpr (FromPosition) {
        assert(pos != nullptr);
        if (!evaluate_from_position(pos, timeMs, output)) return false;
    } else {
        if (!evaluate_from_position(nullptr, timeMs, output)) return false;
    }

    const auto populateEvalCache = [&]() noexcept {
        cachedItemCount_ = output.itemCount;
        lastRenderedTimeMs_ = timeMs;
        lastEvalEpoch_ = planEpoch_;
        cachedActiveDynFxBB_ = output.activeMask & dynamicFxSlotsBB_;
        bool allZero = true;
        for (u32 k = 0; k < output.itemCount; ++k) {
            const double sMs = items_[output.items[k].slot].startMs;
            cachedItemStartMs_[k] = sMs;
            if (sMs != 0.0) allZero = false;
        }
        padRuntime0_ = allZero ? 1u : 0u;
        u64 staticZ = 0ULL;
        const Mask staticCand = output.activeMask & ~cachedActiveDynFxBB_;
        staticCand.for_each_active([&](u32 s) noexcept {
            const PostFxComponent &it = items_[s];
            if (it.bakedEffectFlags != 0u && it.bakedEffectHashMask == FX_HASH_MASK_STATIC_BLOOM) {
                staticZ ^= it.bakedEffectHashSeed;
            }
        });
        cachedStaticFxZobrist_ = staticZ;
        cachedMomentEligible_ = (uniformPassCount_ == 0u && cachedActiveDynFxBB_.empty()) ? 1u : 0u;
    };

    if (cachedPlanValid_ && output.activeMask == lastActiveMask_ && width == lastW_ && height == lastH_) {
        output.passCount = cachedPassCount_;
        output.kernelCount = cachedKernelCount_;
        output.hasZoomTiltBass |= cachedHasPostProcessing_;
        if (!(static_cast<const void*>(&output) == s_lastGlobalOutputPtr && lastOutputToken_ != 0 && lastOutputToken_ == s_lastGlobalToken)) {
            if (activePlanTTIdx_ < PLAN_TT_TOTAL) {
                const PlanTTEntry &way = s_planTT[activePlanTTIdx_];
                copyPlanBuffers(
                    output.passes.data(), output.targets.data(), output.kernels.data(),
                    way.passes.data(), way.targets.data(), way.kernels.data(),
                    cachedPassCount_, cachedKernelCount_);
            } else {
                copyPlanBuffers(
                    output.passes.data(), output.targets.data(), output.kernels.data(),
                    cachedPasses_.data(), cachedTargets_.data(), cachedKernels_.data(),
                    cachedPassCount_, cachedKernelCount_);
            }
            lastOutputToken_ = ++s_lastGlobalToken;
            s_lastGlobalOutputPtr = static_cast<const void*>(&output);
        }
        if (uniformPassCount_ > 0) {
            if (activePlanTTIdx_ < PLAN_TT_TOTAL) {
                const PlanTTEntry &way = s_planTT[activePlanTTIdx_];
                dispatchUniformJobs<false>(way.uniformPasses.data(), way.uniformPassCount, output, items_, widthF, heightF);
            } else {
                dispatchUniformJobs<false>(uniformPasses_.data(), uniformPassCount_, output, items_, widthF, heightF);
            }
        }
        populateEvalCache();
        if (cachedMomentEligible_ != 0u) {
            if (cachedMomentStaticOk_ == 0u) updateMomentHorizon();
        } else {
            cachedMomentStaticOk_ = 0u;
        }
        return true;
    }

    const u64 activeVal = output.activeMask.summary();
    const u64 planHash = hzobrist_mix64(
        activeVal ^
        (static_cast<u64>(width) << 32) ^
        static_cast<u64>(height) ^
        (static_cast<u64>(planEpoch_) * HASH_GOLDEN_64));
    const u32 setBase = (static_cast<u32>(planHash ^ (planHash >> 32)) & (PLAN_TT_SETS - 1)) * PLAN_TT_WAYS;
    for (u32 w = 0; w < PLAN_TT_WAYS; ++w) {
        const u32 ttIdx = setBase + w;
        PlanTTEntry &way = s_planTT[ttIdx];
        if (way.epoch == planEpoch_ &&
            way.activeMask == activeVal &&
            way.width == width &&
            way.height == height)
        {
            lastActiveMask_ = output.activeMask;
            lastW_ = width;
            lastH_ = height;
            cachedPassCount_ = way.passCount;
            cachedPlanValid_ = 1;
            cachedKernelCount_ = way.kernelCount;
            cachedHasPostProcessing_ = way.hasPostProcessing;
            uniformPassCount_ = way.uniformPassCount;
            activePlanTTIdx_ = ttIdx;
            output.passCount = way.passCount;
            output.kernelCount = way.kernelCount;
            output.passKindMask = way.passKindMask;
            output.hasZoomTiltBass |= way.hasPostProcessing;
            copyPlanBuffers(
                output.passes.data(), output.targets.data(), output.kernels.data(),
                way.passes.data(), way.targets.data(), way.kernels.data(),
                way.passCount, way.kernelCount);
            if (way.uniformPassCount > 0) {
                dispatchUniformJobs<true>(way.uniformPasses.data(), way.uniformPassCount, output, items_, widthF, heightF);
            }
            lastOutputToken_ = ++s_lastGlobalToken;
            s_lastGlobalOutputPtr = static_cast<const void*>(&output);
            populateEvalCache();
            if (cachedMomentEligible_ != 0u) {
                if (way.momentStaticOk != 0u && timeMs >= way.momentStartMs && timeMs < way.momentEndMs) {
                    momentStartMs_ = way.momentStartMs;
                    momentEndMs_ = way.momentEndMs;
                    cachedMomentStaticOk_ = 1u;
                } else {
                    updateMomentHorizon();
                    way.momentStartMs = momentStartMs_;
                    way.momentEndMs = momentEndMs_;
                    way.momentStaticOk = cachedMomentStaticOk_;
                }
            } else {
                cachedMomentStaticOk_ = 0u;
            }
            return true;
        }
    }

    const u8 evalZoomTiltBass = output.hasZoomTiltBass;
    if (!buildRenderPlan(output, items_, stageWidth_, stageHeight_, width, height))
        return false;

    cachedPassCount_ = 0;
    cachedPlanValid_ = 0;
    uniformPassCount_ = 0;
    constexpr u32 UNIFORM_UPDATE_MASK = PassKindBitmask::make_mask(
        RuntimePassKind::ApplyShaderEffect,
        RuntimePassKind::ApplyBrightness,
        RuntimePassKind::ApplyZoom,
        RuntimePassKind::ApplyShake
    );
    if (output.passKindMask.has_any(UNIFORM_UPDATE_MASK)) {
        for (u32 p = 0; p < output.passCount; ++p) {
            const auto kind = output.passes[p].kind;
            if (PassUniformTable::hasUniformUpdate(kind) && uniformPassCount_ < RUNTIME_TIMELINE_CAPACITY) {
                uniformPasses_[uniformPassCount_++] = {static_cast<u16>(p), kind, 0};
            }
        }
    }
    lastActiveMask_ = output.activeMask;
    lastW_ = width;
    lastH_ = height;
    cachedPassCount_ = output.passCount;
    cachedPlanValid_ = 1;
    cachedKernelCount_ = output.kernelCount;
    cachedHasPostProcessing_ = static_cast<u8>(output.hasZoomTiltBass & RUNTIME_FRAME_FLAG_POST_PROCESS);
    lastOutputToken_ = ++s_lastGlobalToken;
    s_lastGlobalOutputPtr = static_cast<const void*>(&output);
    populateEvalCache();
    cachedMomentStaticOk_ = 0u;
    if (cachedMomentEligible_ != 0u) updateMomentHorizon();

    if (output.passCount <= PLAN_TT_MAX_PASSES && output.kernelCount <= PLAN_TT_MAX_KERNELS) {
        u32 chosenIdx = setBase;
        if (s_planTT[setBase].epoch == planEpoch_) {
            chosenIdx = (s_planTT[setBase + 1].epoch != planEpoch_)
                ? (setBase + 1)
                : (setBase + ((++s_planVictimToggle) & 1u));
        }
        PlanTTEntry &way = s_planTT[chosenIdx];
        way.epoch = planEpoch_;
        way.width = width;
        way.height = height;
        way.passCount = output.passCount;
        way.kernelCount = output.kernelCount;
        way.uniformPassCount = uniformPassCount_;
        way.activeMask = activeVal;
        way.passKindMask = output.passKindMask;
        way.itemCount = output.itemCount;
        way.activeEffectCount = output.activeEffectCount;
        way.hasPostProcessing = cachedHasPostProcessing_;
        way.hasZoomTiltBass = evalZoomTiltBass;
        way.hasBassReactiveItem = output.hasBassReactiveItem;
        way.momentStaticOk = cachedMomentStaticOk_;
        way.allZeroStartMs = padRuntime0_;
        way.activeFusibleEffectsBB = output.activeFusibleEffectsBB.summary();
        way.activeDynFxBB = cachedActiveDynFxBB_.summary();
        way.staticFxZobrist = cachedStaticFxZobrist_;
        way.momentStartMs = momentStartMs_;
        way.momentEndMs = momentEndMs_;
        for (u32 k = 0; k < output.itemCount; ++k) {
            way.activeSlots[k] = static_cast<u8>(output.items[k].slot);
        }
        std::memcpy(way.slotMap.data(), output.slotMap.data(), sizeof(way.slotMap));
        for (usize t = 0; t < MEDIA_TYPE_NB; ++t) {
            way.byTypeSummary[t] = output.byTypeMap.get(static_cast<MediaType>(t));
        }
        for (u32 j = 0; j < uniformPassCount_; ++j) {
            way.uniformPasses[j] = uniformPasses_[j];
        }
        copyPlanBuffers(
            way.passes.data(), way.targets.data(), way.kernels.data(),
            output.passes.data(), output.targets.data(), output.kernels.data(),
            output.passCount, output.kernelCount);
        activePlanTTIdx_ = chosenIdx;
    } else {
        activePlanTTIdx_ = INVALID_U32;
        copyPlanBuffers(
            cachedPasses_.data(), cachedTargets_.data(), cachedKernels_.data(),
            output.passes.data(), output.targets.data(), output.kernels.data(),
            output.passCount, output.kernelCount);
    }
    return true;
}

template <typename Mask>
bool Timeline<Mask>::renderFromPosition(
    const Position<Mask>& pos, double timeMs, u32 width, u32 height, Frame<Mask>& output
) const {
    return renderFromMaskAndSource<true>(&pos, timeMs, width, height, output);
}

template <typename Mask>
bool Timeline<Mask>::renderAt(
    double timeMs, u32 width, u32 height, Frame<Mask>& output
) const {
    return renderFromMaskAndSource<false>(nullptr, timeMs, width, height, output);
}

template class Timeline<Mask<1>>;
template class Timeline<Mask<2>>;
template struct Frame<Mask<1>>;
template struct Frame<Mask<2>>;
} // namespace stockcut::timeline::runtime
