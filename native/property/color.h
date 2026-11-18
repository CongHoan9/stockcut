// ============================================================================
// CoreEngine - Normalized SIMD Color System (128-bit IEEE 754 Float4)
// ----------------------------------------------------------------------------
// [STOCKFISH DATA-STRUCTURE-CENTRIC & SIMD OPTIMIZATION]:
// - Căn chỉnh 16 bytes = đúng 1 thanh ghi SIMD 128-bit (x86 SSE/AVX / ARM NEON / WASM SIMD128).
// - Triệt tiêu 100% chi phí unpack_rgba_f64 và phép chia 255.0 trong Hot Path 60 FPS.
// - Khớp 100% với kiểu vec4 của GPU WebGL Shader và VBO Vertex Attribute.
// ============================================================================
#pragma once

#include "types.h"
#include "string_parse.h"

namespace stockcut {

struct alignas(16) ColorRGBA {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;

    constexpr ColorRGBA() noexcept = default;

    constexpr ColorRGBA(float r_, float g_, float b_, float a_ = 1.0f) noexcept
        : r(r_), g(g_), b(b_), a(a_) {}

    // ------------------------------------------------------------------------
    // Chuyển đổi siêu tốc từ Packed Integer 0xAABBGGRR (Cold Path)
    // ------------------------------------------------------------------------
    static constexpr ColorRGBA from_packed(u32 rgba) noexcept {
        constexpr float INV_255 = 1.0f / 255.0f;
        return {
            static_cast<float>(rgba & 0xFFu) * INV_255,
            static_cast<float>((rgba >> 8) & 0xFFu) * INV_255,
            static_cast<float>((rgba >> 16) & 0xFFu) * INV_255,
            static_cast<float>((rgba >> 24) & 0xFFu) * INV_255
        };
    }

    // ------------------------------------------------------------------------
    // Đóng gói ngược sang 0xAABBGGRR (Dùng cho Zobrist Hash hoặc Cold Cache)
    // ------------------------------------------------------------------------
    [[nodiscard]] constexpr u32 to_packed() const noexcept {
        const u32 ir = static_cast<u32>(clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
        const u32 ig = static_cast<u32>(clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
        const u32 ib = static_cast<u32>(clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
        const u32 ia = static_cast<u32>(clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        return ir | (ig << 8) | (ib << 16) | (ia << 24);
    }

    // ------------------------------------------------------------------------
    // Các màu mặc định thông dụng
    // ------------------------------------------------------------------------
    static constexpr ColorRGBA opaque_white() noexcept { return {1.0f, 1.0f, 1.0f, 1.0f}; }
    static constexpr ColorRGBA opaque_black() noexcept { return {0.0f, 0.0f, 0.0f, 1.0f}; }
    static constexpr ColorRGBA transparent()  noexcept { return {0.0f, 0.0f, 0.0f, 0.0f}; }

    // ------------------------------------------------------------------------
    // Nhân Alpha & Opacity trong 1 thao tác SIMD (Hot Path)
    // ------------------------------------------------------------------------
    [[nodiscard]] constexpr ColorRGBA premultiply_alpha(float opacity) const noexcept {
        const float effA = a * opacity;
        return { r * effA, g * effA, b * effA, effA };
    }

    [[nodiscard]] constexpr ColorRGBA modulate(const ColorRGBA& other) const noexcept {
        return { r * other.r, g * other.g, b * other.b, a * other.a };
    }

    [[nodiscard]] constexpr ColorRGBA lerp(const ColorRGBA& o, float t) const noexcept {
        return {
            r + (o.r - r) * t,
            g + (o.g - g) * t,
            b + (o.b - b) * t,
            a + (o.a - a) * t
        };
    }

    // ------------------------------------------------------------------------
    // Parse CSS Hex Color (#RGB, #RGBA, #RRGGBB, #RRGGBBAA)
    // ------------------------------------------------------------------------
    static bool parse_hex(StringSpan span, ColorRGBA& outColor) noexcept {
        span = trim_span(span);
        if (span.empty() || span.data[0] != '#') return false;
        const char* hex = span.data + 1;
        const u32 len = span.length - 1;

        const auto hexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };

        if (len == 3 || len == 4) {
            const int r = hexVal(hex[0]);
            const int g = hexVal(hex[1]);
            const int b = hexVal(hex[2]);
            const int a = (len == 4) ? hexVal(hex[3]) : 15;
            if (r < 0 || g < 0 || b < 0 || a < 0) return false;
            constexpr float INV_15 = 1.0f / 15.0f;
            outColor = { r * INV_15, g * INV_15, b * INV_15, a * INV_15 };
            return true;
        }
        if (len == 6 || len == 8) {
            const int r1 = hexVal(hex[0]), r2 = hexVal(hex[1]);
            const int g1 = hexVal(hex[2]), g2 = hexVal(hex[3]);
            const int b1 = hexVal(hex[4]), b2 = hexVal(hex[5]);
            const int a1 = (len == 8) ? hexVal(hex[6]) : 15;
            const int a2 = (len == 8) ? hexVal(hex[7]) : 15;
            if (r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 || b1 < 0 || b2 < 0 || a1 < 0 || a2 < 0) return false;
            constexpr float INV_255 = 1.0f / 255.0f;
            outColor = {
                ((r1 << 4) | r2) * INV_255,
                ((g1 << 4) | g2) * INV_255,
                ((b1 << 4) | b2) * INV_255,
                ((a1 << 4) | a2) * INV_255
            };
            return true;
        }
        return false;
    }

    constexpr bool operator==(const ColorRGBA& o) const noexcept {
        constexpr float EPSILON = 1e-4f;
        return abs_val(r - o.r) < EPSILON &&
               abs_val(g - o.g) < EPSILON &&
               abs_val(b - o.b) < EPSILON &&
               abs_val(a - o.a) < EPSILON;
    }

    constexpr bool operator!=(const ColorRGBA& o) const noexcept {
        return !(*this == o);
    }
};

static_assert(sizeof(ColorRGBA) == 16, "ColorRGBA must be exactly 16 bytes (128-bit SIMD register)");
static_assert(alignof(ColorRGBA) == 16, "ColorRGBA must be 16-byte aligned");

} // namespace stockcut
