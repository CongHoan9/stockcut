// ============================================================================
// CoreEngine - Unified StringSpan, ASCII/CSS, Angle, Color & UTF-8 Primitives
// ============================================================================
#pragma once
#include "types.h"
#include <cstring>

namespace stockcut
{
    // ------------------------------------------------------------------------
    // 1. Canonical Non-Owning StringSpan (Replaces all ad-hoc Span structs)
    // ------------------------------------------------------------------------
    struct StringSpan
    {
        const char* data   = "";
        u32         length = 0;

        constexpr StringSpan() noexcept = default;
        constexpr StringSpan(const char* ptr, u32 len) noexcept
            : data(ptr ? ptr : ""), length(ptr ? len : 0u) {}
        constexpr StringSpan(const char* first, const char* last) noexcept
            : data(first ? first : ""),
              length((first && last && last > first) ? static_cast<u32>(last - first) : 0u) {}

        static constexpr StringSpan from_cstr(const char* s) noexcept
        {
            if (!s) return {};
            u32 len = 0;
            while (s[len]) ++len;
            return {s, len};
        }

        [[nodiscard]] constexpr bool empty() const noexcept { return length == 0; }
        [[nodiscard]] constexpr char operator[](u32 idx) const noexcept { return data[idx]; }
        [[nodiscard]] constexpr const char* begin() const noexcept { return data; }
        [[nodiscard]] constexpr const char* end() const noexcept { return data + length; }
    };

    // ------------------------------------------------------------------------
    // 2. Unified ASCII Character, Trim & Case-Insensitive Comparison Helpers
    // ------------------------------------------------------------------------
    [[nodiscard]] constexpr bool is_ascii_space(char c) noexcept
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    [[nodiscard]] constexpr bool is_ascii_upper(char c) noexcept
    {
        return c >= 'A' && c <= 'Z';
    }

    [[nodiscard]] constexpr char ascii_lower(char c) noexcept
    {
        return is_ascii_upper(c) ? static_cast<char>(c + ('a' - 'A')) : c;
    }

    [[nodiscard]] constexpr char ascii_upper(char c) noexcept
    {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
    }

    [[nodiscard]] constexpr i32 hex_nibble(char c) noexcept
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    constexpr void lower_ascii_inplace(char* value) noexcept
    {
        if (!value) return;
        for (char* p = value; *p; ++p) *p = ascii_lower(*p);
    }

    constexpr void trim_cstr_inplace(char* value) noexcept
    {
        if (!value) return;
        u32 first = 0;
        while (value[first] && is_ascii_space(value[first])) ++first;
        u32 last = first;
        while (value[last]) ++last;
        while (last > first && is_ascii_space(value[last - 1])) --last;
        u32 out = 0;
        while (first < last) value[out++] = value[first++];
        value[out] = '\0';
    }

    [[nodiscard]] constexpr StringSpan trim_span(StringSpan s) noexcept
    {
        while (s.length && is_ascii_space(s.data[0])) {
            ++s.data;
            --s.length;
        }
        while (s.length && is_ascii_space(s.data[s.length - 1])) {
            --s.length;
        }
        return s;
    }

    [[nodiscard]] [[gnu::always_inline]] constexpr bool cstr_equal(const char* left, const char* right) noexcept
    {
        if (left == right) return true;
        if (!left || !right) return false;
        while (*left && *left == *right) {
            ++left;
            ++right;
        }
        return *left == *right;
    }

    // ------------------------------------------------------------------------
    // Compile-Time Literal Matching (Zero-Loop, Unrolled, Zero Memory Allocation)
    // ------------------------------------------------------------------------
    template <usize N>
    [[nodiscard]] [[gnu::always_inline]] constexpr bool match_suffix(const char* s, const char (&lit)[N]) noexcept
    {
        // lit có N bytes bao gồm ký tự kết thúc '\0'.
        // s[0] đã được kiểm tra bởi switch(s[0]). Ta so sánh s[1..N-1].
        #pragma clang loop unroll(full)
        for (usize i = 1; i < N; ++i) {
            if (s[i] != lit[i]) return false;
        }
        return true;
    }

    template <usize N>
    [[nodiscard]] [[gnu::always_inline]] constexpr bool match_literal(const char* s, const char (&lit)[N]) noexcept
    {
        if (!s) return false;
        #pragma clang loop unroll(full)
        for (usize i = 0; i < N; ++i) {
            if (s[i] != lit[i]) return false;
        }
        return true;
    }

    [[nodiscard]] inline bool cstr_equal_n(const char* left, const char* right, usize len) noexcept
    {
        if (len == 0) return true;
        if (!left || !right) return false;
        return __builtin_memcmp(left, right, len) == 0;
    }

    [[nodiscard]] constexpr bool cstr_iequal(const char* left, const char* right) noexcept
    {
        if (!left || !right) return left == right;
        while (*left && *right && ascii_lower(*left) == ascii_lower(*right)) {
            ++left;
            ++right;
        }
        return *left == '\0' && *right == '\0';
    }

    constexpr usize copy_cstr_bounded(char* dst, usize dstCap, const char* src) noexcept
    {
        if (!dst || dstCap == 0) return 0;
        if (!src) {
            dst[0] = '\0';
            return 0;
        }
        usize i = 0;
        const usize limit = dstCap - 1;
        while (i < limit && src[i] != '\0') {
            dst[i] = src[i];
            ++i;
        }
        dst[i] = '\0';
        return i;
    }

    [[nodiscard]] constexpr bool cstr_equal_icase_n(const char* value, const char* expected, u32 length) noexcept
    {
        if (!value || !expected) return false;
        for (u32 i = 0; i < length; ++i) {
            if (!value[i] || ascii_lower(value[i]) != ascii_lower(expected[i])) return false;
        }
        return true;
    }

    [[nodiscard]] inline bool span_equals_n(StringSpan value, const char* expected, u32 length) noexcept
    {
        return value.length == length &&
               (length == 0 || __builtin_memcmp(value.data, expected, length) == 0);
    }

    template <usize N>
    [[nodiscard]] [[gnu::always_inline]] constexpr bool span_equals(StringSpan value, const char (&expected)[N]) noexcept
    {
        constexpr u32 expectedLen = static_cast<u32>(N - 1);
        if (value.length != expectedLen) return false;
        #pragma clang loop unroll(full)
        for (usize i = 0; i < expectedLen; ++i) {
            if (value.data[i] != expected[i]) return false;
        }
        return true;
    }

    [[nodiscard]] constexpr bool span_equals(StringSpan value, const char* expected) noexcept
    {
        if (!expected) return false;
        u32 i = 0;
        while (expected[i]) {
            if (i >= value.length || value.data[i] != expected[i]) return false;
            ++i;
        }
        return i == value.length;
    }

    // ========================================================================
    // Stockfish-Style Centralized MediaType Descriptor & make_media_type Mapping
    // ========================================================================
    constexpr const char* kMediaTypeNames[MEDIA_TYPE_NB] = {
        "none",        // 0: None
        "background",  // 1: Background
        "video",       // 2: Video
        "image",       // 3: Image
        "path",        // 4: Path
        "lyric",       // 5: Lyric
        "text",        // 6: Text
        "audio"        // 7: Audio
    };

    // Ánh xạ O(1) từ enum index (Stockfish make_piece style: zero-branch direct cast)
    template <typename T>
        requires std::is_integral_v<T>
    [[nodiscard]] constexpr MediaType make_media_type(T idx) noexcept
    {
        return static_cast<MediaType>(idx);
    }

    template <MediaType T>
    [[nodiscard]] constexpr MediaType make_media_type() noexcept
    {
        return T;
    }

    [[nodiscard]] constexpr const char* media_type_name(MediaType type) noexcept
    {
        return kMediaTypeNames[static_cast<u32>(type)];
    }

    template <MediaType T>
    [[nodiscard]] constexpr const char* media_type_name() noexcept
    {
        return kMediaTypeNames[static_cast<u32>(T)];
    }

    [[nodiscard]] constexpr bool is_valid_media_type(MediaType type) noexcept
    {
        return static_cast<u32>(type) > 0 && static_cast<u32>(type) < MEDIA_TYPE_NB;
    }

    // Ánh xạ từ C-string (Stockfish parser style: Zero-Loop, Unrolled Literal Suffix Matching)
    template <bool CheckNull = true>
    [[nodiscard]] [[gnu::always_inline]] constexpr MediaType make_media_type(const char* name) noexcept
    {
        if constexpr (CheckNull) {
            if (!name) return MediaType::None;
        }
        // Physical Gatekeeper: Ký tự '\0' tự nhiên rơi vào default: return None.
        // Tuyệt đối không cần kiểm tra name[0] == '\0' trước!
        switch (name[0]) {
            case 'v': return match_suffix(name, "video") ? MediaType::Video : MediaType::None;
            case 'i': return match_suffix(name, "image") ? MediaType::Image : MediaType::None;
            case 'p': return match_suffix(name, "path") ? MediaType::Path : MediaType::None;
            case 'l': return match_suffix(name, "lyric") ? MediaType::Lyric : MediaType::None;
            case 't': return match_suffix(name, "text") ? MediaType::Text : MediaType::None;
            case 'a': return match_suffix(name, "audio") ? MediaType::Audio : MediaType::None;
            case 'e': return MediaType::None; // Effect là autonomous struct Effect riêng biệt, không thuộc MediaType
            case 'b': return match_suffix(name, "background") ? MediaType::Background : MediaType::None;
            case 'd': return match_suffix(name, "div") ? MediaType::Background : MediaType::None;
            case 'n': return match_suffix(name, "none") ? MediaType::None : MediaType::None;
            default:  return MediaType::None;
        }
    }

    // Ánh xạ từ StringSpan (Góc độ vật lý: 1 thứ rỗng không có kích thước không gian -> không tương tác,
    // switch (span.length) nhảy thẳng default: return None trong 1 chu kỳ vi xử lý,
    // 0 kiểm tra non-null, 0 chi phí duyệt bộ nhớ!)
    [[nodiscard]] [[gnu::always_inline]] constexpr MediaType make_media_type(StringSpan span) noexcept
    {
        switch (span.length) {
            case 3:
                return (span.data[0]=='d' && span.data[1]=='i' && span.data[2]=='v') ? MediaType::Background : MediaType::None;
            case 4:
                switch (span.data[0]) {
                    case 'p': return (span.data[1]=='a' && span.data[2]=='t' && span.data[3]=='h') ? MediaType::Path : MediaType::None;
                    case 't': return (span.data[1]=='e' && span.data[2]=='x' && span.data[3]=='t') ? MediaType::Text : MediaType::None;
                    case 'n': return (span.data[1]=='o' && span.data[2]=='n' && span.data[3]=='e') ? MediaType::None : MediaType::None;
                    default:  return MediaType::None;
                }
            case 5:
                switch (span.data[0]) {
                    case 'v': return (span.data[1]=='i' && span.data[2]=='d' && span.data[3]=='e' && span.data[4]=='o') ? MediaType::Video : MediaType::None;
                    case 'i': return (span.data[1]=='m' && span.data[2]=='a' && span.data[3]=='g' && span.data[4]=='e') ? MediaType::Image : MediaType::None;
                    case 'l': return (span.data[1]=='y' && span.data[2]=='r' && span.data[3]=='i' && span.data[4]=='c') ? MediaType::Lyric : MediaType::None;
                    case 'a': return (span.data[1]=='u' && span.data[2]=='d' && span.data[3]=='i' && span.data[4]=='o') ? MediaType::Audio : MediaType::None;
                    default:  return MediaType::None;
                }
            case 6:
                return MediaType::None; // Effect là autonomous struct Effect riêng biệt, không thuộc MediaType
            case 10:
                return (span.data[0]=='b' && span.data[1]=='a' && span.data[2]=='c' && span.data[3]=='k' &&
                        span.data[4]=='g' && span.data[5]=='r' && span.data[6]=='o' && span.data[7]=='u' &&
                        span.data[8]=='n' && span.data[9]=='d') ? MediaType::Background : MediaType::None;
            default:
                return MediaType::None;
        }
    }

    // ========================================================================
    // Stockfish-Style Centralized EffectKind Descriptor & make_effect_kind Mapping
    // ========================================================================
    constexpr const char* kEffectKindNames[EFFECT_KIND_NB] = {
        "none",          // 0: None
        "blur",          // 1: GaussianBlur
        "glow",          // 2: Glow
        "flicker",       // 3: Flicker
        "zoom",          // 4: Zoom
        "shake",         // 5: Shake
        "bloom",         // 6: Bloom
        "intro",         // 7: Intro
        "outro",         // 8: Outro
        "glitch",        // 9: Glitch
        "matrix",        // 10: Matrix
        "rain-drop",     // 11: RainDrop
        "turbulent",     // 12: Turbulent
        "twirl",         // 13: Twirl
        "pinch",         // 14: Pinch
        "lens",          // 15: Lens
        "chromatic",     // 16: Chromatic
        "zoom-blur",     // 17: ZoomBlur
        "motion-blur",   // 18: MotionBlur
        "glitch-rgb",    // 19: GlitchRgb
        "cyber-scan",    // 20: CyberScan
        "fire",          // 21: Fire
        "rain-bokeh",    // 22: RainBokeh
        "film-burn",     // 23: FilmBurn
        "fxaa"           // 24: Fxaa
    };

    struct EffectAlias {
        const char* alias;
        EffectKind kind;
    };

    constexpr EffectAlias kEffectAliases[] = {
        {"gaussian-blur", EffectKind::GaussianBlur},
        {"gaussianblur", EffectKind::GaussianBlur},
        {"matrixerror", EffectKind::Matrix},
        {"matrix-error", EffectKind::Matrix},
        {"matrix_error", EffectKind::Matrix},
        {"loimatran", EffectKind::Matrix},
        {"glitch2", EffectKind::GlitchRgb},
        {"rain drop", EffectKind::RainDrop},
        {"rain_drop", EffectKind::RainDrop},
        {"raindrop", EffectKind::RainDrop},
        {"raindroppro", EffectKind::RainDrop},
        {"_raindroppro", EffectKind::RainDrop},
        {"rain-drop-pro", EffectKind::RainDrop},
        {"transition-zoom", EffectKind::ZoomBlur},
        {"motion-smool", EffectKind::MotionBlur},
        {"radio", EffectKind::CyberScan},
        {"radar", EffectKind::CyberScan},
        {"flame", EffectKind::Fire},
        {"fire-border", EffectKind::Fire},
        {"drive-home", EffectKind::RainBokeh},
        {"outro2", EffectKind::FilmBurn},
        {"antialiasing", EffectKind::Fxaa},
        {"anti-aliasing", EffectKind::Fxaa},
        {"aa", EffectKind::Fxaa}
    };

    // Ánh xạ O(1) từ enum index (Stockfish make_piece style: zero-branch direct cast)
    template <typename T>
        requires std::is_integral_v<T>
    [[nodiscard]] constexpr EffectKind make_effect_kind(T idx) noexcept
    {
        return static_cast<EffectKind>(idx);
    }

    template <EffectKind E>
    [[nodiscard]] constexpr EffectKind make_effect_kind() noexcept
    {
        return E;
    }

    [[nodiscard]] constexpr const char* effect_kind_name(EffectKind kind) noexcept
    {
        const u32 idx = static_cast<u32>(kind);
        return (idx < EFFECT_KIND_NB) ? kEffectKindNames[idx] : "none";
    }

    template <EffectKind E>
    [[nodiscard]] constexpr const char* effect_kind_name() noexcept
    {
        return kEffectKindNames[static_cast<u32>(E)];
    }

    [[nodiscard]] constexpr bool is_valid_effect_kind(EffectKind kind) noexcept
    {
        return static_cast<u32>(kind) > 0 && static_cast<u32>(kind) < EFFECT_KIND_NB;
    }

    [[nodiscard]] constexpr u64 effect_mask(EffectKind kind) noexcept
    {
        return 1ULL << static_cast<u8>(kind);
    }

    // Ánh xạ từ C-string (Stockfish parser style: Zero-Loop, Unrolled Literal Suffix Matching)
    [[nodiscard]] [[gnu::always_inline]] constexpr EffectKind make_effect_kind(const char* name) noexcept
    {
        if (!name) return EffectKind::None;
        switch (name[0]) {
            case 'z':
                if (match_suffix(name, "zoom")) return EffectKind::Zoom;
                if (match_suffix(name, "zoom-blur")) return EffectKind::ZoomBlur;
                break;
            case 's':
                if (match_suffix(name, "shake")) return EffectKind::Shake;
                break;
            case 'f':
                if (match_suffix(name, "flicker")) return EffectKind::Flicker;
                if (match_suffix(name, "fire")) return EffectKind::Fire;
                if (match_suffix(name, "film-burn")) return EffectKind::FilmBurn;
                if (match_suffix(name, "fxaa")) return EffectKind::Fxaa;
                if (match_suffix(name, "flame") || match_suffix(name, "fire-border")) return EffectKind::Fire;
                break;
            case 'b':
                if (match_suffix(name, "blur")) return EffectKind::GaussianBlur;
                if (match_suffix(name, "bloom")) return EffectKind::Bloom;
                break;
            case 'g':
                if (match_suffix(name, "glitch")) return EffectKind::Glitch;
                if (match_suffix(name, "glow")) return EffectKind::Glow;
                if (match_suffix(name, "glitch-rgb") || match_suffix(name, "glitch2")) return EffectKind::GlitchRgb;
                if (match_suffix(name, "gaussian-blur") || match_suffix(name, "gaussianblur")) return EffectKind::GaussianBlur;
                break;
            case 'i':
                if (match_suffix(name, "intro")) return EffectKind::Intro;
                break;
            case 'o':
                if (match_suffix(name, "outro")) return EffectKind::Outro;
                if (match_suffix(name, "outro2")) return EffectKind::FilmBurn;
                break;
            case 'm':
                if (match_suffix(name, "matrix")) return EffectKind::Matrix;
                if (match_suffix(name, "motion-blur") || match_suffix(name, "motion-smool")) return EffectKind::MotionBlur;
                if (match_suffix(name, "matrixerror") || match_suffix(name, "matrix-error") || match_suffix(name, "matrix_error")) return EffectKind::Matrix;
                break;
            case 'r':
                if (match_suffix(name, "rain drop") || match_suffix(name, "rain-drop") ||
                    match_suffix(name, "rain_drop") || match_suffix(name, "raindrop") ||
                    match_suffix(name, "raindroppro") || match_suffix(name, "_raindroppro")) return EffectKind::RainDrop;
                if (match_suffix(name, "rain-bokeh") || match_suffix(name, "drive-home")) return EffectKind::RainBokeh;
                if (match_suffix(name, "radio") || match_suffix(name, "radar")) return EffectKind::CyberScan;
                break;
            case 'c':
                if (match_suffix(name, "chromatic")) return EffectKind::Chromatic;
                if (match_suffix(name, "cyber-scan")) return EffectKind::CyberScan;
                break;
            case 't':
                if (match_suffix(name, "turbulent")) return EffectKind::Turbulent;
                if (match_suffix(name, "twirl")) return EffectKind::Twirl;
                if (match_suffix(name, "transition-zoom")) return EffectKind::ZoomBlur;
                break;
            case 'p':
                if (match_suffix(name, "pinch")) return EffectKind::Pinch;
                break;
            case 'l':
                if (match_suffix(name, "lens")) return EffectKind::Lens;
                if (match_suffix(name, "loimatran")) return EffectKind::Matrix;
                break;
            case 'a':
                if (match_suffix(name, "antialiasing") || match_suffix(name, "anti-aliasing") || match_suffix(name, "aa")) return EffectKind::Fxaa;
                break;
            case 'd':
                if (match_suffix(name, "drive-home")) return EffectKind::RainBokeh;
                break;
            default:
                break;
        }
        for (u32 i = 1; i < EFFECT_KIND_NB; ++i)
        {
            if (cstr_equal(name, kEffectKindNames[i]))
                return static_cast<EffectKind>(i);
        }
        for (const auto& a : kEffectAliases)
        {
            if (cstr_equal(name, a.alias))
                return a.kind;
        }
        return EffectKind::None;
    }

    // Ánh xạ từ StringSpan (Stockfish parser style với First-Byte Jump Table O(1))
    [[nodiscard]] [[gnu::always_inline]] constexpr EffectKind make_effect_kind(StringSpan span) noexcept
    {
        if (span.empty()) return EffectKind::None;
        switch (span.data[0]) {
            case 'z':
                if (span_equals(span, "zoom")) return EffectKind::Zoom;
                if (span_equals(span, "zoom-blur")) return EffectKind::ZoomBlur;
                break;
            case 's':
                if (span_equals(span, "shake")) return EffectKind::Shake;
                break;
            case 'f':
                if (span_equals(span, "flicker")) return EffectKind::Flicker;
                if (span_equals(span, "fire")) return EffectKind::Fire;
                if (span_equals(span, "film-burn")) return EffectKind::FilmBurn;
                if (span_equals(span, "fxaa")) return EffectKind::Fxaa;
                if (span_equals(span, "flame") || span_equals(span, "fire-border")) return EffectKind::Fire;
                break;
            case 'b':
                if (span_equals(span, "blur")) return EffectKind::GaussianBlur;
                if (span_equals(span, "bloom")) return EffectKind::Bloom;
                break;
            case 'g':
                if (span_equals(span, "glitch")) return EffectKind::Glitch;
                if (span_equals(span, "glow")) return EffectKind::Glow;
                if (span_equals(span, "glitch-rgb") || span_equals(span, "glitch2")) return EffectKind::GlitchRgb;
                if (span_equals(span, "gaussian-blur") || span_equals(span, "gaussianblur")) return EffectKind::GaussianBlur;
                break;
            case 'i':
                if (span_equals(span, "intro")) return EffectKind::Intro;
                break;
            case 'o':
                if (span_equals(span, "outro")) return EffectKind::Outro;
                if (span_equals(span, "outro2")) return EffectKind::FilmBurn;
                break;
            case 'm':
                if (span_equals(span, "matrix")) return EffectKind::Matrix;
                if (span_equals(span, "motion-blur") || span_equals(span, "motion-smool")) return EffectKind::MotionBlur;
                if (span_equals(span, "matrixerror") || span_equals(span, "matrix-error") || span_equals(span, "matrix_error")) return EffectKind::Matrix;
                break;
            case 'r':
                if (span_equals(span, "rain drop") || span_equals(span, "rain-drop") ||
                    span_equals(span, "rain_drop") || span_equals(span, "raindrop") ||
                    span_equals(span, "raindroppro") || span_equals(span, "_raindroppro")) return EffectKind::RainDrop;
                if (span_equals(span, "rain-bokeh") || span_equals(span, "drive-home")) return EffectKind::RainBokeh;
                if (span_equals(span, "radio") || span_equals(span, "radar")) return EffectKind::CyberScan;
                break;
            case 'c':
                if (span_equals(span, "chromatic")) return EffectKind::Chromatic;
                if (span_equals(span, "cyber-scan")) return EffectKind::CyberScan;
                break;
            case 't':
                if (span_equals(span, "turbulent")) return EffectKind::Turbulent;
                if (span_equals(span, "twirl")) return EffectKind::Twirl;
                if (span_equals(span, "transition-zoom")) return EffectKind::ZoomBlur;
                break;
            case 'p':
                if (span_equals(span, "pinch")) return EffectKind::Pinch;
                break;
            case 'l':
                if (span_equals(span, "lens")) return EffectKind::Lens;
                if (span_equals(span, "loimatran")) return EffectKind::Matrix;
                break;
            case 'a':
                if (span_equals(span, "antialiasing") || span_equals(span, "anti-aliasing") || span_equals(span, "aa")) return EffectKind::Fxaa;
                break;
            case 'd':
                if (span_equals(span, "drive-home")) return EffectKind::RainBokeh;
                break;
            default:
                break;
        }
        for (u32 i = 1; i < EFFECT_KIND_NB; ++i)
        {
            if (span_equals(span, kEffectKindNames[i]))
                return static_cast<EffectKind>(i);
        }
        for (const auto& a : kEffectAliases)
        {
            if (span_equals(span, a.alias))
                return a.kind;
        }
        return EffectKind::None;
    }

    // Stockfish-Style Unified Alias: make_effect == make_effect_kind
    template <typename... Args>
    [[nodiscard]] constexpr EffectKind make_effect(Args&&... args) noexcept
    {
        return make_effect_kind(std::forward<Args>(args)...);
    }

    [[nodiscard]] constexpr bool span_equals_icase_n(StringSpan value, const char* expectedLower, u32 length) noexcept
    {
        value = trim_span(value);
        if (value.length != length) return false;
        for (u32 i = 0; i < length; ++i) {
            if (ascii_lower(value.data[i]) != ascii_lower(expectedLower[i])) return false;
        }
        return true;
    }

    [[nodiscard]] constexpr bool span_equals_icase(StringSpan value, const char* expected) noexcept
    {
        value = trim_span(value);
        u32 i = 0;
        while (expected[i]) {
            if (i >= value.length || ascii_lower(value.data[i]) != ascii_lower(expected[i])) return false;
            ++i;
        }
        return i == value.length;
    }

    [[nodiscard]] inline bool span_ends_with(StringSpan value, const char* suffix, u32 suffixLength) noexcept
    {
        return value.length >= suffixLength &&
               (suffixLength == 0 || __builtin_memcmp(value.data + value.length - suffixLength, suffix, suffixLength) == 0);
    }

    [[nodiscard]] constexpr bool span_contains_icase(StringSpan value, const char* needleLower, u32 length) noexcept
    {
        if (!length || value.length < length) return false;
        for (u32 start = 0; start <= value.length - length; ++start) {
            u32 matched = 0;
            while (matched < length && ascii_lower(value.data[start + matched]) == needleLower[matched]) {
                ++matched;
            }
            if (matched == length) return true;
        }
        return false;
    }

    // ------------------------------------------------------------------------
    // 3. Unified CSS Number & Transform `rotate(...)` Angle Parser
    // ------------------------------------------------------------------------
    inline bool parse_css_number_prefix(StringSpan value, double& result, u32* outConsumed = nullptr) noexcept
    {
        value = trim_span(value);
        u32 pos = 0;
        bool negative = false;
        if (pos < value.length && (value.data[pos] == '-' || value.data[pos] == '+')) {
            negative = (value.data[pos++] == '-');
        }
        double mantissa = 0.0;
        bool hasDigit = false;
        while (pos < value.length && value.data[pos] >= '0' && value.data[pos] <= '9') {
            mantissa = mantissa * 10.0 + static_cast<double>(value.data[pos++] - '0');
            hasDigit = true;
        }
        if (pos < value.length && value.data[pos] == '.') {
            ++pos;
            double place = 0.1;
            while (pos < value.length && value.data[pos] >= '0' && value.data[pos] <= '9') {
                mantissa += static_cast<double>(value.data[pos++] - '0') * place;
                place *= 0.1;
                hasDigit = true;
            }
        }
        if (!hasDigit) return false;
        i32 exponent = 0;
        bool expNeg = false;
        if (pos < value.length && (value.data[pos] == 'e' || value.data[pos] == 'E')) {
            u32 expPos = pos + 1;
            if (expPos < value.length && (value.data[expPos] == '-' || value.data[expPos] == '+')) {
                expNeg = (value.data[expPos++] == '-');
            }
            const u32 firstExp = expPos;
            while (expPos < value.length && value.data[expPos] >= '0' && value.data[expPos] <= '9') {
                if (exponent < 308) exponent = exponent * 10 + (value.data[expPos] - '0');
                ++expPos;
            }
            if (expPos != firstExp) {
                pos = expPos;
                if (exponent > 308) exponent = 308;
            }
        }
        double scale = 1.0;
        for (i32 i = 0; i < exponent; ++i) scale *= 10.0;
        result = mantissa * (expNeg ? (1.0 / scale) : scale);
        if (negative) result = -result;
        if (outConsumed) *outConsumed = pos;
        return true;
    }

    // Unified CSS `rotate(...)` extractor supporting deg, rad, grad, turn
    template <bool ReturnRadians>
    inline bool parse_css_transform_rotate(StringSpan transform, double& outAngle) noexcept
    {
        for (u32 i = 0; i + 7 <= transform.length; ++i) {
            if (i > 0) {
                const char prev = ascii_lower(transform.data[i - 1]);
                if ((prev >= 'a' && prev <= 'z') || prev == '-') continue;
            }
            if (!cstr_equal_icase_n(transform.data + i, "rotate", 6)) continue;
            u32 pos = i + 6;
            while (pos < transform.length && is_ascii_space(transform.data[pos])) ++pos;
            if (pos >= transform.length || transform.data[pos] != '(') continue;
            ++pos;
            u32 close = pos;
            while (close < transform.length && transform.data[close] != ')') ++close;
            if (close >= transform.length) return false;
            StringSpan arg = trim_span({transform.data + pos, close - pos});
            double val = 0.0;
            u32 consumed = 0;
            if (!parse_css_number_prefix(arg, val, &consumed)) return false;
            StringSpan unit = trim_span({arg.data + consumed, arg.length - consumed});
            double deg = val;
            if (span_equals_icase_n(unit, "rad", 3)) {
                deg = val * (180.0 / PI_F64);
            } else if (span_equals_icase_n(unit, "grad", 4)) {
                deg = val * 0.9;
            } else if (span_equals_icase_n(unit, "turn", 4)) {
                deg = val * 360.0;
            }
            outAngle = ReturnRadians ? (deg * DEG_TO_RAD_F64) : deg;
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------------
    // CSS Transform Origin Parser
    // Formats:
    //   "(300, 300)", "(300px, 300px)", "(50%, 50%)"
    //   "300px 300px", "300 300", "50% 50%", "50%, 50%"
    //   "center", "center center", "top left", "bottom right", etc.
    // ------------------------------------------------------------------------
    struct CssTransformOrigin {
        double x = 0.5;
        double y = 0.5;
        bool isPixelX = false;
        bool isPixelY = false;

        inline void resolve(double baseW, double baseH, double& outPxX, double& outPxY) const noexcept {
            outPxX = isPixelX ? x : (x * baseW);
            outPxY = isPixelY ? y : (y * baseH);
        }
    };

    inline bool parse_css_transform_origin(StringSpan value, CssTransformOrigin& outOrigin) noexcept
    {
        value = trim_span(value);
        if (value.empty()) return false;
        if (value.data[0] == '(' && value.data[value.length - 1] == ')') {
            value = trim_span({value.data + 1, value.length - 2});
        }
        if (value.empty()) return false;

        const auto parseOriginToken = [](StringSpan tok, double& outVal, bool& outIsPx) -> bool {
            tok = trim_span(tok);
            if (tok.empty()) return false;
            if (span_equals_icase_n(tok, "center", 6)) {
                outVal = 0.5; outIsPx = false; return true;
            } else if (span_equals_icase_n(tok, "top", 3) || span_equals_icase_n(tok, "left", 4)) {
                outVal = 0.0; outIsPx = false; return true;
            } else if (span_equals_icase_n(tok, "bottom", 6) || span_equals_icase_n(tok, "right", 5)) {
                outVal = 1.0; outIsPx = false; return true;
            }
            double num = 0.0;
            u32 consumed = 0;
            if (!parse_css_number_prefix(tok, num, &consumed)) return false;
            StringSpan unit = trim_span({tok.data + consumed, tok.length - consumed});
            if (unit.length && unit.data[0] == '%') {
                outVal = num / 100.0;
                outIsPx = false;
            } else {
                outVal = num;
                outIsPx = true;
            }
            return true;
        };

        u32 firstEnd = 0;
        while (firstEnd < value.length && !is_ascii_space(value.data[firstEnd]) && value.data[firstEnd] != ',') {
            ++firstEnd;
        }
        StringSpan tok1 = {value.data, firstEnd};
        double v1 = 0.5; bool isPx1 = false;
        if (!parseOriginToken(tok1, v1, isPx1)) return false;

        u32 secondStart = firstEnd;
        while (secondStart < value.length && (is_ascii_space(value.data[secondStart]) || value.data[secondStart] == ',')) {
            ++secondStart;
        }
        if (secondStart >= value.length) {
            if (span_equals_icase_n(tok1, "top", 3) || span_equals_icase_n(tok1, "bottom", 6)) {
                outOrigin.x = 0.5; outOrigin.isPixelX = false;
                outOrigin.y = v1;  outOrigin.isPixelY = isPx1;
            } else {
                outOrigin.x = v1;  outOrigin.isPixelX = isPx1;
                outOrigin.y = isPx1 ? v1 : 0.5; outOrigin.isPixelY = isPx1;
            }
            return true;
        }

        u32 secondEnd = secondStart;
        while (secondEnd < value.length && !is_ascii_space(value.data[secondEnd]) && value.data[secondEnd] != ',') {
            ++secondEnd;
        }
        StringSpan tok2 = {value.data + secondStart, secondEnd - secondStart};
        double v2 = 0.5; bool isPx2 = false;
        if (!parseOriginToken(tok2, v2, isPx2)) return false;

        if ((span_equals_icase_n(tok1, "top", 3) || span_equals_icase_n(tok1, "bottom", 6)) &&
            (span_equals_icase_n(tok2, "left", 4) || span_equals_icase_n(tok2, "right", 5))) {
            outOrigin.x = v2; outOrigin.isPixelX = isPx2;
            outOrigin.y = v1; outOrigin.isPixelY = isPx1;
        } else {
            outOrigin.x = v1; outOrigin.isPixelX = isPx1;
            outOrigin.y = v2; outOrigin.isPixelY = isPx2;
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // CSS Scale Parser (Standalone Property & Transform Functions)
    // ------------------------------------------------------------------------
    inline bool parse_css_scale_property(StringSpan value, double& outSx, double& outSy) noexcept
    {
        value = trim_span(value);
        if (value.empty()) return false;
        double s1 = 1.0;
        u32 consumed = 0;
        if (!parse_css_number_prefix(value, s1, &consumed)) return false;
        outSx = s1;
        outSy = s1;

        u32 pos = consumed;
        while (pos < value.length && (is_ascii_space(value.data[pos]) || value.data[pos] == ',')) ++pos;
        if (pos < value.length) {
            double s2 = 1.0;
            if (parse_css_number_prefix({value.data + pos, value.length - pos}, s2)) {
                outSy = s2;
            }
        }
        return true;
    }

    inline bool parse_css_transform_scale(StringSpan transform, double& outSx, double& outSy) noexcept
    {
        bool found = false;
        for (u32 i = 0; i + 5 <= transform.length; ++i) {
            if (i > 0) {
                const char prev = ascii_lower(transform.data[i - 1]);
                if ((prev >= 'a' && prev <= 'z') || prev == '-') continue;
            }
            if (i + 6 <= transform.length && cstr_equal_icase_n(transform.data + i, "scaleX", 6)) {
                u32 pos = i + 6;
                while (pos < transform.length && is_ascii_space(transform.data[pos])) ++pos;
                if (pos < transform.length && transform.data[pos] == '(') {
                    ++pos;
                    u32 close = pos;
                    while (close < transform.length && transform.data[close] != ')') ++close;
                    if (close < transform.length) {
                        double val = 1.0;
                        if (parse_css_number_prefix({transform.data + pos, close - pos}, val)) {
                            outSx = val;
                            found = true;
                        }
                    }
                }
            } else if (i + 6 <= transform.length && cstr_equal_icase_n(transform.data + i, "scaleY", 6)) {
                u32 pos = i + 6;
                while (pos < transform.length && is_ascii_space(transform.data[pos])) ++pos;
                if (pos < transform.length && transform.data[pos] == '(') {
                    ++pos;
                    u32 close = pos;
                    while (close < transform.length && transform.data[close] != ')') ++close;
                    if (close < transform.length) {
                        double val = 1.0;
                        if (parse_css_number_prefix({transform.data + pos, close - pos}, val)) {
                            outSy = val;
                            found = true;
                        }
                    }
                }
            } else if (cstr_equal_icase_n(transform.data + i, "scale", 5)) {
                const char nextC = (i + 5 < transform.length) ? ascii_lower(transform.data[i + 5]) : '\0';
                if (nextC != 'x' && nextC != 'y' && nextC != 'z' && nextC != '3') {
                    u32 pos = i + 5;
                    while (pos < transform.length && is_ascii_space(transform.data[pos])) ++pos;
                    if (pos < transform.length && transform.data[pos] == '(') {
                        ++pos;
                        u32 close = pos;
                        while (close < transform.length && transform.data[close] != ')') ++close;
                        if (close < transform.length) {
                            StringSpan args = trim_span({transform.data + pos, close - pos});
                            double s1 = 1.0;
                            u32 c1 = 0;
                            if (parse_css_number_prefix(args, s1, &c1)) {
                                outSx = s1;
                                outSy = s1;
                                found = true;
                                u32 p2 = c1;
                                while (p2 < args.length && (is_ascii_space(args.data[p2]) || args.data[p2] == ',')) ++p2;
                                if (p2 < args.length) {
                                    double s2 = 1.0;
                                    if (parse_css_number_prefix({args.data + p2, args.length - p2}, s2)) {
                                        outSy = s2;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        return found;
    }

    inline bool parse_anchor_token(StringSpan arg, double& outAnchor) noexcept
    {
        arg = trim_span(arg);
        if (arg.empty()) return false;
        if (arg.data[arg.length - 1] == '%') {
            double pct = 0.0;
            if (parse_css_number_prefix({arg.data, arg.length - 1}, pct)) {
                outAnchor = -pct / 100.0;
                return true;
            }
        } else {
            double num = 0.0;
            if (parse_css_number_prefix(arg, num) && num == 0.0) {
                outAnchor = 0.0;
                return true;
            }
        }
        return false;
    }

    inline bool parse_css_translate_property(StringSpan value, double& anchorX, double& anchorY) noexcept
    {
        value = trim_span(value);
        if (value.empty()) return false;
        u32 firstEnd = 0;
        while (firstEnd < value.length && !is_ascii_space(value.data[firstEnd]) &&
               value.data[firstEnd] != ',') ++firstEnd;
        StringSpan tok1 = trim_span({value.data, firstEnd});
        bool found = false;
        if (parse_anchor_token(tok1, anchorX)) found = true;
        u32 secondStart = firstEnd;
        while (secondStart < value.length &&
               (is_ascii_space(value.data[secondStart]) || value.data[secondStart] == ','))
            ++secondStart;
        u32 secondEnd = secondStart;
        while (secondEnd < value.length && !is_ascii_space(value.data[secondEnd]) &&
               value.data[secondEnd] != ',') ++secondEnd;
        if (secondEnd > secondStart) {
            StringSpan tok2 = trim_span({value.data + secondStart, secondEnd - secondStart});
            if (parse_anchor_token(tok2, anchorY)) found = true;
        }
        return found;
    }

    inline bool parse_css_transform_translate(StringSpan transform, double& outAnchorX, double& outAnchorY) noexcept
    {
        bool found = false;
        for (u32 i = 0; i + 9 <= transform.length; ++i) {
            if (i > 0) {
                const char prev = ascii_lower(transform.data[i - 1]);
                if ((prev >= 'a' && prev <= 'z') || prev == '-') continue;
            }
            if (i + 10 <= transform.length && cstr_equal_icase_n(transform.data + i, "translateX", 10)) {
                u32 pos = i + 10;
                while (pos < transform.length && is_ascii_space(transform.data[pos])) ++pos;
                if (pos < transform.length && transform.data[pos] == '(') {
                    ++pos;
                    u32 close = pos;
                    while (close < transform.length && transform.data[close] != ')') ++close;
                    if (close < transform.length) {
                        StringSpan arg = trim_span({transform.data + pos, close - pos});
                        if (parse_anchor_token(arg, outAnchorX)) found = true;
                    }
                }
            } else if (i + 10 <= transform.length && cstr_equal_icase_n(transform.data + i, "translateY", 10)) {
                u32 pos = i + 10;
                while (pos < transform.length && is_ascii_space(transform.data[pos])) ++pos;
                if (pos < transform.length && transform.data[pos] == '(') {
                    ++pos;
                    u32 close = pos;
                    while (close < transform.length && transform.data[close] != ')') ++close;
                    if (close < transform.length) {
                        StringSpan arg = trim_span({transform.data + pos, close - pos});
                        if (parse_anchor_token(arg, outAnchorY)) found = true;
                    }
                }
            } else if (cstr_equal_icase_n(transform.data + i, "translate", 9)) {
                const char nextC = (i + 9 < transform.length) ? ascii_lower(transform.data[i + 9]) : '\0';
                if (nextC != 'x' && nextC != 'y' && nextC != 'z' && nextC != '3') {
                    u32 pos = i + 9;
                    while (pos < transform.length && is_ascii_space(transform.data[pos])) ++pos;
                    if (pos < transform.length && transform.data[pos] == '(') {
                        ++pos;
                        u32 close = pos;
                        while (close < transform.length && transform.data[close] != ')') ++close;
                        if (close < transform.length) {
                            StringSpan args = trim_span({transform.data + pos, close - pos});
                            u32 firstEnd = 0;
                            while (firstEnd < args.length && !is_ascii_space(args.data[firstEnd]) && args.data[firstEnd] != ',') ++firstEnd;
                            StringSpan xArg = trim_span({args.data, firstEnd});
                            if (parse_anchor_token(xArg, outAnchorX)) found = true;
                            u32 secondStart = firstEnd;
                            while (secondStart < args.length && (is_ascii_space(args.data[secondStart]) || args.data[secondStart] == ',')) ++secondStart;
                            if (secondStart < args.length) {
                                StringSpan yArg = trim_span({args.data + secondStart, args.length - secondStart});
                                if (parse_anchor_token(yArg, outAnchorY)) found = true;
                            }
                        }
                    }
                }
            }
        }
        return found;
    }

    // ------------------------------------------------------------------------
    // 4. Unified RGBA Unpacking Helpers (0xAABBGGRR -> Normalized Channels)
    // ------------------------------------------------------------------------
    constexpr void unpack_rgba_f32(u32 rgba, float& r, float& g, float& b, float& a) noexcept
    {
        r = static_cast<float>(rgba & 0xFFu) * INV_255_F32;
        g = static_cast<float>((rgba >> 8) & 0xFFu) * INV_255_F32;
        b = static_cast<float>((rgba >> 16) & 0xFFu) * INV_255_F32;
        a = static_cast<float>((rgba >> 24) & 0xFFu) * INV_255_F32;
    }

    constexpr void unpack_rgba_f64(u32 rgba, double& r, double& g, double& b, double& a) noexcept
    {
        r = static_cast<double>(rgba & 0xFFu) * INV_255_F64;
        g = static_cast<double>((rgba >> 8) & 0xFFu) * INV_255_F64;
        b = static_cast<double>((rgba >> 16) & 0xFFu) * INV_255_F64;
        a = static_cast<double>((rgba >> 24) & 0xFFu) * INV_255_F64;
    }

    // ------------------------------------------------------------------------
    // 5. Unified UTF-8 & Vietnamese/Latin Extended Unicode Case Folding
    // ------------------------------------------------------------------------
    constexpr void utf8_lead_byte_info(u8 b0, u32& outByteCount, u32& outUtf16Units) noexcept
    {
        if ((b0 & 0xF8u) == 0xF0u) {
            outByteCount = 4u;
            outUtf16Units = 2u;
        } else if ((b0 & 0xF0u) == 0xE0u) {
            outByteCount = 3u;
            outUtf16Units = 1u;
        } else if ((b0 & 0xE0u) == 0xC0u) {
            outByteCount = 2u;
            outUtf16Units = 1u;
        } else {
            outByteCount = 1u;
            outUtf16Units = 1u;
        }
    }

    inline u32 utf8_decode_next(const char*& p, const char* end) noexcept
    {
        const u8 b0 = static_cast<u8>(*p++);
        if (b0 < 0x80u) return b0;
        if ((b0 & 0xE0u) == 0xC0u && p < end) {
            const u32 b1 = static_cast<u8>(*p++) & 0x3Fu;
            return ((b0 & 0x1Fu) << 6) | b1;
        }
        if ((b0 & 0xF0u) == 0xE0u && p + 1 < end) {
            const u32 b1 = static_cast<u8>(*p++) & 0x3Fu;
            const u32 b2 = static_cast<u8>(*p++) & 0x3Fu;
            return ((b0 & 0x0Fu) << 12) | (b1 << 6) | b2;
        }
        if ((b0 & 0xF8u) == 0xF0u && p + 2 < end) {
            const u32 b1 = static_cast<u8>(*p++) & 0x3Fu;
            const u32 b2 = static_cast<u8>(*p++) & 0x3Fu;
            const u32 b3 = static_cast<u8>(*p++) & 0x3Fu;
            return ((b0 & 0x07u) << 18) | (b1 << 12) | (b2 << 6) | b3;
        }
        return 0xFFFDu;
    }

    inline u32 utf8_encode_cp(u32 cp, char* out) noexcept
    {
        if (cp < 0x80u) {
            out[0] = static_cast<char>(cp);
            return 1u;
        }
        if (cp < 0x800u) {
            out[0] = static_cast<char>(0xC0u | (cp >> 6));
            out[1] = static_cast<char>(0x80u | (cp & 0x3Fu));
            return 2u;
        }
        if (cp < 0x10000u) {
            out[0] = static_cast<char>(0xE0u | (cp >> 12));
            out[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            out[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
            return 3u;
        }
        out[0] = static_cast<char>(0xF0u | (cp >> 18));
        out[1] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = static_cast<char>(0x80u | (cp & 0x3Fu));
        return 4u;
    }

    template <TextTransformMode Mode>
    [[nodiscard]] constexpr u32 transform_unicode_case(u32 cp) noexcept
    {
        if constexpr (Mode == TextTransformMode::Uppercase) {
            if (cp >= 'a' && cp <= 'z') return cp - 32u;
            if (cp >= 0x00E0u && cp <= 0x00FEu && cp != 0x00F7u) return cp - 32u;
            if (cp == 0x00FFu) return 0x0178u;
            if (cp >= 0x0100u && cp <= 0x0177u) {
                if (cp == 0x0131u) return 'I';
                return (cp & 1u) ? (cp - 1u) : cp;
            }
            if (cp == 0x01A1u || cp == 0x01B0u) return cp - 1u;
            if (cp >= 0x1E00u && cp <= 0x1EFFu) return (cp & 1u) ? (cp - 1u) : cp;
            return cp;
        } else if constexpr (Mode == TextTransformMode::Lowercase) {
            if (cp >= 'A' && cp <= 'Z') return cp + 32u;
            if (cp >= 0x00C0u && cp <= 0x00DEu && cp != 0x00D7u) return cp + 32u;
            if (cp == 0x0178u) return 0x00FFu;
            if (cp >= 0x0100u && cp <= 0x0177u) {
                if (cp == 0x0130u) return 'i';
                return ((cp & 1u) == 0u) ? (cp + 1u) : cp;
            }
            if (cp == 0x01A0u || cp == 0x01AFu) return cp + 1u;
            if (cp >= 0x1E00u && cp <= 0x1EFFu) return ((cp & 1u) == 0u) ? (cp + 1u) : cp;
            return cp;
        } else {
            return cp;
        }
    }
} // namespace stockcut
