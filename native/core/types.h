// ============================================================================
// CoreEngine - Types, Foundation Data Structures & Canonical Constants
// ----------------------------------------------------------------------------
// [STOCKFISH DATA-STRUCTURE-CENTRIC & MASTER TYPE SYSTEM]:
// - Phân chia chuyên ngành kiến trúc Stockfish:
//   * types.h: Kiểu vô hướng nguyên thủy, hằng số, BitOps<T>, toán tử mặt nạ không rẽ nhánh.
//   * memory.h: Quản lý bộ nhớ căn lề, MultiArray, SentinelArray, ValueList, LinearStream, Span.
//   * history.h: RelaxedAtomic, StatsEntry, BoundedGravityReal, Envelopes, DynStats.
//   * data_structures.h: MonoidComposer, Group<T>, FlatMonoidSequence, TrackBucket, ZRankArena.
//   * scientific_foundations.h: 5 trụ cột toán lý (Symplectic, Sheaf, Gibbs, Wavefront, Galois).
// ============================================================================
#pragma once

#if defined(__wasm_simd128__) && __has_include(<wasm_simd128.h>)
#include <wasm_simd128.h>
#define HAS_WASM_SIMD 1
#else
#define HAS_WASM_SIMD 0
#endif

#define WASM_EXPORT(name) extern "C" __attribute__((used, visibility("default"), export_name(#name)))

#include <atomic>
#include <type_traits>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <utility>

namespace stockcut
{
    // ------------------------------------------------------------------------
    // 1. Canonical Fixed-Width Primitives & Domain Aliases
    // ------------------------------------------------------------------------
    using u64 = unsigned long long;
    using u32 = unsigned int;
    using u16 = unsigned short;
    using u8  = unsigned char;
    using i64 = long long;
    using i32 = int;
    using i16 = short;
    using i8  = signed char;
    using usize = __SIZE_TYPE__;
    using isize = __PTRDIFF_TYPE__;
    using Key       = u64;
    using TimePoint = i64;

    // ------------------------------------------------------------------------
    // Hardware-Aligned Standard Size Tiers (Stockfish Cacheline Multiples)
    // ------------------------------------------------------------------------
    namespace tier {
        constexpr usize Tiny   = 16;  // 16 bytes: 1 SIMD 128-bit Vector / 4 floats
        constexpr usize Small  = 32;  // 32 bytes: Half L1 Cacheline / 8 floats
        constexpr usize Normal = 64;  // 64 bytes: 1 Exact L1 Cacheline / 16 floats
        constexpr usize Large  = 128; // 128 bytes: 2 L1 Cachelines / 32 floats
        constexpr usize Huge   = 256; // 256 bytes: 4 L1 Cachelines / 64 floats
    }

    enum class SizeTier : usize {
        Tiny   = tier::Tiny,
        Small  = tier::Small,
        Normal = tier::Normal,
        Large  = tier::Large,
        Huge   = tier::Huge
    };

    using Tiny   = std::integral_constant<usize, tier::Tiny>;
    using Small  = std::integral_constant<usize, tier::Small>;
    using Normal = std::integral_constant<usize, tier::Normal>;
    using Large  = std::integral_constant<usize, tier::Large>;
    using Huge   = std::integral_constant<usize, tier::Huge>;

    // ------------------------------------------------------------------------
    // 2. Single-Source Math & Numerical Constants
    // ------------------------------------------------------------------------
    constexpr double DBL_MAX_VAL          = 1.7976931348623157e+308;
    constexpr float  FLT_MAX_SAFE         = 3.402823466e+38f;
    constexpr float  PI_VAL               = 3.14159265358979323846f;
    constexpr double PI_F64               = 3.14159265358979323846;
    constexpr float  TWO_PI_F32           = 6.28318530717958647692f;
    constexpr double TWO_PI_F64           = 6.28318530717958647692;
    constexpr float  INV_TWO_PI_F32       = 0.15915494309189535f;
    constexpr float  DEG_TO_RAD_F32       = PI_VAL / 180.0f;
    constexpr double DEG_TO_RAD_F64       = PI_F64 / 180.0;
    constexpr double GRAD_TO_RAD_F64      = PI_F64 / 200.0;
    constexpr float  RAD_TO_DEG_F32       = 180.0f / PI_VAL;
    constexpr float  INV_255_F32          = 1.0f / 255.0f;
    constexpr double INV_255_F64          = 1.0 / 255.0;

    // ------------------------------------------------------------------------
    // 3. Single-Source Sentinels, Gatekeepers & Epsilons
    // ------------------------------------------------------------------------
    constexpr u32    INVALID_U32          = 0xFFFFFFFFu;
    constexpr u16    INVALID_U16          = 0xFFFFu;
    constexpr u8     INVALID_U8           = 0xFFu;
    constexpr i32    KF_GATEKEEPER_MIN_I32 = -(1 << 30);
    constexpr i32    KF_GATEKEEPER_MAX_I32 = (1 << 30) - 1;
    constexpr double KF_GATEKEEPER_MIN_F64 = -1.0e30;
    constexpr double KF_GATEKEEPER_MAX_F64 = 1.0e30;
    constexpr i64    TIME_MIN_SENTINEL_I64 = -(1LL << 60);
    constexpr i64    TIME_MAX_SENTINEL_I64 = 0x7FFFFFFFFFFFFFFFLL;

    constexpr float  OPACITY_EPSILON      = 0.001f;
    constexpr float  OPACITY_OPAQUE_MIN   = 0.999f;
    constexpr float  IDENTITY_EPSILON     = 0.0001f;
    constexpr float  DELTA_EPSILON        = 0.002f;
    constexpr float  AABB_INF_BOUND       = 1.0e9f;
    constexpr float  RENDER_OFFSCREEN_BOUND = 3.0e38f;
    constexpr float  TEXT_UNCONSTRAINED_W = 100000.0f;

    // ------------------------------------------------------------------------
    // 4. Single-Source Stage, Timeline & Engine Constants
    // ------------------------------------------------------------------------
    constexpr double DEFAULT_STAGE_WIDTH      = 1920.0;
    constexpr double DEFAULT_STAGE_HEIGHT     = 1080.0;
    constexpr float  DEFAULT_STAGE_WIDTH_F32  = 1920.0f;
    constexpr float  DEFAULT_STAGE_HEIGHT_F32 = 1080.0f;
    constexpr u32    DEFAULT_STAGE_WIDTH_U32  = 1920u;
    constexpr u32    DEFAULT_STAGE_HEIGHT_U32 = 1080u;

    constexpr u32    MAX_TIMELINE_SLOTS       = 64;
    constexpr u32    MAX_KEYFRAMES_PER_SLOT   = 32;
    constexpr double FALLBACK_DURATION_1H_MS  = 3600000.0;
    constexpr double DEFAULT_ITEM_DURATION_MS = 1000.0;
    constexpr double PROJECT_TIME_SENTINEL_MS = 999999.0;
    constexpr double MAX_SAFE_Z_INDEX         = 9000000000000000.0;
    constexpr i64    Z_INDEX_SORT_BIAS        = 0x80000000LL;
    constexpr i32    PERMILLE_MAX             = 1000;
    constexpr double DEFAULT_BASS_HOP_SEC     = 0.05;
    constexpr float  DEFAULT_BASS_HOP_F32     = 0.05f;
    constexpr double DEFAULT_FONT_SIZE        = 32.0;
    constexpr double DEFAULT_LINE_HEIGHT      = 1.25;
    constexpr double DIRTY_AREA_FULL_RATIO    = 0.65;
    constexpr int    DIRTY_TILE_FULL_THRESHOLD = 56;
    constexpr double SEEK_DELTA_THRESHOLD_MS  = 50.0;

    constexpr u32    RGBA_OPAQUE_BLACK        = 0xFF000000u;
    constexpr u32    RGBA_OPAQUE_WHITE        = 0xFFFFFFFFu;
    constexpr const char* DEFAULT_FONT_MANIFEST_PATH = "Asset/font/fonts.json";

    // ------------------------------------------------------------------------
    // 5. Canonical Engine Enumerations & Bitflags
    // ------------------------------------------------------------------------
    enum class MediaType : u16
    {
        Root           = 0, // Tầng -1 Root Stage Canvas (luôn hiện diện ở đáy)
        None           = 0, // Sentinel / alias tương thích ngược
        Background     = 1,
        Video          = 2,
        Image          = 3,
        Path           = 4,
        Lyric          = 5,
        Text           = 6,
        Audio          = 7,
        TypeNb         = 8  // Effect đã tách thành autonomous struct Effect riêng biệt
    };
    constexpr u32 MEDIA_TYPE_NB = static_cast<u32>(MediaType::TypeNb);
    constexpr u32 ALL_ITEMS     = 0u; // Chuẩn Stockfish: byTypeBB[ALL_ITEMS] lưu toàn bộ occupied slots
    constexpr u16 ROOT_SLOT_SENTINEL = 0xFFFEu;
    constexpr MediaType MEDIA_TYPE_INVALID = MediaType::TypeNb;

    enum class EffectKind : u8
    {
        None         = 0,
        GaussianBlur = 1,
        Glow         = 2,
        Flicker      = 3,
        Zoom         = 4,
        Shake        = 5,
        Bloom        = 6,
        Intro        = 7,
        Outro        = 8,
        Glitch       = 9,
        Matrix       = 10,
        RainDrop     = 11,
        Turbulent    = 12,
        Twirl        = 13,
        Pinch        = 14,
        Lens         = 15,
        Chromatic    = 16,
        ZoomBlur     = 17,
        MotionBlur   = 18,
        GlitchRgb    = 19,
        CyberScan    = 20,
        Fire         = 21,
        RainBokeh    = 22,
        FilmBurn     = 23,
        Fxaa         = 24,
        EffectNb     = 25
    };
    constexpr u32 EFFECT_KIND_NB = static_cast<u32>(EffectKind::EffectNb);

    enum RainLayerFlag : u32 {
        RAIN_LAYER_NONE       = 0u,
        RAIN_LAYER_STATIC1    = 1u << 0,
        RAIN_LAYER_STATIC2    = 1u << 1,
        RAIN_LAYER_DYNAMIC    = 1u << 2,
        RAIN_LAYER_TRAIL      = 1u << 3,
        RAIN_LAYER_TRAIL2     = 1u << 4,
        RAIN_LAYER_ALL        = 0x1Fu,
        RAIN_FLAG_PREWARM     = 1u << 5,
        RAIN_FLAG_CUSTOM_TINT = 1u << 6
    };

    // Zero-Allocation 256-byte POD Parameter Block for Unified NodeVideo _RaindropPro ("rain drop")
    struct alignas(64) RainDropParams {
        // --- Global (16B) ---
        u32   layerMask          = RAIN_FLAG_PREWARM; // 0..3 (all 5 child layers default false unless enabled)
        u32   blurType           = 1u;     // 4..7   (_blur_type: 1=_glass, 0=_gaussian)
        float strength           = 1.0f;   // 8..11  (_strength)
        float blur               = 0.0f;   // 12..15 (_blur)

        // --- Layer 1: _static1 (20B) ---
        float amount1            = 0.5f;   // 16..19
        float intensity1         = 1.0f;   // 20..23
        float scale1             = 4.0f;   // 24..27
        float noise1             = 1.2f;   // 28..31
        float speed1             = 0.2f;   // 32..35

        // --- Layer 2: _static2 (20B) ---
        float amount2            = 0.5f;   // 36..39
        float intensity2         = 1.0f;   // 40..43
        float scale2             = 1.5f;   // 44..47
        float noise2             = 1.2f;   // 48..51
        float speed2             = 0.1f;   // 52..55

        // --- Layer 3: _dynamic (16B) ---
        float amount3            = 0.6f;   // 56..59
        float intensity3         = 1.0f;   // 60..63
        float scale3             = 2.0f;   // 64..67
        float speed3             = 0.2f;   // 68..71

        // --- Layer 4: _trail / ViewRaindrop (64B) ---
        i32   trailSeed          = 0;      // 72..75
        float trailSpeed         = 1.0f;   // 76..79
        float trailAmount        = 30.0f;  // 80..83
        float trailIntensity     = 0.8f;   // 84..87
        float trailScale         = 1.0f;   // 88..91
        float trailStartSizeMin  = 0.3f;   // 92..95
        float trailStartSizeMax  = 0.5f;   // 96..99
        float trailSpread        = 0.45f;  // 100..103
        float trailLifeMin       = 1.0f;   // 104..107
        float trailLifeMax       = 10.0f;  // 108..111
        float trailSpeedMin      = 0.0f;   // 112..115
        float trailSpeedMax      = 5.0f;   // 116..119
        float trailDirection     = 0.0f;   // 120..123
        float trailRadial        = 0.2f;   // 124..127
        float trailOffset        = 0.0f;   // 128..131
        float trailNoise         = 0.2f;   // 132..135

        // --- Layer 5: _trail2 / RainStreaks + layer_style_glass (64B) ---
        i32   trail2Seed         = 0;      // 136..139
        float trail2Strength     = 0.3f;   // 140..143
        float trail2Amount       = 25.0f;  // 144..147
        float trail2SpeedMin     = 5.0f;   // 148..151
        float trail2SpeedMax     = 15.0f;  // 152..155
        float trail2ScaleMin     = 2.0f;   // 156..159
        float trail2ScaleMax     = 5.0f;   // 160..163
        float trail2IntensityMin = 0.6f;   // 164..167
        float trail2IntensityMax = 1.0f;   // 168..171
        float trail2Rotation     = 0.0f;   // 172..175
        float trail2RandRotation = 0.1f;   // 176..179
        float trail2RandPos      = 1.0f;   // 180..183
        float trail2TintR        = 0.0f;   // 184..187
        float trail2TintG        = 0.0f;   // 188..191
        float trail2TintB        = 0.0f;   // 192..195
        float trail2TintA        = 1.0f;   // 196..199

        // --- Hash & Cache Padding (56B) ---
        u64   paramHash64        = 0ULL;   // 200..207
        u64   reserved[6]        = {};     // 208..255
    };
    static_assert(sizeof(RainDropParams) == 256, "RainDropParams must be 256 bytes");

    enum class TrackMode : u8
    {
        Static     = 0,
        Harmonic   = 1,
        Kinematic  = 2,
        Procedural = 3,
        ModeNb     = 4
    };
    constexpr u32 TRACK_MODE_NB = static_cast<u32>(TrackMode::ModeNb);

    enum class MediaTier : u8
    {
        Static     = 0,
        Finite     = 1,
        Continuous = 2,
        TierNb     = 3
    };
    constexpr u32 MEDIA_TIER_NB = static_cast<u32>(MediaTier::TierNb);

    enum class DynamicNature : u8
    {
        IntrinsicStatic  = 0,
        IntrinsicDynamic = 1,
        NatureNb         = 2
    };
    constexpr u32 DYNAMIC_NATURE_NB = static_cast<u32>(DynamicNature::NatureNb);

    enum class DynamicAxis : u8
    {
        Intrinsic       = 0,
        IntrinsicVisual = 1,
        Keyframe        = 2,
        KeyframeVisual  = 3,
        AxisNb          = 4
    };
    constexpr u32 DYNAMIC_AXIS_NB = static_cast<u32>(DynamicAxis::AxisNb);

    enum TimelineItemFlag : u32
    {
        ITEM_FLAG_NONE            = 0u,
        ITEM_FLAG_MASK_HOLE       = 1u << 0,
        ITEM_FLAG_MASK_CIRCLE     = 1u << 1,
        ITEM_FLAG_SPIN            = 1u << 2,
        ITEM_FLAG_EXPLICIT_WIDTH  = 1u << 3,
        ITEM_FLAG_EXPLICIT_HEIGHT = 1u << 4
    };

    enum RenderQuadMode : u32
    {
        QUAD_MODE_COLOR          = 0u,
        QUAD_MODE_TEXTURE        = 1u,
        QUAD_MODE_MASKED_TEXTURE = 2u,
        QUAD_MODE_TEXT           = 3u,
        QUAD_MODE_TEXT_REVEAL    = 4u
    };

    enum class TextTransformMode : u8
    {
        None       = 0,
        Uppercase  = 1,
        Lowercase  = 2,
        Capitalize = 3,
        ModeNb     = 4
    };
    constexpr u32 TEXT_TRANSFORM_MODE_NB = static_cast<u32>(TextTransformMode::ModeNb);

    enum class TextAlignMode : u8
    {
        Left    = 0,
        Center  = 1,
        Right   = 2,
        Justify = 3,
        ModeNb  = 4
    };
    constexpr u32 TEXT_ALIGN_MODE_NB = static_cast<u32>(TextAlignMode::ModeNb);

    enum class FontCategory : u8
    {
        Sans      = 0,
        Serif     = 1,
        Mono      = 2,
        Condensed = 3,
        Script    = 4,
        CatNb     = 5
    };
    constexpr u32 FONT_CATEGORY_NB = static_cast<u32>(FontCategory::CatNb);

    enum class EasingCurve : u8
    {
        Linear    = 0,
        EaseIn    = 1,
        EaseOut   = 2,
        EaseInOut = 3,
        CurveNb   = 4
    };
    constexpr u32 EASING_CURVE_NB = static_cast<u32>(EasingCurve::CurveNb);

    template <typename T, typename U>
    constexpr inline auto min(T a, U b) noexcept { return (a < b) ? a : b; }
    template <typename T, typename U>
    constexpr inline auto max(T a, U b) noexcept { return (a > b) ? a : b; }
    template <typename T>
    constexpr inline T clamp(T v, T lo, T hi) noexcept { return (v < lo) ? lo : ((v > hi) ? hi : v); }
    template <typename T>
    constexpr inline T abs_val(T v) noexcept { return (v < static_cast<T>(0)) ? -v : v; }

    // ========================================================================
    // 0. BitOps<T>: Toán tử Bit Nguyên Tử Thống Nhất Template <T>
    // ========================================================================
    template <typename T = u64>
    struct BitOps {
        [[nodiscard]] static inline constexpr u32 popcount(T val) noexcept {
            if constexpr (sizeof(T) <= 4) {
                return static_cast<u32>(__builtin_popcount(static_cast<unsigned int>(val)));
            } else {
                return static_cast<u32>(__builtin_popcountll(static_cast<unsigned long long>(val)));
            }
        }

        [[nodiscard]] static inline constexpr u32 popcount64(u64 val) noexcept {
            return static_cast<u32>(__builtin_popcountll(val));
        }

        [[nodiscard]] static inline constexpr u32 lsb(T val) noexcept {
            if constexpr (sizeof(T) <= 4) {
                return static_cast<u32>(__builtin_ctz(static_cast<unsigned int>(val)));
            } else {
                return static_cast<u32>(__builtin_ctzll(static_cast<unsigned long long>(val)));
            }
        }

        [[nodiscard]] static inline constexpr u32 safe_lsb(T val, u32 defaultVal = static_cast<u32>(sizeof(T) * 8)) noexcept {
            if (val == 0) return defaultVal;
            return lsb(val);
        }

        [[nodiscard]] static inline constexpr u32 msb(T val) noexcept {
            if constexpr (sizeof(T) <= 4) {
                return static_cast<u32>(31 - __builtin_clz(static_cast<unsigned int>(val)));
            } else {
                return static_cast<u32>(63 - __builtin_clzll(static_cast<unsigned long long>(val)));
            }
        }

        [[nodiscard]] static inline constexpr i32 safe_msb(T val, i32 defaultVal = -1) noexcept {
            if (val == 0) return defaultVal;
            return static_cast<i32>(msb(val));
        }

        // Stockfish-style pop_lsb: trích xuất lsb và xóa nó khỏi biến truyền vào
        static inline u32 pop_lsb(T& val) noexcept {
            const u32 s = lsb(val);
            val &= val - static_cast<T>(1);
            return s;
        }
    };

    // ========================================================================
    // Toán tử mặt nạ không rẽ nhánh (Branchless Masking & Arithmetic)
    // ========================================================================
    template<bool Cond, typename U = u64>
    [[nodiscard]] constexpr U branchless_mask() noexcept {
        static_assert(std::is_unsigned_v<U>, "branchless_mask requires an unsigned integer type");
        if constexpr (Cond) {
            return static_cast<U>(~static_cast<U>(0));
        } else {
            return static_cast<U>(0);
        }
    }

    template<typename U = u64>
    [[nodiscard]] constexpr U branchless_mask(bool cond) noexcept {
        static_assert(std::is_unsigned_v<U>, "branchless_mask requires an unsigned integer type");
        return static_cast<U>(0) - static_cast<U>(cond);
    }

} // namespace stockcut

// ============================================================================
// Phân Tầng Chuyên Ngành Kỹ Thuật (Stockfish Architecture):
// - memory.h: Continuous memory containers, MultiArray, SentinelArray, ValueList, Span.
// - history.h: Heuristic decay, RelaxedAtomic, StatsEntry, BoundedGravity, Envelopes.
// - data_structures.h: Monoid algebra, Group, FlatMonoidSequence, TrackBucket, ZRankArena.
// - scientific_foundations.h: 5 scientific pillars (Hamiltonian, Sheaf, Gibbs, Wavefront, Galois).
// ============================================================================
#include "memory.h"
#include "history.h"
#include "data_structures.h"
#include "nested_mark.h"
#include "scientific_foundations.h"
