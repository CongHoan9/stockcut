// ============================================================================
// [ĐÁNH GIÁ ĐIỂM NGHẼN HỆ THỐNG]:
// 1. Duyệt cây phân cấp đệ quy trong Hot Path 60 FPS:
//    - Duyệt qua lại các node timeline làm phát sinh cache miss và chi phí con trỏ.
// 2. Kế thừa template lặp lại ở mỗi khung hình:
//    - Hỏi quan hệ cha-con liên tục làm giảm hiệu suất xử lý khung hình.
//
// [HƯỚNG KHẮC PHỤC TRIỆT ĐỂ — KHÔNG VÁ]:
// 1. Kế thừa 1 lần duy nhất ở Cold Path & Bộ nhớ chia sẻ:
//    - Con trỏ trực tiếp vào bộ nhớ phẳng của cha, đọc O(1) không duyệt cây.
// 2. Bitboard Layer Masking & Ngăn xếp lịch sử StateInfo:
//    - Lọc trạng thái layer bằng bitmask 64-bit và lưu vết vi sai kiểu Stockfish.
// ============================================================================
// Bottleneck: the preparation pass scans raw project JSON repeatedly, so its cost
// is proportional to items multiplied by field lookups and keyframes.
// Optimization path: tokenize once, use byType function tables and explicit
// template specializations for media-specific accumulation. Remaining ifs validate
// untrusted JSON bounds/syntax; they are not repeated per-media business rules.
#include "duration.h"
#include "data_structures.h"
#include "json_reader.h"
#include "json_buffer.h"
#include "string_parse.h"
namespace stockcut::timeline::project
{
    namespace
    {
        constexpr usize ENDPOINT_COUNT   = 2;
        constexpr usize KEYFRAME_MODE_NB = 2;

        struct DurationAcc
        {
            double maxItem = 0;
            double maxAudio = 0;
            u32 flags = 0;
        };
                // Accumulates valid non-audio endpoints into the project visual-duration maximum.
        template <MediaType Type>
        void accumulateEndpoint(double value, DurationAcc &duration)
        {
            if (value > 0 && value < PROJECT_TIME_SENTINEL_MS && value > duration.maxItem)
                duration.maxItem = value;
        }
        // Specializes audio endpoints into their independent duration maximum.
        template <>
        void accumulateEndpoint<MediaType::Audio>(double value, DurationAcc &duration)
        {
            if (value > 0 && value < PROJECT_TIME_SENTINEL_MS && value > duration.maxAudio)
                duration.maxAudio = value;
        }
        using TypedEndpointAccumulator = void (*)(double, DurationAcc &);
        constexpr MultiArray<TypedEndpointAccumulator, MEDIA_TYPE_NB> endpointByType = {{
            &accumulateEndpoint<MediaType::None>,
            &accumulateEndpoint<MediaType::Background>,
            &accumulateEndpoint<MediaType::Video>,
            &accumulateEndpoint<MediaType::Image>,
            &accumulateEndpoint<MediaType::Path>,
            &accumulateEndpoint<MediaType::Lyric>,
            &accumulateEndpoint<MediaType::Text>,
            &accumulateEndpoint<MediaType::Audio>}};
        // Resolves an input type name to its compile-time-specialized accumulator row.
        MediaType readItemMediaType(const JsonReader &json, u32 itemStart)
        {
            u32 typeStart = 0, typeEnd = 0;
            if (json.field(itemStart, "type", typeStart, typeEnd))
            {
                MultiArray<char, 32> typeBuf;
                if (json.readStringValue(typeStart, typeEnd, typeBuf.data(), static_cast<u32>(typeBuf.size())))
                {
                    return make_media_type(typeBuf.data());
                }
            }
            return MediaType::None;
        }
        // Reads a positive configured duration, which takes precedence over inferred endpoints.
        bool readConfigDuration(const JsonReader &json, double &duration)
        {
            u32 configStart = 0, configEnd = 0;
            return json.field(0, "config", configStart, configEnd) &&
                   json.isObject(configStart, configEnd) &&
                   json.numberField(configStart, "durationMs", duration) &&
                   duration > 0;
        }
        // Locates and validates the required top-level timeline array.
        bool readTimelineBounds(const JsonReader &json, u32 &timelineStart, u32 &timelineEnd)
        {
            return json.field(0, "timeline", timelineStart, timelineEnd) &&
                   json.isArray(timelineStart, timelineEnd);
        }
        bool inheritedGroupField(
            const JsonReader &json,
            u32 itemStart,
            const char *fieldName,
            u32 &valueStart,
            u32 &valueEnd)
        {
            u32 contextStart = 0, contextEnd = 0;
            if (!json.field(itemStart, "_nativeGroup", contextStart, contextEnd) ||
                !json.isArray(contextStart, contextEnd))
                return false;
            bool found = false;
            u32 cursor = contextStart + 1;
            while (cursor < contextEnd)
            {
                u32 groupStart = 0, groupEnd = 0;
                if (!json.nextArrayValue(cursor, contextEnd, groupStart, groupEnd))
                    break;
                if (json.isObject(groupStart, groupEnd) &&
                    json.field(groupStart, fieldName, valueStart, valueEnd))
                    found = true;
            }
            return found;
        }
        // Accumulates endpoints and optional keyframe extents for one compile-time item mode.
        template <MediaType Type, bool HasKeyframes>
        void accumulateProjectItem(const JsonReader &json, u32 itemStart,
                                   u32 keyframesStart, u32 keyframesEnd,
                                   DurationAcc &duration)
        {
            constexpr MultiArray<const char *, ENDPOINT_COUNT> endpointNames = {{"start", "end"}};
            u32 startValueStart = 0, startValueEnd = 0;
            bool hasStart = json.field(
                itemStart, "start", startValueStart, startValueEnd);
            if (!hasStart)
                hasStart = inheritedGroupField(
                    json, itemStart, "start", startValueStart, startValueEnd);
            for (usize endpoint = 0; endpoint < ENDPOINT_COUNT; ++endpoint)
            {
                u32 valueStart = 0, valueEnd = 0;
                double value = 0;
                bool hasValue = false;
                if (endpoint == 0)
                {
                    valueStart = startValueStart;
                    valueEnd = startValueEnd;
                    hasValue = hasStart;
                }
                else
                {
                    hasValue = json.field(
                        itemStart, endpointNames[endpoint], valueStart, valueEnd);
                    if (!hasValue)
                        hasValue = inheritedGroupField(
                            json, itemStart, endpointNames[endpoint], valueStart, valueEnd);
                }
                if (!hasValue)
                    continue;
                if (json.valueIsPercent(valueStart, valueEnd))
                {
                    duration.flags |= PROJECT_DURATION_HAS_PERCENT_TIMES;
                    continue;
                }
                if (!json.valueNumber(valueStart, valueEnd, value))
                    continue;
                endpointByType[static_cast<usize>(Type)](value, duration);
            }
            if constexpr (HasKeyframes)
            {
                double itemStartMs = 0;
                if (hasStart && !json.timeValue(
                                    startValueStart, startValueEnd, itemStartMs))
                    itemStartMs = 0;
                u32 keyCursor = keyframesStart + 1;
                while (keyCursor < keyframesEnd)
                {
                    u32 keyframeStart = 0, keyframeEnd = 0;
                    if (!json.nextArrayValue(keyCursor, keyframesEnd, keyframeStart, keyframeEnd))
                        break;
                    if (!json.isObject(keyframeStart, keyframeEnd))
                        continue;
                    double keyframeMs = 0;
                    u32 msStart = 0, msEnd = 0;
                    if (json.field(keyframeStart, "ms", msStart, msEnd))
                    {
                        json.valueNumber(msStart, msEnd, keyframeMs);
                    }
                    else
                    {
                        u32 offsetStart = 0, offsetEnd = 0;
                        if (json.field(keyframeStart, "offset", offsetStart, offsetEnd) &&
                            !json.valueIsPercent(offsetStart, offsetEnd))
                        {
                            json.valueNumber(offsetStart, offsetEnd, keyframeMs);
                        }
                    }
                    const double candidate = itemStartMs + keyframeMs;
                    if (keyframeMs > 0 && candidate > duration.maxItem &&
                        candidate < PROJECT_TIME_SENTINEL_MS)
                    {
                        duration.maxItem = candidate;
                    }
                }
            }
        }
        // Dispatches by normalized media type and keyframe presence with no per-item policy branch.
        using ProjectItemAccumulator = void (*)(const JsonReader &, u32, u32, u32,
                                                DurationAcc &);
        template <MediaType Type>
        constexpr MultiArray<ProjectItemAccumulator, KEYFRAME_MODE_NB> keyframeModes = {{
            &accumulateProjectItem<Type, false>,
            &accumulateProjectItem<Type, true>}};
        constexpr MultiArray<ProjectItemAccumulator, MEDIA_TYPE_NB, KEYFRAME_MODE_NB> byType = {{
            keyframeModes<MediaType::None>,
            keyframeModes<MediaType::Background>,
            keyframeModes<MediaType::Video>,
            keyframeModes<MediaType::Image>,
            keyframeModes<MediaType::Path>,
            keyframeModes<MediaType::Lyric>,
            keyframeModes<MediaType::Text>,
            keyframeModes<MediaType::Audio>}};
    } // namespace
    // Computes configured or inferred project duration directly from a non-owning JSON span.
    DurationResult computeProjectDuration(const char *jsonData, u32 length)
    {
        DurationResult result{-1.0, 0, 0};
        if (!jsonData || !length)
            return result;
        const JsonReader json(jsonData, length);
        if (!json.isObjectDocument())
            return result;
        double configuredDuration = 0;
        if (readConfigDuration(json, configuredDuration))
        {
            result.durationMs = configuredDuration;
            return result;
        }
        u32 timelineStart = 0, timelineEnd = 0;
        if (!readTimelineBounds(json, timelineStart, timelineEnd))
            return result;
        DurationAcc duration;
        u32 cursor = timelineStart + 1;
        while (cursor < timelineEnd)
        {
            u32 itemStart = 0, itemEnd = 0;
            if (!json.nextArrayValue(cursor, timelineEnd, itemStart, itemEnd))
                break;
            if (!json.isObject(itemStart, itemEnd))
                continue;
            const MediaType mType = readItemMediaType(json, itemStart);
            if (mType == MediaType::None)
                continue;
            const usize mediaType = static_cast<usize>(mType);
            u32 keyframesStart = 0, keyframesEnd = 0;
            const usize hasKeyframes = static_cast<usize>(
                json.field(itemStart, "keyframes", keyframesStart, keyframesEnd) &&
                json.isArray(keyframesStart, keyframesEnd));
            byType[mediaType][hasKeyframes](
                json, itemStart, keyframesStart, keyframesEnd, duration);
        }
        result.durationMs = duration.maxItem > duration.maxAudio
                                ? duration.maxItem
                                : duration.maxAudio;
        result.flags = duration.flags;
        return result;
    }
} // namespace stockcut::timeline::project