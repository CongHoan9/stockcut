// ============================================================================
// CoreEngine - Arbitrary-Capacity Property System (Types & Categories)
// ----------------------------------------------------------------------------
// [STOCKFISH DATA-STRUCTURE-CENTRIC SPECIFICATION]:
// - Thiết kế phân tầng 256 thuộc tính tĩnh (Static Well-Known Properties 0..255)
//   được intern hóa trong L1 Cacheline, kết hợp FNV-1a Hash LUT cho Dynamic Properties.
// - Cố định MAX_REGISTERED_PROPERTIES = 256 cho phân vùng Static Slot, triệt tiêu
//   mọi cấp phát động trong quá trình xử lý thuộc tính.
// ============================================================================
#pragma once
#include "types.h"
#include "memory.h"
#include "enum_traits.h"
#include "core/hash_primitives.h"
#include <array>
#include <utility>
namespace stockcut {
namespace prop {
using PropertyId = u32;
constexpr PropertyId INVALID_PROPERTY_ID = INVALID_U32;
constexpr u32 MAX_REGISTERED_PROPERTIES  = 256;
constexpr u32 PROPERTY_NAME_MAX_LEN      = 64;
// ----------------------------------------------------------------------------
// CÁC THUỘC TÍNH TIÊU CHUẨN ĐƯỢC ĐỊNH NGHĨA TĨNH (WELL-KNOWN IDS 0..63)
// ----------------------------------------------------------------------------
enum StandardPropId : PropertyId {
    // Tier 0: Không Gian & Hình Học Cơ Bản (0..7)
    PROP_LEFT               = 0,
    PROP_TOP                = 1,
    PROP_WIDTH              = 2,
    PROP_HEIGHT             = 3,
    PROP_ROTATE             = 4,
    PROP_SCALE              = 5,
    PROP_TRANSFORM          = 6,
    PROP_OPACITY            = 7,
    // Tier 1: Typography & Phân Cảnh Văn Bản (8..15)
    PROP_CONTENT            = 8,
    PROP_FONT_SIZE          = 9,
    PROP_FONT_FAMILY        = 10,
    PROP_FONT_WEIGHT        = 11,
    PROP_LINE_HEIGHT        = 12,
    PROP_LETTER_SPACING     = 13,
    PROP_TEXT_ALIGN         = 14,
    PROP_COLOR              = 15,
    // Tier 2: Viền, Nền & Hiệu Ứng Chữ (16..23)
    PROP_STROKE_WIDTH       = 16,
    PROP_STROKE_COLOR       = 17,
    PROP_BACKGROUND_COLOR   = 18,
    PROP_BACKGROUND         = 19,
    PROP_REVEAL_MODE        = 20,
    PROP_REVEAL_DURATION    = 21,
    PROP_REVEAL_STAGGER     = 22,
    PROP_MASK_HOLE          = 23,
    // Tier 3: Động Lực Học, Âm Thanh & Layer Hierarchy (24..31)
    PROP_SPIN_SPEED         = 24,
    PROP_VOLUME             = 25,
    PROP_Z_INDEX            = 26,
    PROP_FILTER             = 27,
    PROP_BASS_ACTIVE        = 28,
    PROP_TRIM_START         = 29,
    PROP_TIER               = 30,
    PROP_FLAGS              = 31,
    // Tier 4: Timing & Cấu Trúc Khung Thời Gian (32..39)
    PROP_START              = 32,
    PROP_END                = 33,
    PROP_DURATION           = 34,
    PROP_CLASS_NAME         = 35,
    PROP_SRC                = 36,
    PROP_TYPE               = 37,
    PROP_EFFECT             = 38,
    PROP_MUTED              = 39,
    // Tier 5: Bố Cục Nâng Cao (Flexbox, Grid & Box Model) (40..55)
    PROP_PADDING            = 40,
    PROP_MARGIN             = 41,
    PROP_DISPLAY            = 42,
    PROP_FLEX_DIRECTION     = 43,
    PROP_GAP                = 44,
    PROP_COLUMN_GAP         = 45,
    PROP_ROW_GAP            = 46,
    PROP_GRID_TEMPLATE_COLS = 47,
    PROP_GRID_TEMPLATE_ROWS = 48,
    PROP_BORDER             = 49,
    PROP_BORDER_WIDTH       = 50,
    PROP_BORDER_RADIUS      = 51,
    PROP_BORDER_COLOR       = 52,
    PROP_OVERFLOW           = 53,
    PROP_ORIGIN_X           = 54,
    PROP_ORIGIN_Y           = 55,
    // Tier 6: Hiệu Ứng Đồ Họa & Bóng Đổ (56..63)
    PROP_SHADOW_COLOR       = 56,
    PROP_SHADOW_BLUR        = 57,
    PROP_SHADOW_OFFSET_X    = 58,
    PROP_SHADOW_OFFSET_Y    = 59,
    PROP_MASK_IMAGE         = 60,
    PROP_BLEND_MODE         = 61,
    PROP_BLUR               = 62,
    PROP_PLAYBACK_RATE      = 63,
    // Tier 7: Mở Rộng Bố Cục & Kiểu Dáng Văn Bản (64..71)
    PROP_TRANSLATE          = 64,
    PROP_FONT_STYLE         = 65,
    PROP_TEXT_TRANSFORM     = 66,
    PROP_WHITE_SPACE        = 67,
    PROP_TEXT_SHADOW        = 68,
    PROP_ANIMATION          = 69,
    // Tier 8: Vector Graphics & CSS Path (70..78)
    PROP_PATH_DATA          = 70, // "d", "path", "pathData"
    PROP_FILL               = 71, // "fill", "fill-color"
    PROP_FILL_RULE          = 72, // "fill-rule", "fillRule"
    PROP_STROKE             = 73, // "stroke", "stroke-color"
    PROP_STROKE_LINECAP     = 74, // "stroke-linecap", "strokeLinecap"
    PROP_STROKE_LINEJOIN    = 75, // "stroke-linejoin", "strokeLinejoin"
    PROP_STROKE_MITERLIMIT  = 76, // "stroke-miterlimit", "strokeMiterlimit"
    PROP_STROKE_DASHARRAY   = 77, // "stroke-dasharray", "strokeDasharray"
    PROP_STROKE_DASHOFFSET  = 78, // "stroke-dashoffset", "strokeDashoffset"
    // Mốc bắt đầu cho các Dynamic / Custom Properties (Shader Uniforms, CSS Variables, Plugins)
    STANDARD_PROP_COUNT     = 79
};

[[nodiscard]] constexpr bool is_custom_property_id(PropertyId id) noexcept {
    return id >= STANDARD_PROP_COUNT && id != INVALID_PROPERTY_ID;
}

// ============================================================================
// COLD-GATE PROP DESCRIPTOR SCHEMA (Dành riêng cho Cổng Biên Dịch JSON/CSS)
// ============================================================================
enum PropSchemaFlags : u16 {
    PROP_FLAG_NONE        = 0,
    PROP_FLAG_INHERITABLE = 1u << 0,
    PROP_FLAG_KEYFRAMABLE = 1u << 1,
    PROP_FLAG_STYLE_RULE  = 1u << 2,
    PROP_FLAG_ARRAY       = 1u << 3
};

struct PropDescriptor {
    StandardPropId id;
    u16 flags;
    const char* kebab;
    const char* camel;
    const char* alias;
};

inline constexpr MultiArray<PropDescriptor, STANDARD_PROP_COUNT> kPropSchemas = {{
    // Tier 0: Geometry (0..7)
    {PROP_LEFT,      PROP_FLAG_KEYFRAMABLE,                          "left",      nullptr, nullptr},
    {PROP_TOP,       PROP_FLAG_KEYFRAMABLE,                          "top",       nullptr, nullptr},
    {PROP_WIDTH,     PROP_FLAG_KEYFRAMABLE,                          "width",     nullptr, nullptr},
    {PROP_HEIGHT,    PROP_FLAG_KEYFRAMABLE,                          "height",    nullptr, nullptr},
    {PROP_ROTATE,    PROP_FLAG_KEYFRAMABLE | PROP_FLAG_STYLE_RULE,   "rotate",    nullptr, nullptr},
    {PROP_SCALE,     PROP_FLAG_KEYFRAMABLE | PROP_FLAG_STYLE_RULE,   "scale",     nullptr, nullptr},
    {PROP_TRANSFORM, PROP_FLAG_KEYFRAMABLE,                          "transform", nullptr, nullptr},
    {PROP_OPACITY,   PROP_FLAG_KEYFRAMABLE | PROP_FLAG_STYLE_RULE,   "opacity",   nullptr, nullptr},
    // Tier 1: Typography (8..15)
    {PROP_CONTENT,        PROP_FLAG_KEYFRAMABLE,                          "content",        "content",       "text"},
    {PROP_FONT_SIZE,      PROP_FLAG_INHERITABLE | PROP_FLAG_STYLE_RULE,   "font-size",      "fontSize",      nullptr},
    {PROP_FONT_FAMILY,    PROP_FLAG_INHERITABLE | PROP_FLAG_STYLE_RULE,   "font-family",    "fontFamily",    nullptr},
    {PROP_FONT_WEIGHT,    PROP_FLAG_INHERITABLE | PROP_FLAG_STYLE_RULE,   "font-weight",    "fontWeight",    nullptr},
    {PROP_LINE_HEIGHT,    PROP_FLAG_INHERITABLE | PROP_FLAG_STYLE_RULE,   "line-height",    "lineHeight",    nullptr},
    {PROP_LETTER_SPACING, PROP_FLAG_INHERITABLE | PROP_FLAG_STYLE_RULE,   "letter-spacing", "letterSpacing", nullptr},
    {PROP_TEXT_ALIGN,     PROP_FLAG_INHERITABLE | PROP_FLAG_STYLE_RULE,   "text-align",     "textAlign",     nullptr},
    {PROP_COLOR,          PROP_FLAG_INHERITABLE | PROP_FLAG_KEYFRAMABLE | PROP_FLAG_STYLE_RULE, "color", nullptr, nullptr},
    // Tier 2: Style & Animation (16..23)
    {PROP_STROKE_WIDTH,     PROP_FLAG_KEYFRAMABLE | PROP_FLAG_STYLE_RULE, "stroke-width",              "strokeWidth",    "-webkit-text-stroke-width"},
    {PROP_STROKE_COLOR,     PROP_FLAG_KEYFRAMABLE | PROP_FLAG_STYLE_RULE, "-webkit-text-stroke-color", nullptr,          "text-stroke-color"},
    {PROP_BACKGROUND_COLOR, PROP_FLAG_KEYFRAMABLE | PROP_FLAG_STYLE_RULE, "background-color",          nullptr,          nullptr},
    {PROP_BACKGROUND,       PROP_FLAG_KEYFRAMABLE | PROP_FLAG_STYLE_RULE, "background",                nullptr,          nullptr},
    {PROP_REVEAL_MODE,      PROP_FLAG_NONE,                               "reveal-mode",               "revealMode",     nullptr},
    {PROP_REVEAL_DURATION,  PROP_FLAG_NONE,                               "reveal-duration",           "revealDuration", nullptr},
    {PROP_REVEAL_STAGGER,   PROP_FLAG_NONE,                               "reveal-stagger",            "revealStagger",  nullptr},
    {PROP_MASK_HOLE,        PROP_FLAG_NONE,                               "mask-hole",                 "maskHole",       nullptr},
    // Tier 3: Media & Audio Dynamics (24..31)
    {PROP_SPIN_SPEED,  PROP_FLAG_NONE, "spin-speed",  "spinSpeed",  "spin"},
    {PROP_VOLUME,      PROP_FLAG_NONE, "volume",      "volume",     "volunm"},
    {PROP_Z_INDEX,     PROP_FLAG_NONE, "z-index",     nullptr,      nullptr},
    {PROP_FILTER,      PROP_FLAG_NONE, "filter",      nullptr,      nullptr},
    {PROP_BASS_ACTIVE, PROP_FLAG_NONE, "bass-active", "bassActive", nullptr},
    {PROP_TRIM_START,  PROP_FLAG_NONE, "trim-start",  "trimStart",  "trim"},
    {PROP_TIER,        PROP_FLAG_NONE, "tier",        "tier",       nullptr},
    {PROP_FLAGS,       PROP_FLAG_NONE, "flags",       "flags",      nullptr},
    // Tier 4: Timing & Metadata (32..39)
    {PROP_START,      PROP_FLAG_NONE, "start",    "start",     nullptr},
    {PROP_END,        PROP_FLAG_NONE, "end",      "end",       nullptr},
    {PROP_DURATION,   PROP_FLAG_NONE, "duration", "duration",  "durationMs"},
    {PROP_CLASS_NAME, PROP_FLAG_NONE, "class",    "className", nullptr},
    {PROP_SRC,        PROP_FLAG_NONE, "src",      "src",       "source"},
    {PROP_TYPE,       PROP_FLAG_NONE, "type",     "type",      nullptr},
    {PROP_EFFECT,     PROP_FLAG_NONE, "effect",   "effect",    nullptr},
    {PROP_MUTED,      PROP_FLAG_NONE, "muted",    "muted",     nullptr},
    // Tier 5: Layout & Box Model (40..55)
    {PROP_PADDING,            PROP_FLAG_NONE,                        "padding",               nullptr,   nullptr},
    {PROP_MARGIN,             PROP_FLAG_NONE,                        "margin",                nullptr,   nullptr},
    {PROP_DISPLAY,            PROP_FLAG_NONE,                        "display",               nullptr,   nullptr},
    {PROP_FLEX_DIRECTION,     PROP_FLAG_NONE,                        "flex-direction",        nullptr,   nullptr},
    {PROP_GAP,                PROP_FLAG_NONE,                        "gap",                   nullptr,   nullptr},
    {PROP_COLUMN_GAP,         PROP_FLAG_NONE,                        "column-gap",            nullptr,   nullptr},
    {PROP_ROW_GAP,            PROP_FLAG_NONE,                        "row-gap",               nullptr,   nullptr},
    {PROP_GRID_TEMPLATE_COLS, PROP_FLAG_ARRAY,                       "grid-template-columns", nullptr,   nullptr},
    {PROP_GRID_TEMPLATE_ROWS, PROP_FLAG_ARRAY,                       "grid-template-rows",    nullptr,   nullptr},
    {PROP_BORDER,             PROP_FLAG_NONE,                        "border",                nullptr,   nullptr},
    {PROP_BORDER_WIDTH,       PROP_FLAG_NONE,                        "border-width",          nullptr,   nullptr},
    {PROP_BORDER_RADIUS,      PROP_FLAG_STYLE_RULE,                  "border-radius",         nullptr,   nullptr},
    {PROP_BORDER_COLOR,       PROP_FLAG_NONE,                        "border-color",          nullptr,   nullptr},
    {PROP_OVERFLOW,           PROP_FLAG_NONE,                        "overflow",              nullptr,   nullptr},
    {PROP_ORIGIN_X,           PROP_FLAG_KEYFRAMABLE | PROP_FLAG_STYLE_RULE, "origin-x",       "originX", nullptr},
    {PROP_ORIGIN_Y,           PROP_FLAG_NONE,                        "origin-y",              "originY", nullptr},
    // Tier 6: Graphic Effects (56..63)
    {PROP_SHADOW_COLOR,    PROP_FLAG_NONE,                             "shadow-color",    "shadowColor",   nullptr},
    {PROP_SHADOW_BLUR,     PROP_FLAG_NONE,                             "shadow-blur",     "shadowBlur",    nullptr},
    {PROP_SHADOW_OFFSET_X, PROP_FLAG_NONE,                             "shadow-offset-x", "shadowOffsetX", nullptr},
    {PROP_SHADOW_OFFSET_Y, PROP_FLAG_NONE,                             "shadow-offset-y", "shadowOffsetY", nullptr},
    {PROP_MASK_IMAGE,      PROP_FLAG_STYLE_RULE,                       "mask-image",      nullptr,         "-webkit-mask-image"},
    {PROP_BLEND_MODE,      PROP_FLAG_NONE,                             "blend-mode",      "blendMode",     "mix-blend-mode"},
    {PROP_BLUR,            PROP_FLAG_KEYFRAMABLE | PROP_FLAG_STYLE_RULE, "blur",          "blur",          nullptr},
    {PROP_PLAYBACK_RATE,   PROP_FLAG_NONE,                             "playback-rate",   "playbackRate",  nullptr},
    // Tier 7: Typography & Scene Ext (64..69)
    {PROP_TRANSLATE,      PROP_FLAG_KEYFRAMABLE,                          "translate",      nullptr,         nullptr},
    {PROP_FONT_STYLE,     PROP_FLAG_INHERITABLE | PROP_FLAG_STYLE_RULE,   "font-style",     nullptr,         nullptr},
    {PROP_TEXT_TRANSFORM, PROP_FLAG_INHERITABLE | PROP_FLAG_STYLE_RULE,   "text-transform", "textTransform", nullptr},
    {PROP_WHITE_SPACE,    PROP_FLAG_INHERITABLE,                          "white-space",    nullptr,         nullptr},
    {PROP_TEXT_SHADOW,    PROP_FLAG_NONE,                                 "text-shadow",    nullptr,         nullptr},
    {PROP_ANIMATION,      PROP_FLAG_NONE,                                 "animation",      nullptr,         nullptr},
    // Tier 8: Vector Graphics & CSS Path (70..78)
    {PROP_PATH_DATA,         PROP_FLAG_NONE,                           "d",                 "pathData",         "path"},
    {PROP_FILL,              PROP_FLAG_KEYFRAMABLE | PROP_FLAG_STYLE_RULE, "fill",          "fillColor",        "fill-color"},
    {PROP_FILL_RULE,         PROP_FLAG_NONE,                           "fill-rule",         "fillRule",         nullptr},
    {PROP_STROKE,            PROP_FLAG_KEYFRAMABLE | PROP_FLAG_STYLE_RULE, "stroke",        "strokeColor",      "stroke-color"},
    {PROP_STROKE_LINECAP,    PROP_FLAG_NONE,                           "stroke-linecap",    "strokeLinecap",    nullptr},
    {PROP_STROKE_LINEJOIN,   PROP_FLAG_NONE,                           "stroke-linejoin",   "strokeLinejoin",   nullptr},
    {PROP_STROKE_MITERLIMIT, PROP_FLAG_KEYFRAMABLE,                    "stroke-miterlimit", "strokeMiterlimit", nullptr},
    {PROP_STROKE_DASHARRAY,  PROP_FLAG_ARRAY,                          "stroke-dasharray",  "strokeDasharray",  nullptr},
    {PROP_STROKE_DASHOFFSET, PROP_FLAG_KEYFRAMABLE,                    "stroke-dashoffset", "strokeDashoffset", nullptr}
}};

constexpr bool verify_prop_schemas() noexcept {
    for (u32 i = 0; i < STANDARD_PROP_COUNT; ++i) {
        if (static_cast<u32>(kPropSchemas[i].id) != i) return false;
    }
    return true;
}
static_assert(verify_prop_schemas(), "kPropSchemas must exactly match StandardPropId");

constexpr usize count_inheritable_props() noexcept {
    usize c = 0;
    for (u32 i = 0; i < STANDARD_PROP_COUNT; ++i) {
        if ((kPropSchemas[i].flags & PROP_FLAG_INHERITABLE) != 0u) ++c;
    }
    return c;
}
inline constexpr usize INHERITABLE_PROP_COUNT = count_inheritable_props();
static_assert(INHERITABLE_PROP_COUNT == 10, "Expected exact 10 CSS inheritable typography properties");

constexpr MultiArray<StandardPropId, INHERITABLE_PROP_COUNT> build_inheritable_props() noexcept {
    MultiArray<StandardPropId, INHERITABLE_PROP_COUNT> out{};
    usize idx = 0;
    for (u32 i = 0; i < STANDARD_PROP_COUNT; ++i) {
        if ((kPropSchemas[i].flags & PROP_FLAG_INHERITABLE) != 0u) {
            out[idx++] = kPropSchemas[i].id;
        }
    }
    return out;
}
inline constexpr MultiArray<StandardPropId, INHERITABLE_PROP_COUNT> kInheritablePropIds = build_inheritable_props();

#include "prop_switch.inl"
} // namespace prop
} // namespace stockcut
