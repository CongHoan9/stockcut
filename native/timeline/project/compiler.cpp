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
#include "compiler.h"
#include "dsp.h"
#include "manager.h"
#include "position.h"
#include "json_reader.h"
#include "text/font_map.h"
#include "renderer.h"
#include "core/data_structures.h"
#include "core/differential.h"
#include "core/hash_primitives.h"
#include "core/string_parse.h"
#include "core/stream.h"
#include "core/registry.h"
#include "templates.h"
#include "property/property_types.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
namespace stockcut::timeline::project
{
    namespace
    {
        constexpr u32 MAX_SCENE_DEPTH = 32;
        constexpr u32 MAX_OBJECT_FIELDS = 256;
        constexpr u32 MAX_TEMPLATE_NAME = 128;
        constexpr u32 MAX_TEMPLATE_TYPE = 32;
        constexpr u32 MAX_TEMPLATE_EFFECT = 64;
        constexpr u32 MAX_CLASS_TOKENS = 64;
        constexpr u32 MAX_STYLE_VALUE_TEXT = 512;
        constexpr u32 MAX_CSS_DECLARATION_TEXT = 4096;
        constexpr u32 MAX_INLINE_VOLUME_TEXT = 2048;
        constexpr u32 MAX_FULL_TEXT_BYTES = 2048;
        constexpr u32 MAX_FULL_TEXT_CHARS = 1024;
        constexpr u32 MAX_CLASS_LIST_TEXT = 1024;
        constexpr u32 MAX_SOURCE_PATH_TEXT = 1024;
        constexpr u32 MAX_KEY_TEXT = 256;
        constexpr u32 MAX_TOKEN_TEXT = 64;
        constexpr u32 MAX_SHORT_TEXT = 32;
        constexpr u32 MAX_TEMPLATE_HIERARCHY_DEPTH = 16;
        constexpr u32 MAX_COMPILED_AUDIO_CLIPS = MAX_AUDIO_TRACKS - 1;
        constexpr u32 MAX_SCENE_CONTEXTS = PROJECT_COMPILE_MAX_SCENE_LEAVES * MAX_SCENE_DEPTH;
        constexpr double MAX_AUDIO_VOLUME = 2.0;
        constexpr double DEFAULT_BG_Z_INDEX = -1.0;
        constexpr double DEFAULT_EFFECT_Z_INDEX = 9999.0;
        constexpr double GROUP_SUB_Z_STEP = 0.001;
        constexpr u32 SCENE_PROPERTY_COUNT =
            static_cast<u32>(stockcut::prop::STANDARD_PROP_COUNT);

        struct SceneEntry
        {
            u32 start;
            u32 end;
            u32 topLevelIndex;
            u32 depth;
            u32 groupContext;
            double x;
            double y;
            double width;
            double height;
            bool resolved;
        };
        struct SceneGroupContext
        {
            u32 sourceStart;
            u32 sourceEnd;
            u32 parent;
        };
        struct SceneLayout
        {
            double x;
            double y;
            double width;
            double height;
            u32 groupContext = INVALID_U32;
            bool hasGeometry;
            bool forceBackground;
            bool surface;
        };
                inline constexpr const MultiArray<stockcut::prop::PropDescriptor, stockcut::prop::STANDARD_PROP_COUNT> &scenePropertyByEnum =
            stockcut::prop::kPropSchemas;
        static_assert(stockcut::prop::STANDARD_PROP_COUNT == SCENE_PROPERTY_COUNT,
                      "prop::kPropSchemas must stay aligned with SCENE_PROPERTY_COUNT");
        struct SceneStyleValue
        {
            MultiArray<char, MAX_STYLE_VALUE_TEXT> text; // KHONG CO {}
            bool found = false;
            bool numericSource = false;
            SceneStyleValue() noexcept { text[0] = '\0'; }
        };
        bool findCssTextProperty(
            const JsonReader &json,
            u32 valueStart,
            u32 valueEnd,
            const stockcut::prop::PropDescriptor &spec,
            SceneStyleValue &output);
        struct GridTracks
        {
            MultiArray<double, PROJECT_COMPILE_MAX_SCENE_LEAVES> values; // KHONG CO {}
            u32 count = 0;
        };
        struct TemplateSpec
        {
            u32 start;
            u32 end;
            MultiArray<char, MAX_TEMPLATE_TYPE> type;     // KHONG CO {}
            MultiArray<char, MAX_TEMPLATE_NAME> name;     // KHONG CO {}
            MultiArray<char, MAX_TEMPLATE_NAME> mapClass; // KHONG CO {}
            MultiArray<char, MAX_TEMPLATE_EFFECT> effect; // KHONG CO {}
            MultiArray<char, MAX_TEMPLATE_NAME> parentName; // KHONG CO {}
            i32 parentIndex;
            bool hasName;
            bool hasSyntheticName;
        };
        struct FieldSpan
        {
            u32 keyStart;
            u32 keyEnd;
            u32 valueStart;
            u32 valueEnd;
            u8 kind;
        };
        struct CompiledAudioTrackSetup
        {
            u32 slot;
            MediaType type;
            i64 startMs;
            i64 endMs;
            float trimOffsetSec;
            float baseVolume;
            u32 keyframeCount;
            MultiArray<AudioVolumePoint, MAX_AUDIO_VOL_KF> keyframes; // KHONG CO {}
        };
        bool writeChar(char value);
        bool writeJsonString(const char *value);
        alignas(16) MultiArray<CompiledSceneNode, PROJECT_COMPILE_MAX_SCENE_LEAVES> sceneNodes;
        MultiArray<SceneLayout, PROJECT_COMPILE_MAX_SCENE_LEAVES> sceneLayouts;
        MultiArray<SceneGroupContext, MAX_SCENE_CONTEXTS> sceneGroupContexts;
        alignas(16) MultiArray<char, PROJECT_COMPILE_OUTPUT_CAPACITY> outputJson;
        MultiArray<TemplateSpec, PROJECT_COMPILE_MAX_TEMPLATES> templates;
        u32 outputLength = 0;
        u32 templateLength = 0;
        u32 sceneNodeCount = 0;
        u32 sceneRootCount = 0;
        u32 sceneLeafCount = 0;
        u32 sceneGroupContextCount = 0;
        constexpr bool isAsciiIdentifierChar(char value) noexcept
        {
            return (value >= 'a' && value <= 'z') ||
                   (value >= 'A' && value <= 'Z') ||
                   (value >= '0' && value <= '9') || value == '-' || value == '_';
        }
        double parseRadialMaskHole(StringSpan maskText)
        {
            if (!maskText.data || !maskText.length)
                return 0.0;
            const u32 textLength = maskText.length;
            constexpr const char* radialGradient = "radial-gradient(";
            constexpr u32 radialGradientLength = 16u;
            constexpr const char* transparent = "transparent";
            constexpr u32 transparentLength = 11u;
            for (u32 start = 0;
                 start + radialGradientLength <= textLength;
                 ++start)
            {
                if (!cstr_equal_icase_n(
                        maskText.data + start, radialGradient,
                        radialGradientLength))
                    continue;
                u32 depth = 1;
                u32 index = start + radialGradientLength;
                for (; index < textLength && depth; ++index)
                {
                    if (maskText.data[index] == '(')
                    {
                        ++depth;
                        continue;
                    }
                    if (maskText.data[index] == ')')
                    {
                        --depth;
                        continue;
                    }
                    if (depth != 1 || index + transparentLength >= textLength ||
                        !cstr_equal_icase_n(
                            maskText.data + index, transparent,
                            transparentLength) ||
                        (index && isAsciiIdentifierChar(maskText.data[index - 1])) ||
                        isAsciiIdentifierChar(
                            maskText.data[index + transparentLength]))
                        continue;
                    u32 numberStart = index + transparentLength;
                    while (numberStart < textLength && is_ascii_space(maskText.data[numberStart]))
                        ++numberStart;
                    if (numberStart == index + transparentLength)
                        continue;
                    u32 numberEnd = numberStart;
                    bool hasDigit = false;
                    while (numberEnd < textLength &&
                           maskText.data[numberEnd] >= '0' &&
                           maskText.data[numberEnd] <= '9')
                    {
                        hasDigit = true;
                        ++numberEnd;
                    }
                    if (numberEnd < textLength && maskText.data[numberEnd] == '.')
                    {
                        ++numberEnd;
                        while (numberEnd < textLength &&
                               maskText.data[numberEnd] >= '0' &&
                               maskText.data[numberEnd] <= '9')
                        {
                            hasDigit = true;
                            ++numberEnd;
                        }
                    }
                    if (!hasDigit || numberEnd >= textLength || maskText.data[numberEnd] != '%')
                        continue;
                    const char afterPercent = (numberEnd + 1 < textLength) ? maskText.data[numberEnd + 1] : '\0';
                    if (afterPercent && !is_ascii_space(afterPercent) &&
                        afterPercent != ',' && afterPercent != ')')
                        continue;
                    MultiArray<char, MAX_TOKEN_TEXT> number;
                    const u32 numberLength = numberEnd - numberStart;
                    if (numberLength >= number.size())
                        continue;
                    for (u32 digit = 0; digit < numberLength; ++digit)
                        number[digit] = maskText.data[numberStart + digit];
                    number[numberLength] = '\0';
                    char *parsedEnd = nullptr;
                    const double percent = std::strtod(number.data(), &parsedEnd);
                    if (parsedEnd != number.data() + numberLength ||
                        !std::isfinite(percent))
                        continue;
                    return percent / 100.0;
                }
            }
            return 0.0;
        }
        double parseRadialMaskHole(const SceneStyleValue &maskImage)
        {
            return maskImage.found ? parseRadialMaskHole(StringSpan::from_cstr(maskImage.text.data())) : 0.0;
        }
        bool readStringField(
            const JsonReader &json,
            u32 objectStart,
            const char *fieldName,
            char *output,
            u32 capacity)
        {
            u32 valueStart = 0;
            u32 valueEnd = 0;
            if (!json.field(objectStart, fieldName, valueStart, valueEnd))
                return false;
            return json.readStringValue(valueStart, valueEnd, output, capacity);
        }
        bool readFirstStringField(
            const JsonReader &json,
            u32 objectStart,
            const char *first,
            const char *second,
            char *output,
            u32 capacity)
        {
            if (readStringField(json, objectStart, first, output, capacity) &&
                output[0])
                return true;
            return readStringField(json, objectStart, second, output, capacity);
        }
        struct StyleRuleTargetOffset
        {
            u32 ruleStart;
            u32 ruleEnd;
            MediaType targetType;
        };
        struct TemporalStyleScope
        {
            double startMs;
            double endMs;
            u16 targetTypesBB;
            MultiArray<StyleRuleTargetOffset, MEDIA_TYPE_NB> byType;
        };
        constexpr usize MAX_TEMPORAL_STYLE_SCOPES = 16;
        MultiArray<TemporalStyleScope, MAX_TEMPORAL_STYLE_SCOPES> s_styleScopes;
        u32 s_styleScopeCount = 0;
        const char* s_sourceJsonPtr = nullptr;
        u32 s_sourceJsonLen = 0;

        void parseSingleRoleScope(const JsonReader &json, u32 itStart, u32 itEnd)
        {
            if (s_styleScopeCount >= MAX_TEMPORAL_STYLE_SCOPES) return;
            double sMs = 0.0, eMs = 0.0;
            json.numberField(itStart, "start", sMs);
            json.numberField(itStart, "end", eMs);
            u32 endStart = 0, endEnd = 0;
            if (json.field(itStart, "end", endStart, endEnd))
            {
                MultiArray<char, MAX_TOKEN_TEXT> endText{};
                if (json.readStringValue(endStart, endEnd, endText.data(), static_cast<u32>(endText.size())))
                {
                    if (cstr_equal(endText.data(), "100%"))
                    {
                        eMs = 1e9;
                    }
                    else
                    {
                        double parsed = eMs;
                        if (parse_css_number_prefix(StringSpan::from_cstr(endText.data()), parsed))
                        {
                            eMs = parsed;
                        }
                    }
                }
            }
            if (eMs <= sMs)
            {
                eMs = 1e9; // Mặc định role/style bao phủ toàn bộ timeline nếu không giới hạn end
            }
            TemporalStyleScope &scope = s_styleScopes[s_styleScopeCount];
            scope.startMs = sMs;
            scope.endMs = eMs;
            scope.targetTypesBB = 0;
            for (usize t = 0; t < MEDIA_TYPE_NB; ++t)
            {
                scope.byType[t] = {0, 0, MediaType::None};
            }
            u32 setStart = 0, setEnd = 0;
            if ((json.field(itStart, "set", setStart, setEnd) ||
                 json.field(itStart, "rules", setStart, setEnd) ||
                 json.field(itStart, "styles", setStart, setEnd)) &&
                json.isArray(setStart, setEnd))
            {
                u32 ruleCursor = setStart + 1;
                while (ruleCursor < setEnd)
                {
                    u32 rStart = 0, rEnd = 0;
                    if (!json.nextArrayValue(ruleCursor, setEnd, rStart, rEnd) || !json.isObject(rStart, rEnd))
                        break;
                    MultiArray<char, MAX_TEMPLATE_TYPE> rType{};
                    if (readStringField(json, rStart, "type", rType.data(), static_cast<u32>(rType.size())))
                    {
                        const MediaType rMediaType = make_media_type(rType.data());
                        if (rMediaType != MediaType::None)
                        {
                            const u32 typeIdx = static_cast<u32>(rMediaType);
                            scope.byType[typeIdx] = {rStart, rEnd, rMediaType};
                            scope.targetTypesBB |= static_cast<u16>(1u << typeIdx);
                        }
                    }
                }
            }
            if (scope.targetTypesBB != 0)
            {
                ++s_styleScopeCount;
            }
        }

        void extractTemporalStyleScopes(const JsonReader &json, u32 timelineStart, u32 timelineEnd)
        {
            s_styleScopeCount = 0;
            // 1. Uu tien doc tu block root doc lap: "roles": [ ... ] hoac "styles": [ ... ] hoac "style": [ ... ]
            u32 rolesStart = 0, rolesEnd = 0;
            if ((json.field(0, "roles", rolesStart, rolesEnd) ||
                 json.field(0, "styles", rolesStart, rolesEnd) ||
                 json.field(0, "style", rolesStart, rolesEnd)) &&
                json.isArray(rolesStart, rolesEnd))
            {
                u32 cursor = rolesStart + 1;
                while (cursor < rolesEnd && s_styleScopeCount < MAX_TEMPORAL_STYLE_SCOPES)
                {
                    u32 itStart = 0, itEnd = 0;
                    if (!json.nextArrayValue(cursor, rolesEnd, itStart, itEnd) || !json.isObject(itStart, itEnd))
                        break;
                    parseSingleRoleScope(json, itStart, itEnd);
                }
            }

            // 2. Fallback tuong thich nguoc: Neu chua co roles o root, quet timeline tim legacy "type": "style"
            if (s_styleScopeCount == 0 && timelineStart < timelineEnd)
            {
                u32 cursor = timelineStart + 1;
                while (cursor < timelineEnd && s_styleScopeCount < MAX_TEMPORAL_STYLE_SCOPES)
                {
                    u32 itStart = 0, itEnd = 0;
                    if (!json.nextArrayValue(cursor, timelineEnd, itStart, itEnd) || !json.isObject(itStart, itEnd))
                        break;
                    if (json.stringFieldEquals(itStart, "type", "style"))
                    {
                        parseSingleRoleScope(json, itStart, itEnd);
                    }
                }
            }
        }

        const StyleRuleTargetOffset* findMatchingStyleRule(MediaType targetType, double targetStartMs)
        {
            const u32 typeIdx = static_cast<u32>(targetType);
            const u16 targetBit = static_cast<u16>(1u << typeIdx);
            double bestSpan = DBL_MAX_VAL;
            const StyleRuleTargetOffset* bestRule = nullptr;
            for (u32 s = 0; s < s_styleScopeCount; ++s)
            {
                if (targetStartMs >= s_styleScopes[s].startMs && targetStartMs < s_styleScopes[s].endMs)
                {
                    if ((s_styleScopes[s].targetTypesBB & targetBit) != 0)
                    {
                        const double span = s_styleScopes[s].endMs - s_styleScopes[s].startMs;
                        if (span <= bestSpan)
                        {
                            bestSpan = span;
                            bestRule = &s_styleScopes[s].byType[typeIdx];
                        }
                    }
                    else if (targetType == MediaType::Lyric &&
                             (s_styleScopes[s].targetTypesBB & (1u << static_cast<u32>(MediaType::Text))) != 0)
                    {
                        const double span = s_styleScopes[s].endMs - s_styleScopes[s].startMs;
                        if (span <= bestSpan)
                        {
                            bestSpan = span;
                            bestRule = &s_styleScopes[s].byType[static_cast<u32>(MediaType::Text)];
                        }
                    }
                    else if (targetType == MediaType::Text &&
                             (s_styleScopes[s].targetTypesBB & (1u << static_cast<u32>(MediaType::Lyric))) != 0)
                    {
                        const double span = s_styleScopes[s].endMs - s_styleScopes[s].startMs;
                        if (span <= bestSpan)
                        {
                            bestSpan = span;
                            bestRule = &s_styleScopes[s].byType[static_cast<u32>(MediaType::Lyric)];
                        }
                    }
                }
            }
            return bestRule;
        }
        bool hasClassToken(const char *classList, const char *token)
        {
            u32 cursor = 0;
            while (classList[cursor])
            {
                while (classList[cursor] && is_ascii_space(classList[cursor]))
                    ++cursor;
                const u32 start = cursor;
                while (classList[cursor] && !is_ascii_space(classList[cursor]))
                    ++cursor;
                u32 tokenLength = 0;
                while (token[tokenLength])
                    ++tokenLength;
                if (cursor - start != tokenLength)
                    continue;
                u32 index = 0;
                while (index < tokenLength && classList[start + index] == token[index])
                    ++index;
                if (index == tokenLength)
                    return true;
            }
            return false;
        }
        bool objectHasBassSource(const JsonReader &json, u32 objStart)
        {
            MultiArray<char, MAX_TEMPLATE_NAME> source{};
            if (readFirstStringField(json, objStart, "source", "sourse", source.data(), static_cast<u32>(source.size())) &&
                source[0])
            {
                return cstr_iequal(source.data(), "bass");
            }
            return false;
        }
        bool isBassItem(const JsonReader &json, u32 itemStart)
        {
            if (objectHasBassSource(json, itemStart))
                return true;
            u32 paramsStart = 0;
            u32 paramsEnd = 0;
            if (!json.field(itemStart, "params", paramsStart, paramsEnd))
                return false;
            return objectHasBassSource(json, paramsStart);
        }
        bool readTemplateName(
            const JsonReader &json,
            u32 objectStart,
            const char *mapName,
            char *output,
            u32 capacity)
        {
            constexpr MultiArray<const char *, 3> names = {{"class", "name", "id"}};
            for (const char *fieldName : names)
            {
                if (readStringField(json, objectStart, fieldName, output, capacity) &&
                    output[0])
                {
                    trim_cstr_inplace(output);
                    if (output[0])
                        return true;
                }
            }
            if (!mapName[0])
                return false;
            u32 length = 0;
            while (mapName[length])
                ++length;
            if (length >= capacity)
                return false;
            for (u32 i = 0; i <= length; ++i)
                output[i] = mapName[i];
            trim_cstr_inplace(output);
            return output[0] != '\0';
        }
        bool addTemplate(
            const JsonReader &json,
            u32 start,
            u32 end,
            const char *mapName)
        {
            if (!json.isObject(start, end))
                return true;
            if (templateLength == PROJECT_COMPILE_MAX_TEMPLATES)
                return false;
            TemplateSpec &entry = templates[templateLength];
            entry.type[0] = '\0';
            entry.name[0] = '\0';
            entry.mapClass[0] = '\0';
            entry.effect[0] = '\0';
            entry.parentName[0] = '\0';
            entry.hasSyntheticName = false;
            entry.parentIndex = -1;
            entry.start = start;
            entry.end = end;
            readStringField(json, start, "type", entry.type.data(), static_cast<u32>(entry.type.size()));
            readStringField(json, start, "effect", entry.effect.data(), static_cast<u32>(entry.effect.size()));
            entry.hasName = readTemplateName(
                json, start, "", entry.name.data(), static_cast<u32>(entry.name.size()));
            if (!entry.hasName && mapName[0])
            {
                u32 nameLength = 0;
                while (mapName[nameLength])
                    ++nameLength;
                if (nameLength >= entry.name.size())
                    return false;
                for (u32 i = 0; i <= nameLength; ++i)
                {
                    entry.name[i] = mapName[i];
                    entry.mapClass[i] = mapName[i];
                }
                trim_cstr_inplace(entry.name.data());
                entry.hasName = entry.name[0] != '\0';
                entry.hasSyntheticName = entry.hasName;
            }
            // Đọc liên kết template cha (parent, extends, inherit)
            if (!readFirstStringField(json, start, "parent", "extends", entry.parentName.data(), static_cast<u32>(entry.parentName.size())))
            {
                readStringField(json, start, "inherit", entry.parentName.data(), static_cast<u32>(entry.parentName.size()));
            }
            trim_cstr_inplace(entry.parentName.data());
            trim_cstr_inplace(entry.effect.data());
            lower_ascii_inplace(entry.type.data());
            lower_ascii_inplace(entry.effect.data());

            // Đăng ký và nướng vào TemplateArena ở Cold Path
            if (entry.hasName)
            {
                TemplateNode* node = TemplateArena::instance().allocate_template(entry.name.data(), entry.type.data());
                if (node)
                {
                    u32 cursor = start + 1;
                    while (cursor < end)
                    {
                        u32 kStart = 0, kEnd = 0, vStart = 0, vEnd = 0;
                        if (!json.nextObjectField(cursor, end, kStart, kEnd, vStart, vEnd))
                            break;
                        MultiArray<char, 48> key{};
                        json.readStringValue(kStart, kEnd, key.data(), static_cast<u32>(key.size()));
                        if (cstr_equal(key.data(), "name") || cstr_equal(key.data(), "id") || cstr_equal(key.data(), "class") ||
                            cstr_equal(key.data(), "type") || cstr_equal(key.data(), "parent") || cstr_equal(key.data(), "extends") ||
                            cstr_equal(key.data(), "inherit") || cstr_equal(key.data(), "effect"))
                            continue;
                        MultiArray<char, MAX_TEMPLATE_NAME> val{};
                        json.readStringValue(vStart, vEnd, val.data(), static_cast<u32>(val.size()));
                        double numVal = 0.0;
                        const bool isNum = json.numberField(start, key.data(), numVal);
                        prop::PropertyId pId = prop::INVALID_PROPERTY_ID;
                        for (u32 i = 0; i < prop::STANDARD_PROP_COUNT; ++i)
                        {
                            const auto &sd = prop::kPropSchemas[i];
                            if ((sd.kebab && cstr_equal(key.data(), sd.kebab)) ||
                                (sd.camel && cstr_equal(key.data(), sd.camel)) ||
                                (sd.alias && cstr_equal(key.data(), sd.alias)))
                            {
                                pId = static_cast<prop::PropertyId>(sd.id);
                                break;
                            }
                        }
                        if (pId < TemplateNode::MAX_PROPS)
                        {
                            if (isNum)
                                node->set_defined_property<true>(pId, key.data(), val.data(), numVal);
                            else
                                node->set_defined_property<false>(pId, key.data(), val.data());
                        }
                    }
                }
            }

            ++templateLength;
            return true;
        }
        i32 findGeneralTemplate(const char *name);
        bool buildTemplateIndex(
            const JsonReader &json,
            u32 projectStart,
            u32 &count)
        {
            TemplateArena::instance().reset();
            constexpr MultiArray<const char *, 4> fields = {{
                "template", "templates", "Template", "Templates"}};
            u32 templatesStart = 0;
            u32 templatesEnd = 0;
            bool found = false;
            for (const char *fieldName : fields)
            {
                if (json.field(projectStart, fieldName, templatesStart, templatesEnd))
                {
                    if (json.isArray(templatesStart, templatesEnd) ||
                        json.isObject(templatesStart, templatesEnd))
                    {
                        found = true;
                        break;
                    }
                }
            }
            if (!found)
                return true;
            if (json.isArray(templatesStart, templatesEnd))
            {
                u32 cursor = templatesStart + 1;
                while (cursor < templatesEnd)
                {
                    u32 valueStart = 0;
                    u32 valueEnd = 0;
                    if (!json.nextArrayValue(cursor, templatesEnd, valueStart, valueEnd))
                        break;
                    if (!addTemplate(json, valueStart, valueEnd, ""))
                        return false;
                }
            }
            else if (json.isObject(templatesStart, templatesEnd))
            {
                u32 cursor = templatesStart + 1;
                while (cursor < templatesEnd)
                {
                    u32 keyStart = 0;
                    u32 keyEnd = 0;
                    u32 valueStart = 0;
                    u32 valueEnd = 0;
                    if (!json.nextObjectField(
                            cursor, templatesEnd, keyStart, keyEnd,
                            valueStart, valueEnd))
                        break;
                    MultiArray<char, MAX_TEMPLATE_NAME> mapName{};
                    if (!json.readStringValue(
                            keyStart, keyEnd, mapName.data(), static_cast<u32>(mapName.size())))
                        return false;
                    if (!addTemplate(json, valueStart, valueEnd, mapName.data()))
                        return false;
                }
            }

            // Liên kết con trỏ cha duy nhất: TemplateNode* parent = ...
            for (u32 i = 0; i < templateLength; ++i)
            {
                if (templates[i].parentName[0])
                {
                    templates[i].parentIndex = findGeneralTemplate(templates[i].parentName.data());
                    TemplateNode* childNode = TemplateArena::instance().find_template(templates[i].name.data());
                    TemplateNode* parentNode = TemplateArena::instance().find_template(templates[i].parentName.data());
                    if (childNode && parentNode)
                    {
                        TemplateArena::instance().link_parent(childNode, parentNode);
                    }
                }
            }
            // Nướng hòa trộn phân cấp toàn bộ templates 1 lần duy nhất ở Cold Path
            TemplateArena::instance().bake_all_templates();

            count = templateLength;
            return true;
        }
        bool readItemType(
            const JsonReader &json,
            u32 itemStart,
            char *output,
            u32 capacity)
        {
            if (!readStringField(json, itemStart, "type", output, capacity))
                return false;
            lower_ascii_inplace(output);
            return true;
        }
        bool readItemEffect(
            const JsonReader &json,
            u32 itemStart,
            char *output,
            u32 capacity)
        {
            if (readFirstStringField(
                    json, itemStart, "effect", "name", output, capacity))
            {
                trim_cstr_inplace(output);
                lower_ascii_inplace(output);
                return true;
            }
            output[0] = '\0';
            return false;
        }
        bool templateIsDefault(const TemplateSpec &entry, const char *type)
        {
            return cstr_equal(entry.type.data(), type) && !entry.hasName && !entry.effect[0];
        }
        i32 findDefaultTemplate(const char *type)
        {
            i32 found = -1;
            for (u32 i = 0; i < templateLength; ++i)
                if (templateIsDefault(templates[i], type))
                    found = static_cast<i32>(i);
            return found;
        }
        i32 findEffectTemplate(const char *effect)
        {
            i32 found = -1;
            for (u32 i = 0; i < templateLength; ++i)
            {
                const TemplateSpec &entry = templates[i];
                if (cstr_equal(entry.type.data(), "effect") && cstr_equal(entry.effect.data(), effect) &&
                    !entry.hasName)
                    found = static_cast<i32>(i);
            }
            return found;
        }
        i32 findTypedTemplate(const char *type, const char *name)
        {
            i32 found = -1;
            for (u32 i = 0; i < templateLength; ++i)
            {
                const TemplateSpec &entry = templates[i];
                if (entry.hasName && cstr_equal(entry.type.data(), type) &&
                    cstr_equal(entry.name.data(), name))
                    found = static_cast<i32>(i);
            }
            return found;
        }
        i32 findGeneralTemplate(const char *name)
        {
            for (u32 i = 0; i < templateLength; ++i)
                if (templates[i].hasName && cstr_equal(templates[i].name.data(), name))
                    return static_cast<i32>(i);
            return -1;
        }
        bool keyEquals(
            const JsonReader &json,
            const FieldSpan &field,
            const char *name)
        {
            return json.stringValueEquals(field.keyStart, field.keyEnd, name);
        }
        bool sameKey(
            const JsonReader &json,
            const FieldSpan &left,
            u32 rightStart,
            u32 rightEnd)
        {
            MultiArray<char, MAX_KEY_TEXT> leftKey{};
            MultiArray<char, MAX_KEY_TEXT> rightKey{};
            return json.readStringValue(
                       left.keyStart, left.keyEnd, leftKey.data(), static_cast<u32>(leftKey.size())) &&
                   json.readStringValue(
                       rightStart, rightEnd, rightKey.data(), static_cast<u32>(rightKey.size())) &&
                   cstr_equal(leftKey.data(), rightKey.data());
        }
        i32 findFieldIndex(
            const JsonReader &json,
            FieldSpan *fields,
            u32 count,
            u32 keyStart,
            u32 keyEnd)
        {
            for (u32 i = 0; i < count; ++i)
                if (sameKey(json, fields[i], keyStart, keyEnd))
                    return static_cast<i32>(i);
            return -1;
        }
        template <bool StripIdentity>
        bool mergeObjectFields(
            const JsonReader &json,
            u32 objectStart,
            u32 objectEnd,
            FieldSpan *fields,
            u32 &count)
        {
            if (!json.isObject(objectStart, objectEnd))
                return true;
            u32 cursor = objectStart + 1;
            while (cursor < objectEnd)
            {
                u32 keyStart = 0;
                u32 keyEnd = 0;
                u32 valueStart = 0;
                u32 valueEnd = 0;
                if (!json.nextObjectField(
                        cursor, objectEnd, keyStart, keyEnd, valueStart, valueEnd))
                    break;
                FieldSpan incoming{keyStart, keyEnd, valueStart, valueEnd, 0};
                if constexpr (StripIdentity)
                {
                    if (keyEquals(json, incoming, "uid") ||
                        keyEquals(json, incoming, "seq"))
                        continue;
                }
                const i32 existing = findFieldIndex(
                    json, fields, count, keyStart, keyEnd);
                if (existing < 0)
                {
                    if (count == MAX_OBJECT_FIELDS)
                        return false;
                    fields[count++] = incoming;
                }
                else
                {
                    fields[existing].valueStart = valueStart;
                    fields[existing].valueEnd = valueEnd;
                    fields[existing].kind = 0;
                }
            }
            return true;
        }
        bool mergeNestedField(
            const JsonReader &json,
            FieldSpan &rootField,
            FieldSpan *nestedFields,
            u32 &nestedCount,
            u32 incomingStart,
            u32 incomingEnd)
        {
            if (!json.isObject(incomingStart, incomingEnd))
            {
                rootField.valueStart = incomingStart;
                rootField.valueEnd = incomingEnd;
                rootField.kind = 0;
                nestedCount = 0;
                return true;
            }
            if (rootField.kind != 1)
                nestedCount = 0;
            rootField.valueStart = incomingStart;
            rootField.valueEnd = incomingEnd;
            rootField.kind = 1;
            return mergeObjectFields<false>(
                json, incomingStart, incomingEnd, nestedFields, nestedCount);
        }
        bool mergeLayer(
            const JsonReader &json,
            u32 objectStart,
            u32 objectEnd,
            FieldSpan *rootFields,
            u32 &rootCount,
            FieldSpan *cssFields,
            u32 &cssCount,
            FieldSpan *styleFields,
            u32 &styleCount,
            FieldSpan *paramsFields,
            u32 &paramsCount)
        {
            if (!json.isObject(objectStart, objectEnd))
                return true;
            u32 cursor = objectStart + 1;
            while (cursor < objectEnd)
            {
                u32 keyStart = 0;
                u32 keyEnd = 0;
                u32 valueStart = 0;
                u32 valueEnd = 0;
                if (!json.nextObjectField(
                        cursor, objectEnd, keyStart, keyEnd, valueStart, valueEnd))
                    break;
                FieldSpan incoming{keyStart, keyEnd, valueStart, valueEnd, 0};
                if (keyEquals(json, incoming, "uid") ||
                    keyEquals(json, incoming, "seq") ||
                    keyEquals(json, incoming, "_nativeGroup"))
                    continue;
                i32 rootIndex = findFieldIndex(
                    json, rootFields, rootCount, keyStart, keyEnd);
                if (rootIndex < 0)
                {
                    if (rootCount == MAX_OBJECT_FIELDS)
                        return false;
                    rootIndex = static_cast<i32>(rootCount++);
                    rootFields[rootIndex] = incoming;
                }
                FieldSpan &rootField = rootFields[rootIndex];
                if (keyEquals(json, incoming, "css"))
                {
                    if (!mergeNestedField(
                            json, rootField, cssFields, cssCount, valueStart, valueEnd))
                        return false;
                }
                else if (keyEquals(json, incoming, "style"))
                {
                    if (!mergeNestedField(
                            json, rootField, styleFields, styleCount, valueStart, valueEnd))
                        return false;
                }
                else if (keyEquals(json, incoming, "params"))
                {
                    if (!mergeNestedField(
                            json, rootField, paramsFields, paramsCount, valueStart, valueEnd))
                        return false;
                }
                else
                {
                    rootField.valueStart = valueStart;
                    rootField.valueEnd = valueEnd;
                    rootField.kind = 0;
                }
            }
            return true;
        }
        bool mergeStyleRule(
            const JsonReader &json,
            u32 ruleStart,
            u32 ruleEnd,
            FieldSpan *rootFields,
            u32 &rootCount,
            FieldSpan *cssFields,
            u32 &cssCount,
            FieldSpan *styleFields,
            u32 &styleCount,
            FieldSpan *paramsFields,
            u32 &paramsCount)
        {
            if (!json.isObject(ruleStart, ruleEnd))
                return true;
            u32 cursor = ruleStart + 1;
            while (cursor < ruleEnd)
            {
                u32 keyStart = 0;
                u32 keyEnd = 0;
                u32 valueStart = 0;
                u32 valueEnd = 0;
                if (!json.nextObjectField(
                        cursor, ruleEnd, keyStart, keyEnd, valueStart, valueEnd))
                    break;
                FieldSpan incoming{keyStart, keyEnd, valueStart, valueEnd, 0};
                if (keyEquals(json, incoming, "type") ||
                    keyEquals(json, incoming, "start") ||
                    keyEquals(json, incoming, "end") ||
                    keyEquals(json, incoming, "rules") ||
                    keyEquals(json, incoming, "set"))
                    continue;
                i32 rootIndex = findFieldIndex(
                    json, rootFields, rootCount, keyStart, keyEnd);
                if (rootIndex < 0)
                {
                    if (rootCount == MAX_OBJECT_FIELDS)
                        return false;
                    rootIndex = static_cast<i32>(rootCount++);
                    rootFields[rootIndex] = incoming;
                }
                FieldSpan &rootField = rootFields[rootIndex];
                if (keyEquals(json, incoming, "css"))
                {
                    if (!mergeNestedField(
                            json, rootField, cssFields, cssCount, valueStart, valueEnd))
                        return false;
                }
                else if (keyEquals(json, incoming, "style"))
                {
                    if (!mergeNestedField(
                            json, rootField, styleFields, styleCount, valueStart, valueEnd))
                        return false;
                }
                else if (keyEquals(json, incoming, "params"))
                {
                    if (!mergeNestedField(
                            json, rootField, paramsFields, paramsCount, valueStart, valueEnd))
                        return false;
                }
                else
                {
                    rootField.valueStart = valueStart;
                    rootField.valueEnd = valueEnd;
                    rootField.kind = 0;
                }
            }
            return true;
        }
        bool mergeTemplateHierarchy(
            const JsonReader &json,
            i32 templateIndex,
            FieldSpan *rootFields,
            u32 &rootCount,
            FieldSpan *cssFields,
            u32 &cssCount,
            FieldSpan *styleFields,
            u32 &styleCount,
            FieldSpan *paramsFields,
            u32 &paramsCount)
        {
            if (templateIndex < 0 || static_cast<u32>(templateIndex) >= templateLength)
                return true;

            // 1. Hòa trộn thuộc tính của chính template con trước
            const TemplateSpec &entry = templates[templateIndex];
            if (!mergeLayer(json, entry.start, entry.end, rootFields, rootCount,
                            cssFields, cssCount, styleFields, styleCount,
                            paramsFields, paramsCount))
                return false;

            // 2. Tra cứu chuỗi cha: cha ghi đè con, cha cao hơn ghi đè cha thấp hơn
            // Nếu không có cha (parentIndex < 0): vòng lặp chạy 0 lần (0 branch check if (has_parent))
            i32 pIdx = entry.parentIndex;
            u32 depth = 0;
            while (pIdx >= 0 && static_cast<u32>(pIdx) < templateLength && depth < MAX_TEMPLATE_HIERARCHY_DEPTH)
            {
                const TemplateSpec &parentEntry = templates[pIdx];
                if (!mergeLayer(json, parentEntry.start, parentEntry.end, rootFields, rootCount,
                                cssFields, cssCount, styleFields, styleCount,
                                paramsFields, paramsCount))
                    return false;
                pIdx = parentEntry.parentIndex;
                ++depth;
            }
            return true;
        }
        const FieldSpan *findMergedField(
            const JsonReader &json,
            const FieldSpan *fields,
            u32 count,
            const char *name)
        {
            for (u32 index = 0; index < count; ++index)
                if (keyEquals(json, fields[index], name))
                    return &fields[index];
            return nullptr;
        }
        constexpr u32 INHERITED_STYLE_COUNT =
            static_cast<u32>(stockcut::prop::INHERITABLE_PROP_COUNT);
        static_assert(INHERITED_STYLE_COUNT <= 32u,
                      "GroupInheritance bitmasks require <= 32 inheritable properties");
        struct InheritedStyleState
        {
            FieldSpan source;
            MultiArray<char, MAX_STYLE_VALUE_TEXT> sourceText; // KHONG CO {} (guarded by GroupInheritance bitmasks)
        };
        struct GroupInheritance
        {
            bool hasStart = false;
            bool hasEnd = false;
            u32 startStart = 0;
            u32 startEnd = 0;
            u32 endStart = 0;
            u32 endEnd = 0;
            double zIndex = 0.0;
            double opacity = 1.0;
            bool hasGroups = false;
            u32 foundMask = 0u;
            u32 appliedMask = 0u;
            u32 sourceOwnedMask = 0u;
            MultiArray<InheritedStyleState, INHERITED_STYLE_COUNT> styles; // KHONG CO {} (guarded by bitmasks)
        };
        bool spanIsNull(
            const JsonReader &json,
            u32 start,
            u32 end)
        {
            MultiArray<char, 16> value;
            u32 used = 0;
            if (!json.copySpan(start, end, value.data(), static_cast<u32>(value.size()), used) ||
                used >= value.size())
                return false;
            value[used] = '\0';
            trim_cstr_inplace(value.data());
            return cstr_equal(value.data(), "null");
        }
        bool findStyleField(
            const JsonReader &json,
            u32 objectStart,
            u32 objectEnd,
            stockcut::prop::StandardPropId propId,
            InheritedStyleState &result,
            bool &outSourceOwned)
        {
            const stockcut::prop::PropDescriptor &spec = scenePropertyByEnum[static_cast<u32>(propId)];
            bool found = false;
            outSourceOwned = false;
            constexpr MultiArray<const char *, 3> containers = {{"", "style", "css"}};
            for (const char *containerName : containers)
            {
                u32 containerStart = objectStart;
                u32 containerEnd = objectEnd;
                if (containerName[0])
                {
                    if (!json.field(objectStart, containerName,
                                    containerStart, containerEnd))
                        continue;
                    if (!json.isObject(containerStart, containerEnd))
                    {
                        SceneStyleValue inlineValue;
                        if (findCssTextProperty(
                                json, containerStart, containerEnd,
                                spec,
                                inlineValue))
                        {
                            copy_cstr_bounded(result.sourceText.data(), result.sourceText.size(), inlineValue.text.data());
                            found = true;
                            outSourceOwned = true;
                        }
                        continue;
                    }
                }
                u32 cursor = containerStart + 1;
                while (cursor < containerEnd)
                {
                    u32 keyStart = 0;
                    u32 keyEnd = 0;
                    u32 valueStart = 0;
                    u32 valueEnd = 0;
                    if (!json.nextObjectField(
                            cursor, containerEnd, keyStart, keyEnd,
                            valueStart, valueEnd))
                        break;
                    if (json.stringValueEquals(keyStart, keyEnd, spec.kebab) &&
                        !spanIsNull(json, valueStart, valueEnd))
                    {
                        result.source = {keyStart, keyEnd, valueStart, valueEnd, 0};
                        found = true;
                        outSourceOwned = false;
                    }
                }
            }
            return found;
        }
        bool setMergedField(
            const JsonReader &json,
            FieldSpan *fields,
            u32 &count,
            const FieldSpan &incoming)
        {
            const i32 existing = findFieldIndex(
                json, fields, count, incoming.keyStart, incoming.keyEnd);
            if (existing >= 0)
            {
                fields[existing].valueStart = incoming.valueStart;
                fields[existing].valueEnd = incoming.valueEnd;
                return true;
            }
            if (count == MAX_OBJECT_FIELDS)
                return false;
            fields[count++] = incoming;
            return true;
        }
        bool scenePropertyMatches(
            const JsonReader &json,
            u32 keyStart,
            u32 keyEnd,
            const stockcut::prop::PropDescriptor &spec)
        {
            return json.stringValueEquals(keyStart, keyEnd, spec.kebab) ||
                   (spec.camel && json.stringValueEquals(keyStart, keyEnd, spec.camel)) ||
                   (spec.alias && json.stringValueEquals(keyStart, keyEnd, spec.alias));
        }
        bool copyStyleValue(
            const JsonReader &json,
            u32 valueStart,
            u32 valueEnd,
            SceneStyleValue &output)
        {
            output.found = false;
            if (valueStart < valueEnd && json.readStringValue(
                                             valueStart, valueEnd, output.text.data(), static_cast<u32>(output.text.size())))
            {
                output.found = true;
                output.numericSource = false;
                return true;
            }
            u32 used = 0;
            if (!json.copySpan(
                    valueStart, valueEnd, output.text.data(), static_cast<u32>(output.text.size()), used) ||
                used >= output.text.size())
                return false;
            output.text[used] = '\0';
            output.found = true;
            output.numericSource = used &&
                                   (output.text[0] == '-' ||
                                    (output.text[0] >= '0' && output.text[0] <= '9'));
            return true;
        }
        bool findObjectStyleProperty(
            const JsonReader &json,
            u32 objectStart,
            u32 objectEnd,
            const stockcut::prop::PropDescriptor &spec,
            SceneStyleValue &output)
        {
            if (!json.isObject(objectStart, objectEnd))
                return false;
            bool found = false;
            u32 cursor = objectStart + 1;
            while (cursor < objectEnd)
            {
                u32 keyStart = 0;
                u32 keyEnd = 0;
                u32 valueStart = 0;
                u32 valueEnd = 0;
                if (!json.nextObjectField(
                        cursor, objectEnd, keyStart, keyEnd, valueStart, valueEnd))
                    break;
                if (!scenePropertyMatches(json, keyStart, keyEnd, spec) ||
                    spanIsNull(json, valueStart, valueEnd))
                    continue;
                if (!copyStyleValue(json, valueStart, valueEnd, output))
                    return false;
                found = true;
            }
            return found;
        }
        bool findCssTextProperty(
            const JsonReader &json,
            u32 valueStart,
            u32 valueEnd,
            const stockcut::prop::PropDescriptor &spec,
            SceneStyleValue &output)
        {
            MultiArray<char, MAX_CSS_DECLARATION_TEXT> declarations;
            if (!json.readStringValue(
                    valueStart, valueEnd, declarations.data(), static_cast<u32>(declarations.size())))
                return false;
            u32 length = 0;
            while (declarations[length])
                ++length;
            u32 declarationStart = 0;
            u32 depth = 0;
            char quote = '\0';
            bool found = false;
            for (u32 position = 0; position <= length; ++position)
            {
                const char current = position < length ? declarations[position] : ';';
                if (quote)
                {
                    if (current == quote)
                        quote = '\0';
                    continue;
                }
                if (current == '\'' || current == '"')
                {
                    quote = current;
                    continue;
                }
                if (current == '(')
                    ++depth;
                else if (current == ')' && depth)
                    --depth;
                if (current != ';' || depth)
                    continue;
                u32 colon = declarationStart;
                while (colon < position && declarations[colon] != ':')
                    ++colon;
                if (colon < position)
                {
                    u32 keyStart = declarationStart;
                    while (keyStart < colon && is_ascii_space(declarations[keyStart]))
                        ++keyStart;
                    u32 keyEnd = colon;
                    while (keyEnd > keyStart && is_ascii_space(declarations[keyEnd - 1]))
                        --keyEnd;
                    MultiArray<char, MAX_TEMPLATE_NAME> key;
                    const u32 keyLength = keyEnd - keyStart;
                    if (keyLength < key.size())
                    {
                        for (u32 i = 0; i < keyLength; ++i)
                            key[i] = declarations[keyStart + i];
                        key[keyLength] = '\0';
                        MultiArray<char, MAX_TEMPLATE_NAME> normalizedKey;
                        u32 normalizedLength = 0;
                        for (u32 i = 0; key[i] && normalizedLength + 2 < normalizedKey.size(); ++i)
                        {
                            if (key[i] >= 'A' && key[i] <= 'Z')
                            {
                                normalizedKey[normalizedLength++] = '-';
                                normalizedKey[normalizedLength++] = ascii_lower(key[i]);
                            }
                            else
                            {
                                normalizedKey[normalizedLength++] = ascii_lower(key[i]);
                            }
                        }
                        normalizedKey[normalizedLength] = '\0';
                        const bool matches = cstr_equal(normalizedKey.data(), spec.kebab) ||
                                             (spec.alias && cstr_equal(normalizedKey.data(), spec.alias));
                        if (matches)
                        {
                            u32 valueTextStart = colon + 1;
                            while (valueTextStart < position &&
                                   is_ascii_space(declarations[valueTextStart]))
                                ++valueTextStart;
                            u32 valueTextEnd = position;
                            while (valueTextEnd > valueTextStart &&
                                   is_ascii_space(declarations[valueTextEnd - 1]))
                                --valueTextEnd;
                            const u32 valueLength = valueTextEnd - valueTextStart;
                            if (valueLength < output.text.size())
                            {
                                for (u32 i = 0; i < valueLength; ++i)
                                    output.text[i] = declarations[valueTextStart + i];
                                output.text[valueLength] = '\0';
                                output.found = valueLength > 0;
                                output.numericSource = false;
                                found = output.found;
                            }
                        }
                    }
                }
                declarationStart = position + 1;
            }
            return found;
        }
        bool findSceneStylePropertyBySpec(
            const JsonReader &json,
            u32 objectStart,
            u32 objectEnd,
            const stockcut::prop::PropDescriptor &spec,
            SceneStyleValue &output)
        {
            output.found = false;
            output.numericSource = false;
            output.text[0] = '\0';
            bool found = findObjectStyleProperty(
                json, objectStart, objectEnd, spec, output);
            constexpr MultiArray<const char *, 2> containers = {{"style", "css"}};
            for (const char *container : containers)
            {
                u32 valueStart = 0;
                u32 valueEnd = 0;
                if (!json.field(objectStart, container, valueStart, valueEnd))
                    continue;
                if (json.isObject(valueStart, valueEnd))
                {
                    SceneStyleValue nested;
                    if (findObjectStyleProperty(
                            json, valueStart, valueEnd, spec, nested))
                    {
                        output = nested;
                        found = true;
                    }
                }
                else
                {
                    SceneStyleValue nested;
                    if (findCssTextProperty(
                            json, valueStart, valueEnd, spec, nested))
                    {
                        output = nested;
                        found = true;
                    }
                }
            }
            return found;
        }
        template <stockcut::prop::StandardPropId Prop>
        bool findSceneStyleProperty(
            const JsonReader &json,
            u32 objectStart,
            u32 objectEnd,
            SceneStyleValue &output)
        {
            return findSceneStylePropertyBySpec(
                json, objectStart, objectEnd,
                scenePropertyByEnum[static_cast<u32>(Prop)], output);
        }
        bool parseSceneLength(
            const char *text,
            double reference,
            double fallback,
            double &result)
        {
            u32 position = 0;
            while (is_ascii_space(text[position]))
                ++position;
            bool negative = false;
            if (text[position] == '-')
            {
                negative = true;
                ++position;
            }
            double number = 0.0;
            bool hasDigits = false;
            while (text[position] >= '0' && text[position] <= '9')
            {
                number = number * 10.0 + (text[position++] - '0');
                hasDigits = true;
            }
            if (text[position] == '.')
            {
                ++position;
                double factor = 0.1;
                while (text[position] >= '0' && text[position] <= '9')
                {
                    number += (text[position++] - '0') * factor;
                    factor *= 0.1;
                    hasDigits = true;
                }
            }
            if (!hasDigits)
            {
                result = fallback;
                return false;
            }
            while (is_ascii_space(text[position]))
                ++position;
            MultiArray<char, 8> unit;
            u32 unitLength = 0;
            while (text[position] && unitLength + 1 < unit.size())
            {
                unit[unitLength++] = ascii_lower(text[position++]);
            }
            unit[unitLength] = '\0';
            while (is_ascii_space(text[position]))
                ++position;
            if (text[position])
            {
                result = fallback;
                return false;
            }
            if (negative)
                number = -number;
            if (cstr_equal(unit.data(), "%") || cstr_equal(unit.data(), "cqw") || cstr_equal(unit.data(), "cqh") ||
                cstr_equal(unit.data(), "vw") || cstr_equal(unit.data(), "vh"))
                number *= reference / 100.0;
            else if (!cstr_equal(unit.data(), "") && !cstr_equal(unit.data(), "px"))
            {
                result = fallback;
                return false;
            }
            result = number;
            return true;
        }
        double sceneLength(
            const SceneStyleValue &value,
            double reference,
            double fallback)
        {
            double result = fallback;
            if (!value.found || !value.text[0])
                return fallback;
            parseSceneLength(value.text.data(), reference, fallback, result);
            return result;
        }
        double sceneLength(
            const char *value,
            double reference,
            double fallback)
        {
            double result = fallback;
            parseSceneLength(value, reference, fallback, result);
            return result;
        }
        void estimateTextDimensions(
            const JsonReader &json,
            u32 start,
            u32 end,
            double stageW,
            double &outW,
            double &outH)
        {
            SceneStyleValue fontSizeValue, lineHeightValue;
            findSceneStyleProperty<prop::PROP_FONT_SIZE>(json, start, end, fontSizeValue);
            findSceneStyleProperty<prop::PROP_LINE_HEIGHT>(json, start, end, lineHeightValue);
            double fontSize = 50.0;
            if (fontSizeValue.found && fontSizeValue.text[0])
            {
                double parsed = 0.0;
                if (parseSceneLength(fontSizeValue.text.data(), stageW, 50.0, parsed) && parsed > 0.0)
                    fontSize = parsed;
            }
            double lineHeight = DEFAULT_LINE_HEIGHT;
            if (lineHeightValue.found && lineHeightValue.text[0])
            {
                double parsed = 0.0;
                if (parseSceneLength(lineHeightValue.text.data(), 1.0, DEFAULT_LINE_HEIGHT, parsed) && parsed > 0.0)
                    lineHeight = parsed;
            }
            u32 contentStart = 0, contentEnd = 0;
            u32 lineCount = 1;
            u32 maxLineChars = 0;
            u32 curLineChars = 0;
            if (json.field(start, "content", contentStart, contentEnd))
            {
                MultiArray<char, MAX_CSS_DECLARATION_TEXT> scratch;
                if (json.readStringValue(contentStart, contentEnd, scratch.data(), static_cast<u32>(scratch.size())))
                {
                    for (u32 idx = 0; scratch[idx]; ++idx)
                    {
                        if (scratch[idx] == '\n')
                        {
                            maxLineChars = std::max(maxLineChars, curLineChars);
                            curLineChars = 0;
                            ++lineCount;
                        }
                        else
                        {
                            ++curLineChars;
                        }
                    }
                    maxLineChars = std::max(maxLineChars, curLineChars);
                }
            }
            if (maxLineChars == 0)
                maxLineChars = 10;
            const double estimated = std::ceil(static_cast<double>(maxLineChars) * (fontSize * 0.47));
            outW = std::min(stageW, std::max(fontSize * 1.5, estimated));
            outH = static_cast<double>(lineCount) * fontSize * lineHeight;
        }
        void sceneInsets(
            const SceneStyleValue &value,
            double width,
            double height,
            double *output)
        {
            const char *text = value.found && value.text[0] ? value.text.data() : "0";
            MultiArray<char, 4, MAX_TOKEN_TEXT> tokens;
            u32 tokenCount = 0;
            u32 position = 0;
            while (text[position] && tokenCount < 4)
            {
                while (is_ascii_space(text[position]))
                    ++position;
                if (!text[position])
                    break;
                u32 length = 0;
                while (text[position] && !is_ascii_space(text[position]))
                {
                    if (length + 1 < MAX_TOKEN_TEXT)
                        tokens[tokenCount][length++] = text[position];
                    ++position;
                }
                tokens[tokenCount][length] = '\0';
                ++tokenCount;
            }
            if (!tokenCount)
            {
                output[0] = output[1] = output[2] = output[3] = 0.0;
            }
            else if (tokenCount == 1)
            {
                const double all = sceneLength(tokens[0].data(), height, 0.0);
                output[0] = output[1] = output[2] = output[3] = all;
            }
            else if (tokenCount == 2)
            {
                output[0] = output[2] = sceneLength(tokens[0].data(), height, 0.0);
                output[1] = output[3] = sceneLength(tokens[1].data(), width, 0.0);
            }
            else if (tokenCount == 3)
            {
                output[0] = sceneLength(tokens[0].data(), height, 0.0);
                output[1] = output[3] = sceneLength(tokens[1].data(), width, 0.0);
                output[2] = sceneLength(tokens[2].data(), height, 0.0);
            }
            else
            {
                output[0] = sceneLength(tokens[0].data(), height, 0.0);
                output[1] = sceneLength(tokens[1].data(), width, 0.0);
                output[2] = sceneLength(tokens[2].data(), height, 0.0);
                output[3] = sceneLength(tokens[3].data(), width, 0.0);
            }
        }
        GridTracks sceneGridTracks(
            const SceneStyleValue &value,
            double available,
            u32 fallbackCount)
        {
            GridTracks result;
            MultiArray<char, MAX_STYLE_VALUE_TEXT> text;
            const char *source = value.found ? value.text.data() : "";
            copy_cstr_bounded(text.data(), text.size(), source);
            trim_cstr_inplace(text.data());
            u32 textLength = 0;
            while (text[textLength])
                ++textLength;
            const bool repeated = textLength > 7 &&
                                  cstr_equal_icase_n(text.data(), "repeat(", 7) &&
                                  text[textLength - 1] == ')';
            u32 repeatCount = 0;
            u32 tracksStart = 0;
            u32 tracksEnd = textLength;
            if (repeated)
            {
                u32 position = 7;
                while (text[position] >= '0' && text[position] <= '9')
                {
                    repeatCount = repeatCount * 10 + (text[position++] - '0');
                }
                while (is_ascii_space(text[position]))
                    ++position;
                if (!repeatCount || text[position] != ',')
                {
                    repeatCount = 0;
                }
                else
                {
                    tracksStart = ++position;
                    tracksEnd = textLength - 1;
                    while (tracksStart < tracksEnd && is_ascii_space(text[tracksStart]))
                        ++tracksStart;
                    while (tracksEnd > tracksStart && is_ascii_space(text[tracksEnd - 1]))
                        --tracksEnd;
                }
            }
            MultiArray<char, PROJECT_COMPILE_MAX_SCENE_LEAVES, MAX_TOKEN_TEXT> trackTokens;
            u32 tokenCount = 0;
            u32 cursor = tracksStart;
            while (cursor < tracksEnd && tokenCount < PROJECT_COMPILE_MAX_SCENE_LEAVES)
            {
                while (cursor < tracksEnd && is_ascii_space(text[cursor]))
                    ++cursor;
                if (cursor >= tracksEnd)
                    break;
                u32 tokenLength = 0;
                while (cursor < tracksEnd && !is_ascii_space(text[cursor]))
                {
                    if (tokenLength + 1 < MAX_TOKEN_TEXT)
                        trackTokens[tokenCount][tokenLength++] = text[cursor];
                    ++cursor;
                }
                trackTokens[tokenCount][tokenLength] = '\0';
                ++tokenCount;
            }
            result.count = repeatCount ? repeatCount
                                       : (tokenCount ? tokenCount : fallbackCount);
            if (result.count > PROJECT_COMPILE_MAX_SCENE_LEAVES)
                result.count = PROJECT_COMPILE_MAX_SCENE_LEAVES;
            if (!result.count)
                result.count = 1;
            double fixed = 0.0;
            double fractional = 0.0;
            for (u32 index = 0; index < result.count; ++index)
            {
                const char *token = tokenCount
                                        ? trackTokens[index < tokenCount ? index : tokenCount - 1].data()
                                        : "1fr";
                u32 tokenLength = 0;
                while (token[tokenLength])
                    ++tokenLength;
                if (tokenLength >= 2 &&
                    cstr_equal_icase_n(token + tokenLength - 2, "fr", 2))
                {
                    MultiArray<char, MAX_TOKEN_TEXT> weightText;
                    const u32 weightLength = tokenLength - 2;
                    for (u32 i = 0; i < weightLength; ++i)
                        weightText[i] = token[i];
                    weightText[weightLength] = '\0';
                    double weight = 0.0;
                    if (!weightLength || !parseSceneLength(weightText.data(), 1.0, 1.0, weight) ||
                        weight == 0.0)
                        weight = 1.0;
                    result.values[index] = -weight;
                    fractional += weight;
                }
                else
                {
                    SceneStyleValue trackValue;
                    copy_cstr_bounded(trackValue.text.data(), trackValue.text.size(), token);
                    trackValue.found = true;
                    result.values[index] = std::max(
                        0.0, sceneLength(trackValue, available, 0.0));
                    fixed += result.values[index];
                }
            }
            for (u32 index = 0; index < result.count; ++index)
            {
                if (result.values[index] < 0.0)
                {
                    result.values[index] = std::max(0.0, available - fixed) *
                                           -result.values[index] / (fractional ? fractional : 1.0);
                }
            }
            return result;
        }
        bool copySceneValueToBuffer(
            char *destination,
            u32 capacity,
            const char *text,
            u32 &length)
        {
            length = 0;
            while (text[length])
            {
                if (length >= capacity)
                    return false;
                destination[length] = text[length];
                ++length;
            }
            return true;
        }
        bool sceneGeometry(
            const JsonReader &json,
            u32 objectStart,
            u32 objectEnd,
            const SceneEntry &frame,
            double stageW,
            double stageH,
            SceneLayout &layout)
        {
            if (frame.resolved)
            {
                layout = {
                    frame.x, frame.y,
                    std::max(0.0, frame.width), std::max(0.0, frame.height),
                    frame.groupContext, true, false, false};
                return true;
            }
            SceneStyleValue width, height, left, top, transform, translate;
            findSceneStyleProperty<prop::PROP_WIDTH>(json, objectStart, objectEnd, width);
            findSceneStyleProperty<prop::PROP_HEIGHT>(json, objectStart, objectEnd, height);
            findSceneStyleProperty<prop::PROP_LEFT>(json, objectStart, objectEnd, left);
            findSceneStyleProperty<prop::PROP_TOP>(json, objectStart, objectEnd, top);
            findSceneStyleProperty<prop::PROP_TRANSFORM>(json, objectStart, objectEnd, transform);
            findSceneStyleProperty<prop::PROP_TRANSLATE>(json, objectStart, objectEnd, translate);
            MultiArray<char, MAX_TOKEN_TEXT> widthDefault;
            MultiArray<char, MAX_TOKEN_TEXT> heightDefault;
            std::snprintf(widthDefault.data(), widthDefault.size(), "%.17gpx", frame.width);
            std::snprintf(heightDefault.data(), heightDefault.size(), "%.17gpx", frame.height);
            const char *widthText = frame.resolved || !width.found ? widthDefault.data() : width.text.data();
            const char *heightText = frame.resolved || !height.found ? heightDefault.data() : height.text.data();
            const char *leftText = frame.resolved ? "0px" : (left.found ? left.text.data() : "0px");
            const char *topText = frame.resolved ? "0px" : (top.found ? top.text.data() : "0px");
            const char *transformText = transform.found ? transform.text.data() : "";
            const char *translateText = translate.found ? translate.text.data() : (frame.resolved ? "0 0" : "");
            EngineManager &manager = engineManager();
            char *input = manager.timelineSetupInputBuffer();
            if (!input)
                return false;
            MultiArray<u32, 6> lengths;
            char *cursor = input;
            const MultiArray<const char *, 6> values = {{
                widthText, heightText, leftText, topText, transformText, translateText}};
            for (u32 index = 0; index < 6; ++index)
            {
                if (!copySceneValueToBuffer(
                        cursor, manager.timelineSetupInputCapacity(),
                        values[index], lengths[index]))
                    return false;
                cursor += lengths[index];
            }
            double *result = manager.computeTimelineGeometry(
                TIMELINE_GEOMETRY_HAS_WIDTH | TIMELINE_GEOMETRY_HAS_HEIGHT |
                    TIMELINE_GEOMETRY_HAS_LEFT | TIMELINE_GEOMETRY_HAS_TOP,
                frame.width, frame.height, frame.width, frame.height,
                lengths[0], lengths[1], lengths[2], lengths[3], lengths[4], lengths[5]);
            if (!result)
                return false;
            const bool isText = json.stringFieldEquals(objectStart, "type", "text") ||
                                json.stringFieldEquals(objectStart, "type", "lyric");
            if (isText && frame.resolved && (!width.found || !height.found))
            {
                // Đối với text con trong container div không có explicit width/height:
                // Bảo toàn authoring origin frame.x, frame.y để translate() và HarfBuzz shaping
                // re-anchor chính xác 100% trong runtime C++ Engine mà không bị double translation.
                layout = {
                    frame.x, frame.y,
                    std::max(0.0, frame.width), std::max(0.0, frame.height),
                    frame.groupContext, true, false, false};
            }
            else
            {
                layout = {
                    frame.x + result[0], frame.y + result[1],
                    std::max(0.0, result[2]), std::max(0.0, result[3]),
                    frame.groupContext, true, false, false};
            }
            (void)stageW;
            (void)stageH;
            return true;
        }
        double sceneOpacity(
            const JsonReader &json,
            u32 objectStart,
            u32 objectEnd)
        {
            SceneStyleValue value{};
            if (!findSceneStyleProperty<prop::PROP_OPACITY>(
                    json, objectStart, objectEnd, value))
                return 1.0;
            if (!value.text[0])
                return 1.0;
            char *end = nullptr;
            double opacity = std::strtod(value.text.data(), &end);
            while (end && is_ascii_space(*end))
                ++end;
            if (!end || *end || !std::isfinite(opacity))
                return 1.0;
            if (value.numericSource && opacity > 1.0)
                opacity /= 100.0;
            return std::clamp(opacity, 0.0, 1.0);
        }
        double sceneZIndex(
            const JsonReader &json,
            u32 objectStart,
            u32 objectEnd)
        {
            SceneStyleValue value{};
            if (!findSceneStyleProperty<prop::PROP_Z_INDEX>(
                    json, objectStart, objectEnd, value))
                return 0.0;
            if (!value.text[0])
                return 0.0;
            char *end = nullptr;
            const double result = std::strtod(value.text.data(), &end);
            while (end && is_ascii_space(*end))
                ++end;
            return end && !*end && std::isfinite(result) ? result : 0.0;
        }
        bool collectGroupInheritance(
            const JsonReader &json,
            u32 groupContext,
            GroupInheritance &inheritance)
        {
            if (groupContext == INVALID_U32)
            {
                inheritance.hasGroups = false;
                return true;
            }
            MultiArray<u32, MAX_SCENE_DEPTH> chain;
            u32 chainLength = 0;
            u32 context = groupContext;
            while (context != INVALID_U32)
            {
                if (context >= sceneGroupContextCount || chainLength == MAX_SCENE_DEPTH)
                    return false;
                chain[chainLength++] = context;
                context = sceneGroupContexts[context].parent;
            }
            inheritance.hasGroups = chainLength != 0;
            constexpr u32 kAllInheritedMask =
                (INHERITED_STYLE_COUNT >= 32u) ? INVALID_U32 : ((1u << INHERITED_STYLE_COUNT) - 1u);
            for (u32 chainIndex = 0; chainIndex < chainLength; ++chainIndex)
            {
                const SceneGroupContext &entry = sceneGroupContexts[chain[chainIndex]];
                if (!inheritance.hasStart && json.field(
                                                 entry.sourceStart, "start", inheritance.startStart,
                                                 inheritance.startEnd))
                    inheritance.hasStart = true;
                if (!inheritance.hasEnd && json.field(
                                               entry.sourceStart, "end", inheritance.endStart,
                                               inheritance.endEnd))
                    inheritance.hasEnd = true;
                inheritance.zIndex += sceneZIndex(
                    json, entry.sourceStart, entry.sourceEnd);
                inheritance.opacity *= sceneOpacity(
                    json, entry.sourceStart, entry.sourceEnd);
                u32 missing = kAllInheritedMask & ~inheritance.foundMask;
                while (missing)
                {
                    const u32 styleIndex = BitOps<u32>::pop_lsb(missing);
                    bool owned = false;
                    if (findStyleField(
                            json, entry.sourceStart, entry.sourceEnd,
                            stockcut::prop::kInheritablePropIds[styleIndex],
                            inheritance.styles[styleIndex], owned))
                    {
                        const u32 bit = 1u << styleIndex;
                        inheritance.foundMask |= bit;
                        if (owned)
                            inheritance.sourceOwnedMask |= bit;
                    }
                }
            }
            return true;
        }
        struct GroupTimeWindow
        {
            bool hasStart = false;
            bool hasEnd = false;
            u32 startStart = 0;
            u32 startEnd = 0;
            u32 endStart = 0;
            u32 endEnd = 0;
        };
        bool collectGroupTimeWindow(
            const JsonReader &json,
            u32 groupContext,
            GroupTimeWindow &window)
        {
            u32 depth = 0;
            u32 context = groupContext;
            while (context != INVALID_U32)
            {
                if (context >= sceneGroupContextCount || depth == MAX_SCENE_DEPTH)
                    return false;
                const SceneGroupContext &entry = sceneGroupContexts[context];
                if (!window.hasStart && json.field(
                                            entry.sourceStart, "start", window.startStart,
                                            window.startEnd))
                    window.hasStart = true;
                if (!window.hasEnd && json.field(
                                          entry.sourceStart, "end", window.endStart,
                                          window.endEnd))
                    window.hasEnd = true;
                if (window.hasStart && window.hasEnd)
                    return true;
                context = entry.parent;
                ++depth;
            }
            return true;
        }
        DurationResult computeExpandedSceneDuration(
            const JsonReader &json)
        {
            DurationResult result{-1.0, 0, 0};
            u32 configStart = 0;
            u32 configEnd = 0;
            double configuredDuration = 0.0;
            if (json.field(0, "config", configStart, configEnd) &&
                json.isObject(configStart, configEnd) &&
                json.numberField(configStart, "durationMs", configuredDuration) &&
                configuredDuration > 0.0)
            {
                result.durationMs = configuredDuration;
                return result;
            }
            double maxItem = 0.0;
            double maxAudio = 0.0;
            for (u32 index = 0; index < sceneNodeCount; ++index)
            {
                const CompiledSceneNode &node = sceneNodes[index];
                MultiArray<char, MAX_TEMPLATE_TYPE> itemType;
                if (!readItemType(json, node.sourceStart, itemType.data(), static_cast<u32>(itemType.size())))
                    itemType[0] = '\0';
                const char *type = itemType.data();
                GroupTimeWindow inheritance;
                if (!collectGroupTimeWindow(
                        json, sceneLayouts[index].groupContext, inheritance))
                    continue;
                u32 startStart = 0;
                u32 startEnd = 0;
                bool hasStart = json.field(
                    node.sourceStart, "start", startStart, startEnd);
                if (!hasStart && inheritance.hasStart)
                {
                    startStart = inheritance.startStart;
                    startEnd = inheritance.startEnd;
                    hasStart = true;
                }
                u32 endStart = 0;
                u32 endEnd = 0;
                bool hasEnd = json.field(node.sourceStart, "end", endStart, endEnd);
                if (!hasEnd && inheritance.hasEnd)
                {
                    endStart = inheritance.endStart;
                    endEnd = inheritance.endEnd;
                    hasEnd = true;
                }
                const bool audio = (make_media_type(type) == MediaType::Audio);
                for (u32 endpoint = 0; endpoint < 2; ++endpoint)
                {
                    const bool exists = endpoint == 0 ? hasStart : hasEnd;
                    if (!exists)
                        continue;
                    const u32 valueStart = endpoint == 0 ? startStart : endStart;
                    const u32 valueEnd = endpoint == 0 ? startEnd : endEnd;
                    if (json.valueIsPercent(valueStart, valueEnd))
                    {
                        result.flags |= PROJECT_DURATION_HAS_PERCENT_TIMES;
                        continue;
                    }
                    double value = 0.0;
                    if (!json.valueNumber(valueStart, valueEnd, value) ||
                        value <= 0.0 || value >= PROJECT_TIME_SENTINEL_MS)
                        continue;
                    double &maximum = audio ? maxAudio : maxItem;
                    if (value > maximum)
                        maximum = value;
                }
                double itemStartMs = 0.0;
                if (hasStart && !json.timeValue(startStart, startEnd, itemStartMs))
                    itemStartMs = 0.0;
                u32 keyframesStart = 0;
                u32 keyframesEnd = 0;
                if (!json.field(node.sourceStart, "keyframes", keyframesStart,
                                keyframesEnd) ||
                    !json.isArray(keyframesStart, keyframesEnd))
                    continue;
                u32 keyCursor = keyframesStart + 1;
                while (keyCursor < keyframesEnd)
                {
                    u32 keyframeStart = 0;
                    u32 keyframeEnd = 0;
                    if (!json.nextArrayValue(
                            keyCursor, keyframesEnd, keyframeStart, keyframeEnd))
                        break;
                    if (!json.isObject(keyframeStart, keyframeEnd))
                        continue;
                    double keyframeMs = 0.0;
                    u32 valueStart = 0;
                    u32 valueEnd = 0;
                    if (json.field(keyframeStart, "ms", valueStart, valueEnd))
                    {
                        json.valueNumber(valueStart, valueEnd, keyframeMs);
                    }
                    else if (json.field(
                                 keyframeStart, "offset", valueStart, valueEnd) &&
                             !json.valueIsPercent(valueStart, valueEnd))
                    {
                        json.valueNumber(valueStart, valueEnd, keyframeMs);
                    }
                    const double candidate = itemStartMs + keyframeMs;
                    if (keyframeMs > 0.0 && candidate > maxItem &&
                        candidate < PROJECT_TIME_SENTINEL_MS)
                        maxItem = candidate;
                }
            }
            result.durationMs = maxItem > maxAudio ? maxItem : maxAudio;
            return result;
        }
        bool applyGroupInheritedStyles(
            const JsonReader &json,
            u32 itemStart,
            u32 itemEnd,
            FieldSpan *rootFields,
            u32 &rootCount,
            GroupInheritance &inheritance)
        {
            u32 pending = inheritance.foundMask;
            if (!pending)
                return true;
            InheritedStyleState ownTmp;
            bool ownOwned = false;
            while (pending)
            {
                const u32 styleIndex = BitOps<u32>::pop_lsb(pending);
                if (findStyleField(
                        json, itemStart, itemEnd, stockcut::prop::kInheritablePropIds[styleIndex], ownTmp, ownOwned))
                    continue;
                const u32 bit = 1u << styleIndex;
                if ((inheritance.sourceOwnedMask & bit) == 0u &&
                    !setMergedField(json, rootFields, rootCount, inheritance.styles[styleIndex].source))
                    return false;
                inheritance.appliedMask |= bit;
            }
            return true;
        }
        bool fieldExists(
            const JsonReader &json,
            u32 objectStart,
            const char *name)
        {
            u32 valueStart = 0;
            u32 valueEnd = 0;
            return json.field(objectStart, name, valueStart, valueEnd);
        }
        bool writeSignedInteger(i64 value)
        {
            MultiArray<char, 24> digits;
            u32 count = 0;
            const bool negative = value < 0;
            u64 magnitude = negative
                                ? static_cast<u64>(-(value + 1)) + 1
                                : static_cast<u64>(value);
            do
            {
                digits[count++] = static_cast<char>('0' + magnitude % 10);
                magnitude /= 10;
            } while (magnitude);
            if (negative && !writeChar('-'))
                return false;
            while (count)
                if (!writeChar(digits[--count]))
                    return false;
            return true;
        }
        bool writeJsonNumber(double value)
        {
            MultiArray<char, MAX_SHORT_TEXT> text;
            const int length = std::snprintf(text.data(), text.size(), "%.17g", value);
            if (length < 0 || static_cast<u32>(length) >= text.size())
                return false;
            for (int index = 0; index < length; ++index)
                if (!writeChar(text[index]))
                    return false;
            return true;
        }
        bool sceneLayoutKey(
            const JsonReader &json,
            const FieldSpan &field)
        {
            constexpr MultiArray<const char *, 8> names = {{
                "left", "x", "top", "y", "width", "height",
                "transform", "translate"}};
            for (const char *name : names)
                if (keyEquals(json, field, name))
                    return true;
            return false;
        }
        void stripPercentTranslations(const char *input, char *output, u32 capacity)
        {
            u32 source = 0;
            u32 written = 0;
            bool pendingSpace = false;
            while (input[source] && written + 1 < capacity)
            {
                if (cstr_equal_icase_n(input + source, "translate", 9))
                {
                    u32 nameEnd = source + 9;
                    if (input[nameEnd] == 'X' || input[nameEnd] == 'x' ||
                        input[nameEnd] == 'Y' || input[nameEnd] == 'y')
                        ++nameEnd;
                    else if (input[nameEnd] == '3' &&
                             (input[nameEnd + 1] == 'd' || input[nameEnd + 1] == 'D'))
                    {
                        nameEnd += 2;
                    }
                    while (input[nameEnd] == ' ' || input[nameEnd] == '\t')
                        ++nameEnd;
                    if (input[nameEnd] == '(')
                    {
                        u32 depth = 1;
                        u32 end = nameEnd + 1;
                        bool hasPercent = false;
                        while (input[end] && depth)
                        {
                            if (input[end] == '(')
                                ++depth;
                            else if (input[end] == ')')
                                --depth;
                            if (input[end] == '%')
                                hasPercent = true;
                            ++end;
                        }
                        if (!depth && hasPercent)
                        {
                            source = end;
                            pendingSpace = written != 0;
                            continue;
                        }
                    }
                }
                if (pendingSpace && !is_ascii_space(input[source]) && written &&
                    output[written - 1] != ' ' && written + 1 < capacity)
                {
                    output[written++] = ' ';
                }
                pendingSpace = false;
                output[written++] = input[source++];
            }
            while (written && is_ascii_space(output[written - 1]))
                --written;
            output[written] = '\0';
        }
        bool writePxField(const char *name, double value)
        {
            MultiArray<char, MAX_TOKEN_TEXT> text{};
            const int length = std::snprintf(text.data(), text.size(), "%.17gpx", value);
            return length >= 0 && static_cast<u32>(length) < text.size() &&
                   writeJsonString(name) && writeChar(':') && writeJsonString(text.data());
        }
        bool writeSurfaceId(const JsonReader &json, u32 itemStart)
        {
            MultiArray<char, MAX_TEMPLATE_NAME> base{};
            u32 idStart = 0;
            u32 idEnd = 0;
            if (json.field(itemStart, "id", idStart, idEnd))
            {
                if (!json.readStringValue(idStart, idEnd, base.data(), static_cast<u32>(base.size())))
                {
                    u32 used = 0;
                    if (!json.copySpan(idStart, idEnd, base.data(), static_cast<u32>(base.size()), used) ||
                        used >= base.size())
                        return false;
                    base[used] = '\0';
                }
            }
            else
            {
                copy_cstr_bounded(base.data(), base.size(), "group");
            }
            u32 baseLength = 0;
            while (base[baseLength])
                ++baseLength;
            constexpr const char *suffix = "_surface";
            constexpr u32 suffixLength = 8u;
            if (baseLength + suffixLength >= base.size())
                baseLength = static_cast<u32>(base.size()) - suffixLength - 1u;
            for (u32 index = 0; index < suffixLength; ++index)
                base[baseLength + index] = suffix[index];
            base[baseLength + suffixLength] = '\0';
            return writeJsonString(base.data());
        }
        bool writeSceneLayoutFields(
            const SceneLayout &layout,
            const SceneStyleValue &transform,
            const SceneStyleValue &translate,
            bool isText = false,
            bool explicitWidth = true,
            bool explicitHeight = true)
        {
            MultiArray<char, MAX_STYLE_VALUE_TEXT> strippedTransform;
            if (transform.found && !layout.surface)
                stripPercentTranslations(transform.text.data(), strippedTransform.data(),
                                         static_cast<u32>(strippedTransform.size()));
            else
                strippedTransform[0] = '\0';
            const char *translateText = translate.found && !layout.surface
                                            ? translate.text.data()
                                            : "";
            bool hasTranslatePercent = false;
            for (u32 index = 0; translateText[index]; ++index)
                if (translateText[index] == '%')
                    hasTranslatePercent = true;

            const bool preserveTranslate = isText && (!explicitWidth || !explicitHeight);
            const char *outTranslate = (hasTranslatePercent && !preserveTranslate) ? "" : translateText;

            bool ok = writePxField("left", layout.x) && writeChar(',') &&
                      writePxField("top", layout.y) && writeChar(',') &&
                      writePxField("width", layout.width) && writeChar(',') &&
                      writePxField("height", layout.height);
            return ok && writeChar(',') &&
                   writeJsonString("transform") && writeChar(':') &&
                   writeJsonString(transform.found && !layout.surface
                                       ? strippedTransform.data()
                                       : "") &&
                   writeChar(',') && writeJsonString("translate") && writeChar(':') &&
                   writeJsonString(outTranslate);
        }
        double parseIntegerPrefix(const JsonReader &json, const FieldSpan *field)
        {
            if (!field)
                return 0.0;
            MultiArray<char, MAX_TEMPLATE_NAME> value{};
            if (!json.readStringValue(
                    field->valueStart, field->valueEnd, value.data(), static_cast<u32>(value.size())))
            {
                double number = 0.0;
                return json.valueNumber(field->valueStart, field->valueEnd, number)
                           ? std::trunc(number)
                           : 0.0;
            }
            u32 position = 0;
            while (is_ascii_space(value[position]))
                ++position;
            double sign = 1.0;
            if (value[position] == '+' || value[position] == '-')
            {
                if (value[position] == '-')
                    sign = -1.0;
                ++position;
            }
            double result = 0.0;
            bool hasDigits = false;
            while (value[position] >= '0' && value[position] <= '9')
            {
                result = result * 10.0 + static_cast<double>(value[position] - '0');
                ++position;
                hasDigits = true;
            }
            return hasDigits ? sign * result : 0.0;
        }
        bool resolveTimelineField(
            const JsonReader &json,
            const FieldSpan *field,
            double projectDurationMs,
            double missingValue,
            double &resolved)
        {
            if (!field)
            {
                resolved = missingValue;
                return true;
            }
            const bool isPercent =
                json.valueIsPercent(field->valueStart, field->valueEnd);
            if (isPercent && !(projectDurationMs > 0.0))
                return false;
            if (!json.timeValue(field->valueStart, field->valueEnd, resolved))
                return false;
            if (isPercent)
            {
                resolved = resolved * projectDurationMs / FALLBACK_DURATION_1H_MS;
            }
            return true;
        }
        bool resolveItemOrder(
            const JsonReader &json,
            const FieldSpan *fields,
            u32 count,
            const char *type,
            double projectDurationMs,
            const FieldSpan *startOverride,
            const FieldSpan *endOverride,
            double &startMs,
            double &endMs,
            double &zIndex)
        {
            const FieldSpan *start = startOverride
                                         ? startOverride
                                         : findMergedField(json, fields, count, "start");
            if (!resolveTimelineField(json, start, projectDurationMs, 0.0, startMs))
                return false;
            const FieldSpan *end = endOverride
                                       ? endOverride
                                       : findMergedField(json, fields, count, "end");
            const MediaType mType = make_media_type(type);
            const double defaultEnd = (mType == MediaType::Audio) &&
                                              projectDurationMs > startMs
                                          ? projectDurationMs
                                          : startMs + DEFAULT_ITEM_DURATION_MS;
            if (!resolveTimelineField(
                    json, end, projectDurationMs, defaultEnd, endMs))
                return false;
            if (startMs < 0.0 || endMs < startMs)
                return false;
            const bool isEffect = cstr_equal(type, "effect");
            zIndex = (mType == MediaType::Audio)      ? 0.0
                   : (mType == MediaType::Background) ? DEFAULT_BG_Z_INDEX
                   : (isEffect)                       ? DEFAULT_EFFECT_Z_INDEX
                                                      : 0.0;
            if (mType == MediaType::Audio)
                return true;
            const FieldSpan *explicitZ = findMergedField(
                json, fields, count, "z-index");
            if (explicitZ)
                zIndex = parseIntegerPrefix(json, explicitZ);
            return true;
        }
        bool writeChar(char value)
        {
            if (outputLength == PROJECT_COMPILE_OUTPUT_CAPACITY)
                return false;
            outputJson[outputLength++] = value;
            return true;
        }
        bool writeJsonString(const char *value)
        {
            static constexpr const char *hex = "0123456789abcdef";
            if (!writeChar('"'))
                return false;
            for (u32 i = 0; value[i]; ++i)
            {
                const u8 ch = static_cast<u8>(value[i]);
                if (ch == '"' || ch == '\\')
                {
                    if (!writeChar('\\') || !writeChar(static_cast<char>(ch)))
                        return false;
                }
                else if (ch == '\b')
                {
                    if (!writeChar('\\') || !writeChar('b'))
                        return false;
                }
                else if (ch == '\f')
                {
                    if (!writeChar('\\') || !writeChar('f'))
                        return false;
                }
                else if (ch == '\n')
                {
                    if (!writeChar('\\') || !writeChar('n'))
                        return false;
                }
                else if (ch == '\r')
                {
                    if (!writeChar('\\') || !writeChar('r'))
                        return false;
                }
                else if (ch == '\t')
                {
                    if (!writeChar('\\') || !writeChar('t'))
                        return false;
                }
                else if (ch < 0x20)
                {
                    if (!writeChar('\\') || !writeChar('u') ||
                        !writeChar('0') || !writeChar('0') ||
                        !writeChar(hex[ch >> 4]) || !writeChar(hex[ch & 0x0f]))
                        return false;
                }
                else if (!writeChar(static_cast<char>(ch)))
                {
                    return false;
                }
            }
            return writeChar('"');
        }
        bool writeSpan(
            const JsonReader &json,
            u32 start,
            u32 end)
        {
            return json.copySpan(
                start, end, outputJson.data(), PROJECT_COMPILE_OUTPUT_CAPACITY, outputLength);
        }
        template <bool StripOpacity>
        bool writeObjectFields(
            const JsonReader &json,
            FieldSpan *fields,
            u32 count,
            u32 appliedInheritedMask,
            const SceneLayout &sceneLayout)
        {
            if (!writeChar('{'))
                return false;
            bool wroteField = false;
            for (u32 i = 0; i < count; ++i)
            {
                if constexpr (StripOpacity)
                {
                    if (keyEquals(json, fields[i], "opacity"))
                        continue;
                }
                if (sceneLayout.hasGeometry && sceneLayoutKey(json, fields[i]))
                    continue;
                bool stripStyle = false;
                u32 mask = appliedInheritedMask;
                while (mask)
                {
                    const u32 styleIndex = BitOps<u32>::pop_lsb(mask);
                    if (keyEquals(json, fields[i], scenePropertyByEnum[static_cast<u32>(stockcut::prop::kInheritablePropIds[styleIndex])].kebab))
                    {
                        stripStyle = true;
                        break;
                    }
                }
                if (stripStyle)
                    continue;
                if (wroteField && !writeChar(','))
                    return false;
                if (!writeSpan(json, fields[i].keyStart, fields[i].keyEnd) ||
                    !writeChar(':') ||
                    !writeSpan(json, fields[i].valueStart, fields[i].valueEnd))
                    return false;
                wroteField = true;
            }
            return writeChar('}');
        }
        bool writeNativeOverrides(
            const JsonReader &json,
            const GroupInheritance &inheritance,
            const SceneLayout &layout,
            const SceneStyleValue &transform,
            const SceneStyleValue &translate,
            double opacity,
            MediaType itemMediaType = MediaType::None,
            double startMs = 0.0)
        {
            if (!writeJsonString("_nativeOverrides") || !writeChar(':') ||
                !writeChar('{'))
                return false;
            bool wroteField = false;
            if (layout.hasGeometry)
            {
                if (!writeSceneLayoutFields(layout, transform, translate))
                    return false;
                wroteField = true;
            }
            if (inheritance.hasGroups)
            {
                if ((wroteField && !writeChar(',')) ||
                    !writeJsonString("opacity") || !writeChar(':') ||
                    !writeJsonNumber(opacity))
                    return false;
                wroteField = true;
            }
            u32 appMask = inheritance.appliedMask;
            while (appMask)
            {
                const u32 index = BitOps<u32>::pop_lsb(appMask);
                if ((wroteField && !writeChar(',')) ||
                    !writeJsonString(scenePropertyByEnum[static_cast<u32>(stockcut::prop::kInheritablePropIds[index])].kebab) || !writeChar(':'))
                    return false;
                if ((inheritance.sourceOwnedMask & (1u << index)) != 0u)
                {
                    if (!writeJsonString(inheritance.styles[index].sourceText.data()))
                        return false;
                }
                else if (!writeSpan(
                             json, inheritance.styles[index].source.valueStart,
                             inheritance.styles[index].source.valueEnd))
                {
                    return false;
                }
                wroteField = true;
            }
            const StyleRuleTargetOffset* matchedRule = (itemMediaType != MediaType::None) ? findMatchingStyleRule(itemMediaType, startMs) : nullptr;
            if (matchedRule != nullptr && s_sourceJsonPtr != nullptr && s_sourceJsonLen > 0)
            {
                const JsonReader srcJson(s_sourceJsonPtr, s_sourceJsonLen);
                u32 cursor = matchedRule->ruleStart + 1;
                u32 keyStart = 0, keyEnd = 0, valStart = 0, valEnd = 0;
                while (cursor < matchedRule->ruleEnd && srcJson.nextObjectField(cursor, matchedRule->ruleEnd, keyStart, keyEnd, valStart, valEnd))
                {
                    const u32 keyLen = (keyEnd > keyStart) ? (keyEnd - keyStart) : 0;
                    if (keyLen == 6 && std::memcmp(s_sourceJsonPtr + keyStart, "\"type\"", 6) == 0)
                    {
                        continue;
                    }
                    if (wroteField && !writeChar(',')) return false;
                    if (!writeSpan(srcJson, keyStart, keyEnd) || !writeChar(':') || !writeSpan(srcJson, valStart, valEnd))
                        return false;
                    wroteField = true;

                    // Emit camelCase alias if key contains hyphen (e.g., "stroke-width" -> "strokeWidth")
                    const u32 rawKeyLen = (keyLen > 2) ? (keyLen - 2) : 0;
                    if (rawKeyLen > 0 && rawKeyLen < 60)
                    {
                        const char* keyChars = s_sourceJsonPtr + keyStart + 1;
                        bool hasHyphen = false;
                        for (u32 k = 0; k < rawKeyLen; ++k)
                        {
                            if (keyChars[k] == '-') { hasHyphen = true; break; }
                        }
                        if (hasHyphen)
                        {
                            MultiArray<char, 64> camelBuf;
                            camelBuf[0] = '\0';
                            u32 cOut = 0;
                            bool cap = false;
                            for (u32 k = 0; k < rawKeyLen && cOut + 1 < camelBuf.size(); ++k)
                            {
                                if (keyChars[k] == '-')
                                {
                                    cap = true;
                                }
                                else
                                {
                                    if (cap && keyChars[k] >= 'a' && keyChars[k] <= 'z')
                                        camelBuf[cOut++] = static_cast<char>(keyChars[k] - 'a' + 'A');
                                    else
                                        camelBuf[cOut++] = keyChars[k];
                                    cap = false;
                                }
                            }
                            camelBuf[cOut] = '\0';
                            if (wroteField && !writeChar(',')) return false;
                            if (!writeJsonString(camelBuf.data()) || !writeChar(':') || !writeSpan(srcJson, valStart, valEnd))
                                return false;
                            wroteField = true;
                        }
                    }
                }
            }
            return writeChar('}');
        }
        bool writeMergedItem(
            const JsonReader &json,
            u32 itemStart,
            u32 itemEnd,
            const char *type,
            const char *effect,
            double projectDurationMs,
            GroupInheritance &groupInheritance,
            const SceneLayout &sceneLayout,
            double &startMs,
            double &endMs,
            double &zIndex,
            bool &invalidTimelineTime)
        {
            invalidTimelineTime = false;
            MultiArray<FieldSpan, MAX_OBJECT_FIELDS> rootFields;  // KHONG CO {}
            MultiArray<FieldSpan, MAX_OBJECT_FIELDS> cssFields;   // KHONG CO {}
            MultiArray<FieldSpan, MAX_OBJECT_FIELDS> styleFields; // KHONG CO {}
            MultiArray<FieldSpan, MAX_OBJECT_FIELDS> paramsFields;// KHONG CO {}
            u32 rootCount = 0;
            u32 cssCount = 0;
            u32 styleCount = 0;
            u32 paramsCount = 0;
            MultiArray<char, MAX_TEMPLATE_NAME> syntheticClass;   // KHONG CO {}
            syntheticClass[0] = '\0';
            bool hasSyntheticClass = false;
            const MediaType itemMediaType = make_media_type(type);
            const bool isEffect = cstr_equal(type, "effect");
            const i32 defaultIndex = findDefaultTemplate(type);
            const bool applyDefault = defaultIndex >= 0 &&
                                      (!isEffect ||
                                       !templates[defaultIndex].effect[0] ||
                                       cstr_equal(templates[defaultIndex].effect.data(), effect));
            if (applyDefault)
            {
                const TemplateSpec &entry = templates[defaultIndex];
                if (entry.hasSyntheticName)
                {
                    syntheticClass = entry.mapClass;
                    hasSyntheticClass = true;
                }
                if (!mergeTemplateHierarchy(json, defaultIndex, rootFields.data(), rootCount,
                                            cssFields.data(), cssCount, styleFields.data(), styleCount,
                                            paramsFields.data(), paramsCount))
                    return false;
            }
            if (isEffect && effect[0])
            {
                const i32 effectIndex = findEffectTemplate(effect);
                if (effectIndex >= 0)
                {
                    const TemplateSpec &entry = templates[effectIndex];
                    if (entry.hasSyntheticName)
                    {
                        syntheticClass = entry.mapClass;
                        hasSyntheticClass = true;
                    }
                    if (!mergeTemplateHierarchy(json, effectIndex, rootFields.data(), rootCount,
                                                cssFields.data(), cssCount, styleFields.data(), styleCount,
                                                paramsFields.data(), paramsCount))
                        return false;
                }
            }
            MultiArray<char, MAX_CLASS_LIST_TEXT> classNames; // KHONG CO {}
            if (!readFirstStringField(
                    json, itemStart, "class", "className",
                    classNames.data(), static_cast<u32>(classNames.size())))
                classNames[0] = '\0';
            u32 tokenCount = 0;
            u32 pos = 0;
            while (classNames[pos])
            {
                while (classNames[pos] && is_ascii_space(classNames[pos]))
                    ++pos;
                if (!classNames[pos])
                    break;
                const u32 begin = pos;
                while (classNames[pos] && !is_ascii_space(classNames[pos]))
                    ++pos;
                const u32 length = pos - begin;
                if (length >= MAX_TEMPLATE_NAME || tokenCount == MAX_CLASS_TOKENS)
                    return false;
                MultiArray<char, MAX_TEMPLATE_NAME> token; // KHONG CO {}
                for (u32 i = 0; i < length; ++i)
                    token[i] = classNames[begin + i];
                token[length] = '\0';
                ++tokenCount;
                i32 templateIndex = findTypedTemplate(type, token.data());
                if (templateIndex < 0)
                    templateIndex = findGeneralTemplate(token.data());
                if (templateIndex >= 0)
                {
                    const TemplateSpec &entry = templates[templateIndex];
                    if (entry.hasSyntheticName)
                    {
                        syntheticClass = entry.mapClass;
                        hasSyntheticClass = true;
                    }
                    if (!mergeTemplateHierarchy(json, templateIndex, rootFields.data(), rootCount,
                                                cssFields.data(), cssCount, styleFields.data(), styleCount,
                                                paramsFields.data(), paramsCount))
                        return false;
                }
            }
            // Hỗ trợ thuộc tính "template" trực tiếp trên item
            MultiArray<char, MAX_TEMPLATE_NAME> templateAttr{};
            if (readStringField(json, itemStart, "template", templateAttr.data(), static_cast<u32>(templateAttr.size())))
            {
                trim_cstr_inplace(templateAttr.data());
                if (templateAttr[0])
                {
                    i32 tIdx = findTypedTemplate(type, templateAttr.data());
                    if (tIdx < 0) tIdx = findGeneralTemplate(templateAttr.data());
                    if (tIdx >= 0)
                    {
                        if (!mergeTemplateHierarchy(json, tIdx, rootFields.data(), rootCount,
                                                    cssFields.data(), cssCount, styleFields.data(), styleCount,
                                                    paramsFields.data(), paramsCount))
                            return false;
                    }
                }
            }
            if (!mergeLayer(json, itemStart, itemEnd, rootFields.data(), rootCount,
                            cssFields.data(), cssCount, styleFields.data(), styleCount,
                            paramsFields.data(), paramsCount))
                return false;
            if (!applyGroupInheritedStyles(
                    json, itemStart, itemEnd, rootFields.data(), rootCount,
                    groupInheritance))
                return false;
            const bool hasItemStart = fieldExists(json, itemStart, "start");
            const bool hasItemEnd = fieldExists(json, itemStart, "end");
            const bool useInheritedStart = groupInheritance.hasStart && !hasItemStart;
            const bool useInheritedEnd = groupInheritance.hasEnd && !hasItemEnd;
            FieldSpan inheritedStart{};
            const FieldSpan *startOverride = nullptr;
            if (useInheritedStart)
            {
                inheritedStart.valueStart = groupInheritance.startStart;
                inheritedStart.valueEnd = groupInheritance.startEnd;
                startOverride = &inheritedStart;
            }
            FieldSpan inheritedEnd{};
            const FieldSpan *endOverride = nullptr;
            if (useInheritedEnd)
            {
                inheritedEnd.valueStart = groupInheritance.endStart;
                inheritedEnd.valueEnd = groupInheritance.endEnd;
                endOverride = &inheritedEnd;
            }
            if (!resolveItemOrder(
                    json, rootFields.data(), rootCount, type, projectDurationMs,
                    startOverride, endOverride, startMs, endMs, zIndex))
            {
                invalidTimelineTime = true;
                return false;
            }
            const StyleRuleTargetOffset* matchedStyleRule = (itemMediaType != MediaType::None)
                ? findMatchingStyleRule(itemMediaType, startMs)
                : nullptr;
            if (matchedStyleRule != nullptr && s_sourceJsonPtr != nullptr && s_sourceJsonLen > 0)
            {
                const JsonReader srcJson(s_sourceJsonPtr, s_sourceJsonLen);
                if (!mergeStyleRule(srcJson, matchedStyleRule->ruleStart, matchedStyleRule->ruleEnd,
                                    rootFields.data(), rootCount,
                                    cssFields.data(), cssCount,
                                    styleFields.data(), styleCount,
                                    paramsFields.data(), paramsCount))
                    return false;
            }
            if (make_media_type(type) == MediaType::Audio)
            {
                zIndex = 0.0;
            }
            else if (groupInheritance.hasGroups)
            {
                zIndex = groupInheritance.zIndex + sceneZIndex(json, itemStart, itemEnd);
            }
            if (!std::isfinite(zIndex))
                zIndex = 0.0;
            zIndex = std::clamp(std::trunc(zIndex), -MAX_SAFE_Z_INDEX, MAX_SAFE_Z_INDEX);
            double opacity = groupInheritance.opacity *
                             sceneOpacity(json, itemStart, itemEnd);
            if (!std::isfinite(opacity))
                opacity = 1.0;
            opacity = std::clamp(opacity, 0.0, 1.0);
            SceneStyleValue transform{};
            SceneStyleValue translate{};
            if (sceneLayout.hasGeometry)
            {
                findSceneStyleProperty<prop::PROP_TRANSFORM>(
                    json, itemStart, itemEnd, transform);
                findSceneStyleProperty<prop::PROP_TRANSLATE>(
                    json, itemStart, itemEnd, translate);
            }
            bool hasClassField = false;
            bool hasIdField = false;
            for (u32 i = 0; i < rootCount; ++i)
                if (keyEquals(json, rootFields[i], "class"))
                    hasClassField = true;
                else if (keyEquals(json, rootFields[i], "id"))
                    hasIdField = true;
            if (!writeChar('{'))
                return false;
            bool wroteField = false;
            if (hasSyntheticClass && !hasClassField)
            {
                if (!writeJsonString("class") || !writeChar(':') ||
                    !writeJsonString(syntheticClass.data()))
                    return false;
                wroteField = true;
            }
            if (sceneLayout.surface && !hasIdField)
            {
                if ((wroteField && !writeChar(',')) ||
                    !writeJsonString("id") || !writeChar(':') ||
                    !writeSurfaceId(json, itemStart))
                    return false;
                wroteField = true;
            }
            if ((wroteField && !writeChar(',')) ||
                !writeJsonString("start") || !writeChar(':') ||
                !writeJsonNumber(startMs) || !writeChar(',') ||
                !writeJsonString("end") || !writeChar(':') ||
                !writeJsonNumber(endMs) || !writeChar(',') ||
                !writeJsonString("durationMs") || !writeChar(':') ||
                !writeJsonNumber(endMs - startMs))
                return false;
            wroteField = true;
            if ((wroteField && !writeChar(',')) || !writeJsonString("z-index") ||
                !writeChar(':') || !writeSignedInteger(static_cast<i64>(zIndex)))
                return false;
            wroteField = true;
            if (sceneLayout.hasGeometry)
            {
                const bool isText = (make_media_type(type) == MediaType::Text || make_media_type(type) == MediaType::Lyric);
                SceneStyleValue explicitW{}, explicitH{};
                findSceneStyleProperty<prop::PROP_WIDTH>(json, itemStart, itemEnd, explicitW);
                findSceneStyleProperty<prop::PROP_HEIGHT>(json, itemStart, itemEnd, explicitH);
                if (!writeChar(',') ||
                    !writeSceneLayoutFields(sceneLayout, transform, translate,
                                           isText, explicitW.found, explicitH.found))
                    return false;
            }
            for (u32 i = 0; i < rootCount; ++i)
            {
                if (sceneLayout.surface &&
                    (keyEquals(json, rootFields[i], "children") ||
                     keyEquals(json, rootFields[i], "childs")))
                    continue;
                if (keyEquals(json, rootFields[i], "z-index") ||
                    keyEquals(json, rootFields[i], "start") ||
                    keyEquals(json, rootFields[i], "end") ||
                    keyEquals(json, rootFields[i], "durationMs") ||
                    keyEquals(json, rootFields[i], "_nativeSlot") ||
                    keyEquals(json, rootFields[i], "_topLevelIndex") ||
                    (groupInheritance.hasGroups &&
                     keyEquals(json, rootFields[i], "opacity")) ||
                    (sceneLayout.hasGeometry && sceneLayoutKey(json, rootFields[i])))
                    continue;
                if (wroteField && !writeChar(','))
                    return false;
                if (!writeSpan(json, rootFields[i].keyStart, rootFields[i].keyEnd) ||
                    !writeChar(':'))
                    return false;
                wroteField = true;
                if (keyEquals(json, rootFields[i], "type") &&
                    sceneLayout.forceBackground)
                {
                    if (!writeJsonString("background"))
                        return false;
                }
                else if (keyEquals(json, rootFields[i], "id") &&
                         sceneLayout.surface)
                {
                    if (!writeSurfaceId(json, itemStart))
                        return false;
                }
                else if (rootFields[i].kind != 1)
                {
                    if (!writeSpan(
                            json, rootFields[i].valueStart, rootFields[i].valueEnd))
                        return false;
                }
                else
                {
                    FieldSpan *fields = nullptr;
                    u32 fieldCount = 0;
                    if (keyEquals(json, rootFields[i], "css"))
                    {
                        fields = cssFields.data();
                        fieldCount = cssCount;
                    }
                    else if (keyEquals(json, rootFields[i], "style"))
                    {
                        fields = styleFields.data();
                        fieldCount = styleCount;
                    }
                    else
                    {
                        fields = paramsFields.data();
                        fieldCount = paramsCount;
                    }
                    const bool isStyleMap =
                        keyEquals(json, rootFields[i], "css") ||
                        keyEquals(json, rootFields[i], "style");
                    const SceneLayout emptyLayout{};
                    const u32 appliedMask = isStyleMap ? groupInheritance.appliedMask : 0u;
                    const SceneLayout &targetLayout =
                        isStyleMap ? sceneLayout : emptyLayout;
                    const bool ok = (isStyleMap && groupInheritance.hasGroups)
                                        ? writeObjectFields<true>(json, fields, fieldCount, appliedMask, targetLayout)
                                        : writeObjectFields<false>(json, fields, fieldCount, appliedMask, targetLayout);
                    if (!ok)
                        return false;
                }
            }
            if (wroteField && !writeChar(','))
                return false;
            if (!writeNativeOverrides(
                    json, groupInheritance, sceneLayout,
                    transform, translate, opacity,
                    itemMediaType, startMs))
                return false;
            return writeChar('}');
        }
        struct OutputItemSpan
        {
            u32 start;
            u32 end;
            double timelineStart;
            double zIndex;
            u32 topLevelIndex;
            bool isGroupChild;
        };
        void reverseOutputRange(u32 start, u32 end)
        {
            while (start < end && start < --end)
            {
                const char value = outputJson[start];
                outputJson[start] = outputJson[end];
                outputJson[end] = value;
                ++start;
            }
        }
        bool outputItemBefore(const OutputItemSpan &left, const OutputItemSpan &right)
        {
            if (left.timelineStart != right.timelineStart)
                return left.timelineStart < right.timelineStart;
            return left.zIndex < right.zIndex;
        }
        void swapAdjacentOutputItems(
            OutputItemSpan *items,
            u32 leftIndex)
        {
            const OutputItemSpan left = items[leftIndex];
            const OutputItemSpan right = items[leftIndex + 1];
            const u32 middle = left.end;
            const u32 rightEnd = right.end;
            reverseOutputRange(left.start, middle);
            reverseOutputRange(middle, rightEnd);
            reverseOutputRange(left.start, rightEnd);
            const u32 rightLength = right.end - right.start;
            items[leftIndex] = {
                left.start, left.start + rightLength, right.timelineStart, right.zIndex, right.topLevelIndex, right.isGroupChild};
            items[leftIndex + 1] = {
                left.start + rightLength, rightEnd, left.timelineStart, left.zIndex, left.topLevelIndex, left.isGroupChild};
        }
        MultiArray<u32, PROJECT_COMPILE_MAX_SCENE_LEAVES> s_compiledItemStarts{};
        MultiArray<u32, PROJECT_COMPILE_MAX_SCENE_LEAVES> s_compiledItemEnds{};
        u32 s_compiledItemCount = 0u;
        bool emitCompiledItems(
            const JsonReader &json,
            double projectDurationMs,
            u32 &status)
        {
            MultiArray<OutputItemSpan, PROJECT_COMPILE_MAX_SCENE_LEAVES> outputItems;
            // Bo dem thu tu noi bo per-groupContext de tranh xao tron z-order khi recompile.
            MultiArray<u32, MAX_SCENE_CONTEXTS> groupSeqCount{};
            outputLength = 0;
            if (!writeChar('['))
            {
                status = PROJECT_COMPILE_OUTPUT_CAPACITY_EXCEEDED;
                return false;
            }
            for (u32 i = 0; i < sceneNodeCount; ++i)
            {
                MultiArray<char, MAX_TEMPLATE_TYPE> type{};
                MultiArray<char, MAX_TEMPLATE_EFFECT> effect{};
                double timelineStart = 0.0;
                double timelineEnd = 0.0;
                double zIndex = 0.0;
                readItemType(json, sceneNodes[i].sourceStart, type.data(), static_cast<u32>(type.size()));
                readItemEffect(json, sceneNodes[i].sourceStart, effect.data(), static_cast<u32>(effect.size()));
                const SceneLayout &sceneLayout = sceneLayouts[i];
                if (sceneLayout.forceBackground)
                {
                    copy_cstr_bounded(type.data(), type.size(), "background");
                }
                GroupInheritance groupInheritance;
                if (!collectGroupInheritance(
                        json, sceneLayout.groupContext, groupInheritance))
                {
                    status = PROJECT_COMPILE_SCENE_CAPACITY_EXCEEDED;
                    return false;
                }
                if (sceneLayout.hasGeometry)
                    groupInheritance.hasGroups = true;
                const u32 itemStart = outputLength;
                bool invalidTimelineTime = false;
                if (!writeMergedItem(
                        json, sceneNodes[i].sourceStart, sceneNodes[i].sourceEnd,
                        type.data(), effect.data(), projectDurationMs, groupInheritance,
                        sceneLayout, timelineStart, timelineEnd, zIndex,
                        invalidTimelineTime))
                {
                    status = invalidTimelineTime
                                 ? PROJECT_COMPILE_INVALID_TIMELINE_TIME
                             : outputLength == PROJECT_COMPILE_OUTPUT_CAPACITY
                                 ? PROJECT_COMPILE_OUTPUT_CAPACITY_EXCEEDED
                                 : PROJECT_COMPILE_FIELD_CAPACITY_EXCEEDED;
                    return false;
                }
                if (!writeChar(','))
                {
                    status = PROJECT_COMPILE_OUTPUT_CAPACITY_EXCEEDED;
                    return false;
                }
                // Sub-offset per-groupContext: surface(seq=0) < child[0](seq=1) < ...
                // Dung GROUP_SUB_Z_STEP per step -- outputItemBefore() so sanh double nen
                // thu tu con on dinh, khong phu thuoc slot number (tranh xao tron khi recompile).
                {
                    const u32 ctx = sceneLayout.groupContext;
                    if (ctx != INVALID_U32 && ctx < MAX_SCENE_CONTEXTS)
                    {
                        zIndex += static_cast<double>(groupSeqCount[ctx]) * GROUP_SUB_Z_STEP;
                        ++groupSeqCount[ctx];
                    }
                }
                outputItems[i] = {itemStart, outputLength, timelineStart, zIndex, sceneNodes[i].topLevelIndex, sceneNodes[i].depth > 0};
            }
            if (!writeChar(']'))
            {
                status = PROJECT_COMPILE_OUTPUT_CAPACITY_EXCEEDED;
                return false;
            }
            for (u32 index = 1; index < sceneNodeCount; ++index)
            {
                u32 insertion = index;
                while (insertion > 0 && outputItemBefore(
                                            outputItems[insertion], outputItems[insertion - 1]))
                {
                    swapAdjacentOutputItems(outputItems.data(), insertion - 1);
                    --insertion;
                }
            }
            // The browser adapter needs a stable link from a compiled item to
            // its native timeline slot. Emit it only after ordering so the
            // value matches the slot used by native setup and evaluation.
            for (u32 reverseIndex = sceneNodeCount; reverseIndex > 0; --reverseIndex)
            {
                const u32 itemIndex = reverseIndex - 1;
                const u32 insertAt = outputItems[itemIndex].end - 2;
                MultiArray<char, MAX_TOKEN_TEXT> slotField{};
                const int slotFieldLength = std::snprintf(
                    slotField.data(), slotField.size(), ",\"_nativeSlot\":%u,\"_topLevelIndex\":%u,\"_isGroupChild\":%u", itemIndex, outputItems[itemIndex].topLevelIndex, outputItems[itemIndex].isGroupChild ? 1u : 0u);
                if (slotFieldLength <= 0 ||
                    static_cast<u32>(slotFieldLength) >= slotField.size() ||
                    outputLength + static_cast<u32>(slotFieldLength) >=
                        PROJECT_COMPILE_OUTPUT_CAPACITY)
                {
                    status = PROJECT_COMPILE_OUTPUT_CAPACITY_EXCEEDED;
                    return false;
                }
                std::memmove(
                    outputJson.data() + insertAt + slotFieldLength,
                    outputJson.data() + insertAt,
                    outputLength - insertAt);
                std::memcpy(outputJson.data() + insertAt, slotField.data(),
                            static_cast<u32>(slotFieldLength));
                outputLength += static_cast<u32>(slotFieldLength);
                outputItems[itemIndex].end += static_cast<u32>(slotFieldLength);
            }
            if (sceneNodeCount)
            {
                outputJson[outputLength - 2] = ']';
                --outputLength;
            }
            // Record exact O(1) JSON object spans for Incremental Merkle-AST (0 nextArrayValue char scan!)
            s_compiledItemCount = sceneNodeCount;
            u32 spanPos = 1u;
            for (u32 i = 0u; i < sceneNodeCount; ++i)
            {
                const u32 itemLenWithComma = outputItems[i].end - outputItems[i].start;
                s_compiledItemStarts[i] = spanPos;
                s_compiledItemEnds[i]   = spanPos + itemLenWithComma - 1u;
                spanPos += itemLenWithComma;
            }
            return true;
        }
        bool countArrayValues(
            const JsonReader &json,
            u32 start,
            u32 end,
            u32 &count)
        {
            if (!json.isArray(start, end))
                return false;
            u32 cursor = start + 1;
            count = 0;
            while (cursor < end)
            {
                u32 valueStart = 0;
                u32 valueEnd = 0;
                if (!json.nextArrayValue(cursor, end, valueStart, valueEnd))
                    break;
                ++count;
            }
            return true;
        }
        template <bool LogicalLeaf>
        bool appendSceneNode(
            const SceneEntry &entry,
            const SceneLayout &layout,
            u32 &leafCount)
        {
            if (sceneNodeCount == PROJECT_COMPILE_MAX_SCENE_LEAVES)
                return false;
            sceneNodes[sceneNodeCount] = {
                entry.start, entry.end, entry.topLevelIndex, sceneNodeCount,
                static_cast<u16>(entry.depth), 0};
            sceneLayouts[sceneNodeCount] = layout;
            ++sceneNodeCount;
            if constexpr (LogicalLeaf)
                ++leafCount;
            return true;
        }
        bool expandSceneGroups(
            const JsonReader &json,
            u32 timelineStart,
            u32 timelineEnd,
            double stageW,
            double stageH,
            u32 &leafCount)
        {
            MultiArray<SceneEntry, PROJECT_COMPILE_MAX_SCENE_LEAVES + MAX_SCENE_DEPTH> stack;
            MultiArray<SceneEntry, PROJECT_COMPILE_MAX_SCENE_LEAVES> roots;
            u32 stackSize = 0;
            u32 rootCount = 0;
            sceneNodeCount = 0;
            sceneLeafCount = 0;
            sceneGroupContextCount = 0;
            u32 cursor = timelineStart + 1;
            u32 rawIndex = 0;
            while (cursor < timelineEnd)
            {
                u32 itemStart = 0;
                u32 itemEnd = 0;
                if (!json.nextArrayValue(cursor, timelineEnd, itemStart, itemEnd))
                    break;
                const u32 currentRawIndex = rawIndex++;
                if (json.stringFieldEquals(itemStart, "type", "style"))
                {
                    continue;
                }
                if (rootCount == PROJECT_COMPILE_MAX_SCENE_LEAVES)
                    return false;
                const u32 topLevelIndex = currentRawIndex;
                roots[rootCount++] = {
                    itemStart, itemEnd, topLevelIndex, 0, INVALID_U32,
                    0.0, 0.0, stageW, stageH, false};
            }
            sceneRootCount = rootCount;
            while (rootCount)
                stack[stackSize++] = roots[--rootCount];
            leafCount = 0;
            while (stackSize)
            {
                const SceneEntry entry = stack[--stackSize];
                if (entry.depth > MAX_SCENE_DEPTH)
                    return false;
                if (!json.isObject(entry.start, entry.end))
                    continue;
                const bool isGroup = json.stringFieldEquals(entry.start, "type", "div");
                if (entry.depth == 0 && !isGroup)
                {
                    SceneLayout layout{};
                    layout.groupContext = entry.groupContext;
                    if (!appendSceneNode<true>(entry, layout, leafCount))
                        return false;
                    continue;
                }
                u32 childrenStart = 0;
                u32 childrenEnd = 0;
                // Chap nhan "childs" va "children" - bo qua "childrent" va moi bien the sai.
                const bool hasChildren =
                    (json.field(entry.start, "childs", childrenStart, childrenEnd) ||
                     json.field(entry.start, "children", childrenStart, childrenEnd)) &&
                    json.isArray(childrenStart, childrenEnd);
                SceneLayout box{};
                if (!sceneGeometry(
                        json, entry.start, entry.end, entry, stageW, stageH, box))
                    return false;
                if (!isGroup || !hasChildren)
                {
                    box.forceBackground = isGroup;
                    if (!appendSceneNode<true>(entry, box, leafCount))
                        return false;
                    continue;
                }
                if (sceneGroupContextCount == MAX_SCENE_CONTEXTS)
                    return false;
                const u32 groupContext = sceneGroupContextCount++;
                sceneGroupContexts[groupContext] = {
                    entry.start, entry.end, entry.groupContext};
                SceneStyleValue background, backgroundColor, border, borderWidth, borderRadius;
                findSceneStyleProperty<prop::PROP_BACKGROUND>(json, entry.start, entry.end, background);
                findSceneStyleProperty<prop::PROP_BACKGROUND_COLOR>(json, entry.start, entry.end, backgroundColor);
                findSceneStyleProperty<prop::PROP_BORDER>(json, entry.start, entry.end, border);
                findSceneStyleProperty<prop::PROP_BORDER_WIDTH>(json, entry.start, entry.end, borderWidth);
                findSceneStyleProperty<prop::PROP_BORDER_RADIUS>(json, entry.start, entry.end, borderRadius);
                const bool hasPaint = (background.found && background.text[0]) ||
                                      (backgroundColor.found && backgroundColor.text[0]) ||
                                      (border.found && border.text[0]) ||
                                      (borderWidth.found && borderWidth.text[0]) ||
                                      (borderRadius.found && borderRadius.text[0]);
                if (hasPaint)
                {
                    SceneLayout surfaceLayout = box;
                    surfaceLayout.groupContext = entry.groupContext;
                    surfaceLayout.hasGeometry = true;
                    surfaceLayout.forceBackground = true;
                    surfaceLayout.surface = true;
                    if (!appendSceneNode<false>(entry, surfaceLayout, leafCount))
                        return false;
                }
                MultiArray<double, 4> padding;
                SceneStyleValue paddingValue;
                findSceneStyleProperty<prop::PROP_PADDING>(json, entry.start, entry.end, paddingValue);
                sceneInsets(paddingValue, box.width, box.height, padding.data());
                const double contentX = box.x + padding[3];
                const double contentY = box.y + padding[0];
                const double contentW = std::max(
                    0.0, box.width - padding[1] - padding[3]);
                const double contentH = std::max(
                    0.0, box.height - padding[0] - padding[2]);
                SceneStyleValue displayValue, flexDirectionValue;
                findSceneStyleProperty<prop::PROP_DISPLAY>(json, entry.start, entry.end, displayValue);
                findSceneStyleProperty<prop::PROP_FLEX_DIRECTION>(json, entry.start, entry.end, flexDirectionValue);
                MultiArray<char, MAX_TOKEN_TEXT> display;
                copy_cstr_bounded(display.data(), display.size(), displayValue.text.data());
                lower_ascii_inplace(display.data());
                const bool flex = cstr_equal(display.data(), "flex") || cstr_equal(display.data(), "inline-flex");
                const bool grid = cstr_equal(display.data(), "grid") || cstr_equal(display.data(), "inline-grid");
                const bool column = flex &&
                                    flexDirectionValue.found &&
                                    std::strncmp(flexDirectionValue.text.data(), "column", 6) == 0;
                SceneStyleValue gapValue, columnGapValue, rowGapValue;
                findSceneStyleProperty<prop::PROP_GAP>(json, entry.start, entry.end, gapValue);
                findSceneStyleProperty<prop::PROP_COLUMN_GAP>(json, entry.start, entry.end, columnGapValue);
                findSceneStyleProperty<prop::PROP_ROW_GAP>(json, entry.start, entry.end, rowGapValue);
                if (!columnGapValue.found || !columnGapValue.text[0])
                    columnGapValue = gapValue;
                if (!rowGapValue.found || !rowGapValue.text[0])
                    rowGapValue = gapValue;
                const double gapX = std::max(
                    0.0, sceneLength(columnGapValue, contentW, 0.0));
                const double gapY = std::max(
                    0.0, sceneLength(rowGapValue, contentH, 0.0));
                u32 childCount = 0;
                MultiArray<SceneEntry, PROJECT_COMPILE_MAX_SCENE_LEAVES> children;
                u32 childCursor = childrenStart + 1;
                while (childCursor < childrenEnd)
                {
                    u32 childStart = 0;
                    u32 childEnd = 0;
                    if (!json.nextArrayValue(
                            childCursor, childrenEnd, childStart, childEnd))
                        break;
                    if (json.stringFieldEquals(childStart, "type", "style"))
                        continue;
                    if (childCount == PROJECT_COMPILE_MAX_SCENE_LEAVES)
                        return false;
                    children[childCount++] = {
                        childStart, childEnd, entry.topLevelIndex, entry.depth + 1,
                        groupContext, contentX, contentY, contentW, contentH, true};
                }
                const double mainSize = column ? contentH : contentW;
                const double mainGap = column ? gapY : gapX;
                double explicitTotal = 0.0;
                for (u32 childIndex = 0; childIndex < childCount; ++childIndex)
                {
                    const SceneEntry &child = children[childIndex];
                    if (!json.isObject(child.start, child.end))
                        continue;
                    SceneStyleValue childSize;
                    if (column)
                        findSceneStyleProperty<prop::PROP_HEIGHT>(json, child.start, child.end, childSize);
                    else
                        findSceneStyleProperty<prop::PROP_WIDTH>(json, child.start, child.end, childSize);
                    if (childSize.found)
                        explicitTotal += sceneLength(childSize, mainSize, 0.0);
                }
                const double autoSize = std::max(0.0,
                                                 (mainSize - explicitTotal -
                                                  static_cast<double>(childCount ? childCount - 1 : 0) * mainGap) /
                                                     std::max(1u, childCount));
                GridTracks columns;
                GridTracks rows;
                if (grid)
                {
                    SceneStyleValue columnsValue, rowsValue;
                    findSceneStyleProperty<prop::PROP_GRID_TEMPLATE_COLS>(json, entry.start, entry.end, columnsValue);
                    findSceneStyleProperty<prop::PROP_GRID_TEMPLATE_ROWS>(json, entry.start, entry.end, rowsValue);
                    const u32 defaultColumns = static_cast<u32>(std::ceil(
                        std::sqrt(static_cast<double>(childCount))));
                    columns = sceneGridTracks(columnsValue, contentW,
                                              std::max(1u, defaultColumns));
                    rows = sceneGridTracks(rowsValue, contentH,
                                           std::max(1u, static_cast<u32>(std::ceil(
                                                            static_cast<double>(childCount) / columns.count))));
                }
                double blockCursor = column ? contentY : contentX;
                const double blockRowHeight = contentH / std::max(1u, childCount);
                for (u32 childIndex = 0; childIndex < childCount; ++childIndex)
                {
                    SceneEntry &child = children[childIndex];
                    if (!json.isObject(child.start, child.end))
                        continue;
                    SceneStyleValue marginValue, childWidthValue, childHeightValue;
                    SceneStyleValue childLeftValue, childTopValue;
                    findSceneStyleProperty<prop::PROP_MARGIN>(json, child.start, child.end, marginValue);
                    findSceneStyleProperty<prop::PROP_WIDTH>(json, child.start, child.end, childWidthValue);
                    findSceneStyleProperty<prop::PROP_HEIGHT>(json, child.start, child.end, childHeightValue);
                    findSceneStyleProperty<prop::PROP_LEFT>(json, child.start, child.end, childLeftValue);
                    findSceneStyleProperty<prop::PROP_TOP>(json, child.start, child.end, childTopValue);
                    MultiArray<double, 4> margin;
                    sceneInsets(marginValue, contentW, contentH, margin.data());
                    const bool isChildText = json.stringFieldEquals(child.start, "type", "text") ||
                                             json.stringFieldEquals(child.start, "type", "lyric");
                    double textEstimatedW = 0.0, textEstimatedH = 0.0;
                    if (isChildText && (!childWidthValue.found || !childHeightValue.found))
                    {
                        estimateTextDimensions(json, child.start, child.end, stageW, textEstimatedW, textEstimatedH);
                    }
                    child.x = contentX + margin[3];
                    child.y = contentY + margin[0];
                    child.width = childWidthValue.found
                                      ? std::max(0.0, sceneLength(childWidthValue, contentW, contentW - margin[3] - margin[1]) - margin[3] - margin[1])
                                      : (isChildText ? textEstimatedW : std::max(0.0, contentW - margin[3] - margin[1]));
                    child.height = childHeightValue.found
                                       ? std::max(0.0, sceneLength(childHeightValue, contentH, contentH - margin[0] - margin[2]) - margin[0] - margin[2])
                                       : (isChildText ? textEstimatedH : std::max(0.0, contentH - margin[0] - margin[2]));
                    if (flex)
                    {
                        const SceneStyleValue &explicitMain =
                            column ? childHeightValue : childWidthValue;
                        const double fallbackMain = isChildText ? (column ? textEstimatedH : textEstimatedW) : autoSize;
                        const double itemMain = std::max(
                            0.0, sceneLength(explicitMain, mainSize, fallbackMain));
                        if (column)
                        {
                            child.y = blockCursor + margin[0];
                            child.height = std::max(
                                0.0, itemMain - margin[0] - margin[2]);
                        }
                        else
                        {
                            child.x = blockCursor + margin[3];
                            child.width = std::max(
                                0.0, itemMain - margin[3] - margin[1]);
                        }
                        blockCursor += itemMain + mainGap;
                    }
                    else if (grid)
                    {
                        const u32 columnIndex = childIndex % columns.count;
                        const u32 rowIndex = childIndex / columns.count;
                        double columnOffset = 0.0;
                        double rowOffset = 0.0;
                        for (u32 index = 0; index < columnIndex; ++index)
                            columnOffset += columns.values[index];
                        for (u32 index = 0; index < rowIndex && index < rows.count; ++index)
                            rowOffset += rows.values[index];
                        child.x = contentX + columnOffset + columnIndex * gapX + margin[3];
                        child.y = contentY + rowOffset + rowIndex * gapY + margin[0];
                        const double cellW = columns.values[columnIndex];
                        const double cellH = rowIndex < rows.count
                                                 ? rows.values[rowIndex]
                                                 : contentH;
                        child.width = std::max(0.0,
                                               sceneLength(childWidthValue, cellW, cellW) - margin[3] - margin[1]);
                        child.height = std::max(0.0,
                                                sceneLength(childHeightValue, cellH, cellH) - margin[0] - margin[2]);
                    }
                    else
                    {
                        const double childW = childWidthValue.found
                                                  ? std::max(0.0, sceneLength(childWidthValue, contentW, contentW - margin[3] - margin[1]))
                                                  : (isChildText ? textEstimatedW : std::max(0.0, contentW - margin[3] - margin[1]));
                        const double childH = childHeightValue.found
                                                  ? std::max(0.0, sceneLength(childHeightValue, contentH, contentH - margin[0] - margin[2]))
                                                  : (isChildText ? textEstimatedH : std::max(0.0, contentH - margin[0] - margin[2]));
                        child.width = childW;
                        child.height = childH;

                        // Chính sách hệ thống: Mặc định left = 50%, top = 50% của container cha
                        const double posX = childLeftValue.found
                                                ? sceneLength(childLeftValue, contentW, 0.5 * contentW)
                                                : (0.5 * contentW);
                        const double posY = childTopValue.found
                                                ? sceneLength(childTopValue, contentH, 0.5 * contentH)
                                                : (0.5 * contentH);

                        child.x = contentX + posX + margin[3];
                        child.y = contentY + posY + margin[0];
                    }
                }
                while (childCount)
                {
                    if (stackSize ==
                        PROJECT_COMPILE_MAX_SCENE_LEAVES + MAX_SCENE_DEPTH)
                        return false;
                    stack[stackSize++] = children[--childCount];
                }
                if (sceneNodeCount + stackSize > PROJECT_COMPILE_MAX_SCENE_LEAVES)
                    return false;
            }
            sceneLeafCount = leafCount;
            return true;
        }
        bool parseCssNumber(const char *first, const char *last, double &result)
        {
            while (first < last && is_ascii_space(*first))
                ++first;
            while (last > first && is_ascii_space(last[-1]))
                --last;
            if (first == last)
                return false;
            MultiArray<char, MAX_TEMPLATE_NAME> text{};
            const u32 length = static_cast<u32>(last - first);
            if (length >= text.size())
                return false;
            for (u32 i = 0; i < length; ++i)
                text[i] = first[i];
            text[length] = '\0';
            char *end = nullptr;
            result = std::strtod(text.data(), &end);
            while (end && *end && is_ascii_space(*end))
                ++end;
            return end && end != text.data() && !*end && std::isfinite(result);
        }
        void parseInlineVolumeDeclaration(
            const char *text,
            u32 start,
            u32 end,
            bool &found,
            bool &valid,
            double &value)
        {
            u32 colon = start;
            while (colon < end && text[colon] != ':')
                ++colon;
            if (colon == end)
                return;
            u32 keyStart = start;
            u32 keyEnd = colon;
            while (keyStart < keyEnd && is_ascii_space(text[keyStart]))
                ++keyStart;
            while (keyEnd > keyStart && is_ascii_space(text[keyEnd - 1]))
                --keyEnd;
            constexpr const char *property = "--volume";
            constexpr u32 propertyLength = 8u;
            if (keyEnd - keyStart != propertyLength)
                return;
            for (u32 i = 0; i < propertyLength; ++i)
                if (text[keyStart + i] != property[i])
                    return;
            found = true;
            valid = parseCssNumber(text + colon + 1, text + end, value);
        }
        bool readInlineVolume(
            const JsonReader &json,
            u32 valueStart,
            u32 valueEnd,
            bool &found,
            bool &valid,
            double &value)
        {
            MultiArray<char, MAX_INLINE_VOLUME_TEXT> text;
            if (!json.readStringValue(valueStart, valueEnd, text.data(), static_cast<u32>(text.size())))
                return false;
            u32 declarationStart = 0;
            u32 depth = 0;
            char quote = '\0';
            for (u32 i = 0;; ++i)
            {
                const char ch = text[i];
                if (!ch || (ch == ';' && !depth && !quote))
                {
                    parseInlineVolumeDeclaration(
                        text.data(), declarationStart, i, found, valid, value);
                    declarationStart = i + 1;
                    if (!ch)
                        break;
                    continue;
                }
                if (quote)
                {
                    if (ch == quote)
                        quote = '\0';
                }
                else if (ch == '\'' || ch == '"')
                {
                    quote = ch;
                }
                else if (ch == '(')
                {
                    ++depth;
                }
                else if (ch == ')' && depth)
                {
                    --depth;
                }
            }
            return true;
        }
        bool readStyleVolume(
            const JsonReader &json,
            u32 objectStart,
            const char *styleField,
            bool &found,
            bool &valid,
            double &value)
        {
            u32 styleStart = 0;
            u32 styleEnd = 0;
            if (!json.field(objectStart, styleField, styleStart, styleEnd))
                return true;
            if (json.isObject(styleStart, styleEnd))
            {
                u32 volumeStart = 0;
                u32 volumeEnd = 0;
                if (!json.field(styleStart, "--volume", volumeStart, volumeEnd))
                    return true;
                found = true;
                valid = json.valueNumber(volumeStart, volumeEnd, value) &&
                        std::isfinite(value);
                return true;
            }
            return readInlineVolume(json, styleStart, styleEnd, found, valid, value);
        }
        bool readKeyframeVolume(
            const JsonReader &json,
            u32 keyframeStart,
            bool &hasVolume,
            double &volume)
        {
            hasVolume = false;
            bool valid = true;
            if (!readStyleVolume(
                    json, keyframeStart, "css", hasVolume, valid, volume))
                return false;
            if (!readStyleVolume(
                    json, keyframeStart, "style", hasVolume, valid, volume))
                return false;
            u32 rootVolumeStart = 0;
            u32 rootVolumeEnd = 0;
            if (json.field(
                    keyframeStart, "volume", rootVolumeStart, rootVolumeEnd) ||
                json.field(
                    keyframeStart, "volunm", rootVolumeStart, rootVolumeEnd) ||
                json.field(
                    keyframeStart, "vol", rootVolumeStart, rootVolumeEnd) ||
                json.field(
                    keyframeStart, "level", rootVolumeStart, rootVolumeEnd))
            {
                hasVolume = true;
                valid = json.valueNumber(rootVolumeStart, rootVolumeEnd, volume) &&
                        std::isfinite(volume);
            }
            if (hasVolume && valid) volume = std::clamp(volume, 0.0, MAX_AUDIO_VOLUME);
            return !hasVolume || valid;
        }
        bool readAudioKeyframes(
            const JsonReader &json,
            u32 itemStart,
            double durationMs,
            CompiledAudioTrackSetup &setup)
        {
            u32 keyframesStart = 0;
            u32 keyframesEnd = 0;
            if (!json.field(itemStart, "keyframes", keyframesStart, keyframesEnd) &&
                !json.field(itemStart, "keyframe", keyframesStart, keyframesEnd))
                return true;
            if (!json.isArray(keyframesStart, keyframesEnd))
                return false;
            const double duration = durationMs;
            u32 cursor = keyframesStart + 1;
            while (cursor < keyframesEnd)
            {
                u32 keyframeStart = 0;
                u32 keyframeEnd = 0;
                if (!json.nextArrayValue(
                        cursor, keyframesEnd, keyframeStart, keyframeEnd))
                    break;
                if (!json.isObject(keyframeStart, keyframeEnd))
                    return false;
                bool hasVolume = false;
                double volume = 0.0;
                if (!readKeyframeVolume(
                        json, keyframeStart, hasVolume, volume))
                    return false;
                if (!hasVolume)
                    continue;
                double offsetMs = 0.0;
                u32 offsetStart = 0;
                u32 offsetEnd = 0;
                if (json.field(keyframeStart, "offset", offsetStart, offsetEnd) ||
                    json.field(keyframeStart, "time", offsetStart, offsetEnd))
                {
                    const bool percent = json.valueIsPercent(offsetStart, offsetEnd);
                    if (!json.timeValue(offsetStart, offsetEnd, offsetMs))
                        return false;
                    if (percent)
                        offsetMs = offsetMs * duration / FALLBACK_DURATION_1H_MS;
                    else if (offsetMs >= 0.0 && offsetMs <= 1.0 && duration > DEFAULT_ITEM_DURATION_MS)
                        offsetMs = offsetMs * duration;
                }
                if (!std::isfinite(offsetMs) ||
                    offsetMs < static_cast<double>(std::numeric_limits<i32>::min()) ||
                    offsetMs > static_cast<double>(std::numeric_limits<i32>::max()))
                    return false;
                if (setup.keyframeCount >= MAX_AUDIO_VOL_KF) break;
                const i32 offset = static_cast<i32>(offsetMs);
                u32 insertion = setup.keyframeCount;
                while (insertion > 0 &&
                       setup.keyframes[insertion - 1].offsetMs >
                           offset)
                {
                    setup.keyframes[insertion] = setup.keyframes[insertion - 1];
                    --insertion;
                }
                setup.keyframes[insertion] = {
                    offset,
                    static_cast<float>(volume)};
                ++setup.keyframeCount;
            }
            return true;
        }
        bool readOptionalNumber(
            const JsonReader &json,
            u32 objectStart,
            const char *field,
            double defaultValue,
            double &value)
        {
            u32 valueStart = 0;
            u32 valueEnd = 0;
            if (!json.field(objectStart, field, valueStart, valueEnd))
            {
                value = defaultValue;
                return true;
            }
            return json.valueNumber(valueStart, valueEnd, value) &&
                   std::isfinite(value);
        }
        bool collectAudioTrackSetup(
            const JsonReader &json,
            u32 itemStart,
            u32 slot,
            CompiledAudioTrackSetup &setup)
        {
            const bool isAudio = json.stringFieldEquals(itemStart, "type", "audio");
            setup.slot = slot;
            setup.type = isAudio ? MediaType::Audio : MediaType::Video;
            setup.keyframeCount = 0u;
            double startMs = 0.0;
            double endMs = 0.0;
            double durationMs = 0.0;
            double volume = 1.0;
            double trim = 0.0;
            if (!json.numberField(itemStart, "start", startMs) ||
                !json.numberField(itemStart, "end", endMs) ||
                !readOptionalNumber(json, itemStart, "durationMs", std::max(0.0, endMs - startMs), durationMs) ||
                !readOptionalNumber(json, itemStart, "volume", 1.0, volume) ||
                !readOptionalNumber(json, itemStart, "trimStart", 0.0, trim))
                return false;
            constexpr MultiArray<const char *, 3> volAliases = {{"volunm", "vol", "level"}};
            for (const char *alias : volAliases)
            {
                if (volume == 1.0)
                    readOptionalNumber(json, itemStart, alias, 1.0, volume);
            }
            if (trim == 0.0 && !readOptionalNumber(
                                   json, itemStart, "offset", 0.0, trim))
                return false;
            volume = std::clamp(volume, 0.0, MAX_AUDIO_VOLUME);
            if (durationMs <= 0.0) durationMs = std::max(0.0, endMs - startMs);
            if (trim < -static_cast<double>(std::numeric_limits<float>::max()) ||
                trim > static_cast<double>(std::numeric_limits<float>::max()) ||
                startMs < static_cast<double>(std::numeric_limits<i64>::min()) ||
                startMs > static_cast<double>(std::numeric_limits<i64>::max()) ||
                endMs < static_cast<double>(std::numeric_limits<i64>::min()) ||
                endMs > static_cast<double>(std::numeric_limits<i64>::max()) ||
                startMs < 0.0 || endMs < startMs)
                return false;
            setup.startMs = static_cast<i64>(std::llround(startMs));
            setup.endMs = static_cast<i64>(std::llround(endMs));
            setup.trimOffsetSec = static_cast<float>(trim);
            setup.baseVolume = static_cast<float>(volume);
            return readAudioKeyframes(json, itemStart, durationMs, setup);
        }
        double leadingNumberCstr(const char *text, double fallback)
        {
            if (!text || !text[0])
                return fallback;
            char *end = nullptr;
            const double result = std::strtod(text, &end);
            return end != text && std::isfinite(result) ? result : fallback;
        }
        double leadingNumber(const SceneStyleValue &value, double fallback)
        {
            return value.found ? leadingNumberCstr(value.text.data(), fallback) : fallback;
        }
        bool extractRotateDegFromTransformCstr(
            const char *transformText,
            double &angleOut)
        {
            if (!transformText)
                return false;
            for (u32 index = 0; transformText[index]; ++index)
            {
                if (std::strncmp(transformText + index, "rotate(", 7) != 0)
                    continue;
                const char *valueStart = transformText + index + 7;
                char *valueEnd = nullptr;
                const double angle = std::strtod(valueStart, &valueEnd);
                if (valueEnd != valueStart && std::isfinite(angle))
                {
                    angleOut = angle;
                    return true;
                }
                break;
            }
            return false;
        }
        void extractRotateFromTransform(
            const SceneStyleValue &transform,
            SceneStyleValue &rotateOut)
        {
            if (rotateOut.found || !transform.found)
                return;
            double angle = 0.0;
            if (extractRotateDegFromTransformCstr(transform.text.data(), angle))
            {
                rotateOut.found = true;
                rotateOut.numericSource = false;
                std::snprintf(rotateOut.text.data(), rotateOut.text.size(), "%.17g", angle);
            }
        }
        u32 parseStagedTimelineColor(
            EngineManager &manager,
            char *setupInput,
            const char *colorText,
            u32 fallback)
        {
            if (!colorText || !colorText[0])
                return fallback;
            u32 length = 0;
            while (colorText[length])
                ++length;
            if (length > TIMELINE_SETUP_INPUT_CAPACITY)
                return fallback;
            std::memcpy(setupInput, colorText, length);
            return manager.parseTimelineColor(length, fallback);
        }
        void resolveItemFontFamilyCstr(
            const JsonReader &json,
            u32 itemStart,
            const char *fontFamilyText,
            bool hasFontFamily,
            char *out,
            u32 cap)
        {
            out[0] = '\0';
            readFirstStringField(json, itemStart, "font-family", "fontFamily", out, cap);
            if (!out[0] && hasFontFamily && fontFamilyText)
            {
                copy_cstr_bounded(out, cap, fontFamilyText);
            }
        }
        void resolveItemFontFamily(
            const JsonReader &json,
            u32 itemStart,
            const SceneStyleValue &fontFamilyValue,
            char *out,
            u32 cap)
        {
            resolveItemFontFamilyCstr(json, itemStart, fontFamilyValue.text.data(), fontFamilyValue.found, out, cap);
        }
        template <bool StripTilde>
        void appendTextSpan(
            const char *str,
            u16 kfIdx,
            char *fullTextBuf,
            u32 fullTextCap,
            u32 &fullTextLen,
            u16 *charKfMap,
            u32 charKfCap,
            u32 &totalChars)
        {
            for (u32 idx = 0; str[idx];)
            {
                const u8 b0 = static_cast<u8>(str[idx]);
                if constexpr (StripTilde)
                {
                    if (b0 == '~')
                    {
                        ++idx;
                        continue;
                    }
                }
                u32 byteCount = 1;
                u32 codeUnits = 1;
                utf8_lead_byte_info(b0, byteCount, codeUnits);
                if (fullTextLen + byteCount < fullTextCap - 1u)
                {
                    for (u32 b = 0; b < byteCount && str[idx + b]; ++b)
                        fullTextBuf[fullTextLen++] = str[idx + b];
                    for (u32 cu = 0; cu < codeUnits && totalChars < charKfCap; ++cu)
                        charKfMap[totalChars++] = kfIdx;
                }
                idx += byteCount;
            }
        }

        struct KeyframeCompileContext
        {
            const JsonReader &json;
            u32 keyframesStart = 0;
            u32 keyframesEnd = 0;
            u32 keyframeCount = 0;
            double durationMs = 0.0;
            u32 slot = 0;
            const TimelineItem &item;
            float baseOpacity = 1.0f;
            double baseScaleX = 1.0;
            double baseScaleY = 1.0;
            const double *geometry = nullptr;
            const CssTransformOrigin &baseOrigin;
            double stageWidth = 0.0;
            double stageHeight = 0.0;
            EngineManager &manager;
            char *setupInput = nullptr;
            Position<> *position = nullptr;
            bool hasKeyframeContent = false;
            u32 easing = 0;
            char *fullTextBuf = nullptr;
            u32 fullTextCap = 0;
            u32 *fullTextLen = nullptr;
            u16 *charKfMap = nullptr;
            u32 charKfCap = 0;
            u32 *totalChars = nullptr;
            char *scratchBuf = nullptr;
            u32 scratchCap = 0;
        };

        struct KeyframeCompileResult
        {
            u32 contentMask = 0;
            u32 hasSpatial = 0;
            u64 kfProps = 0;
        };

        enum ScannedPropSlot : u8
        {
            SP_OPACITY = 0,
            SP_WIDTH,
            SP_HEIGHT,
            SP_LEFT,
            SP_TOP,
            SP_ROTATE,
            SP_TRANSFORM,
            SP_SCALE,
            SP_TRANSLATE,
            SP_ORIGIN_X,
            SP_FILL,
            SP_COLOR,
            SP_BG_COLOR,
            SP_BG,
            SP_STROKE,
            SP_STROKE_COL,
            SP_STROKE_W,
            SP_BLUR,
            SP_KF_NB,
            SP_FONT_SIZE = SP_KF_NB,
            SP_LINE_HEIGHT,
            SP_FONT_FAMILY,
            SP_FONT_WEIGHT,
            SP_TEXT_ALIGN,
            SP_TEXT_TRANSFORM,
            SP_LETTER_SPACING,
            SP_MASK_IMAGE,
            SP_BORDER_RADIUS,
            SP_ANIMATION,
            SP_PATH_DATA,
            SP_FILL_RULE,
            SP_STROKE_LINECAP,
            SP_STROKE_LINEJOIN,
            SP_FONT_STYLE,
            SP_FILTER,
            SP_ITEM_NB
        };
        static_assert(SP_KF_NB == 18u, "Keyframe prop slot count");
        static_assert(SP_ITEM_NB == 34u && SP_ITEM_NB <= 64u, "Item prop slot count must fit in u64 mask");

                inline constexpr ScannedPropSlot KF_OPACITY    = SP_OPACITY;
        inline constexpr ScannedPropSlot KF_WIDTH      = SP_WIDTH;
        inline constexpr ScannedPropSlot KF_HEIGHT     = SP_HEIGHT;
        inline constexpr ScannedPropSlot KF_LEFT       = SP_LEFT;
        inline constexpr ScannedPropSlot KF_TOP        = SP_TOP;
        inline constexpr ScannedPropSlot KF_ROTATE     = SP_ROTATE;
        inline constexpr ScannedPropSlot KF_TRANSFORM  = SP_TRANSFORM;
        inline constexpr ScannedPropSlot KF_SCALE      = SP_SCALE;
        inline constexpr ScannedPropSlot KF_TRANSLATE  = SP_TRANSLATE;
        inline constexpr ScannedPropSlot KF_ORIGIN_X   = SP_ORIGIN_X;
        inline constexpr ScannedPropSlot KF_FILL       = SP_FILL;
        inline constexpr ScannedPropSlot KF_COLOR      = SP_COLOR;
        inline constexpr ScannedPropSlot KF_BG_COLOR   = SP_BG_COLOR;
        inline constexpr ScannedPropSlot KF_BG         = SP_BG;
        inline constexpr ScannedPropSlot KF_STROKE     = SP_STROKE;
        inline constexpr ScannedPropSlot KF_STROKE_COL = SP_STROKE_COL;
        inline constexpr ScannedPropSlot KF_STROKE_W   = SP_STROKE_W;
        inline constexpr ScannedPropSlot KF_BLUR       = SP_BLUR;
        inline constexpr ScannedPropSlot KF_PROP_NB    = SP_KF_NB;

        inline constexpr MultiArray<stockcut::prop::StandardPropId, SP_ITEM_NB> kSlotToStandardProp = {{
            stockcut::prop::PROP_OPACITY,          // 0:  SP_OPACITY
            stockcut::prop::PROP_WIDTH,            // 1:  SP_WIDTH
            stockcut::prop::PROP_HEIGHT,           // 2:  SP_HEIGHT
            stockcut::prop::PROP_LEFT,             // 3:  SP_LEFT
            stockcut::prop::PROP_TOP,              // 4:  SP_TOP
            stockcut::prop::PROP_ROTATE,           // 5:  SP_ROTATE
            stockcut::prop::PROP_TRANSFORM,        // 6:  SP_TRANSFORM
            stockcut::prop::PROP_SCALE,            // 7:  SP_SCALE
            stockcut::prop::PROP_TRANSLATE,        // 8:  SP_TRANSLATE
            stockcut::prop::PROP_ORIGIN_X,         // 9:  SP_ORIGIN_X
            stockcut::prop::PROP_FILL,             // 10: SP_FILL
            stockcut::prop::PROP_COLOR,            // 11: SP_COLOR
            stockcut::prop::PROP_BACKGROUND_COLOR, // 12: SP_BG_COLOR
            stockcut::prop::PROP_BACKGROUND,       // 13: SP_BG
            stockcut::prop::PROP_STROKE,           // 14: SP_STROKE
            stockcut::prop::PROP_STROKE_COLOR,     // 15: SP_STROKE_COL
            stockcut::prop::PROP_STROKE_WIDTH,     // 16: SP_STROKE_W
            stockcut::prop::PROP_BLUR,             // 17: SP_BLUR
            stockcut::prop::PROP_FONT_SIZE,        // 18: SP_FONT_SIZE
            stockcut::prop::PROP_LINE_HEIGHT,      // 19: SP_LINE_HEIGHT
            stockcut::prop::PROP_FONT_FAMILY,      // 20: SP_FONT_FAMILY
            stockcut::prop::PROP_FONT_WEIGHT,      // 21: SP_FONT_WEIGHT
            stockcut::prop::PROP_TEXT_ALIGN,       // 22: SP_TEXT_ALIGN
            stockcut::prop::PROP_TEXT_TRANSFORM,   // 23: SP_TEXT_TRANSFORM
            stockcut::prop::PROP_LETTER_SPACING,   // 24: SP_LETTER_SPACING
            stockcut::prop::PROP_MASK_IMAGE,       // 25: SP_MASK_IMAGE
            stockcut::prop::PROP_BORDER_RADIUS,    // 26: SP_BORDER_RADIUS
            stockcut::prop::PROP_ANIMATION,        // 27: SP_ANIMATION
            stockcut::prop::PROP_PATH_DATA,        // 28: SP_PATH_DATA
            stockcut::prop::PROP_FILL_RULE,        // 29: SP_FILL_RULE
            stockcut::prop::PROP_STROKE_LINECAP,   // 30: SP_STROKE_LINECAP
            stockcut::prop::PROP_STROKE_LINEJOIN,  // 31: SP_STROKE_LINEJOIN
            stockcut::prop::PROP_FONT_STYLE,       // 32: SP_FONT_STYLE
            stockcut::prop::PROP_FILTER            // 33: SP_FILTER
        }};

        [[nodiscard]] constexpr bool eq_bytes_n(const char *a, const char *b, u32 n) noexcept
        {
            for (u32 i = 0; i < n; ++i)
            {
                if (a[i] != b[i]) return false;
            }
            return true;
        }

        [[nodiscard]] constexpr i32 matchScenePropSlot(const char *key, u32 len) noexcept
        {
            switch (len)
            {
                case 1:
                    if (key[0] == 'd') return SP_PATH_DATA;
                    return -1;
                case 3:
                    if (eq_bytes_n(key, "top", 3)) return SP_TOP;
                    return -1;
                case 4:
                    if (eq_bytes_n(key, "left", 4)) return SP_LEFT;
                    if (eq_bytes_n(key, "fill", 4)) return SP_FILL;
                    if (eq_bytes_n(key, "blur", 4)) return SP_BLUR;
                    if (eq_bytes_n(key, "path", 4)) return SP_PATH_DATA;
                    return -1;
                case 5:
                    if (eq_bytes_n(key, "width", 5)) return SP_WIDTH;
                    if (eq_bytes_n(key, "scale", 5)) return SP_SCALE;
                    if (eq_bytes_n(key, "color", 5)) return SP_COLOR;
                    return -1;
                case 6:
                    if (eq_bytes_n(key, "height", 6)) return SP_HEIGHT;
                    if (eq_bytes_n(key, "rotate", 6)) return SP_ROTATE;
                    if (eq_bytes_n(key, "stroke", 6)) return SP_STROKE;
                    if (eq_bytes_n(key, "filter", 6)) return SP_FILTER;
                    return -1;
                case 7:
                    if (eq_bytes_n(key, "opacity", 7)) return SP_OPACITY;
                    if (eq_bytes_n(key, "originX", 7)) return SP_ORIGIN_X;
                    return -1;
                case 8:
                    if (eq_bytes_n(key, "origin-x", 8)) return SP_ORIGIN_X;
                    if (eq_bytes_n(key, "fontSize", 8)) return SP_FONT_SIZE;
                    if (eq_bytes_n(key, "pathData", 8)) return SP_PATH_DATA;
                    if (eq_bytes_n(key, "fillRule", 8)) return SP_FILL_RULE;
                    return -1;
                case 9:
                    if (eq_bytes_n(key, "transform", 9)) return SP_TRANSFORM;
                    if (eq_bytes_n(key, "translate", 9)) return SP_TRANSLATE;
                    if (eq_bytes_n(key, "fillColor", 9)) return SP_FILL;
                    if (eq_bytes_n(key, "font-size", 9)) return SP_FONT_SIZE;
                    if (eq_bytes_n(key, "textAlign", 9)) return SP_TEXT_ALIGN;
                    if (eq_bytes_n(key, "animation", 9)) return SP_ANIMATION;
                    if (eq_bytes_n(key, "fill-rule", 9)) return SP_FILL_RULE;
                    return -1;
                case 10:
                    if (eq_bytes_n(key, "background", 10)) return SP_BG;
                    if (eq_bytes_n(key, "fill-color", 10)) return SP_FILL;
                    if (eq_bytes_n(key, "lineHeight", 10)) return SP_LINE_HEIGHT;
                    if (eq_bytes_n(key, "fontFamily", 10)) return SP_FONT_FAMILY;
                    if (eq_bytes_n(key, "fontWeight", 10)) return SP_FONT_WEIGHT;
                    if (eq_bytes_n(key, "text-align", 10)) return SP_TEXT_ALIGN;
                    if (eq_bytes_n(key, "mask-image", 10)) return SP_MASK_IMAGE;
                    if (eq_bytes_n(key, "font-style", 10)) return SP_FONT_STYLE;
                    return -1;
                case 11:
                    if (eq_bytes_n(key, "strokeWidth", 11)) return SP_STROKE_W;
                    if (eq_bytes_n(key, "strokeColor", 11)) return SP_STROKE;
                    if (eq_bytes_n(key, "line-height", 11)) return SP_LINE_HEIGHT;
                    if (eq_bytes_n(key, "font-family", 11)) return SP_FONT_FAMILY;
                    if (eq_bytes_n(key, "font-weight", 11)) return SP_FONT_WEIGHT;
                    return -1;
                case 12:
                    if (eq_bytes_n(key, "stroke-width", 12)) return SP_STROKE_W;
                    if (eq_bytes_n(key, "stroke-color", 12)) return SP_STROKE;
                    return -1;
                case 13:
                    if (eq_bytes_n(key, "border-radius", 13)) return SP_BORDER_RADIUS;
                    if (eq_bytes_n(key, "letterSpacing", 13)) return SP_LETTER_SPACING;
                    if (eq_bytes_n(key, "textTransform", 13)) return SP_TEXT_TRANSFORM;
                    if (eq_bytes_n(key, "strokeLinecap", 13)) return SP_STROKE_LINECAP;
                    return -1;
                case 14:
                    if (eq_bytes_n(key, "letter-spacing", 14)) return SP_LETTER_SPACING;
                    if (eq_bytes_n(key, "text-transform", 14)) return SP_TEXT_TRANSFORM;
                    if (eq_bytes_n(key, "stroke-linecap", 14)) return SP_STROKE_LINECAP;
                    if (eq_bytes_n(key, "strokeLinejoin", 14)) return SP_STROKE_LINEJOIN;
                    return -1;
                case 15:
                    if (eq_bytes_n(key, "stroke-linejoin", 15)) return SP_STROKE_LINEJOIN;
                    if (eq_bytes_n(key, "backgroundColor", 15)) return SP_BG_COLOR;
                    return -1;
                case 16:
                    if (eq_bytes_n(key, "background-color", 16)) return SP_BG_COLOR;
                    return -1;
                case 17:
                    if (eq_bytes_n(key, "text-stroke-color", 17)) return SP_STROKE_COL;
                    return -1;
                case 18:
                    if (eq_bytes_n(key, "-webkit-mask-image", 18)) return SP_MASK_IMAGE;
                    return -1;
                case 25:
                    if (eq_bytes_n(key, "-webkit-text-stroke-width", 25)) return SP_STROKE_W;
                    if (eq_bytes_n(key, "-webkit-text-stroke-color", 25)) return SP_STROKE_COL;
                    return -1;
                default:
                    return -1;
            }
        }

        [[nodiscard]] constexpr bool verifyScannedSlotMatcher() noexcept
        {
            auto lenOf = [](const char *p) noexcept -> u32 {
                u32 n = 0;
                while (p[n]) ++n;
                return n;
            };
            for (u32 s = 0; s < SP_ITEM_NB; ++s)
            {
                const auto &desc = stockcut::prop::kPropSchemas[kSlotToStandardProp[s]];
                if (matchScenePropSlot(desc.kebab, lenOf(desc.kebab)) != static_cast<i32>(s))
                    return false;
                if (desc.camel && matchScenePropSlot(desc.camel, lenOf(desc.camel)) != static_cast<i32>(s))
                    return false;
                if (desc.alias && matchScenePropSlot(desc.alias, lenOf(desc.alias)) != static_cast<i32>(s))
                    return false;
            }
            return true;
        }
        static_assert(verifyScannedSlotMatcher(), "matchScenePropSlot must match prop::kPropSchemas");

        template <u32 Capacity>
        struct ScannedPropBag
        {
            u64 foundMask = 0;
            u32 offsetStart = 0;
            u32 offsetEnd = 0;
            u32 contentStart = 0;
            u32 contentEnd = 0;
            u32 styleStart = 0;
            u32 styleEnd = 0;
            u32 cssStart = 0;
            u32 cssEnd = 0;
            bool hasOffset = false;
            bool hasContent = false;
            bool hasStyle = false;
            bool hasCss = false;
            MultiArray<u16, Capacity> textLen;
            MultiArray<MultiArray<char, MAX_STYLE_VALUE_TEXT>, Capacity> text;

            inline void reset() noexcept
            {
                foundMask = 0;
                offsetStart = offsetEnd = 0;
                contentStart = contentEnd = 0;
                styleStart = styleEnd = 0;
                cssStart = cssEnd = 0;
                hasOffset = false;
                hasContent = false;
                hasStyle = false;
                hasCss = false;
            }

            [[nodiscard]] inline bool has(ScannedPropSlot s) const noexcept
            {
                return static_cast<u32>(s) < Capacity &&
                       (foundMask & (1ULL << static_cast<u32>(s))) != 0ULL;
            }

            [[nodiscard]] inline const char *cstr(ScannedPropSlot s) const noexcept
            {
                return has(s) ? text[s].data() : "";
            }

            [[nodiscard]] inline StringSpan span(ScannedPropSlot s) const noexcept
            {
                return has(s) ? StringSpan{text[s].data(), static_cast<u32>(textLen[s])} : StringSpan{};
            }

            [[nodiscard]] inline double lead_num(ScannedPropSlot s, double fallback) const noexcept
            {
                return has(s) ? leadingNumberCstr(text[s].data(), fallback) : fallback;
            }

            [[nodiscard]] inline double scene_len(ScannedPropSlot s, double ref, double fallback) const noexcept
            {
                return has(s) ? sceneLength(text[s].data(), ref, fallback) : fallback;
            }
        };

        using KfScannedProps = ScannedPropBag<SP_KF_NB>;
        using ItemScannedProps = ScannedPropBag<SP_ITEM_NB>;

        template <u32 Capacity>
        void storeSceneJsonSpanValue(
            const JsonReader &json,
            u32 valueStart,
            u32 valueEnd,
            ScannedPropSlot slot,
            ScannedPropBag<Capacity> &out)
        {
            if (static_cast<u32>(slot) >= Capacity || spanIsNull(json, valueStart, valueEnd))
                return;
            char *dst = out.text[slot].data();
            constexpr u32 cap = MAX_STYLE_VALUE_TEXT;
            if (valueStart < valueEnd && json.readStringValue(valueStart, valueEnd, dst, cap))
            {
                out.textLen[slot] = static_cast<u16>(std::strlen(dst));
                out.foundMask |= (1ULL << static_cast<u32>(slot));
                return;
            }
            u32 used = 0;
            if (!json.copySpan(valueStart, valueEnd, dst, cap, used) || used >= cap)
                return;
            dst[used] = '\0';
            out.textLen[slot] = static_cast<u16>(used);
            out.foundMask |= (1ULL << static_cast<u32>(slot));
        }

        template <bool RootPass, bool CaptureMeta, u32 Capacity>
        void scanSceneObjectFields(
            const JsonReader &json,
            u32 objectStart,
            u32 objectEnd,
            ScannedPropBag<Capacity> &out)
        {
            if (!json.isObject(objectStart, objectEnd))
                return;
            u32 cursor = objectStart + 1;
            MultiArray<char, MAX_TOKEN_TEXT> keyBuf;
            while (cursor < objectEnd)
            {
                u32 ks = 0, ke = 0, vs = 0, ve = 0;
                if (!json.nextObjectField(cursor, objectEnd, ks, ke, vs, ve))
                    break;
                if (!json.readStringValue(ks, ke, keyBuf.data(), static_cast<u32>(keyBuf.size())))
                    continue;
                const u32 keyLen = static_cast<u32>(std::strlen(keyBuf.data()));
                if constexpr (RootPass)
                {
                    if constexpr (CaptureMeta)
                    {
                        if (keyLen == 6 && std::memcmp(keyBuf.data(), "offset", 6) == 0)
                        {
                            if (!out.hasOffset)
                            {
                                out.offsetStart = vs;
                                out.offsetEnd = ve;
                                out.hasOffset = true;
                            }
                            continue;
                        }
                        if (keyLen == 7 && std::memcmp(keyBuf.data(), "content", 7) == 0)
                        {
                            if (!out.hasContent)
                            {
                                out.contentStart = vs;
                                out.contentEnd = ve;
                                out.hasContent = true;
                            }
                            continue;
                        }
                    }
                    if (keyLen == 5 && std::memcmp(keyBuf.data(), "style", 5) == 0)
                    {
                        if (!out.hasStyle)
                        {
                            out.styleStart = vs;
                            out.styleEnd = ve;
                            out.hasStyle = true;
                        }
                        continue;
                    }
                    if (keyLen == 3 && std::memcmp(keyBuf.data(), "css", 3) == 0)
                    {
                        if (!out.hasCss)
                        {
                            out.cssStart = vs;
                            out.cssEnd = ve;
                            out.hasCss = true;
                        }
                        continue;
                    }
                }
                const i32 slotIdx = matchScenePropSlot(keyBuf.data(), keyLen);
                if (slotIdx >= 0 && static_cast<u32>(slotIdx) < Capacity)
                    storeSceneJsonSpanValue(json, vs, ve, static_cast<ScannedPropSlot>(slotIdx), out);
            }
        }

        template <u32 Capacity>
        void scanSceneCssDeclarations(
            const JsonReader &json,
            u32 valueStart,
            u32 valueEnd,
            ScannedPropBag<Capacity> &out)
        {
            MultiArray<char, MAX_CSS_DECLARATION_TEXT> declarations;
            declarations[0] = '\0';
            if (!json.readStringValue(valueStart, valueEnd, declarations.data(), static_cast<u32>(declarations.size())))
                return;
            const u32 length = static_cast<u32>(std::strlen(declarations.data()));
            u32 declarationStart = 0;
            u32 depth = 0;
            char quote = '\0';
            for (u32 pos = 0; pos <= length; ++pos)
            {
                const char current = (pos < length) ? declarations[pos] : ';';
                if (quote)
                {
                    if (current == quote) quote = '\0';
                    continue;
                }
                if (current == '\'' || current == '"')
                {
                    quote = current;
                    continue;
                }
                if (current == '(') ++depth;
                else if (current == ')' && depth) --depth;
                if (current != ';' || depth)
                    continue;

                u32 colon = declarationStart;
                while (colon < pos && declarations[colon] != ':') ++colon;
                if (colon < pos)
                {
                    u32 ks = declarationStart;
                    while (ks < colon && is_ascii_space(declarations[ks])) ++ks;
                    u32 ke = colon;
                    while (ke > ks && is_ascii_space(declarations[ke - 1])) --ke;

                    MultiArray<char, MAX_TEMPLATE_NAME> normalizedKey;
                    u32 normLen = 0;
                    bool overflowed = false;
                    for (u32 i = ks; i < ke; ++i)
                    {
                        const char ch = declarations[i];
                        if (ch >= 'A' && ch <= 'Z')
                        {
                            if (normLen + 2 >= normalizedKey.size()) { overflowed = true; break; }
                            normalizedKey[normLen++] = '-';
                            normalizedKey[normLen++] = ascii_lower(ch);
                        }
                        else
                        {
                            if (normLen + 1 >= normalizedKey.size()) { overflowed = true; break; }
                            normalizedKey[normLen++] = ascii_lower(ch);
                        }
                    }
                    if (!overflowed)
                    {
                        const i32 slotIdx = matchScenePropSlot(normalizedKey.data(), normLen);
                        if (slotIdx >= 0 && static_cast<u32>(slotIdx) < Capacity)
                        {
                            u32 vs = colon + 1;
                            while (vs < pos && is_ascii_space(declarations[vs])) ++vs;
                            u32 ve = pos;
                            while (ve > vs && is_ascii_space(declarations[ve - 1])) --ve;
                            const u32 valLen = ve - vs;
                            if (valLen > 0 && valLen < MAX_STYLE_VALUE_TEXT)
                            {
                                char *dst = out.text[slotIdx].data();
                                std::memcpy(dst, declarations.data() + vs, valLen);
                                dst[valLen] = '\0';
                                out.textLen[slotIdx] = static_cast<u16>(valLen);
                                out.foundMask |= (1ULL << static_cast<u32>(slotIdx));
                            }
                        }
                    }
                }
                declarationStart = pos + 1;
            }
        }

        template <bool CaptureMeta, u32 Capacity>
        inline void scanScenePropsSinglePass(
            const JsonReader &json,
            u32 objectStart,
            u32 objectEnd,
            ScannedPropBag<Capacity> &out)
        {
            out.reset();
            scanSceneObjectFields<true, CaptureMeta>(json, objectStart, objectEnd, out);
            if (out.hasStyle)
            {
                if (json.isObject(out.styleStart, out.styleEnd))
                    scanSceneObjectFields<false, false>(json, out.styleStart, out.styleEnd, out);
                else
                    scanSceneCssDeclarations(json, out.styleStart, out.styleEnd, out);
            }
            if (out.hasCss)
            {
                if (json.isObject(out.cssStart, out.cssEnd))
                    scanSceneObjectFields<false, false>(json, out.cssStart, out.cssEnd, out);
                else
                    scanSceneCssDeclarations(json, out.cssStart, out.cssEnd, out);
            }
        }

        inline void scanKeyframeSinglePass(
            const JsonReader &json,
            u32 keyframeStart,
            u32 keyframeEnd,
            KfScannedProps &out)
        {
            scanScenePropsSinglePass<true>(json, keyframeStart, keyframeEnd, out);
        }

        struct StagingKfFrame
        {
            i32 offsetMs = 0;
            Mask<2> presentMask{};
            NativeKeyframe spatial{};
            StyleColorKeyframe styleColor{};
            double unscaledX = 0.0;
            double unscaledY = 0.0;
            double unscaledW = 0.0;
            double unscaledH = 0.0;
            double stageOriginX = 0.0;
            double stageOriginY = 0.0;
            double sx = 1.0;
            double sy = 1.0;
            u32 contentStart = 0;
            u32 contentEnd = 0;
        };

        template <typename Accessor>
        inline void fuseStagingScalar(
            StagingKfFrame *frames,
            u32 count,
            prop::StandardPropId reqA,
            prop::StandardPropId reqB,
            Accessor &&acc,
            double baseVal) noexcept
        {
            auto hasProp = [&](const StagingKfFrame &f) -> bool {
                return f.presentMask.test_bit(static_cast<u32>(reqA)) ||
                       (reqB != reqA && f.presentMask.test_bit(static_cast<u32>(reqB)));
            };
            bool anySet = false;
            for (u32 i = 0; i < count; ++i)
            {
                if (hasProp(frames[i]))
                {
                    anySet = true;
                    break;
                }
            }
            if (!anySet)
                return;

            for (u32 i = 0; i < count; ++i)
            {
                if (hasProp(frames[i]))
                    continue;
                i32 prevIdx = -1;
                for (i32 p = static_cast<i32>(i) - 1; p >= 0; --p)
                {
                    if (hasProp(frames[p])) { prevIdx = p; break; }
                }
                i32 nextIdx = -1;
                for (u32 n = i + 1; n < count; ++n)
                {
                    if (hasProp(frames[n])) { nextIdx = static_cast<i32>(n); break; }
                }
                if (prevIdx >= 0 && nextIdx >= 0)
                {
                    const double dt = static_cast<double>(frames[nextIdx].offsetMs - frames[prevIdx].offsetMs);
                    const double alpha = (dt > 0.0)
                                             ? static_cast<double>(frames[i].offsetMs - frames[prevIdx].offsetMs) / dt
                                             : 0.0;
                    const double v0 = static_cast<double>(acc(frames[prevIdx]));
                    const double v1 = static_cast<double>(acc(frames[nextIdx]));
                    acc(frames[i]) = static_cast<std::remove_reference_t<decltype(acc(frames[i]))>>(v0 + alpha * (v1 - v0));
                }
                else if (prevIdx >= 0)
                {
                    acc(frames[i]) = acc(frames[prevIdx]);
                }
                else if (nextIdx >= 0)
                {
                    const double dt = static_cast<double>(frames[nextIdx].offsetMs);
                    const double alpha = (dt > 0.0) ? static_cast<double>(frames[i].offsetMs) / dt : 0.0;
                    const double v1 = static_cast<double>(acc(frames[nextIdx]));
                    acc(frames[i]) = static_cast<std::remove_reference_t<decltype(acc(frames[i]))>>(baseVal + alpha * (v1 - baseVal));
                }
            }
        }

        static KeyframeCompileResult compileItemKeyframes(
            KeyframeCompileContext &ctx,
            MediaType mediaType)
        {
            const bool hasSpatialGroup = (mediaType != MediaType::Audio && mediaType != MediaType::None);
            const bool hasStyleColorGroup = (mediaType != MediaType::Audio && mediaType != MediaType::None);
            const bool hasTypographyGroup = (mediaType == MediaType::Text || mediaType == MediaType::Lyric);

            MultiArray<StagingKfFrame, MAX_KEYFRAMES_PER_SLOT> frames;
            StagingKfFrame baseSentinel{};
            const TextBlock &bSlot = ctx.position->base_slot_block(ctx.slot);

            if (hasSpatialGroup)
            {
                baseSentinel.spatial.opacity = ctx.baseOpacity;
                baseSentinel.spatial.x = ctx.item.baseX;
                baseSentinel.spatial.y = ctx.item.baseY;
                baseSentinel.spatial.w = ctx.item.baseW;
                baseSentinel.spatial.h = ctx.item.baseH;
                baseSentinel.spatial.rotateDeg = ctx.item.rotateDeg;
                baseSentinel.spatial.scale = static_cast<float>(ctx.baseScaleX);
            }
            if (hasStyleColorGroup)
            {
                baseSentinel.styleColor.fillColor = ColorRGBA::from_packed(bSlot.get_value<u32, 4>(16));
                baseSentinel.styleColor.strokeColor = ColorRGBA::from_packed(bSlot.get_value<u32, 4>(17));
                baseSentinel.styleColor.strokeWidth = bSlot.get_value<float, 4>(12);
                baseSentinel.styleColor.blurRadius = 0.0f;
            }

            KfScannedProps props;
            u32 keyCursor = ctx.keyframesStart + 1;
            for (u32 keyIndex = 0; keyIndex < ctx.keyframeCount; ++keyIndex)
            {
                u32 keyframeStart = 0;
                u32 keyframeEnd = 0;
                if (!ctx.json.nextArrayValue(keyCursor, ctx.keyframesEnd, keyframeStart, keyframeEnd))
                    break;

                scanKeyframeSinglePass(ctx.json, keyframeStart, keyframeEnd, props);

                double offsetMs = 0.0;
                if (props.hasOffset)
                {
                    if (!ctx.json.timeValue(props.offsetStart, props.offsetEnd, offsetMs))
                        offsetMs = 0.0;
                    if (ctx.json.valueIsPercent(props.offsetStart, props.offsetEnd))
                        offsetMs *= ctx.durationMs / FALLBACK_DURATION_1H_MS;
                }
                offsetMs = std::clamp(offsetMs, 0.0, ctx.durationMs);

                StagingKfFrame &frame = frames[keyIndex];
                frame = StagingKfFrame{};
                frame.offsetMs = static_cast<i32>(offsetMs);

                if (hasSpatialGroup)
                {
                    frame.spatial = baseSentinel.spatial;
                    frame.sx = ctx.baseScaleX;
                    frame.sy = ctx.baseScaleY;
                    frame.unscaledX = ctx.geometry[0];
                    frame.unscaledY = ctx.geometry[1];
                    frame.unscaledW = ctx.geometry[2];
                    frame.unscaledH = ctx.geometry[3];

                    double basePxX = 0.0, basePxY = 0.0;
                    ctx.baseOrigin.resolve(ctx.geometry[2], ctx.geometry[3], basePxX, basePxY);
                    frame.stageOriginX = ctx.geometry[0] + basePxX;
                    frame.stageOriginY = ctx.geometry[1] + basePxY;

                    if (props.has(KF_OPACITY))
                    {
                        double opacity = leadingNumberCstr(props.text[KF_OPACITY].data(), 1.0);
                        if (opacity > 1.0)
                            opacity /= 100.0;
                        frame.spatial.opacity = static_cast<float>(opacity);
                        frame.presentMask.set_bit(prop::PROP_OPACITY);
                    }
                    if (props.has(KF_TRANSFORM))
                    {
                        double rotAngle = 0.0;
                        if (parse_css_transform_rotate<false>(props.span(KF_TRANSFORM), rotAngle))
                        {
                            frame.spatial.rotateDeg = static_cast<float>(rotAngle);
                            frame.presentMask.set_bit(prop::PROP_ROTATE);
                        }
                    }
                    if (!frame.presentMask.test_bit(prop::PROP_ROTATE) && props.has(KF_ROTATE))
                    {
                        frame.spatial.rotateDeg = static_cast<float>(
                            leadingNumberCstr(props.text[KF_ROTATE].data(), ctx.item.rotateDeg));
                        frame.presentMask.set_bit(prop::PROP_ROTATE);
                    }
                    if (props.has(KF_SCALE))
                    {
                        parse_css_scale_property(props.span(KF_SCALE), frame.sx, frame.sy);
                        frame.presentMask.set_bit(prop::PROP_SCALE);
                    }
                    if (props.has(KF_TRANSFORM))
                    {
                        if (parse_css_transform_scale(props.span(KF_TRANSFORM), frame.sx, frame.sy))
                            frame.presentMask.set_bit(prop::PROP_SCALE);
                    }

                    double kfAnchorX = ctx.geometry[4], kfAnchorY = ctx.geometry[5];
                    if (props.has(KF_TRANSLATE))
                    {
                        parse_css_translate_property(props.span(KF_TRANSLATE), kfAnchorX, kfAnchorY);
                        frame.presentMask.set_bit(prop::PROP_LEFT);
                        frame.presentMask.set_bit(prop::PROP_TOP);
                    }
                    if (props.has(KF_TRANSFORM))
                    {
                        if (parse_css_transform_translate(props.span(KF_TRANSFORM), kfAnchorX, kfAnchorY))
                        {
                            frame.presentMask.set_bit(prop::PROP_LEFT);
                            frame.presentMask.set_bit(prop::PROP_TOP);
                        }
                    }
                    const bool kfHasLeft = props.has(KF_LEFT);
                    const bool kfHasTop = props.has(KF_TOP);
                    const bool kfHasWidth = props.has(KF_WIDTH);
                    const bool kfHasHeight = props.has(KF_HEIGHT);
                    if (kfHasLeft) frame.presentMask.set_bit(prop::PROP_LEFT);
                    if (kfHasTop) frame.presentMask.set_bit(prop::PROP_TOP);
                    if (kfHasWidth) frame.presentMask.set_bit(prop::PROP_WIDTH);
                    if (kfHasHeight) frame.presentMask.set_bit(prop::PROP_HEIGHT);

                    CssTransformOrigin kfOrigin = ctx.baseOrigin;
                    if (props.has(KF_ORIGIN_X))
                    {
                        parse_css_transform_origin(props.span(KF_ORIGIN_X), kfOrigin);
                    }

                    if (frame.presentMask.test_bit(prop::PROP_LEFT) ||
                        frame.presentMask.test_bit(prop::PROP_TOP) ||
                        frame.presentMask.test_bit(prop::PROP_WIDTH) ||
                        frame.presentMask.test_bit(prop::PROP_HEIGHT))
                    {
                        u32 kfInputOffset = 0;
                        const auto stageKfText = [&](ScannedPropSlot s) -> u32
                        {
                            if (!props.has(s)) return 0u;
                            const u32 len = props.textLen[s];
                            if (kfInputOffset + len > TIMELINE_SETUP_INPUT_CAPACITY) return 0u;
                            std::memcpy(ctx.setupInput + kfInputOffset, props.text[s].data(), len);
                            kfInputOffset += len;
                            return len;
                        };
                        const u32 kfWidthLength = stageKfText(KF_WIDTH);
                        const u32 kfHeightLength = stageKfText(KF_HEIGHT);
                        const u32 kfLeftLength = stageKfText(KF_LEFT);
                        const u32 kfTopLength = stageKfText(KF_TOP);
                        const u32 kfFlags =
                            (kfHasWidth ? TIMELINE_GEOMETRY_HAS_WIDTH : 0u) |
                            (kfHasHeight ? TIMELINE_GEOMETRY_HAS_HEIGHT : 0u) |
                            (kfHasLeft ? TIMELINE_GEOMETRY_HAS_LEFT : 0u) |
                            (kfHasTop ? TIMELINE_GEOMETRY_HAS_TOP : 0u);
                        double *kfGeometry = ctx.manager.computeTimelineKeyframeGeometry(
                            kfFlags, ctx.stageWidth, ctx.stageHeight, kfAnchorX, kfAnchorY,
                            ctx.geometry[0], ctx.geometry[1], ctx.geometry[2], ctx.geometry[3],
                            kfWidthLength, kfHeightLength, kfLeftLength, kfTopLength);
                        if (kfGeometry)
                        {
                            frame.unscaledX = kfGeometry[0];
                            frame.unscaledY = kfGeometry[1];
                            frame.unscaledW = kfGeometry[2];
                            frame.unscaledH = kfGeometry[3];
                        }
                    }
                    double kfPxX = 0.0, kfPxY = 0.0;
                    kfOrigin.resolve(frame.unscaledW, frame.unscaledH, kfPxX, kfPxY);
                    frame.stageOriginX = frame.unscaledX + kfPxX;
                    frame.stageOriginY = frame.unscaledY + kfPxY;
                }

                if (hasStyleColorGroup)
                {
                    frame.styleColor = baseSentinel.styleColor;

                    i32 fillSlot = -1;
                    if (props.has(KF_FILL)) fillSlot = KF_FILL;
                    else if (props.has(KF_COLOR)) fillSlot = KF_COLOR;
                    else if (props.has(KF_BG_COLOR)) fillSlot = KF_BG_COLOR;
                    else if (props.has(KF_BG)) fillSlot = KF_BG;
                    if (fillSlot >= 0)
                    {
                        const u32 packed = parseStagedTimelineColor(
                            ctx.manager, ctx.setupInput, props.text[fillSlot].data(), bSlot.get_value<u32, 4>(16));
                        frame.styleColor.fillColor = ColorRGBA::from_packed(packed);
                        frame.styleColor.colorFlags |= 1u;
                        frame.presentMask.set_bit(prop::PROP_FILL);
                    }

                    i32 strokeSlot = -1;
                    if (props.has(KF_STROKE)) strokeSlot = KF_STROKE;
                    else if (props.has(KF_STROKE_COL)) strokeSlot = KF_STROKE_COL;
                    if (strokeSlot >= 0)
                    {
                        const u32 packed = parseStagedTimelineColor(
                            ctx.manager, ctx.setupInput, props.text[strokeSlot].data(), bSlot.get_value<u32, 4>(17));
                        frame.styleColor.strokeColor = ColorRGBA::from_packed(packed);
                        frame.styleColor.colorFlags |= 2u;
                        frame.presentMask.set_bit(prop::PROP_STROKE_COLOR);
                    }

                    if (props.has(KF_STROKE_W))
                    {
                        frame.styleColor.strokeWidth = static_cast<float>(
                            leadingNumberCstr(props.text[KF_STROKE_W].data(), static_cast<double>(bSlot.get_value<float, 4>(12))));
                        frame.presentMask.set_bit(prop::PROP_STROKE_WIDTH);
                    }
                    if (props.has(KF_BLUR))
                    {
                        frame.styleColor.blurRadius = static_cast<float>(
                            std::max(0.0, leadingNumberCstr(props.text[KF_BLUR].data(), 0.0)));
                        frame.presentMask.set_bit(prop::PROP_BLUR);
                    }
                }

                if (hasTypographyGroup && props.hasContent)
                {
                    frame.contentStart = props.contentStart;
                    frame.contentEnd = props.contentEnd;
                    frame.presentMask.set_bit(prop::PROP_CONTENT);
                }
            }

            // Sort chronologically by offsetMs
            for (u32 index = 1; index < ctx.keyframeCount; ++index)
            {
                StagingKfFrame record = frames[index];
                u32 insertion = index;
                while (insertion && frames[insertion - 1].offsetMs > record.offsetMs)
                {
                    frames[insertion] = frames[insertion - 1];
                    --insertion;
                }
                frames[insertion] = record;
            }

            if (hasSpatialGroup)
            {
                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_OPACITY, prop::PROP_OPACITY,
                    [](StagingKfFrame &k) -> float& { return k.spatial.opacity; }, static_cast<double>(ctx.baseOpacity));
                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_ROTATE, prop::PROP_ROTATE,
                    [](StagingKfFrame &k) -> float& { return k.spatial.rotateDeg; }, static_cast<double>(ctx.item.rotateDeg));
                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_SCALE, prop::PROP_SCALE,
                    [](StagingKfFrame &k) -> double& { return k.sx; }, ctx.baseScaleX);
                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_SCALE, prop::PROP_SCALE,
                    [](StagingKfFrame &k) -> double& { return k.sy; }, ctx.baseScaleY);
                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_LEFT, prop::PROP_TOP,
                    [](StagingKfFrame &k) -> double& { return k.unscaledX; }, ctx.geometry[0]);
                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_LEFT, prop::PROP_TOP,
                    [](StagingKfFrame &k) -> double& { return k.unscaledY; }, ctx.geometry[1]);
                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_WIDTH, prop::PROP_HEIGHT,
                    [](StagingKfFrame &k) -> double& { return k.unscaledW; }, ctx.geometry[2]);
                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_WIDTH, prop::PROP_HEIGHT,
                    [](StagingKfFrame &k) -> double& { return k.unscaledH; }, ctx.geometry[3]);

                for (u32 index = 0; index < ctx.keyframeCount; ++index)
                {
                    StagingKfFrame &frame = frames[index];
                    frame.spatial.w = static_cast<float>(frame.unscaledW * frame.sx);
                    frame.spatial.h = static_cast<float>(frame.unscaledH * frame.sy);
                    frame.spatial.x = static_cast<float>(frame.stageOriginX + (frame.unscaledX - frame.stageOriginX) * frame.sx);
                    frame.spatial.y = static_cast<float>(frame.stageOriginY + (frame.unscaledY - frame.stageOriginY) * frame.sy);
                    frame.spatial.scale = static_cast<float>(frame.sx);
                }
            }

            if (hasStyleColorGroup)
            {
                const auto &scBase = baseSentinel.styleColor;
                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_FILL, prop::PROP_FILL,
                    [](StagingKfFrame &k) -> float& { return k.styleColor.fillColor.r; }, scBase.fillColor.r);
                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_FILL, prop::PROP_FILL,
                    [](StagingKfFrame &k) -> float& { return k.styleColor.fillColor.g; }, scBase.fillColor.g);
                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_FILL, prop::PROP_FILL,
                    [](StagingKfFrame &k) -> float& { return k.styleColor.fillColor.b; }, scBase.fillColor.b);
                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_FILL, prop::PROP_FILL,
                    [](StagingKfFrame &k) -> float& { return k.styleColor.fillColor.a; }, scBase.fillColor.a);

                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_STROKE_COLOR, prop::PROP_STROKE_COLOR,
                    [](StagingKfFrame &k) -> float& { return k.styleColor.strokeColor.r; }, scBase.strokeColor.r);
                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_STROKE_COLOR, prop::PROP_STROKE_COLOR,
                    [](StagingKfFrame &k) -> float& { return k.styleColor.strokeColor.g; }, scBase.strokeColor.g);
                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_STROKE_COLOR, prop::PROP_STROKE_COLOR,
                    [](StagingKfFrame &k) -> float& { return k.styleColor.strokeColor.b; }, scBase.strokeColor.b);
                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_STROKE_COLOR, prop::PROP_STROKE_COLOR,
                    [](StagingKfFrame &k) -> float& { return k.styleColor.strokeColor.a; }, scBase.strokeColor.a);

                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_STROKE_WIDTH, prop::PROP_STROKE_WIDTH,
                    [](StagingKfFrame &k) -> float& { return k.styleColor.strokeWidth; }, scBase.strokeWidth);
                fuseStagingScalar(frames.data(), ctx.keyframeCount, prop::PROP_BLUR, prop::PROP_BLUR,
                    [](StagingKfFrame &k) -> float& { return k.styleColor.blurRadius; }, 0.0f);
            }

            KeyframeCompileResult res{};
            u8 colorKfMask = 0;
            for (u32 index = 0; index < ctx.keyframeCount; ++index)
            {
                const StagingKfFrame &frame = frames[index];
                if (hasTypographyGroup && frame.presentMask.test_bit(prop::PROP_CONTENT))
                    res.contentMask |= (1u << index);

                if (hasSpatialGroup)
                {
                    const bool frameHasSpatial = frame.presentMask.test_bit(prop::PROP_OPACITY) ||
                                                 frame.presentMask.test_bit(prop::PROP_ROTATE) ||
                                                 frame.presentMask.test_bit(prop::PROP_SCALE) ||
                                                 frame.presentMask.test_bit(prop::PROP_LEFT) ||
                                                 frame.presentMask.test_bit(prop::PROP_TOP) ||
                                                 frame.presentMask.test_bit(prop::PROP_WIDTH) ||
                                                 frame.presentMask.test_bit(prop::PROP_HEIGHT);
                    if (frameHasSpatial)
                    {
                        res.hasSpatial = 1;
                        if (frame.spatial.x != baseSentinel.spatial.x) res.kfProps |= (1ULL << prop::PROP_LEFT);
                        if (frame.spatial.y != baseSentinel.spatial.y) res.kfProps |= (1ULL << prop::PROP_TOP);
                        if (frame.spatial.w != baseSentinel.spatial.w) res.kfProps |= (1ULL << prop::PROP_WIDTH);
                        if (frame.spatial.h != baseSentinel.spatial.h) res.kfProps |= (1ULL << prop::PROP_HEIGHT);
                        if (frame.spatial.rotateDeg != baseSentinel.spatial.rotateDeg) res.kfProps |= (1ULL << prop::PROP_ROTATE);
                        if (frame.spatial.scale != baseSentinel.spatial.scale) res.kfProps |= (1ULL << prop::PROP_SCALE);
                        if (frame.spatial.opacity != baseSentinel.spatial.opacity) res.kfProps |= (1ULL << prop::PROP_OPACITY);
                    }
                }
                if (hasStyleColorGroup)
                {
                    if (frame.styleColor.fillColor != baseSentinel.styleColor.fillColor || frame.presentMask.test_bit(prop::PROP_FILL))
                    {
                        res.kfProps |= (1ULL << prop::PROP_COLOR);
                        colorKfMask |= 1u;
                    }
                    if (frame.styleColor.strokeColor != baseSentinel.styleColor.strokeColor || frame.presentMask.test_bit(prop::PROP_STROKE_COLOR))
                    {
                        res.kfProps |= (1ULL << prop::PROP_STROKE_COLOR);
                        colorKfMask |= 2u;
                    }
                    if (frame.styleColor.strokeWidth != baseSentinel.styleColor.strokeWidth || frame.presentMask.test_bit(prop::PROP_STROKE_WIDTH))
                    {
                        res.kfProps |= (1ULL << prop::PROP_STROKE_WIDTH);
                        colorKfMask |= 2u;
                    }
                    if (frame.styleColor.blurRadius != baseSentinel.styleColor.blurRadius || frame.presentMask.test_bit(prop::PROP_BLUR))
                    {
                        res.kfProps |= (1ULL << prop::PROP_BLUR);
                        colorKfMask |= 4u;
                    }
                }
                if (hasTypographyGroup && frame.presentMask.test_bit(prop::PROP_CONTENT))
                {
                    res.kfProps |= (1ULL << prop::PROP_CONTENT);
                }
            }

            MultiArray<NativeKeyframe, MAX_KEYFRAMES_PER_SLOT> spatialArr;
            MultiArray<StyleColorKeyframe, MAX_KEYFRAMES_PER_SLOT> styleColorArr;
            for (u32 i = 0; i < ctx.keyframeCount; ++i)
            {
                frames[i].spatial.offsetMs = frames[i].offsetMs;
                spatialArr[i] = frames[i].spatial;
                styleColorArr[i] = frames[i].styleColor;
            }
            ctx.position->set_staged_keyframes(
                ctx.slot,
                hasSpatialGroup ? spatialArr.data() : nullptr,
                hasStyleColorGroup ? styleColorArr.data() : nullptr,
                ctx.keyframeCount,
                ctx.easing,
                baseSentinel.spatial,
                baseSentinel.styleColor,
                colorKfMask,
                res.kfProps);

            if (hasTypographyGroup && ctx.hasKeyframeContent && ctx.fullTextBuf && ctx.scratchBuf &&
                ctx.fullTextLen && ctx.charKfMap && ctx.totalChars)
            {
                for (u32 k = 0; k < ctx.keyframeCount; ++k)
                {
                    if (frames[k].presentMask.test_bit(prop::PROP_CONTENT) &&
                        ctx.json.readStringValue(frames[k].contentStart, frames[k].contentEnd, ctx.scratchBuf, ctx.scratchCap))
                    {
                        appendTextSpan<true>(
                            ctx.scratchBuf, static_cast<u16>(k),
                            ctx.fullTextBuf, ctx.fullTextCap, *ctx.fullTextLen,
                            ctx.charKfMap, ctx.charKfCap, *ctx.totalChars);
                    }
                }
                if (*ctx.fullTextLen < ctx.fullTextCap)
                    ctx.fullTextBuf[*ctx.fullTextLen] = '\0';
            }

            return res;
        }
    } // namespace
    CompileResult compileProject(
        const char *source,
        u32 length,
        const CompileOptions &options)
    {
        CompileResult result{
            {-1.0, 0, 0}, PROJECT_COMPILE_INVALID_JSON, 0, 0, 0};
        outputLength = 0;
        templateLength = 0;
        sceneNodeCount = 0;
        sceneRootCount = 0;
        sceneLeafCount = 0;
        sceneGroupContextCount = 0;
        if (!source || !length)
            return result;
        const JsonReader json(source, length);
        if (!json.isObjectDocument())
            return result;
        u32 timelineStart = 0;
        u32 timelineEnd = 0;
        if (!json.field(0, "timeline", timelineStart, timelineEnd))
        {
            result.status = PROJECT_COMPILE_MISSING_TIMELINE;
            return result;
        }
        if (!countArrayValues(
                json, timelineStart, timelineEnd, result.topLevelItemCount))
        {
            result.status = PROJECT_COMPILE_INVALID_TIMELINE;
            return result;
        }
        s_sourceJsonPtr = source;
        s_sourceJsonLen = length;
        extractTemporalStyleScopes(json, timelineStart, timelineEnd);
        const double stageWidth = std::isfinite(options.stageWidth) &&
                                          options.stageWidth > 0.0
                                      ? options.stageWidth
                                      : DEFAULT_STAGE_WIDTH;
        const double stageHeight = std::isfinite(options.stageHeight) &&
                                           options.stageHeight > 0.0
                                       ? options.stageHeight
                                       : DEFAULT_STAGE_HEIGHT;
        if (!expandSceneGroups(
                json, timelineStart, timelineEnd,
                stageWidth, stageHeight,
                result.sceneLeafCount))
        {
            result.status = PROJECT_COMPILE_SCENE_CAPACITY_EXCEEDED;
            return result;
        }
        if (!buildTemplateIndex(json, 0, result.templateCount))
        {
            result.status = PROJECT_COMPILE_TEMPLATE_CAPACITY_EXCEEDED;
            return result;
        }
        result.duration = computeExpandedSceneDuration(json);
        if (result.duration.durationMs < 0.0)
        {
            result.status = PROJECT_COMPILE_INVALID_TIMELINE;
            return result;
        }
        if (!emitCompiledItems(
                json, result.duration.durationMs, result.status))
            return result;
        if (outputLength < PROJECT_COMPILE_OUTPUT_CAPACITY)
            outputJson[outputLength] = '\0';
        result.status = PROJECT_COMPILE_OK;
        return result;
    }
    const CompiledSceneNode *compiledSceneNodes()
    {
        return sceneNodes.data();
    }
    const char *compiledItemsJson()
    {
        return outputJson.data();
    }
    u32 compiledItemsJsonLength()
    {
        return outputLength;
    }
    void configureCompiledAudioTracks(double projectDurationMs)
    {
        if (!outputLength || !std::isfinite(projectDurationMs) ||
            projectDurationMs < 0.0 ||
            projectDurationMs > static_cast<double>(std::numeric_limits<i64>::max()))
            return;
        const JsonReader json(outputJson.data(), outputLength);
        if (!json.isArray(0, outputLength))
            return;
        MultiArray<CompiledAudioTrackSetup, MAX_COMPILED_AUDIO_CLIPS> setups;
        u32 setupCount = 0;
        u32 slot = 0;
        u32 cursor = 1;
        while (cursor < outputLength - 1)
        {
            u32 itemStart = 0;
            u32 itemEnd = 0;
            if (!json.nextArrayValue(cursor, outputLength - 1, itemStart, itemEnd) ||
                !json.isObject(itemStart, itemEnd))
                break;
            const bool isAudio = json.stringFieldEquals(itemStart, "type", "audio");
            const bool isVideo = json.stringFieldEquals(itemStart, "type", "video");
            if (isAudio || isVideo)
            {
                if (slot >= MAX_COMPILED_AUDIO_CLIPS ||
                    setupCount >= MAX_COMPILED_AUDIO_CLIPS ||
                    !collectAudioTrackSetup(
                        json, itemStart, slot, setups[setupCount]))
                {
                    ++slot;
                    continue;
                }
                ++setupCount;
            }
            ++slot;
        }
        audio_tracks_reset();
        const i64 masterEnd = projectDurationMs > 0.0
                                  ? static_cast<i64>(std::llround(projectDurationMs))
                                  : static_cast<i64>(FALLBACK_DURATION_1H_MS);
        audio_track_configure<MediaType::Audio>(
            MAX_AUDIO_TRACKS - 1, 0, masterEnd, 0.0f, 1.0f, 0);
        for (u32 i = 0; i < setupCount; ++i)
        {
            const CompiledAudioTrackSetup &setup = setups[i];
            if (setup.type == MediaType::Audio)
            {
                audio_track_configure<MediaType::Audio>(
                    setup.slot, setup.startMs, setup.endMs,
                    setup.trimOffsetSec, setup.baseVolume, setup.keyframeCount);
            }
            else
            {
                audio_track_configure<MediaType::Video>(
                    setup.slot, setup.startMs, setup.endMs,
                    setup.trimOffsetSec, setup.baseVolume, setup.keyframeCount);
            }
            for (u32 keyframe = 0; keyframe < setup.keyframeCount; ++keyframe)
            {
                audio_track_set_vol_kf(
                    setup.slot, keyframe,
                    setup.keyframes[keyframe].offsetMs,
                    setup.keyframes[keyframe].volume);
            }
        }
    }

    namespace {
        MerkleDiffTable<MAX_TIMELINE_SLOTS> s_slotMerkle{};
        MultiArray<u64, MAX_TIMELINE_SLOTS> s_slotTextContentHash{};
        double s_lastMerkleStageW = 0.0;
        double s_lastMerkleStageH = 0.0;
    } // namespace

    void resetCompiledMerkleTable() noexcept {
        s_slotMerkle.reset_all();
        s_slotTextContentHash = {};
        s_lastMerkleStageW = 0.0;
        s_lastMerkleStageH = 0.0;
    }

    template <bool Incremental>
    IncrementalPatchReport configureCompiledTimelinePosition_T(double stageWidth, double stageHeight)
    {
        IncrementalPatchReport report{};
        if (!outputLength || !std::isfinite(stageWidth) ||
            !std::isfinite(stageHeight) || stageWidth <= 0.0 || stageHeight <= 0.0)
            return report;
        const JsonReader json(outputJson.data(), outputLength);
        if (!json.isArray(0, outputLength))
            return report;
        EngineManager &manager = engineManager();
        Position<> *position = timeline_position_get_instance();
        char *setupInput = manager.timelineSetupInputBuffer();
        MultiArray<char, MAX_CSS_DECLARATION_TEXT> contentScratch;
        contentScratch[0] = '\0';
        const JsonReader sourceJson(s_sourceJsonPtr ? s_sourceJsonPtr : outputJson.data(), s_sourceJsonPtr ? s_sourceJsonLen : outputLength);
        u64 styleContextHash = hzobrist_mix64(
            static_cast<u64>(__builtin_bit_cast(u32, static_cast<float>(stageWidth))) ^
            (static_cast<u64>(__builtin_bit_cast(u32, static_cast<float>(stageHeight))) << 32)
        );
        if (position)
        {
            ItemScannedProps ruleScanProps;
            position->clear_style_segments();
            for (u32 s = 0; s < s_styleScopeCount; ++s)
            {
                const auto &scope = s_styleScopes[s];
                MultiArray<TextBlock, MEDIA_TYPE_NB> presets;
                u16 typesMask = scope.targetTypesBB;
                for (u32 t = 0; t < MEDIA_TYPE_NB; ++t)
                {
                    presets[t].clear();
                    presets[t].set_value<text_reg::OPACITY>(1.0f);
                    presets[t].set_value<text_reg::LINE_HEIGHT>(1.25f);
                    presets[t].set_value<text_reg::FONT_WEIGHT>(400u);
                    if (scope.byType[t].targetType != MediaType::None)
                    {
                        const u32 rStart = scope.byType[t].ruleStart;
                        const u32 rEnd = scope.byType[t].ruleEnd;
                        TextBlock &preset = presets[t];

                        scanScenePropsSinglePass<false>(sourceJson, rStart, rEnd, ruleScanProps);

                        // 1. Color / Background Color / Fill
                        if (t == static_cast<u32>(MediaType::Background)) {
                            if (ruleScanProps.has(SP_BG_COLOR)) {
                                preset.set_preset_value<text_reg::COLOR>(parseStagedTimelineColor(manager, setupInput, ruleScanProps.cstr(SP_BG_COLOR), RGBA_OPAQUE_BLACK));
                                preset.set_value<0, 1>(true);
                            } else if (ruleScanProps.has(SP_BG)) {
                                preset.set_preset_value<text_reg::COLOR>(parseStagedTimelineColor(manager, setupInput, ruleScanProps.cstr(SP_BG), RGBA_OPAQUE_BLACK));
                                preset.set_value<0, 1>(true);
                            } else if (ruleScanProps.has(SP_COLOR)) {
                                preset.set_preset_value<text_reg::COLOR>(parseStagedTimelineColor(manager, setupInput, ruleScanProps.cstr(SP_COLOR), RGBA_OPAQUE_BLACK));
                                preset.set_value<0, 1>(true);
                            }
                        } else {
                            if (ruleScanProps.has(SP_FILL)) {
                                preset.set_preset_value<text_reg::COLOR>(parseStagedTimelineColor(manager, setupInput, ruleScanProps.cstr(SP_FILL), RGBA_OPAQUE_WHITE));
                            } else if (ruleScanProps.has(SP_COLOR)) {
                                preset.set_preset_value<text_reg::COLOR>(parseStagedTimelineColor(manager, setupInput, ruleScanProps.cstr(SP_COLOR), RGBA_OPAQUE_WHITE));
                            }
                            if (ruleScanProps.has(SP_BG_COLOR)) {
                                preset.set_preset_value<text_reg::BG_COLOR>(parseStagedTimelineColor(manager, setupInput, ruleScanProps.cstr(SP_BG_COLOR), 0u));
                            } else if (ruleScanProps.has(SP_BG)) {
                                preset.set_preset_value<text_reg::BG_COLOR>(parseStagedTimelineColor(manager, setupInput, ruleScanProps.cstr(SP_BG), 0u));
                            }
                        }

                        // 2. Stroke / Stroke Color & Width
                        if (ruleScanProps.has(SP_STROKE_COL) || ruleScanProps.has(SP_STROKE)) {
                            const char *strokeTxt = ruleScanProps.has(SP_STROKE_COL) ? ruleScanProps.cstr(SP_STROKE_COL) : ruleScanProps.cstr(SP_STROKE);
                            preset.set_preset_value<text_reg::STROKE_COLOR>(parseStagedTimelineColor(manager, setupInput, strokeTxt, RGBA_OPAQUE_BLACK));
                        }
                        if (ruleScanProps.has(SP_STROKE_W)) {
                            preset.set_preset_value<text_reg::STROKE_WIDTH>(static_cast<float>(ruleScanProps.lead_num(SP_STROKE_W, 1.0)));
                        }

                        // 3. Opacity
                        if (ruleScanProps.has(SP_OPACITY)) {
                            preset.set_preset_value<text_reg::OPACITY>(static_cast<float>(ruleScanProps.lead_num(SP_OPACITY, 1.0)));
                        }

                        // 4. Typography
                        if (ruleScanProps.has(SP_FONT_SIZE)) {
                            preset.set_preset_value<text_reg::FONT_SIZE>(static_cast<float>(ruleScanProps.scene_len(SP_FONT_SIZE, stageHeight, 32.0)));
                        }
                        if (ruleScanProps.has(SP_LETTER_SPACING)) {
                            preset.set_preset_value<text_reg::LETTER_SPACING>(static_cast<float>(ruleScanProps.lead_num(SP_LETTER_SPACING, 0.0)));
                        }
                        if (ruleScanProps.has(SP_LINE_HEIGHT)) {
                            preset.set_preset_value<text_reg::LINE_HEIGHT>(static_cast<float>(ruleScanProps.lead_num(SP_LINE_HEIGHT, 1.25)));
                        }
                        if (ruleScanProps.has(SP_FONT_WEIGHT)) {
                            preset.set_preset_value<text_reg::FONT_WEIGHT>(static_cast<u32>(ruleScanProps.lead_num(SP_FONT_WEIGHT, 400.0)));
                        }
                        if (ruleScanProps.has(SP_FONT_STYLE) && std::strstr(ruleScanProps.cstr(SP_FONT_STYLE), "italic") != nullptr) {
                            preset.set_value<text_bit::IS_ITALIC, 1>(true);
                        }
                        if (ruleScanProps.has(SP_TEXT_ALIGN)) {
                            const char *ta = ruleScanProps.cstr(SP_TEXT_ALIGN);
                            preset.set_value<text_bit::TEXT_ALIGN_LO, 2>(cstr_equal(ta, "center") ? 1u : (cstr_equal(ta, "right") ? 2u : 0u));
                        }
                        if (ruleScanProps.has(SP_TEXT_TRANSFORM)) {
                            const char *tt = ruleScanProps.cstr(SP_TEXT_TRANSFORM);
                            preset.set_value<text_bit::TEXT_TRANSFORM_LO, 2>(cstr_equal(tt, "uppercase") ? 1u : (cstr_equal(tt, "lowercase") ? 2u : 0u));
                        }
                        if (ruleScanProps.has(SP_FONT_FAMILY)) {
                            preset.set_preset_value<text_reg::FONT_HASH>(hash31_str(ruleScanProps.cstr(SP_FONT_FAMILY)));
                        }

                        // 5. Filter
                        if (ruleScanProps.has(SP_FILTER)) {
                            preset.set_preset_value<text_reg::RESOLVED_FONT_ID>(static_cast<u32>(ruleScanProps.lead_num(SP_FILTER, 0.0)));
                        }
                    }
                }
                const u32 textIdx = static_cast<u32>(MediaType::Text);
                const u32 lyricIdx = static_cast<u32>(MediaType::Lyric);
                if ((typesMask & (1u << textIdx)) != 0 && (typesMask & (1u << lyricIdx)) == 0)
                {
                    presets[lyricIdx] = presets[textIdx];
                    typesMask |= static_cast<u16>(1u << lyricIdx);
                }
                else if ((typesMask & (1u << lyricIdx)) != 0 && (typesMask & (1u << textIdx)) == 0)
                {
                    presets[textIdx] = presets[lyricIdx];
                    typesMask |= static_cast<u16>(1u << textIdx);
                }
                if ((typesMask & (1u << static_cast<u32>(MediaType::Background))) != 0)
                {
                    presets[static_cast<u32>(MediaType::Root)] = presets[static_cast<u32>(MediaType::Background)];
                    typesMask |= static_cast<u16>(1u << static_cast<u32>(MediaType::Root));
                }
                const Key segZobrist = hzobrist_mix64(static_cast<u64>(scope.startMs) ^
                                                (static_cast<u64>(scope.endMs) * SPLITMIX_MUL1) ^
                                                (static_cast<u64>(presets[static_cast<u32>(MediaType::Text)].get_value<u32, text_reg::COLOR>()) * SPLITMIX_MUL2) ^
                                                (static_cast<u64>(presets[static_cast<u32>(MediaType::Background)].get_value<u32, text_reg::COLOR>()) * 0x9e3779b97f4a7c15ULL) ^
                                                (presets[static_cast<u32>(MediaType::Text)].active.words[0]));
                styleContextHash = hzobrist_mix64(styleContextHash ^ segZobrist);
                position->add_style_segment(
                    static_cast<TimePoint>(scope.startMs),
                    static_cast<TimePoint>(scope.endMs),
                    presets.data(),
                    typesMask,
                    segZobrist);
            }
        }
        const bool contextChanged = s_slotMerkle.begin_diff(styleContextHash) ||
                                    (s_lastMerkleStageW != stageWidth) ||
                                    (s_lastMerkleStageH != stageHeight);
        const bool canSkipUnchanged = Incremental && !contextChanged && (s_slotMerkle.prevCount > 0u);
        report.fullRebuild = canSkipUnchanged ? 0u : 1u;
        s_lastMerkleStageW = stageWidth;
        s_lastMerkleStageH = stageHeight;

        timeline::runtime::RuntimeLoadStatus fxStatus = timeline::runtime::RUNTIME_LOAD_OK;
        const bool deferFxStage = canSkipUnchanged;
        bool fxStageBegun = false;
        if (!deferFxStage)
        {
            projectRuntime().begin_slot_stage(
                stageWidth, stageHeight,
                position ? static_cast<double>(position->duration()) : 0.0);
            fxStageBegun = true;
        }
        bool anyFxSlotChanged = !canSkipUnchanged;
        ItemScannedProps itemProps;
        ItemScannedProps ruleProps;
        const StyleRuleTargetOffset *lastMatchedRule = nullptr;
        const u32 totalSpans = min(s_compiledItemCount, static_cast<u32>(MAX_TIMELINE_SLOTS));
        u32 slot = 0;
        for (u32 spanIdx = 0u; spanIdx < totalSpans && slot < MAX_TIMELINE_SLOTS; ++spanIdx)
        {
            const u32 itemStart = s_compiledItemStarts[spanIdx];
            const u32 itemEnd   = s_compiledItemEnds[spanIdx];
            if (!json.isObject(itemStart, itemEnd))
                break;

            // PHA 2: INCREMENTAL MERKLE-AST O(1) SPAN HASH CHECK (0 nextArrayValue scan, 0 re-parse for unchanged slots!)
            const u64 spanHash = MerkleDiffTable<MAX_TIMELINE_SLOTS>::hash_span_64(
                outputJson.data() + itemStart, itemEnd - itemStart, styleContextHash
            );
            const bool slotChanged = s_slotMerkle.update_slot(slot, spanHash);
            if (canSkipUnchanged && !slotChanged)
            {
                if (fxStageBegun) projectRuntime().stage_slot_reuse(slot);
                ++report.skippedSlotCount;
                ++slot;
                continue;
            }
            const TextBlock oldTextBlk = (Incremental && position) ? position->slot_text_block(slot) : TextBlock{};
            const u64 oldContentHash = Incremental ? s_slotTextContentHash[slot] : 0ULL;
            ++report.updatedSlotCount;

            double topLevel = static_cast<double>(slot);
            json.numberField(itemStart, "_topLevelIndex", topLevel);
            double isChild = 0.0;
            json.numberField(itemStart, "_isGroupChild", isChild);
            position->set_slot_top_level(slot, static_cast<u16>(topLevel), isChild > 0.5);

            MultiArray<char, MAX_TEMPLATE_TYPE> typeName{};
            if (!readStringField(
                    json, itemStart, "type", typeName.data(), static_cast<u32>(typeName.size())))
                continue;
            const MediaType mediaType = make_media_type(typeName.data());
            const bool isEffect = cstr_equal(typeName.data(), "effect");
            if (mediaType == MediaType::None && !isEffect)
                continue;
            double startMs = 0.0;
            double endMs = 0.0;
            double durationMs = 0.0;
            double zIndex = 0.0;
            if (!json.numberField(itemStart, "start", startMs) ||
                !json.numberField(itemStart, "end", endMs) ||
                !readOptionalNumber(json, itemStart, "durationMs", std::max(0.0, endMs - startMs), durationMs) ||
                !readOptionalNumber(json, itemStart, "z-index", 0.0, zIndex) ||
                startMs < 0.0 || endMs < startMs || durationMs < 0.0) { continue; }

            // Ghi nhận slot vào assignedSlotsBB của TemplateNode tương ứng
            MultiArray<char, MAX_TEMPLATE_NAME> itemClass{};
            if (readFirstStringField(json, itemStart, "class", "className", itemClass.data(), static_cast<u32>(itemClass.size())))
            {
                trim_cstr_inplace(itemClass.data());
                TemplateNode* tNode = TemplateArena::instance().find_template(itemClass.data());
                if (tNode && slot < MAX_TIMELINE_SLOTS)
                {
                    tNode->assignedSlotsBB |= (1ULL << slot);
                }
            }
            if (readStringField(json, itemStart, "template", itemClass.data(), static_cast<u32>(itemClass.size())))
            {
                trim_cstr_inplace(itemClass.data());
                TemplateNode* tNode = TemplateArena::instance().find_template(itemClass.data());
                if (tNode && slot < MAX_TIMELINE_SLOTS)
                {
                    tNode->assignedSlotsBB |= (1ULL << slot);
                }
            }
            const bool isText = mediaType == MediaType::Text ||
                                mediaType == MediaType::Lyric;
            scanScenePropsSinglePass<false>(json, itemStart, itemEnd, itemProps);
            const bool hasLocalColor = itemProps.has(SP_COLOR);
            const bool hasLocalBg = itemProps.has(SP_BG_COLOR) || itemProps.has(SP_BG);
            const StyleRuleTargetOffset* matchedRule = findMatchingStyleRule(mediaType, startMs);
            if (matchedRule && matchedRule != lastMatchedRule)
            {
                scanScenePropsSinglePass<false>(sourceJson, matchedRule->ruleStart, matchedRule->ruleEnd, ruleProps);
                lastMatchedRule = matchedRule;
            }
            const auto hasEffective = [&](ScannedPropSlot s) noexcept -> bool
            {
                return (matchedRule && ruleProps.has(s)) || itemProps.has(s);
            };
            const auto effectiveCStr = [&](ScannedPropSlot s) noexcept -> const char *
            {
                return (matchedRule && ruleProps.has(s)) ? ruleProps.cstr(s) : itemProps.cstr(s);
            };
            const auto effectiveSpan = [&](ScannedPropSlot s) noexcept -> StringSpan
            {
                return (matchedRule && ruleProps.has(s)) ? ruleProps.span(s) : itemProps.span(s);
            };
            const auto effectiveLeadNum = [&](ScannedPropSlot s, double fallback) noexcept -> double
            {
                return (matchedRule && ruleProps.has(s)) ? ruleProps.lead_num(s, fallback) : itemProps.lead_num(s, fallback);
            };
            const bool hasColor = (matchedRule && (ruleProps.has(SP_COLOR) || ruleProps.has(SP_FILL))) || itemProps.has(SP_COLOR);
            const char *colorText = (matchedRule && ruleProps.has(SP_COLOR)) ? ruleProps.cstr(SP_COLOR)
                                  : (matchedRule && ruleProps.has(SP_FILL))  ? ruleProps.cstr(SP_FILL)
                                                                             : itemProps.cstr(SP_COLOR);
            const bool hasStrokeColor = (matchedRule && (ruleProps.has(SP_STROKE_COL) || ruleProps.has(SP_STROKE))) || itemProps.has(SP_STROKE_COL);
            const char *strokeColorText = (matchedRule && ruleProps.has(SP_STROKE_COL)) ? ruleProps.cstr(SP_STROKE_COL)
                                        : (matchedRule && ruleProps.has(SP_STROKE))     ? ruleProps.cstr(SP_STROKE)
                                                                                        : itemProps.cstr(SP_STROKE_COL);

            const double fontSize = effectiveLeadNum(SP_FONT_SIZE, DEFAULT_FONT_SIZE);
            const double lineHeight = effectiveLeadNum(SP_LINE_HEIGHT, DEFAULT_LINE_HEIGHT);
            const bool hasOpacity = hasEffective(SP_OPACITY);
            double parsedBaseOpacity = effectiveLeadNum(SP_OPACITY, 1.0);
            if (hasOpacity && parsedBaseOpacity > 1.0)
                parsedBaseOpacity /= 100.0;
            const float baseOpacity = static_cast<float>(std::clamp(parsedBaseOpacity, 0.0, 1.0));
            u32 keyframesStart = 0;
            u32 keyframesEnd = 0;
            u32 keyframeCount = 0;
            const bool hasKeyframes = json.field(
                itemStart, "keyframes", keyframesStart, keyframesEnd);
            if (hasKeyframes && !countArrayValues(
                                    json, keyframesStart, keyframesEnd, keyframeCount))
                keyframeCount = 0;
            if (keyframeCount > MAX_KEYFRAMES_PER_SLOT)
                keyframeCount = MAX_KEYFRAMES_PER_SLOT;
            struct TextMeasure
            {
                u32 lineCount = 1;
                u32 currentLine = 0;
                u32 maxLineChars = 0;
            } measure;
            const auto appendTextMeasure = [&](u32 textStart, u32 textEnd)
            {
                if (!json.readStringValue(
                        textStart, textEnd, contentScratch.data(), static_cast<u32>(contentScratch.size())))
                    return;
                for (u32 index = 0; contentScratch[index];)
                {
                    const u8 first = static_cast<u8>(contentScratch[index]);
                    if (first == '\n')
                    {
                        measure.maxLineChars = std::max(
                            measure.maxLineChars, measure.currentLine);
                        measure.currentLine = 0;
                        ++measure.lineCount;
                        ++index;
                        continue;
                    }
                    u32 byteCount = 1;
                    u32 codeUnits = 1;
                    utf8_lead_byte_info(first, byteCount, codeUnits);
                    measure.currentLine += codeUnits;
                    index += byteCount;
                }
            };
            u32 contentStart = 0;
            u32 contentEnd = 0;
            bool hasContent = json.field(itemStart, "content", contentStart, contentEnd);
            if (!hasContent)
                hasContent = json.field(itemStart, "text", contentStart, contentEnd);
            if (hasContent)
                appendTextMeasure(contentStart, contentEnd);
            bool hasKeyframeContent = false;
            if (keyframeCount)
            {
                u32 keyCursor = keyframesStart + 1;
                for (u32 keyIndex = 0; keyIndex < keyframeCount; ++keyIndex)
                {
                    u32 keyframeStart = 0;
                    u32 keyframeEnd = 0;
                    if (!json.nextArrayValue(
                            keyCursor, keyframesEnd, keyframeStart, keyframeEnd))
                        break;
                    if (json.field(keyframeStart, "content", contentStart, contentEnd))
                    {
                        hasKeyframeContent = true;
                        appendTextMeasure(contentStart, contentEnd);
                    }
                }
            }
            measure.maxLineChars = std::max(
                measure.maxLineChars, measure.currentLine);
            MultiArray<char, MAX_SHORT_TEXT> revealMode{};
            copy_cstr_bounded(revealMode.data(), revealMode.size(), "none");
            if (readStringField(
                    json, itemStart, "revealMode", contentScratch.data(),
                    static_cast<u32>(contentScratch.size())) &&
                (cstr_equal(contentScratch.data(), "add") || cstr_equal(contentScratch.data(), "display")))
            {
                copy_cstr_bounded(revealMode.data(), revealMode.size(), contentScratch.data());
            }
            if (hasKeyframeContent && cstr_equal(revealMode.data(), "none"))
                copy_cstr_bounded(revealMode.data(), revealMode.size(), "add");
            const bool hasWidth = itemProps.has(SP_WIDTH);
            const bool hasHeight = itemProps.has(SP_HEIGHT);
            const bool hasLeft = itemProps.has(SP_LEFT);
            const bool hasTop = itemProps.has(SP_TOP);
            const u32 geometryFlags =
                (hasWidth ? TIMELINE_GEOMETRY_HAS_WIDTH : 0u) |
                (hasHeight ? TIMELINE_GEOMETRY_HAS_HEIGHT : 0u) |
                (hasLeft ? TIMELINE_GEOMETRY_HAS_LEFT : 0u) |
                (hasTop ? TIMELINE_GEOMETRY_HAS_TOP : 0u);

            double intrinsicWidth = stageWidth;
            double intrinsicHeight = stageHeight;
            if (isText)
            {
                if (measure.maxLineChars)
                {
                    const double estimated = __builtin_ceil(measure.maxLineChars * (fontSize * 0.47));
                    intrinsicWidth = stockcut::min(stageWidth, stockcut::max(fontSize * 1.5, estimated));
                }
                else
                {
                    intrinsicWidth = stockcut::min(stageWidth, stockcut::max(fontSize * 2.0, 200.0));
                }
                intrinsicHeight = stockcut::max(1.0, measure.lineCount * fontSize * lineHeight);
            }

            u32 inputOffset = 0;
            const auto stageBagText = [&](ScannedPropSlot s)
            {
                if (!itemProps.has(s))
                    return 0u;
                const u32 length = itemProps.textLen[s];
                if (inputOffset + length > TIMELINE_SETUP_INPUT_CAPACITY)
                    return 0u;
                std::memcpy(setupInput + inputOffset, itemProps.text[s].data(), length);
                inputOffset += length;
                return length;
            };
            const u32 widthLength = stageBagText(SP_WIDTH);
            const u32 heightLength = stageBagText(SP_HEIGHT);
            const u32 leftLength = stageBagText(SP_LEFT);
            const u32 topLength = stageBagText(SP_TOP);
            const u32 transformLength = stageBagText(SP_TRANSFORM);
            const u32 translateLength = stageBagText(SP_TRANSLATE);
            double *geometry = manager.computeTimelineGeometry(
                geometryFlags, stageWidth, stageHeight,
                intrinsicWidth, intrinsicHeight,
                widthLength, heightLength, leftLength, topLength,
                transformLength, translateLength);
            if (!geometry)
                continue;
            double spin = 0.0;
            double volume = 1.0;
            double zValue = zIndex;
            json.numberField(itemStart, "spin", spin);
            if (!json.numberField(itemStart, "volume", volume) && matchedRule)
            {
                sourceJson.numberField(matchedRule->ruleStart, "volume", volume);
            }
            if (!std::isfinite(spin) || !std::isfinite(volume) ||
                zValue < static_cast<double>(std::numeric_limits<i32>::min()) ||
                zValue > static_cast<double>(std::numeric_limits<i32>::max()))
            {
                spin = 0.0;
                volume = 1.0;
                zValue = 0.0;
            }
            bool hasRotate = hasEffective(SP_ROTATE);
            double rotate = effectiveLeadNum(SP_ROTATE, 0.0);
            if (!hasRotate && itemProps.has(SP_TRANSFORM))
            {
                if (extractRotateDegFromTransformCstr(itemProps.cstr(SP_TRANSFORM), rotate))
                    hasRotate = true;
            }

            CssTransformOrigin baseOrigin;
            if (hasEffective(SP_ORIGIN_X))
            {
                parse_css_transform_origin(effectiveSpan(SP_ORIGIN_X), baseOrigin);
            }
            double baseScaleX = 1.0, baseScaleY = 1.0;
            if (hasEffective(SP_SCALE))
            {
                parse_css_scale_property(effectiveSpan(SP_SCALE), baseScaleX, baseScaleY);
            }
            if (itemProps.has(SP_TRANSFORM))
            {
                parse_css_transform_scale(itemProps.span(SP_TRANSFORM), baseScaleX, baseScaleY);
            }
            double basePxX = 0.0, basePxY = 0.0;
            baseOrigin.resolve(geometry[2], geometry[3], basePxX, basePxY);
            const double stageOriginX = geometry[0] + basePxX;
            const double stageOriginY = geometry[1] + basePxY;
            position->set_slot_custom_origin(slot, static_cast<float>(stageOriginX), static_cast<float>(stageOriginY));

            if (!keyframeCount && (baseScaleX != 1.0 || baseScaleY != 1.0))
            {
                geometry[0] = stageOriginX + (geometry[0] - stageOriginX) * baseScaleX;
                geometry[1] = stageOriginY + (geometry[1] - stageOriginY) * baseScaleY;
                geometry[2] *= baseScaleX;
                geometry[3] *= baseScaleY;
            }
            u32 flags = spin != 0.0 ? ITEM_FLAG_SPIN : ITEM_FLAG_NONE;
            const double maskHole = (mediaType == MediaType::Image && hasEffective(SP_MASK_IMAGE))
                                        ? parseRadialMaskHole(effectiveSpan(SP_MASK_IMAGE))
                                        : 0.0;
            if (maskHole > 0.0)
                flags |= ITEM_FLAG_MASK_HOLE;
            const bool maskCircle = hasEffective(SP_BORDER_RADIUS) &&
                                    (cstr_equal(effectiveCStr(SP_BORDER_RADIUS), "50%") ||
                                     cstr_equal(effectiveCStr(SP_BORDER_RADIUS), "50% 50%"));
            if (maskCircle)
                flags |= ITEM_FLAG_MASK_CIRCLE;
            if (hasWidth)
                flags |= ITEM_FLAG_EXPLICIT_WIDTH;
            if (hasHeight)
                flags |= ITEM_FLAG_EXPLICIT_HEIGHT;
            const bool isAudio = mediaType == MediaType::Audio;
            const bool hasReveal = !cstr_equal(revealMode.data(), "none");
            MultiArray<char, MAX_CLASS_LIST_TEXT> className;
            if (!readFirstStringField(json, itemStart, "class", "className",
                                      className.data(), static_cast<u32>(className.size())))
                className[0] = '\0';
            const bool silk = hasClassToken(className.data(), "tl-silk");
            const bool hasAnimation = itemProps.has(SP_ANIMATION);
            MultiArray<char, MAX_STYLE_VALUE_TEXT> animationLower;
            if (hasAnimation)
            {
                copy_cstr_bounded(animationLower.data(), animationLower.size(), itemProps.cstr(SP_ANIMATION));
                lower_ascii_inplace(animationLower.data());
            }
            const bool infiniteAnimation = hasAnimation &&
                                           std::strstr(animationLower.data(), "infinite") != nullptr;
            const bool bassItem = isBassItem(json, itemStart);
            const bool bassBackground =
                mediaType == MediaType::Background && bassItem;
            const u8 tier = (isAudio || isEffect)                                                         ? static_cast<u8>(MediaTier::Static)
                            : (mediaType == MediaType::Video || spin != 0.0 || silk || infiniteAnimation) ? static_cast<u8>(MediaTier::Continuous)
                            : (hasAnimation || keyframeCount || hasReveal || bassBackground)      ? static_cast<u8>(MediaTier::Finite)
                                                                                                          : static_cast<u8>(MediaTier::Static);
            MultiArray<char, MAX_KEY_TEXT> id;
            MultiArray<char, MAX_SOURCE_PATH_TEXT> source;
            if (!readStringField(json, itemStart, "id", id.data(), static_cast<u32>(id.size())))
                id[0] = '\0';
            if (!readStringField(json, itemStart, "src", source.data(), static_cast<u32>(source.size())))
                source[0] = '\0';
            if (!source[0])
            {
                copy_cstr_bounded(source.data(), source.size(), id.data());
            }
            u32 resourceId = 0;
            if (mediaType == MediaType::Background)
            {
                const char *itemBgColorText = itemProps.has(SP_BG_COLOR) ? itemProps.cstr(SP_BG_COLOR)
                                            : itemProps.has(SP_BG)       ? itemProps.cstr(SP_BG)
                                                                         : nullptr;
                const char *ruleBgColorText = (matchedRule && ruleProps.has(SP_BG_COLOR)) ? ruleProps.cstr(SP_BG_COLOR)
                                            : (matchedRule && ruleProps.has(SP_BG))       ? ruleProps.cstr(SP_BG)
                                                                                          : nullptr;
                const char *chosenBgText = ruleBgColorText ? ruleBgColorText : itemBgColorText;
                resourceId = chosenBgText
                                 ? parseStagedTimelineColor(manager, setupInput, chosenBgText, RGBA_OPAQUE_BLACK)
                                 : RGBA_OPAQUE_BLACK;
            }
            else
            {
                resourceId = hash31_str(source.data());
                if (source[0])
                {
                    ResourceEntry* res = resource_registry().find(source.data());
                    if (res)
                    {
                        resourceId = res->resourceId;
                    }
                }
            }
            double revealDuration = 0.0;
            double revealStagger = 0.0;
            if (isText)
            {
                json.numberField(itemStart, "revealDuration", revealDuration);
                json.numberField(itemStart, "revealStagger", revealStagger);
                if (revealDuration <= 0.0 && matchedRule)
                {
                    sourceJson.numberField(matchedRule->ruleStart, "revealDuration", revealDuration);
                }
                revealDuration = std::max(0.0, revealDuration);
                revealStagger = std::max(0.0, revealStagger);
            }
            TimelineItem item{};
            item.uid = slot;
            item.type = mediaType;
            item.tier = tier;
            item.flags = static_cast<u8>(flags);
            if (mediaType == MediaType::Background && hasLocalBg) item.flags |= 128u;
            item.startMs = static_cast<TimePoint>(startMs);
            item.endMs = static_cast<TimePoint>(endMs);
            item.baseX = static_cast<float>(geometry[0]);
            item.baseY = static_cast<float>(geometry[1]);
            item.baseW = static_cast<float>(geometry[2]);
            item.baseH = static_cast<float>(geometry[3]);
            item.spinSpeed = static_cast<float>(spin);
            item.maskHole = static_cast<float>(maskHole);
            item.volume = static_cast<float>(volume);
            item.zIndex = static_cast<i32>(zValue);
            item.resourceId = resourceId;
            item.rotateDeg = static_cast<float>(rotate);
            position->put_item(slot, item);
            TextBlock& bSlot = position->base_slot_block(slot);
            bSlot.clear();
            bSlot.set_value(0, true);
            bSlot.set_value(0, RectF{
                static_cast<float>(geometry[0]),
                static_cast<float>(geometry[1]),
                static_cast<float>(geometry[2]),
                static_cast<float>(geometry[3])
            });
            bSlot.set_value(4, static_cast<float>(rotate));
            bSlot.set_value(5, static_cast<float>(baseScaleX));
            bSlot.set_value(6, hasOpacity ? static_cast<float>(effectiveLeadNum(SP_OPACITY, 1.0)) : 1.0f);
            bSlot.set_value(7, static_cast<float>(geometry[4]));
            bSlot.set_value(8, static_cast<float>(geometry[5]));
            bSlot.set_value(9, 32.0f);
            bSlot.set_value(10, 1.25f);
            bSlot.set_value(16, (mediaType == MediaType::Background) ? resourceId : RGBA_OPAQUE_WHITE);
            bSlot.set_value(17, RGBA_OPAQUE_BLACK);
            bSlot.set_value(20, 400u);
            if (mediaType == MediaType::Image || mediaType == MediaType::Video)
            {
                bSlot.set_value(9, static_cast<float>(spin));
                bSlot.set_value(10, static_cast<float>(maskHole));
                bSlot.set_value(11, static_cast<float>(volume));
                bSlot.set_value(12, resourceId);
            }
            else if (mediaType == MediaType::Audio)
            {
                bSlot.set_value(0, static_cast<float>(volume));
                bSlot.set_value(5, resourceId);
            }
            position->slot_block(slot) = bSlot;
            MultiArray<u32, 6> textStyleCodes{};
            bool hasTextStyleCodes = false;
            if (isText || keyframeCount)
            {
                const char *fontWeightText = hasEffective(SP_FONT_WEIGHT) ? effectiveCStr(SP_FONT_WEIGHT) : "400";
                const char *textAlignText = hasEffective(SP_TEXT_ALIGN) ? effectiveCStr(SP_TEXT_ALIGN) : "left";
                const char *textTransformText = hasEffective(SP_TEXT_TRANSFORM) ? effectiveCStr(SP_TEXT_TRANSFORM) : "none";
                u32 easingStart = 0;
                u32 easingEnd = 0;
                MultiArray<char, MAX_TEMPLATE_NAME> easing;
                if (!json.field(itemStart, "easing", easingStart, easingEnd) ||
                    !json.readStringValue(easingStart, easingEnd, easing.data(), static_cast<u32>(easing.size())))
                    easing[0] = '\0';
                inputOffset = 0;
                const auto stageChars = [&](const char *value)
                {
                    u32 length = 0;
                    while (value[length])
                        ++length;
                    if (inputOffset + length > TIMELINE_SETUP_INPUT_CAPACITY)
                        return 0u;
                    std::memcpy(setupInput + inputOffset, value, length);
                    inputOffset += length;
                    return length;
                };
                const u32 fontLength = stageChars(hasEffective(SP_FONT_FAMILY)
                                                      ? effectiveCStr(SP_FONT_FAMILY)
                                                      : "sans-serif");
                const u32 weightLength = stageChars(fontWeightText);
                const u32 alignLength = stageChars(textAlignText);
                const u32 revealLength = stageChars(revealMode.data());
                const u32 transformTextLength = stageChars(textTransformText);
                const u32 easingLength = stageChars(easing.data());
                u32 *normalizedTextStyle = manager.normalizeTimelineTextStyle(
                    fontLength, weightLength, alignLength, revealLength,
                    transformTextLength, easingLength);
                if (!normalizedTextStyle)
                    continue;
                for (u32 index = 0; index < 6; ++index)
                    textStyleCodes[index] = normalizedTextStyle[index];
                hasTextStyleCodes = true;
                if (isText)
                {
                    const float fs = static_cast<float>(fontSize);
                    const float lh = static_cast<float>(lineHeight);
                    const float ls = static_cast<float>(effectiveLeadNum(SP_LETTER_SPACING, 0.0));
                    const float sw = static_cast<float>(effectiveLeadNum(SP_STROKE_W, 0.0));
                    const float ax = static_cast<float>(geometry[4]);
                    const float ay = static_cast<float>(geometry[5]);
                    u32 inheritedTextColor = RGBA_OPAQUE_WHITE;
                    if (matchedRule && ruleProps.has(SP_COLOR))
                    {
                        inheritedTextColor = parseStagedTimelineColor(manager, setupInput, ruleProps.cstr(SP_COLOR), RGBA_OPAQUE_WHITE);
                    }
                    const u32 col = hasColor
                                       ? parseStagedTimelineColor(manager, setupInput, colorText, inheritedTextColor)
                                       : inheritedTextColor;
                    const bool inheritCol = !hasLocalColor;
                    const u32 strkCol = parseStagedTimelineColor(
                        manager, setupInput,
                        hasStrokeColor ? strokeColorText : "#000000", RGBA_OPAQUE_BLACK);
                    const char *itemBgText = itemProps.has(SP_BG_COLOR) ? itemProps.cstr(SP_BG_COLOR)
                                           : itemProps.has(SP_BG)       ? itemProps.cstr(SP_BG)
                                                                        : nullptr;
                    const char *ruleBgText = (matchedRule && ruleProps.has(SP_BG_COLOR)) ? ruleProps.cstr(SP_BG_COLOR)
                                           : (matchedRule && ruleProps.has(SP_BG))       ? ruleProps.cstr(SP_BG)
                                                                                         : nullptr;
                    const char *chosenTextBg = itemBgText ? itemBgText : ruleBgText;
                    const u32 textBgCol = chosenTextBg
                        ? parseStagedTimelineColor(manager, setupInput, chosenTextBg, 0u)
                        : 0u;
                    const u16 fontWeight = static_cast<u16>(textStyleCodes[0]);
                    const u8 textAlign = static_cast<u8>(textStyleCodes[1]);
                    const u8 fontCategory = static_cast<u8>(textStyleCodes[2]);
                    const u8 revealMode = static_cast<u8>(textStyleCodes[3]);
                    const u8 textTransform = static_cast<u8>(textStyleCodes[4]);
                    const u16 lineCount = static_cast<u16>(measure.lineCount);
                    MultiArray<char, MAX_STYLE_VALUE_TEXT> fontName;
                    resolveItemFontFamilyCstr(json, itemStart, effectiveCStr(SP_FONT_FAMILY), hasEffective(SP_FONT_FAMILY), fontName.data(), static_cast<u32>(fontName.size()));
                    const u32 fontHash = hash31_str(fontName.data());
                    MultiArray<char, MAX_SHORT_TEXT> fontStyle;
                    if (!readStringField(json, itemStart, "font-style", fontStyle.data(), static_cast<u32>(fontStyle.size())))
                        fontStyle[0] = '\0';
                    bool isItalic = (fontStyle[0] && (std::strstr(fontStyle.data(), "italic") != nullptr || std::strstr(fontStyle.data(), "oblique") != nullptr));
                    if (!isItalic && fontName[0])
                    {
                        isItalic = (std::strstr(fontName.data(), "Italic") != nullptr || std::strstr(fontName.data(), "italic") != nullptr || std::strstr(fontName.data(), "Oblique") != nullptr);
                    }
                    const u16 resolvedFontFileId = isItalic
                                                    ? font::FontMapRegistry::instance().resolveFontFileId<true>(fontName.data(), fontWeight)
                                                    : font::FontMapRegistry::instance().resolveFontFileId<false>(fontName.data(), fontWeight);
                    // text-decoration / vertical-align -> bitmask (không ảnh hưởng tra file font)
                    u8 textDeco = TextDecoration::None;
                    {
                        MultiArray<char, MAX_TOKEN_TEXT> deco;
                        if (!readFirstStringField(json, itemStart, "text-decoration", "textDecoration", deco.data(), static_cast<u32>(deco.size())))
                            deco[0] = '\0';
                        MultiArray<char, MAX_SHORT_TEXT> valign;
                        if (!readFirstStringField(json, itemStart, "vertical-align", "verticalAlign", valign.data(), static_cast<u32>(valign.size())))
                            valign[0] = '\0';
                        const u8 underline = static_cast<u8>(std::strstr(deco.data(), "underline") != nullptr);
                        const u8 strike = static_cast<u8>(std::strstr(deco.data(), "line-through") != nullptr ||
                                                          std::strstr(deco.data(), "strikethrough") != nullptr);
                        const u8 sup = static_cast<u8>(std::strstr(valign.data(), "super") != nullptr);
                        textDeco = static_cast<u8>(
                            (underline * TextDecoration::Underline) |
                            (strike * TextDecoration::Strikethrough) |
                            (sup * TextDecoration::Superscript));
                    }
                    bSlot.set_value<text_reg::FONT_SIZE>(fs);
                    bSlot.set_value<text_reg::LINE_HEIGHT>(lh);
                    bSlot.set_value<text_reg::LETTER_SPACING>(ls);
                    bSlot.set_value<text_reg::STROKE_WIDTH>(sw);
                    bSlot.set_value<text_reg::ANCHOR_X>(ax);
                    bSlot.set_value<text_reg::ANCHOR_Y>(ay);
                    bSlot.set_value<text_reg::COLOR>(col);
                    bSlot.set_value<text_reg::STROKE_COLOR>(strkCol);
                    bSlot.set_value<text_reg::BG_COLOR>(textBgCol);
                    bSlot.set_value<text_reg::FONT_HASH>(fontHash);
                    bSlot.set_value<text_reg::FONT_WEIGHT>(static_cast<u32>(fontWeight));
                    bSlot.set_value<text_reg::REVEAL_DURATION_MS>(static_cast<float>(revealDuration));
                    bSlot.set_value<text_reg::REVEAL_STAGGER_MS>(static_cast<float>(revealStagger));
                    bSlot.set_value<text_reg::RESOLVED_FONT_ID>(static_cast<u32>(resolvedFontFileId));
                    bSlot.set_value<text_bit::IS_ITALIC, 1>(isItalic != 0);
                    bSlot.set_value<text_bit::INHERIT_COLOR, 1>(inheritCol != 0);
                    bSlot.set_value<text_bit::TEXT_ALIGN_LO, 2>(textAlign);
                    bSlot.set_value<text_bit::FONT_CATEGORY_LO, 3>(fontCategory);
                    bSlot.set_value<text_bit::REVEAL_MODE_0, 4>(revealMode);
                    bSlot.set_value<text_bit::TEXT_TRANSFORM_LO, 2>(textTransform);
                    bSlot.set_value<text_bit::LINE_COUNT_BASE, 8>(max<u16>(1u, lineCount));
                    bSlot.set_value<text_bit::DECORATION_UNDERLINE, 3>(textDeco);
                    position->set_slot_text_block(slot, bSlot);
                }
            }
            // MediaType::Background no longer sets textParams (handled via MediaType::Style/Role)
            if (mediaType == MediaType::Path)
            {
                MultiArray<char, path::MAX_PATH_STRING_CAP> pathData; // KHONG CO {}
                pathData[0] = '\0';
                if (!readFirstStringField(json, itemStart, "d", "path", pathData.data(), static_cast<u32>(pathData.size())))
                {
                    if (itemProps.has(SP_PATH_DATA))
                        copy_cstr_bounded(pathData.data(), pathData.size(), itemProps.cstr(SP_PATH_DATA));
                }

                const usize dLen = std::strlen(pathData.data());
                const u32 pathDataHash = hash31_str(pathData.data());

                float pathBox[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                path::FastPathBoundsEstimator::compute_bounds(pathData.data(), dLen, pathBox);

                if (!hasWidth && pathBox[2] > pathBox[0])
                {
                    item.baseW = pathBox[2] - pathBox[0];
                    position->put_item(slot, item);
                }
                if (!hasHeight && pathBox[3] > pathBox[1])
                {
                    item.baseH = pathBox[3] - pathBox[1];
                    position->put_item(slot, item);
                }

                const char *pathFillText = nullptr;
                bool hasExplicitFill = false;
                if (matchedRule && ruleProps.has(SP_FILL))
                    pathFillText = ruleProps.cstr(SP_FILL);
                else if (matchedRule && ruleProps.has(SP_COLOR))
                    pathFillText = ruleProps.cstr(SP_COLOR);
                else if (itemProps.has(SP_FILL))
                {
                    pathFillText = itemProps.cstr(SP_FILL);
                    hasExplicitFill = true;
                }
                else if (itemProps.has(SP_COLOR))
                {
                    pathFillText = itemProps.cstr(SP_COLOR);
                    hasExplicitFill = true;
                }

                u32 fillColor = 0u;
                bool hasFill = false;
                bool inheritFill = false;
                if (pathFillText && !cstr_equal(pathFillText, "none"))
                {
                    fillColor = parseStagedTimelineColor(manager, setupInput, pathFillText, RGBA_OPAQUE_WHITE);
                    hasFill = true;
                    inheritFill = !hasExplicitFill;
                }

                const char *pathStrokeText = (matchedRule && ruleProps.has(SP_STROKE_COL)) ? ruleProps.cstr(SP_STROKE_COL)
                                           : (matchedRule && ruleProps.has(SP_STROKE))     ? ruleProps.cstr(SP_STROKE)
                                           : itemProps.has(SP_STROKE)                       ? itemProps.cstr(SP_STROKE)
                                           : itemProps.has(SP_STROKE_COL)                   ? itemProps.cstr(SP_STROKE_COL)
                                                                                            : nullptr;
                const double pathStrokeWidth = (matchedRule && ruleProps.has(SP_STROKE_W))
                                                   ? ruleProps.lead_num(SP_STROKE_W, 1.0)
                                                   : itemProps.lead_num(SP_STROKE_W, 1.0);
                u32 strokeColor = 0u;
                float strokeWidth = 0.0f;
                bool hasStroke = false;
                if (pathStrokeText && !cstr_equal(pathStrokeText, "none"))
                {
                    strokeColor = parseStagedTimelineColor(manager, setupInput, pathStrokeText, RGBA_OPAQUE_BLACK);
                    strokeWidth = static_cast<float>(pathStrokeWidth);
                    hasStroke = true;
                }

                const char *fillRuleText = hasEffective(SP_FILL_RULE) ? effectiveCStr(SP_FILL_RULE) : "";
                const u8 fillRule = cstr_equal(fillRuleText, "evenodd")
                                       ? static_cast<u8>(path::PathFillRule::EvenOdd)
                                       : static_cast<u8>(path::PathFillRule::NonZero);

                const char *lineCapText = hasEffective(SP_STROKE_LINECAP) ? effectiveCStr(SP_STROKE_LINECAP) : "";
                const char *lineJoinText = hasEffective(SP_STROKE_LINEJOIN) ? effectiveCStr(SP_STROKE_LINEJOIN) : "";
                const u8 lineCap = cstr_equal(lineCapText, "round")   ? static_cast<u8>(path::PathLineCap::Round)
                                 : cstr_equal(lineCapText, "square")  ? static_cast<u8>(path::PathLineCap::Square)
                                                                      : static_cast<u8>(path::PathLineCap::Butt);
                const u8 lineJoin = cstr_equal(lineJoinText, "round") ? static_cast<u8>(path::PathLineJoin::Round)
                                  : cstr_equal(lineJoinText, "bevel") ? static_cast<u8>(path::PathLineJoin::Bevel)
                                                                      : static_cast<u8>(path::PathLineJoin::Miter);

                bSlot.set_value(2, item.baseW);
                bSlot.set_value(3, item.baseH);
                bSlot.set_value(path_reg::STROKE_WIDTH, strokeWidth);
                bSlot.set_value(path_reg::STROKE_MITERLIMIT, 4.0f);
                bSlot.set_value(path_reg::STROKE_DASHOFFSET, 0.0f);
                bSlot.set_value(path_reg::EFFECTIVE_STROKE_W, hasStroke ? strokeWidth : 0.0f);
                bSlot.set_value(path_reg::BASE_FILL_COLOR, fillColor);
                bSlot.set_value(path_reg::BASE_STROKE_COLOR, strokeColor);
                bSlot.set_value(path_reg::EFFECTIVE_FILL_COL, hasFill ? fillColor : 0u);
                bSlot.set_value(path_reg::EFFECTIVE_STROKE_C, hasStroke ? strokeColor : 0u);
                bSlot.set_value(path_reg::DATA_HASH, pathDataHash);
                bSlot.set_value(path_reg::VIEWBOX_MIN_X, pathBox[0]);
                bSlot.set_value(path_reg::VIEWBOX_MIN_Y, pathBox[1]);
                bSlot.set_value(path_reg::VIEWBOX_MAX_X, pathBox[2]);
                bSlot.set_value(path_reg::VIEWBOX_MAX_Y, pathBox[3]);
                bSlot.set_value(path_reg::DATA_LEN, static_cast<u32>(dLen));

                bSlot.set_value(path_bit::VISIBLE, true);
                bSlot.set_value(path_bit::FILL_RULE, fillRule == static_cast<u8>(path::PathFillRule::EvenOdd));
                bSlot.set_value(path_bit::LINE_CAP_LO, (lineCap & 1u) != 0);
                bSlot.set_value(path_bit::LINE_CAP_HI, (lineCap & 2u) != 0);
                bSlot.set_value(path_bit::LINE_JOIN_LO, (lineJoin & 1u) != 0);
                bSlot.set_value(path_bit::LINE_JOIN_HI, (lineJoin & 2u) != 0);
                bSlot.set_value(path_bit::HAS_FILL, hasFill);
                bSlot.set_value(path_bit::HAS_STROKE, hasStroke);
                bSlot.set_value(path_bit::INHERIT_FILL, inheritFill);
                bSlot.set_value(path_bit::INHERIT_STROKE, false);

                position->slot_block(slot) = bSlot;
                path::PathStringArena::instance().set_path(slot, pathData.data(), dLen);
            }
            if (keyframeCount)
            {
                MultiArray<char, MAX_FULL_TEXT_BYTES> fullTextBuf;
                MultiArray<u16, MAX_FULL_TEXT_CHARS> charKfMap;
                fullTextBuf[0] = '\0';
                u32 fullTextLen = 0;
                u32 totalChars = 0;

                const u32 easing = hasTextStyleCodes ? textStyleCodes[5] : 0u;
                KeyframeCompileContext kfCtx{
                    json, keyframesStart, keyframesEnd, keyframeCount,
                    durationMs, slot, item, baseOpacity, baseScaleX, baseScaleY,
                    geometry, baseOrigin, stageWidth, stageHeight,
                    manager, setupInput, position, hasKeyframeContent, easing,
                    isText ? fullTextBuf.data() : nullptr,
                    isText ? static_cast<u32>(fullTextBuf.size()) : 0u,
                    isText ? &fullTextLen : nullptr,
                    isText ? charKfMap.data() : nullptr,
                    isText ? static_cast<u32>(charKfMap.size()) : 0u,
                    isText ? &totalChars : nullptr,
                    isText ? contentScratch.data() : nullptr,
                    isText ? static_cast<u32>(contentScratch.size()) : 0u
                };

                const KeyframeCompileResult kfRes = compileItemKeyframes(kfCtx, mediaType);

                const u32 contentMask = kfRes.contentMask;
                const u32 hasSpatial = kfRes.hasSpatial;
                const u64 kfProps = kfRes.kfProps;

                u64 defProps = 0ULL;
                if (hasLeft) defProps |= (1ULL << prop::PROP_LEFT);
                if (hasTop) defProps |= (1ULL << prop::PROP_TOP);
                if (hasWidth) defProps |= (1ULL << prop::PROP_WIDTH);
                if (hasHeight) defProps |= (1ULL << prop::PROP_HEIGHT);
                if (hasRotate) defProps |= (1ULL << prop::PROP_ROTATE);
                if (hasOpacity) defProps |= (1ULL << prop::PROP_OPACITY);
                if (spin != 0.0) defProps |= (1ULL << prop::PROP_SPIN_SPEED);
                if (maskHole > 0.0) defProps |= (1ULL << prop::PROP_MASK_HOLE);
                if (volume != 1.0) defProps |= (1ULL << prop::PROP_VOLUME);
                if (zValue != 0) defProps |= (1ULL << prop::PROP_Z_INDEX);
                if (isAudio) defProps |= (1ULL << prop::PROP_VOLUME);
                if (isText)
                {
                    if (hasContent) defProps |= (1ULL << prop::PROP_CONTENT);
                    if (hasColor) defProps |= (1ULL << prop::PROP_COLOR);
                    if (hasEffective(SP_FONT_SIZE)) defProps |= (1ULL << prop::PROP_FONT_SIZE);
                    if (hasEffective(SP_FONT_FAMILY)) defProps |= (1ULL << prop::PROP_FONT_FAMILY);
                    if (hasEffective(SP_FONT_WEIGHT)) defProps |= (1ULL << prop::PROP_FONT_WEIGHT);
                    if (hasEffective(SP_LINE_HEIGHT)) defProps |= (1ULL << prop::PROP_LINE_HEIGHT);
                    if (hasEffective(SP_LETTER_SPACING)) defProps |= (1ULL << prop::PROP_LETTER_SPACING);
                    if (hasEffective(SP_TEXT_ALIGN)) defProps |= (1ULL << prop::PROP_TEXT_ALIGN);
                    if (hasEffective(SP_STROKE_W)) defProps |= (1ULL << prop::PROP_STROKE_WIDTH);
                    if (hasStrokeColor) defProps |= (1ULL << prop::PROP_STROKE_COLOR);
                }
                if (mediaType == MediaType::Background)
                {
                    if (hasEffective(SP_BG_COLOR)) defProps |= (1ULL << prop::PROP_BACKGROUND_COLOR);
                    if (hasEffective(SP_BG)) defProps |= (1ULL << prop::PROP_BACKGROUND);
                }
                position->set_property_masks(slot, defProps, kfProps);
                position->set_track_info(
                    slot, keyframeCount, easing, contentMask, hasSpatial, kfProps);
                if (isText)
                {
                    if (!hasKeyframeContent && hasContent)
                    {
                        u32 cs = 0, ce = 0;
                        const bool gotText = json.field(itemStart, "content", cs, ce) ||
                                             json.field(itemStart, "text", cs, ce);
                        if (gotText &&
                            json.readStringValue(cs, ce, contentScratch.data(), static_cast<u32>(contentScratch.size())))
                        {
                            appendTextSpan<false>(
                                contentScratch.data(), 0,
                                fullTextBuf.data(), static_cast<u32>(fullTextBuf.size()), fullTextLen,
                                charKfMap.data(), static_cast<u32>(charKfMap.size()), totalChars);
                            if (fullTextLen < static_cast<u32>(fullTextBuf.size()))
                                fullTextBuf[fullTextLen] = '\0';
                            for (u32 c = 0; c < totalChars; ++c)
                                charKfMap[c] = static_cast<u16>(c);
                        }
                    }
                    MultiArray<char, MAX_TEMPLATE_NAME> fontFamilyStr;
                    resolveItemFontFamilyCstr(json, itemStart, effectiveCStr(SP_FONT_FAMILY), hasEffective(SP_FONT_FAMILY), fontFamilyStr.data(), static_cast<u32>(fontFamilyStr.size()));
                    Render::set_slot_text_source(slot, fullTextBuf.data(), fullTextLen, charKfMap.data(), totalChars, fontFamilyStr.data());
                    Render::ensure_slot_text_shaped(slot, 1.0f);
                }
            }
            else if (isText && hasContent)
            {
                u32 cs = 0, ce = 0;
                const bool gotText = json.field(itemStart, "content", cs, ce) ||
                                     json.field(itemStart, "text", cs, ce);
                if (gotText &&
                    json.readStringValue(cs, ce, contentScratch.data(), static_cast<u32>(contentScratch.size())))
                {
                    const u32 len = static_cast<u32>(std::strlen(contentScratch.data()));
                    MultiArray<char, MAX_TEMPLATE_NAME> fontFamilyStr;
                    resolveItemFontFamilyCstr(json, itemStart, effectiveCStr(SP_FONT_FAMILY), hasEffective(SP_FONT_FAMILY), fontFamilyStr.data(), static_cast<u32>(fontFamilyStr.size()));
                    const u64 newContentHash = MerkleDiffTable<MAX_TIMELINE_SLOTS>::hash_span_64(
                        contentScratch.data(), len, hash31_str(fontFamilyStr.data())
                    );
                    s_slotTextContentHash[slot] = newContentHash;

                    bool needsShape = true;
                    if constexpr (Incremental)
                    {
                        if (canSkipUnchanged && position)
                        {
                            constexpr u64 kShapeRegMask =
                                (1ULL << text_reg::FONT_SIZE) |
                                (1ULL << text_reg::LINE_HEIGHT) |
                                (1ULL << text_reg::LETTER_SPACING) |
                                (1ULL << text_reg::STROKE_WIDTH) |
                                (1ULL << text_reg::FONT_HASH) |
                                (1ULL << text_reg::FONT_WEIGHT) |
                                (1ULL << text_reg::RESOLVED_FONT_ID);
                            constexpr u64 kShapeBitMask =
                                (1ULL << text_bit::IS_ITALIC) |
                                (3ULL << text_bit::TEXT_ALIGN_LO) |
                                (3ULL << text_bit::TEXT_TRANSFORM_LO) |
                                (7ULL << text_bit::FONT_CATEGORY_LO);
                            const TextBlock& newBlk = position->slot_text_block(slot);
                            const u64 regDiff = oldTextBlk.diff_reg_mask(newBlk).words[0];
                            const u64 bitDiff = oldTextBlk.diff_bit_mask(newBlk).words[0];
                            const float oldW = oldTextBlk.get_value<text_reg::X1>() - oldTextBlk.get_value<text_reg::X0>();
                            const float newW = newBlk.get_value<text_reg::X1>() - newBlk.get_value<text_reg::X0>();
                            const bool widthChanged = abs_val(newW - oldW) > 0.5f;
                            needsShape = (newContentHash != oldContentHash) ||
                                         ((regDiff & kShapeRegMask) != 0ULL) ||
                                         ((bitDiff & kShapeBitMask) != 0ULL) ||
                                         widthChanged;
                        }
                    }
                    if (needsShape)
                    {
                        report.textShapeDirtyBB |= (1ULL << slot);
                        Render::set_slot_text_source(slot, contentScratch.data(), len, nullptr, 0, fontFamilyStr.data());
                        Render::ensure_slot_text_shaped(slot, 1.0f);
                    }
                }
            }
            u32 colorsStart = 0, colorsEnd = 0;
            if (json.field(itemStart, "colors", colorsStart, colorsEnd) && json.isArray(colorsStart, colorsEnd))
            {
                u32 colorCount = 0;
                u32 colCursor = colorsStart + 1;
                while (colCursor < colorsEnd && colorCount < MAX_BASS_COLORS)
                {
                    u32 cStart = 0, cEnd = 0;
                    if (!json.nextArrayValue(colCursor, colorsEnd, cStart, cEnd) || !json.isObject(cStart, cEnd))
                        break;
                    SceneStyleValue bgVal, txtVal;
                    findSceneStyleProperty<prop::PROP_BACKGROUND_COLOR>(json, cStart, cEnd, bgVal);
                    if (!bgVal.found)
                        findSceneStyleProperty<prop::PROP_BACKGROUND>(json, cStart, cEnd, bgVal);
                    findSceneStyleProperty<prop::PROP_COLOR>(json, cStart, cEnd, txtVal);
                    const u32 bgHex = bgVal.found
                                          ? parseStagedTimelineColor(manager, setupInput, bgVal.text.data(), RGBA_OPAQUE_BLACK)
                                          : RGBA_OPAQUE_BLACK;
                    const u32 txtHex = txtVal.found
                                           ? parseStagedTimelineColor(manager, setupInput, txtVal.text.data(), RGBA_OPAQUE_WHITE)
                                           : RGBA_OPAQUE_WHITE;
                    position->set_bass_color(slot, colorCount, bgHex, txtHex);
                    ++colorCount;
                }
                double hop = DEFAULT_BASS_HOP_SEC;
                u32 paramsStart = 0, paramsEnd = 0;
                if (json.field(itemStart, "params", paramsStart, paramsEnd))
                {
                    u32 bassStart = 0, bassEnd = 0;
                    if (json.field(paramsStart, "bass", bassStart, bassEnd))
                    {
                        json.numberField(bassStart, "hop", hop);
                    }
                }
                position->set_bass_params(slot, static_cast<float>(hop), colorCount, 1u);
            }
            else
            {
                position->set_bass_params(slot, DEFAULT_BASS_HOP_F32, 0, bassItem ? 1u : 0u);
            }
            // Tu dong lien ket texture da nap tu Render resource pool & ResourceRegistry noi bo
            if (item.resourceId != 0)
            {
                ResourceEntry* res = resource_registry().find_by_id(item.resourceId);
                if (res)
                {
                    resource_registry().touch(res->resourceId);
                    if (item.endMs <= item.startMs && res->durationMs > 0)
                    {
                        item.endMs = item.startMs + res->durationMs;
                        position->put_item(slot, item);
                    }
                    if (mediaType == MediaType::Video || mediaType == MediaType::Audio)
                    {
                        stream_pipeline().register_resource(res->resourceId, static_cast<u32>(mediaType), res->fileSize);
                    }
                }
                else if (mediaType == MediaType::Video || mediaType == MediaType::Audio)
                {
                    stream_pipeline().register_resource(item.resourceId, static_cast<u32>(mediaType), 0ULL);
                }
                const u32 preloadedTex = Render::get_resource_texture(item.resourceId);
                if (preloadedTex != 0)
                {
                    Render::set_slot_texture(slot, preloadedTex);
                }
            }
            if (fxStatus == timeline::runtime::RUNTIME_LOAD_OK)
            {
                const char *filterText = itemProps.has(SP_FILTER) ? itemProps.cstr(SP_FILTER) : nullptr;
                const auto* prevFx = projectRuntime().item(slot);
                const bool hadFx = (prevFx != nullptr && prevFx->effect != EffectKind::None);
                const bool hasNewFx = isEffect || (filterText != nullptr && filterText[0] != '\0');
                if (hasNewFx || hadFx)
                {
                    anyFxSlotChanged = true;
                    if (!fxStageBegun)
                    {
                        projectRuntime().begin_slot_stage(
                            stageWidth, stageHeight,
                            position ? static_cast<double>(position->duration()) : 0.0);
                        for (u32 prev = 0u; prev < slot; ++prev)
                        {
                            projectRuntime().stage_slot_reuse(prev);
                        }
                        fxStageBegun = true;
                    }
                }
                if (fxStageBegun)
                {
                    const double finalEndMs = (static_cast<double>(item.endMs) > endMs)
                                                  ? static_cast<double>(item.endMs)
                                                  : endMs;
                    const auto st = projectRuntime().stage_slot_fx(
                        slot, mediaType, startMs, finalEndMs,
                        static_cast<i32>(zValue), resourceId,
                        json, itemStart, itemEnd, filterText, isEffect);
                    if (st != timeline::runtime::RUNTIME_LOAD_OK)
                        fxStatus = st;
                }
            }
            ++slot;
        }
        s_slotMerkle.finalize_count(slot);
        report.changedSlotsBB = s_slotMerkle.entities.words[0];
        report.removedSlotsBB = s_slotMerkle.removedMask.words[0];
        if constexpr (Incremental)
        {
            u64 rem = report.removedSlotsBB;
            while (rem != 0ULL && position)
            {
                const u32 remSlot = BitOps<u64>::pop_lsb(rem);
                position->remove_item(remSlot);
                s_slotTextContentHash[remSlot] = 0ULL;
            }
        }
        if (fxStageBegun || report.removedSlotsBB != 0ULL)
        {
            if (!fxStageBegun)
            {
                projectRuntime().begin_slot_stage(
                    stageWidth, stageHeight,
                    position ? static_cast<double>(position->duration()) : 0.0);
                for (u32 prev = 0u; prev < slot; ++prev)
                {
                    projectRuntime().stage_slot_reuse(prev);
                }
            }
            projectRuntimeStatusRef() = projectRuntime().commit_staged_slots(slot, fxStatus);
        }
        if (!canSkipUnchanged || report.removedSlotsBB != 0ULL)
        {
            // Nướng lại toàn bộ allDependentSlotsBB khi cấu trúc template/slot thay đổi
            TemplateArena::instance().bake_all_templates();
            position->set_top_level_count(sceneRootCount);
            position->update_style_environment(0);
        }
        return report;
    }

    void configureCompiledTimelinePosition(double stageWidth, double stageHeight)
    {
        (void)configureCompiledTimelinePosition_T<false>(stageWidth, stageHeight);
    }

    IncrementalPatchReport configureCompiledTimelinePositionIncremental(double stageWidth, double stageHeight)
    {
        return configureCompiledTimelinePosition_T<true>(stageWidth, stageHeight);
    }
} // namespace stockcut::timeline::project
