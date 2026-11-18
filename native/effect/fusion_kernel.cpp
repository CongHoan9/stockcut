// ============================================================================
// native/effect/fusion_kernel.cpp
// ANONYMOUS 3-TIER EFFECT FUSION KERNEL IMPLEMENTATION
// ----------------------------------------------------------------------------
// Replaces legacy dispatch.cpp + 10 standalone per-effect GLSL program files.
// 100% visual fidelity preserved (GEMINI.md Rule 8) — every shader formula is
// expressed as a pure ABI_UV / ABI_SAMPLE / ABI_COLOR GLSL ES 3.00 function
// reading from flat std140 UBO `u_tape[]` + shared 256x256 `u_noiseAtlas`.
// ============================================================================
#include "fusion_kernel.h"
#include "gl_shader_util.h"
#include "raindrop_suite_fx.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace stockcut {
namespace effect {

using namespace stockcut::effect_algebra;

namespace {

// ============================================================================
// 1. ZERO-ALLOCATION STRING APPEND HELPER FOR GLSL MEGA-SHADER SYNTHESIS
// ============================================================================
struct GlslWriter {
    char* buf;
    u32   cap;
    u32   len;

    inline void append(const char* s) noexcept {
        if (!s || len + 1u >= cap) return;
        while (*s && len + 1u < cap) {
            buf[len++] = *s++;
        }
        buf[len] = '\0';
    }

    inline void append_u32(u32 v) noexcept {
        char tmp[16];
        u32 n = 0u;
        if (v == 0u) {
            tmp[n++] = '0';
        } else {
            while (v > 0u && n < 15u) {
                tmp[n++] = static_cast<char>('0' + (v % 10u));
                v /= 10u;
            }
        }
        while (n > 0u && len + 1u < cap) {
            buf[len++] = tmp[--n];
        }
        if (len < cap) buf[len] = '\0';
    }
};

// Order-sensitive 64-bit SplitMix64 Zobrist mixer
[[nodiscard]] [[gnu::always_inline]] inline u64 splitmix64(u64 x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

[[nodiscard]] [[gnu::always_inline]] inline u64 rotl64(u64 x, u32 r) noexcept {
    return (x << r) | (x >> (64u - r));
}

// ============================================================================
// 2. PURE GLSL ES 3.00 HELPER & KERNEL SNIPPETS (ZERO UNIFORM BLOAT)
// All parameters read from `layout(std140) uniform EffectTape { vec4 u_tape[256]; };`
// ============================================================================
constexpr const char* kGlslHeader = R"(#version 300 es
precision highp float;
in vec2 v_uv;
uniform sampler2D u_src;
uniform vec2 u_res;
uniform sampler2D u_noiseAtlas;
layout(std140) uniform EffectTape {
    vec4 u_tape[256];
};
out vec4 outColor;

vec3 fx_srgb_to_lin(vec3 c) {
    vec3 col = max(c, vec3(0.0));
    return mix(col / 12.92, pow((col + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), col));
}
vec3 fx_lin_to_srgb(vec3 c) {
    vec3 col = max(c, vec3(0.0));
    return mix(col * 12.92, 1.055 * pow(col, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), col));
}
float fx_hash1(float n) { return fract(sin(n) * 43758.5453); }
float fx_hash2(vec2 p)  { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float fx_val_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(fx_hash2(i), fx_hash2(i + vec2(1.0, 0.0)), f.x),
               mix(fx_hash2(i + vec2(0.0, 1.0)), fx_hash2(i + vec2(1.0, 1.0)), f.x), f.y);
}
)";

// --- ABI_UV KERNELS (vec2 fx_uv_K(vec2 uv, int o)) ---

// 1: Fused Affine2D (Transform2D: u_tape[o]=(m00,m01,m10,m11), u_tape[o+1]=(tx,ty,tiled,mirror))
constexpr const char* kSnip_Affine2D = R"(
vec2 fx_uv_1(vec2 uv, int o) {
    vec4 m = u_tape[o];
    vec4 t = u_tape[o + 1];
    vec2 p = uv - 0.5;
    vec2 outUv = vec2(m.x * p.x + m.y * p.y + t.x, m.z * p.x + m.w * p.y + t.y) + 0.5;
    if (t.z > 0.5) {
        vec2 tile = floor(outUv);
        vec2 localUv = outUv - tile;
        if (t.w > 0.5) {
            if (mod(abs(tile.x), 2.0) >= 1.0) localUv.x = 1.0 - localUv.x;
            if (mod(abs(tile.y), 2.0) >= 1.0) localUv.y = 1.0 - localUv.y;
        }
        return localUv;
    }
    return clamp(outUv, 0.0, 1.0);
}
)";

// 3: Twirl (u_tape[o] = (angle, radius, env, 0))
constexpr const char* kSnip_Twirl = R"(
vec2 fx_uv_3(vec2 uv, int o) {
    vec4 p = u_tape[o];
    vec2 c = uv - 0.5;
    float aspect = u_res.x / u_res.y;
    c.x *= aspect;
    float dist = length(c);
    float rad = max(0.001, p.y);
    if (dist < rad) {
        float pct = (rad - dist) / rad;
        float theta = pct * pct * p.x * p.z;
        float s = sin(theta), co = cos(theta);
        c = vec2(dot(c, vec2(co, -s)), dot(c, vec2(s, co)));
    }
    c.x /= aspect;
    return c + 0.5;
}
)";

// 4: Pinch / Bulge (u_tape[o] = (amount, radius, env, 0))
constexpr const char* kSnip_Pinch = R"(
vec2 fx_uv_4(vec2 uv, int o) {
    vec4 p = u_tape[o];
    vec2 c = uv - 0.5;
    float aspect = u_res.x / u_res.y;
    c.x *= aspect;
    float dist = length(c);
    float rad = max(0.001, p.y);
    if (dist < rad) {
        float pct = dist / rad;
        float k = p.x * p.z * 0.75;
        if (p.x >= 0.0) c *= mix(1.0, pow(max(pct, 1e-5), k), 1.0 - pct);
        else            c *= mix(1.0, pow(max(pct, 1e-5), 1.0 / (1.0 - k)), 1.0 - pct);
    }
    c.x /= aspect;
    return c + 0.5;
}
)";

// 5: Lens / Fisheye (u_tape[o] = (zoom, radius, env, 0))
constexpr const char* kSnip_Lens = R"(
vec2 fx_uv_5(vec2 uv, int o) {
    vec4 p = u_tape[o];
    vec2 c = uv - 0.5;
    float aspect = u_res.x / u_res.y;
    c.x *= aspect;
    float dist = length(c);
    float rad = max(0.001, p.y);
    if (dist < rad) {
        float nd = dist / rad;
        float z = sqrt(max(0.0, 1.0 - nd * nd));
        float a = (nd > 0.0) ? atan(nd, z) : 0.0;
        c = normalize(c + 1e-5) * (tan(a * (p.x * p.z)) * rad);
    }
    c.x /= aspect;
    return c + 0.5;
}
)";

// 6: Glitch Scanline + Deterministic Block (u_tape[o]=strip, [o+1]=dstRect, [o+2]=srcRect, [o+3].x=hasBlock)
constexpr const char* kSnip_Glitch = R"(
vec2 fx_uv_6(vec2 uv, int o) {
    vec4 st = u_tape[o];
    vec4 dr = u_tape[o + 1];
    vec4 sr = u_tape[o + 2];
    float hb = u_tape[o + 3].x;
    if (abs(uv.y - st.x) < st.y * 0.5) {
        uv.x = fract(uv.x + st.z * st.w);
    }
    if (hb > 0.5) {
        if (uv.x >= dr.x && uv.x < (dr.x + dr.z) && uv.y >= dr.y && uv.y < (dr.y + dr.w)) {
            uv = sr.xy + (uv - dr.xy);
        }
    }
    return uv;
}
)";

// --- ABI_SAMPLE KERNELS (vec4 fx_smp_K(sampler2D s, vec2 uv, int o)) ---

// 7: Turbulent Distortion (u_tape[o]=(time,speed,strength,freq), [o+1]=(octaves,env,0,0))
constexpr const char* kSnip_Turbulent = R"(
vec3 fx_m289_3(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec2 fx_m289_2(vec2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec3 fx_perm3(vec3 x)  { return fx_m289_3(((x * 34.0) + 1.0) * x); }
float fx_snoise(vec2 v) {
    const vec4 C = vec4(0.211324865405187, 0.366025403784439, -0.577350269189626, 0.024390243902439);
    vec2 i = floor(v + dot(v, C.yy));
    vec2 x0 = v - i + dot(i, C.xx);
    float sw = step(x0.y, x0.x);
    vec2 i1 = vec2(sw, 1.0 - sw);
    vec4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;
    i = fx_m289_2(i);
    vec3 p = fx_perm3(fx_perm3(i.y + vec3(0.0, i1.y, 1.0)) + i.x + vec3(0.0, i1.x, 1.0));
    vec3 m = max(0.5 - vec3(dot(x0, x0), dot(x12.xy, x12.xy), dot(x12.zw, x12.zw)), 0.0);
    m = m * m; m = m * m;
    vec3 x = 2.0 * fract(p * C.www) - 1.0;
    vec3 h = abs(x) - 0.5;
    vec3 ox = floor(x + 0.5);
    vec3 a0 = x - ox;
    m *= 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);
    vec3 g;
    g.x = a0.x * x0.x + h.x * x0.y;
    g.yz = a0.yz * x12.xz + h.yz * x12.yw;
    return 130.0 * dot(m, g);
}
vec4 fx_smp_7(sampler2D s, vec2 uv, int o) {
    vec4 p0 = u_tape[o];
    vec4 p1 = u_tape[o + 1];
    float aspect = u_res.x / u_res.y;
    vec2 st = vec2(uv.x * aspect, uv.y) * p0.w * 3.5;
    float t = p0.x * p0.y * 0.75;
    vec2 off = vec2(0.0);
    vec2 sh = vec2(t * 0.4, t * 0.25);
    float oct = p1.x;
    float w1 = step(1.0, oct), w2 = step(2.0, oct), w3 = step(3.0, oct), w4 = step(4.0, oct);
    off.x += 0.500 * w1 * fx_snoise(st + sh);
    off.y += 0.500 * w1 * fx_snoise(st.yx + sh * 1.3 + vec2(4.3, 1.7));
    st *= 2.05; sh *= 1.5;
    off.x += 0.250 * w2 * fx_snoise(st + sh);
    off.y += 0.250 * w2 * fx_snoise(st.yx + sh * 1.3 + vec2(4.3, 1.7));
    st *= 2.05; sh *= 1.5;
    off.x += 0.125 * w3 * fx_snoise(st + sh);
    off.y += 0.125 * w3 * fx_snoise(st.yx + sh * 1.3 + vec2(4.3, 1.7));
    st *= 2.05; sh *= 1.5;
    off.x += 0.0625 * w4 * fx_snoise(st + sh);
    off.y += 0.0625 * w4 * fx_snoise(st.yx + sh * 1.3 + vec2(4.3, 1.7));
    vec2 tuv = uv + off * (p0.z * 0.045 * p1.y);
    vec2 d1 = vec2(0.38, 0.14) / u_res;
    vec2 d2 = vec2(-0.14, 0.38) / u_res;
    return (texture(s, clamp(tuv - d1, 0.001, 0.999)) + texture(s, clamp(tuv + d1, 0.001, 0.999)) +
            texture(s, clamp(tuv - d2, 0.001, 0.999)) + texture(s, clamp(tuv + d2, 0.001, 0.999))) * 0.25;
}
)";

// 8: Chromatic Aberration (u_tape[o]=(dist, angle, radial, env))
constexpr const char* kSnip_Chromatic = R"(
vec4 fx_smp_8(sampler2D s, vec2 uv, int o) {
    vec4 p = u_tape[o];
    float rad = radians(p.y);
    vec2 offset;
    if (p.z > 0.5) {
        vec2 q = uv - 0.5;
        float aspect = u_res.x / u_res.y;
        q.x *= aspect;
        offset = normalize(q + 1e-5) * length(q) * (p.x * 0.018 * p.w);
        offset.x /= aspect;
    } else {
        offset = vec2(cos(rad), sin(rad)) * (p.x * 0.015 * p.w);
    }
    vec3 colSum = vec3(0.0), wSum = vec3(0.0);
    float aSum = 0.0;
    for (int i = 0; i < 8; ++i) {
        float t = (float(i) / 7.0) * 2.0 - 1.0;
        vec4 sc = texture(s, clamp(uv + offset * t, 0.001, 0.999));
        float wR = clamp(1.0 - abs(t - 0.75) * 1.5, 0.0, 1.0);
        float wG = clamp(1.0 - abs(t) * 1.8, 0.0, 1.0);
        float wB = clamp(1.0 - abs(t + 0.75) * 1.5, 0.0, 1.0);
        vec3 w = vec3(wR*wR*(3.0-2.0*wR), wG*wG*(3.0-2.0*wG), wB*wB*(3.0-2.0*wB));
        colSum += sc.rgb * w; wSum += w; aSum += sc.a;
    }
    return vec4(colSum / max(vec3(0.0001), wSum), aSum / 8.0);
}
)";

// 9: Zoom Blur (u_tape[o]=(strength, env, 0, 0))
constexpr const char* kSnip_ZoomBlur = R"(
vec4 fx_smp_9(sampler2D s, vec2 uv, int o) {
    vec4 p = u_tape[o];
    vec2 toC = (0.5 - uv) * (p.x * 0.04 * p.y);
    vec4 acc = vec4(0.0);
    for (int i = 0; i < 14; ++i) {
        float pct = (float(i) + 0.5) / 14.0;
        acc += texture(s, clamp(uv + toC * pct, 0.001, 0.999));
    }
    return acc / 14.0;
}
)";

// 10: Motion Blur (u_tape[o]=(dist, angle, env, 0))
constexpr const char* kSnip_MotionBlur = R"(
vec4 fx_smp_10(sampler2D s, vec2 uv, int o) {
    vec4 p = u_tape[o];
    float rad = radians(p.y);
    vec2 dir = vec2(cos(rad), sin(rad)) * (p.x * 0.03 * p.z);
    vec4 acc = vec4(0.0);
    for (int i = 0; i < 14; ++i) {
        float pct = (float(i) / 13.0) - 0.5;
        acc += texture(s, clamp(uv + dir * pct, 0.001, 0.999));
    }
    return acc / 14.0;
}
)";

// 11: Glitch RGB (u_tape[o]=(time, amount, speed, dispersion), [o+1]=(env,0,0,0))
constexpr const char* kSnip_GlitchRgb = R"(
float fx_grgb_trunc(float x, float n) { return floor(x * n) / n; }
vec2  fx_grgb_trunc2(vec2 x, float n) { return floor(x * n) / n; }
vec3  fx_grgb_spec(float t) {
    float lo = step(t, 0.5), hi = 1.0 - lo;
    float r = clamp((t - 1.0/6.0) / (4.0/6.0), 0.0, 1.0);
    float w = clamp(1.0 - abs(2.0 * r - 1.0), 0.0, 1.0);
    vec3 ret = vec3(lo, 1.0, hi) * vec3(1.0 - w, w, 1.0 - w);
    return pow(ret, vec3(1.0 / 2.2));
}
vec4 fx_smp_11(sampler2D s, vec2 uv, int o) {
    vec4 p = u_tape[o];
    float env = u_tape[o + 1].x;
    vec4 orig = texture(s, uv);
    float GL = p.y * env;
    if (GL <= 0.001) return orig;
    float tm = mod(p.x * p.z * 100.0, 32.0) / 110.0;
    float gnm = clamp(GL, 0.0, 1.0);
    float rnd0 = fract(sin(dot(fx_grgb_trunc2(vec2(tm, tm), 6.0), vec2(12.9898, 78.233))) * 43758.5453);
    float r0 = clamp((1.0 - gnm) * 0.7 + rnd0, 0.0, 1.0);
    float rnd1 = fract(sin(dot(vec2(fx_grgb_trunc(uv.x, 10.0 * r0), tm), vec2(12.9898, 78.233))) * 43758.5453);
    float r1 = 1.0 - max(0.0, min(0.5 - 0.5 * gnm + rnd1, 0.9999999));
    float rnd2 = fract(sin(dot(vec2(fx_grgb_trunc(uv.y, 40.0 * r1), tm), vec2(12.9898, 78.233))) * 43758.5453);
    float rnd3 = fract(sin(dot(vec2(fx_grgb_trunc(uv.y, 10.0 * r0), tm), vec2(12.9898, 78.233))) * 43758.5453);
    float r3 = (1.0 - clamp(rnd3 + 0.8, 0.0, 1.0)) - 0.1;
    float pxrnd = fract(sin(dot(uv + tm, vec2(12.9898, 78.233))) * 43758.5453);
    float ofs = 0.05 * clamp(rnd2, 0.0, 1.0) * GL * (rnd0 > 0.5 ? 1.0 : -1.0) * p.w;
    ofs += 0.5 * pxrnd * ofs;
    uv.y += 0.1 * r3 * GL;
    vec4 sum = vec4(0.0); vec3 wsum = vec3(0.0);
    for (int i = 0; i < 16; ++i) {
        float t = float(i) * 0.0625;
        vec4 sc = texture(s, vec2(clamp(uv.x + ofs * t, 0.0, 1.0), clamp(uv.y, 0.0, 1.0)));
        vec3 sp = fx_grgb_spec(t);
        sum += vec4(sc.rgb * sp, sc.a); wsum += sp;
    }
    return vec4(mix(orig.rgb, sum.rgb / max(vec3(0.0001), wsum), env), orig.a);
}
)";

// 12: CyberScan (u_tape[o]=(time,lines,intensity,speed), [o+1]=(scanStrength,env,0,0))
constexpr const char* kSnip_CyberScan = R"(
vec4 fx_smp_12(sampler2D s, vec2 uv, int o) {
    vec4 p0 = u_tape[o];
    vec4 p1 = u_tape[o + 1];
    float t = p0.x * p0.w;
    float tot = 0.0;
    for (int i = 0; i < 6; ++i) {
        float fi = float(i);
        float aw = step(fi, p0.y - 0.5);
        float sl = fract(t * (1.0 + fi * 0.28) * 0.35 + fi / max(1.0, p0.y));
        float sy = uv.y - 1.0 + sl;
        float sg = step(0.0, sy) * 2.0 - 1.0;
        float dy = max(abs(sy), 0.0012);
        tot += aw * clamp((0.0042 / (dy * sg)) * exp(-dy * 4.2), -0.45, 0.45);
    }
    float fd = tot * p1.x * p1.y;
    vec2 tuv = vec2(uv.x + fd * (u_res.y / u_res.x), uv.y);
    vec2 bd = vec2(fd * (u_res.y / u_res.x) * 0.45, 0.0);
    vec4 col = texture(s, clamp(tuv, 0.001, 0.999)) * 0.36
             + texture(s, clamp(tuv - bd * 0.28, 0.001, 0.999)) * 0.26
             + texture(s, clamp(tuv - bd * 0.58, 0.001, 0.999)) * 0.20
             + texture(s, clamp(tuv - bd * 0.92, 0.001, 0.999)) * 0.18;
    float sf = smoothstep(0.015, 0.16, abs(fd)) * p0.z * p1.y;
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    return vec4(mix(col.rgb, vec3(lum * 0.35 + 0.02), sf * 0.52), col.a);
}
)";

// 13: FXAA 3.11
constexpr const char* kSnip_Fxaa = R"(
vec4 fx_smp_13(sampler2D s, vec2 uv, int o) {
    vec2 invVP = 1.0 / u_res;
    vec3 rgbNW = texture(s, uv + vec2(-1.0, -1.0) * invVP).rgb;
    vec3 rgbNE = texture(s, uv + vec2( 1.0, -1.0) * invVP).rgb;
    vec3 rgbSW = texture(s, uv + vec2(-1.0,  1.0) * invVP).rgb;
    vec3 rgbSE = texture(s, uv + vec2( 1.0,  1.0) * invVP).rgb;
    vec4 rgbaM = texture(s, uv);
    vec3 luma = vec3(0.299, 0.587, 0.114);
    float lNW = dot(rgbNW, luma), lNE = dot(rgbNE, luma), lSW = dot(rgbSW, luma), lSE = dot(rgbSE, luma), lM = dot(rgbaM.rgb, luma);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), ((lNW + lSW) - (lNE + lSE)));
    float dr = max((lNW + lNE + lSW + lSE) * 0.03125, 0.0078125);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + dr), vec2(-8.0), vec2(8.0)) * invVP;
    vec3 rgbA = 0.5 * (texture(s, uv + dir * (1.0/3.0 - 0.5)).rgb + texture(s, uv + dir * (2.0/3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(s, uv - dir * 0.5).rgb + texture(s, uv + dir * 0.5).rgb);
    float lB = dot(rgbB, luma);
    return vec4((lB < lMin || lB > lMax) ? rgbA : rgbB, rgbaM.a);
}
)";

// 14: Film Burn (u_tape[o]=(time,progress,emberGlow,distortion), [o+1]=(env,0,0,0))
constexpr const char* kSnip_FilmBurn = R"(
float fx_fb_fbm(vec2 p) {
    float v = 0.5000 * fx_val_noise(p); p *= 2.02;
    v += 0.2500 * fx_val_noise(p); p *= 2.03;
    v += 0.1250 * fx_val_noise(p);
    return v;
}
vec4 fx_smp_14(sampler2D s, vec2 uv, int o) {
    vec4 p0 = u_tape[o];
    float env = u_tape[o + 1].x;
    float th = (p0.y < 0.0) ? (sin(p0.x * 1.5) * 0.5 + 0.5) : p0.y;
    vec4 orig = texture(s, uv);
    float n = fx_fb_fbm(uv * 8.0 + p0.x * 0.1);
    float lum = dot(orig.rgb, vec3(0.299, 0.587, 0.114));
    float tex = (lum - 0.5) * 0.3 + n * 0.4 + uv.x * 0.8;
    float bp = th * 1.5 - 0.2;
    float mask = smoothstep(bp - 0.1, bp, tex);
    float dist = smoothstep(bp - 0.3, bp + 0.05, tex);
    float cf = pow(smoothstep(bp - 0.2, bp + 0.15, tex), 3.0);
    vec2 d1 = vec2(0.38, 0.14) / u_res, d2 = vec2(-0.14, 0.38) / u_res;
    vec2 muv = uv * (0.7 + dist * dist * 0.3 * p0.w);
    vec3 col = (texture(s, clamp(muv - d1, 0.0, 1.0)).rgb + texture(s, clamp(muv + d1, 0.0, 1.0)).rgb +
                texture(s, clamp(muv - d2, 0.0, 1.0)).rgb + texture(s, clamp(muv + d2, 0.0, 1.0)).rgb) * 0.25;
    vec3 cLin = fx_srgb_to_lin(col);
    vec3 burnLin = mix(cLin * vec3(0.8, 0.4, 0.2), cLin, cf) * mask + vec3(1.0, 0.4, 0.05) * ((1.0 - cf) * (1.0 - mask) * p0.z * 3.0);
    return vec4(mix(orig.rgb, fx_lin_to_srgb(burnLin), env), orig.a);
}
)";

// 15 & 16: Intro / Outro (u_tape[o..o+7] packed IntroOutroGLState)
constexpr const char* kSnip_IntroOutro = R"(
float fx_io_sm(float u) { float c = clamp(u, 0.0, 1.0); return c * c * (3.0 - 2.0 * c); }
float fx_io_tanh(float x) { float e2 = exp(2.0 * clamp(x, -10.0, 10.0)); return (e2 - 1.0) / (e2 + 1.0); }
vec4 fx_smp_io(sampler2D s, vec2 uv, int o) {
    vec4 g0 = u_tape[o];     // (m, flip, tau, uScale)
    vec4 wx = u_tape[o + 1]; // warpX
    vec4 wy = u_tape[o + 2]; // warpY
    vec4 r1 = u_tape[o + 3]; // ring1
    vec4 r2 = u_tape[o + 4]; // ring2
    vec4 b1 = u_tape[o + 5]; // bar1
    vec4 b2 = u_tape[o + 6]; // bar2
    vec4 l1 = u_tape[o + 7]; // line1
    vec4 l2 = u_tape[o + 8]; // line2
    vec2 baseDim = vec2(1920.0, 1080.0);
    vec4 baseCol = vec4(0.0, 0.0, 0.0, 1.0);
    if (g0.x > 0.0001) {
        vec2 suv = uv;
        if (wx.x > 0.001) {
            float u = uv.x * 2.0 - 1.0, v = (1.0 - uv.y) * 2.0 - 1.0;
            float u2 = u * u, v2 = v * v;
            float su = u * (wx.x + wx.y * u2 + wx.z * v2) + wx.w * u2;
            float sv = v * (wy.x + wy.y * v2 + wy.z * u2);
            suv = clamp(vec2(su * 0.5 + 0.5, 1.0 - (sv * 0.5 + 0.5)), 0.0, 1.0);
        }
        baseCol = texture(s, suv);
    }
    float rRef = length(uv * u_res - u_res * 0.5) / max(g0.w, 1e-5);
    float bA = 0.0;
    if (r1.x > 0.0) {
        float r0 = max(0.0, r2.y * r1.x);
        bA = mix(clamp(1.0 - g0.x, 0.0, 1.0), 1.0, clamp((rRef - r0) / max(1.0, r1.x - r0), 0.0, 1.0));
    } else if (g0.x < 1.0) {
        bA = clamp(1.0 - g0.x, 0.0, 1.0);
    }
    vec3 col = baseCol.rgb * (1.0 - bA);
    float outA = baseCol.a + (1.0 - baseCol.a) * bA;
    if (r1.x > 0.0 && r1.w > 0.0) {
        float rMax = min(r1.x * r2.x, length(baseDim) * 0.5);
        if (rRef <= rMax) {
            float vr = (rRef < r1.x)
                ? fx_io_sm((rRef - r2.y * r1.x) / max(1e-4, (1.0 - r2.y) * r1.x))
                : (1.0 / (1.0 + pow(max(1e-6, (rRef / r1.x - 1.0) / max(1e-4, r1.z)), r1.y))) *
                  (1.0 - fx_io_sm((rRef / r1.x - 0.3 * r2.x) / max(1e-4, 0.7 * r2.x)));
            float ra = clamp(r1.w * vr, 0.0, 1.0);
            col = mix(col, vec3(1.0), ra);
            outA = outA + (1.0 - outA) * ra;
        }
    }
    if (b1.y > b1.x || l2.x > 0.0) {
        float yyB = (g0.y > 0.5) ? uv.y : (1.0 - uv.y);
        float X = uv.x * baseDim.x;
        float add = 0.0;
        if (l2.x > 0.0) {
            float dl = abs(yyB - l1.x) * u_res.y / max(g0.w, 1e-5);
            float ln = clamp(l1.y * (1.0 - fx_io_sm((dl - l1.w) / max(1e-4, l2.x - l1.w))) + l1.z * exp(-(dl*dl)/16.0), 0.0, 1.0);
            if (ln > 0.001) add += ln * 0.85;
        }
        if (b1.y > b1.x) {
            float dyB = b1.y - b1.x;
            float fe = (g0.z >= 4.8 ? 20.0 : 24.0) * g0.w / max(u_res.y, 1.0);
            float arch = (g0.z >= 4.47) ? ((uv.x - 0.5) * (uv.x - 0.5) * 0.048) : 0.0;
            float yyE = yyB + arch;
            float sh = fx_io_sm((yyE - b1.x) / max(1e-4, fe) + 0.5) * fx_io_sm((b1.y - yyE) / max(1e-4, fe) + 0.5);
            if (sh > 0.002) {
                float yn = clamp((yyE - b1.x) / max(1e-4, dyB), 0.0, 1.0);
                float ct = fx_io_tanh(2.5 * cos(6.2831853 * X / 10.53)) * fx_io_sm((X - 40.0) / 200.0) * (1.0 - fx_io_sm((X - 1800.0) / 120.0));
                float v = clamp(b1.z + b2.x * (ct * sin(3.14159265 * yn)) + b2.z * (fx_val_noise(vec2(X/6.0, yyB*(baseDim.y/6.0)))*2.0-1.0), 0.0, 1.0);
                add = min(1.0, v * sh + add);
            }
        }
        col = min(vec3(1.0), col + vec3(add));
        outA = min(1.0, outA + add);
    }
    return vec4(col, outA);
}
vec4 fx_smp_15(sampler2D s, vec2 uv, int o) { return fx_smp_io(s, uv, o); }
vec4 fx_smp_16(sampler2D s, vec2 uv, int o) { return fx_smp_io(s, uv, o); }
)";

// --- ABI_COLOR KERNELS (vec4 fx_col_K(vec4 col, vec2 uv, int o)) ---

// 2: Fused AffineColor (ColorTransferOp: u_tape[o]=gain, u_tape[o+1]=emit)
constexpr const char* kSnip_AffineColor = R"(
vec4 fx_col_2(vec4 col, vec2 uv, int o) {
    return col * u_tape[o] + u_tape[o + 1];
}
)";

// 17 & 18: Fire Full & Fire Border (u_tape[o]=(time,intensity,speed,scale), [o+1]=(borderW,mode,env,0))
constexpr const char* kSnip_Fire = R"(
vec3 fx_rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + 1e-10)), d / (q.x + 1e-10), q.x);
}
vec3 fx_hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
float fx_fire_fbm(vec2 n) {
    float tot = 0.0, amp = 1.0;
    for (int i = 0; i < 5; ++i) { tot += fx_val_noise(n) * amp; n += n * 1.7; amp *= 0.47; }
    return tot;
}
float fx_fire_n(vec2 n) { return fx_val_noise(n) + fx_val_noise(n * 2.1) * 0.6 + fx_val_noise(n * 5.4) * 0.42; }
vec3 fx_fire_ramp(float t) {
    return (t <= 0.5) ? vec3(1.0 - t * 1.4, 0.2, 1.05) / max(0.01, t)
                      : vec3(0.3 * (1.0 - t) * 2.0, 0.2, 1.05) / max(0.01, t);
}
vec3 fx_fire_bline(vec3 col, vec2 fc, mat2 mtx, float shift, float t, float ws) {
    vec2 u = (fc / u_res) * mtx;
    u.x += (u.y < 0.5) ? (23.0 + t * 0.35) : (-11.0 + t * 0.3);
    u.y = abs(u.y - shift);
    u *= 5.0 / max(0.2, ws);
    float q = fx_fire_n(u - t * 0.013) * 0.5;
    vec2 r = vec2(fx_fire_n(u + q * 0.5 + t - u.x - u.y), fx_fire_n(u + q - t));
    vec3 c = fx_fire_ramp(pow((r.y + r.y) * max(0.0, u.y) + 0.1, 4.0));
    c /= (1.50 + max(vec3(0.0), c));
    c *= smoothstep(0.000001, 0.005, c.b);
    return mix(col, c, c.b);
}
vec4 fx_col_fire_impl(vec4 baseTex, vec2 uv, int o, bool isBorder) {
    vec4 p0 = u_tape[o];
    vec4 p1 = u_tape[o + 1];
    vec2 fc = uv * u_res;
    float t = p0.x * p0.z;
    vec4 fcol;
    if (isBorder) {
        vec3 c = vec3(0.0);
        float w = p1.x;
        c = fx_fire_bline(c, fc, mat2(1.0,1.0,0.0,1.0),  1.02, t, w);
        c = fx_fire_bline(c, fc, mat2(1.0,1.0,1.0,0.0),  1.02, t, w);
        c = fx_fire_bline(c, fc, mat2(1.0,1.0,0.0,1.0), -0.02, t, w);
        c = fx_fire_bline(c, fc, mat2(1.0,1.0,1.0,0.0), -0.02, t, w);
        fcol = vec4(c, clamp(dot(c, vec3(0.3333)) * 2.5, 0.0, 1.0));
    } else {
        float sh = 1.327 + sin(t * 2.0) / 2.4;
        float dist = (3.5 - sin(t * 0.4) / 1.89) / max(0.1, p0.w);
        vec2 q = fc * dist / u_res.xx; q.x -= t / 1.1;
        float vq = (fx_fire_fbm(q - t*0.01 + sin(t)*0.1) + fx_fire_fbm(q - t*0.002 + cos(t)*0.02)
                  - 0.4*(fx_fire_fbm(q - t*0.44 - 5.0*cos(t)/7.0)-6.0)
                  - 2.0*(fx_fire_fbm(q - t*0.9 - cos(t)/3.0)-4.0)
                  + 0.6*(fx_fire_fbm(q - t*2.0 - sin(t))+2.0)) / 3.8;
        vec2 r = vec2(fx_fire_fbm(q + vq*0.5 + t*1.2 - q.x - q.y), fx_fire_fbm(q + vq - t*0.1));
        vec3 c = mix(vec3(0.5,0.0,0.1), vec3(0.9,0.1,0.0), fx_fire_fbm(q + r))
               + mix(vec3(0.2,0.1,0.7), vec3(1.0,0.9,0.1), r.x) - mix(vec3(0.1), vec3(0.9), r.y);
        vec3 col = c * cos(sh * fc.y / u_res.y) + 0.05; col.r *= 0.8;
        vec3 hsv = fx_rgb2hsv(col);
        hsv.y *= hsv.z * 1.1; hsv.z *= hsv.y * 1.13; hsv.y = (2.2 - hsv.z * 0.9) * 1.20;
        col = fx_hsv2rgb(hsv);
        fcol = vec4(col, clamp(length(col) * 0.75, 0.0, 1.0));
    }
    vec3 bl = baseTex.rgb + (fcol.rgb * p0.y) * fcol.a * p1.z;
    bl = bl / (1.0 + max(vec3(0.0), bl - 1.0) * 0.5);
    return vec4(bl, baseTex.a);
}
vec4 fx_col_17(vec4 col, vec2 uv, int o) { return fx_col_fire_impl(col, uv, o, false); }
vec4 fx_col_18(vec4 col, vec2 uv, int o) { return fx_col_fire_impl(col, uv, o, true); }
)";

// 19: Rain Bokeh (u_tape[o]=(time,intensity,speed,rain), [o+1]=(shake,blend,env,0))
constexpr const char* kSnip_RainBokeh = R"(
float fx_rb_N(float t) { return fract(sin(t * 10234.324) * 123423.23512); }
vec3 fx_rb_N31(float p) {
    vec3 p3 = fract(vec3(p) * vec3(0.1031, 0.11369, 0.13787));
    p3 += dot(p3, p3.yzx + 19.19);
    return fract(vec3((p3.x + p3.y) * p3.z, (p3.x + p3.z) * p3.y, (p3.y + p3.z) * p3.x));
}
float fx_rb_bm(vec3 ro, vec3 rd, vec3 p, float sz, float bl) {
    float d = length(cross(p - ro, rd));
    return smoothstep(sz, sz * (1.0 - bl), d);
}
vec4 fx_col_19(vec4 baseTex, vec2 uv, int o) {
    vec4 p0 = u_tape[o];
    vec4 p1 = u_tape[o + 1];
    vec2 pUV = uv - 0.5; pUV.x *= u_res.x / u_res.y;
    float t = p0.x * p0.z;
    vec3 ro = vec3(0.3, 0.15, 0.0);
    vec3 f = normalize(mix(vec3(0.3, 0.15, 1.0), vec3(0.0, 0.15, 0.7), sin(t * 0.1) * 0.5 + 0.5) - ro);
    vec3 r = cross(vec3(0.0, 1.0, 0.0), f);
    vec3 u = cross(f, r);
    vec3 rd = normalize((ro + f * 2.0 + pUV.x * r + pUV.y * u) - ro);
    float tr = t * 0.03;
    vec3 col = vec3(0.0);
    for (int k = 0; k < 8; ++k) {
        float i = float(k) * 0.125;
        float side = sign(rd.x);
        float z = fract(i - tr + max(side, 0.0) * 0.0625);
        vec3 sp = vec3(2.0 * side, 2.0, z * 60.0);
        col += fx_rb_bm(ro, rd, sp, 0.05 * length(sp - ro), 0.1) * (1.0 - z) * vec3(1.0, 0.7, 0.3);
        float z2 = fract(-tr * 2.0 + i);
        vec3 hp = vec3(-0.3, 0.1, z2 * 40.0);
        float hs = mix(0.03, 0.05, smoothstep(0.02, 0.07, z2)) * length(hp - ro);
        float df = max(0.01, pow(1.0 - z2, 9.0));
        col += vec3(0.8, 0.8, 1.0) * (fx_rb_bm(ro, rd, hp - vec3(0.08,0,0), hs, 0.1) + fx_rb_bm(ro, rd, hp + vec3(0.08,0,0), hs, 0.1)) * df;
    }
    col = (col + clamp(rd.y, 0.0, 1.0) * vec3(0.1, 0.08, 0.15)) * p0.y;
    vec3 bLin = fx_srgb_to_lin(baseTex.rgb);
    vec3 sLin = (p1.y > 0.5) ? col : (1.0 - (1.0 - bLin) * (1.0 - clamp(col, 0.0, 1.0)));
    return vec4(fx_lin_to_srgb(mix(bLin, sLin, p1.z)), baseTex.a);
}
)";

// 20: Matrix Digital Rain (u_tape[o]=(time,speed,density,dim), [o+1]=(env,seed,laserW,0))
constexpr const char* kSnip_Matrix = R"(
float fx_mx_h2(vec2 p, float sd) {
    p = fract(p * vec2(123.34, 456.21) + sd * 0.137);
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
vec4 fx_col_20(vec4 baseCol, vec2 uv, int o) {
    vec4 p0 = u_tape[o];
    vec4 p1 = u_tape[o + 1];
    vec3 uCol = vec3(240.0/255.0, 225.0/255.0, 74.0/255.0);
    vec3 hotW = mix(uCol, vec3(1.0), 0.65);
    if (p0.w > 0.0) baseCol.rgb *= max(0.0, 1.0 - p0.w * p1.x);
    float t = p0.x * p0.y;
    float colCnt = max(20.0, floor(52.0 * max(0.2, p0.z)));
    float cIdx = floor(uv.x * colCnt);
    float cSeed = fx_hash1(cIdx * 19.31 + 2.7 + p1.y * 17.31);
    float cTime = t * (0.45 + cSeed * 0.75) + cSeed * 10.0;
    float yDist = fract(cTime) - (1.0 - uv.y);
    if (yDist < 0.0) yDist += 1.0;
    vec2 cellUv = fract(vec2(uv.x * colCnt, uv.y * 40.0)) - 0.5;
    float gMask = step(0.38, fx_mx_h2(vec2(cIdx, floor(uv.y * 40.0) + floor(cTime * 7.0)), p1.y)) * step(length(cellUv), 0.40);
    vec3 fxC = (uCol * exp(-yDist * 6.5) * 0.85 * gMask + hotW * smoothstep(0.04, 0.0, abs(yDist)) * 1.6) * step(0.18, cSeed);
    float lineX = fx_hash1(floor(t * 1.8) * 23.7 + p1.y * 17.31);
    fxC += hotW * smoothstep(p1.z, 0.0, abs(uv.x - lineX)) * 0.65;
    return vec4(baseCol.rgb + fxC * p1.x, baseCol.a);
}
)";

// O(1) Compile-time table mapping opaque kernelOrdinal -> pure GLSL snippet source
constexpr const char* kKernelSnippets[KERNEL_ORD_COUNT] = {
    nullptr,           // 0: None
    kSnip_Affine2D,    // 1: Affine2D
    kSnip_AffineColor, // 2: AffineColor
    kSnip_Twirl,       // 3: Twirl
    kSnip_Pinch,       // 4: Pinch
    kSnip_Lens,        // 5: Lens
    kSnip_Glitch,      // 6: Glitch
    kSnip_Turbulent,   // 7: Turbulent
    kSnip_Chromatic,   // 8: Chromatic
    kSnip_ZoomBlur,    // 9: ZoomBlur
    kSnip_MotionBlur,  // 10: MotionBlur
    kSnip_GlitchRgb,   // 11: GlitchRgb
    kSnip_CyberScan,   // 12: CyberScan
    kSnip_Fxaa,        // 13: Fxaa
    kSnip_FilmBurn,    // 14: FilmBurn
    kSnip_IntroOutro,  // 15: Intro
    kSnip_IntroOutro,  // 16: Outro
    kSnip_Fire,        // 17: FireFull
    kSnip_Fire,        // 18: FireBorder
    kSnip_RainBokeh,   // 19: RainBokeh
    kSnip_Matrix,      // 20: Matrix
    nullptr,           // 21: RainDrop (ABI_GEOM)
    nullptr,           // 22: Bloom (ABI_GEOM)
    nullptr,           // 23: GaussBlur (ABI_GEOM)
    nullptr            // 24: Glow (ABI_GEOM)
};

// Shared snippet deduplication group id (Intro/Outro and FireFull/FireBorder share one source block)
constexpr u8 kSnippetDedupGroup[KERNEL_ORD_COUNT] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 15, 17, 17, 19, 20, 0, 0, 0, 0
};

// Helper: pack an invocation's parameters into flat paramTape (returns vec4 count written)
inline u16 pack_invocation_to_tape(
    const EffectInvocation& inv,
    u8 kOrd,
    u32 width,
    u32 height,
    float* dstTape,
    u32 maxVec4s
) noexcept {
    if (maxVec4s == 0u) return 0u;
    switch (kOrd) {
        case KERNEL_ORD_TWIRL:
        case KERNEL_ORD_PINCH:
        case KERNEL_ORD_LENS:
            dstTape[0] = inv.p0; dstTape[1] = inv.p2; dstTape[2] = inv.env; dstTape[3] = 0.0f;
            return 1u;

        case KERNEL_ORD_GLITCH: {
            if (maxVec4s < 4u) return 0u;
            const float spd = inv.p1 > 0.0f ? inv.p1 : 2.0f;
            const float effMs = inv.timeSec * 1000.0f * spd;
            const float u = static_cast<float>(width) / DEFAULT_STAGE_WIDTH_F32;
            const float scanY = 1.0f - std::fmod(effMs, 2000.0f) / 2000.0f;
            const float stripH = (25.0f * u) / max(static_cast<float>(height), 1.0f);
            const float shiftX = (250.0f * u) / max(static_cast<float>(width), 1.0f);
            const float step = std::floor(effMs / 200.0f);
            auto prng = [](float s) noexcept -> float {
                const float x = std::sin(s) * 10000.0f;
                return x - std::floor(x);
            };
            const bool rest = (prng(std::floor(effMs / 2000.0f)) < 0.3f) ||
                              (prng(step * 1.5f) > 0.5f) ||
                              (inv.env <= OPACITY_EPSILON);
            float dr[4] = {}, sr[4] = {}, hb = 0.0f;
            const float m = 10.0f * u;
            const float safeW = static_cast<float>(width) - m * 2.0f;
            const float safeH = static_cast<float>(height) - m * 2.0f;
            if (!rest && min(safeW, safeH) > 10.0f) {
                float s = step;
                auto r = [&s, &prng]() noexcept -> float { const float v = prng(s); s += 1.0f; return v; };
                const float primary = 0.2f + r() * 0.05f;
                const float maxSec = 0.05f - (primary - 0.1f) * 0.05f;
                const float sec = 0.05f + r() * (maxSec - 0.05f);
                const bool isH = r() > 0.5f;
                const float bw = min(safeW, max(2.0f, std::round((isH ? primary : sec) * safeW)));
                const float bh = min(safeH, max(2.0f, std::round((isH ? sec : primary) * safeH)));
                const float srcX = std::round(m + r() * (safeW - bw));
                const float srcY = std::round(m + r() * (safeH - bh));
                const float dstX = std::round(m + r() * (safeW - bw));
                const float dstY = std::round(m + r() * (safeH - bh));
                const float invW = 1.0f / max(static_cast<float>(width), 1.0f);
                const float invH = 1.0f / max(static_cast<float>(height), 1.0f);
                dr[0] = dstX * invW; dr[1] = 1.0f - (dstY + bh) * invH; dr[2] = bw * invW; dr[3] = bh * invH;
                sr[0] = srcX * invW; sr[1] = 1.0f - (srcY + bh) * invH; sr[2] = bw * invW; sr[3] = bh * invH;
                hb = 1.0f;
            }
            dstTape[0] = scanY; dstTape[1] = stripH; dstTape[2] = shiftX; dstTape[3] = inv.env;
            std::memcpy(dstTape + 4, dr, 16);
            std::memcpy(dstTape + 8, sr, 16);
            dstTape[12] = hb; dstTape[13] = 0.0f; dstTape[14] = 0.0f; dstTape[15] = 0.0f;
            return 4u;
        }

        case KERNEL_ORD_TURBULENT:
            if (maxVec4s < 2u) return 0u;
            dstTape[0] = inv.timeSec; dstTape[1] = inv.p1; dstTape[2] = inv.p0; dstTape[3] = inv.p3;
            dstTape[4] = inv.p2;      dstTape[5] = inv.env; dstTape[6] = 0.0f; dstTape[7] = 0.0f;
            return 2u;

        case KERNEL_ORD_CHROMATIC:
            dstTape[0] = inv.p0; dstTape[1] = inv.p1; dstTape[2] = inv.p2; dstTape[3] = inv.env;
            return 1u;

        case KERNEL_ORD_ZOOM_BLUR:
            dstTape[0] = inv.p0; dstTape[1] = inv.env; dstTape[2] = 0.0f; dstTape[3] = 0.0f;
            return 1u;

        case KERNEL_ORD_MOTION_BLUR:
            dstTape[0] = inv.p0; dstTape[1] = inv.p1; dstTape[2] = inv.env; dstTape[3] = 0.0f;
            return 1u;

        case KERNEL_ORD_GLITCH_RGB:
            if (maxVec4s < 2u) return 0u;
            dstTape[0] = inv.timeSec; dstTape[1] = inv.p0; dstTape[2] = inv.p1; dstTape[3] = inv.p2;
            dstTape[4] = inv.env;     dstTape[5] = 0.0f;   dstTape[6] = 0.0f;   dstTape[7] = 0.0f;
            return 2u;

        case KERNEL_ORD_CYBER_SCAN:
            if (maxVec4s < 2u) return 0u;
            dstTape[0] = inv.timeSec; dstTape[1] = inv.p2; dstTape[2] = inv.p0; dstTape[3] = inv.p1;
            dstTape[4] = inv.p3;      dstTape[5] = inv.env; dstTape[6] = 0.0f;  dstTape[7] = 0.0f;
            return 2u;

        case KERNEL_ORD_FXAA:
            dstTape[0] = 0.0f; dstTape[1] = 0.0f; dstTape[2] = 0.0f; dstTape[3] = 0.0f;
            return 1u;

        case KERNEL_ORD_FILM_BURN:
            if (maxVec4s < 2u) return 0u;
            dstTape[0] = inv.timeSec; dstTape[1] = inv.p1; dstTape[2] = inv.p0; dstTape[3] = inv.p2;
            dstTape[4] = inv.env;     dstTape[5] = 0.0f;   dstTape[6] = 0.0f;   dstTape[7] = 0.0f;
            return 2u;

        case KERNEL_ORD_INTRO:
        case KERNEL_ORD_OUTRO: {
            if (maxVec4s < 9u) return 0u;
            IntroOutroGLState st{};
            const float dur = inv.p1 > 0.0f ? inv.p1 : 1.0f;
            const float u = clamp(inv.timeSec / dur, 0.0f, 1.0f);
            const bool active = (kOrd == KERNEL_ORD_OUTRO)
                ? effect_compute_intro_outro_state<true>(u, &st)
                : effect_compute_intro_outro_state<false>(u, &st);
            const float baseDiag = std::hypot(DEFAULT_STAGE_WIDTH_F32, DEFAULT_STAGE_HEIGHT_F32);
            const float uScale = std::hypot(static_cast<float>(width), static_cast<float>(height)) / baseDiag;
            dstTape[0] = active ? st.m : 1.0f; dstTape[1] = st.flip; dstTape[2] = st.tau; dstTape[3] = uScale;
            std::memcpy(dstTape + 4,  st.warpX.data(), 16);
            std::memcpy(dstTape + 8,  st.warpY.data(), 16);
            std::memcpy(dstTape + 12, st.ring1.data(), 16);
            std::memcpy(dstTape + 16, st.ring2.data(), 16);
            std::memcpy(dstTape + 20, st.bar1.data(),  16);
            std::memcpy(dstTape + 24, st.bar2.data(),  16);
            std::memcpy(dstTape + 28, st.line1.data(), 16);
            std::memcpy(dstTape + 32, st.line2.data(), 16);
            return 9u;
        }

        case KERNEL_ORD_FIRE_FULL:
        case KERNEL_ORD_FIRE_BORDER:
            if (maxVec4s < 2u) return 0u;
            dstTape[0] = inv.timeSec; dstTape[1] = inv.p0; dstTape[2] = inv.p1; dstTape[3] = inv.p2;
            dstTape[4] = inv.p3;      dstTape[5] = (kOrd == KERNEL_ORD_FIRE_BORDER) ? 1.0f : 0.0f;
            dstTape[6] = inv.env;     dstTape[7] = 0.0f;
            return 2u;

        case KERNEL_ORD_RAIN_BOKEH:
            if (maxVec4s < 2u) return 0u;
            dstTape[0] = inv.timeSec; dstTape[1] = inv.p0; dstTape[2] = inv.p1; dstTape[3] = inv.p2;
            dstTape[4] = inv.p3;      dstTape[5] = inv.extra0; dstTape[6] = inv.env; dstTape[7] = 0.0f;
            return 2u;

        case KERNEL_ORD_MATRIX:
            if (maxVec4s < 2u) return 0u;
            dstTape[0] = inv.timeSec; dstTape[1] = inv.p1; dstTape[2] = inv.p2; dstTape[3] = 0.0f;
            dstTape[4] = inv.env;     dstTape[5] = 7.0f;   dstTape[6] = 0.0035f; dstTape[7] = 0.0f;
            return 2u;

        default: {
            // Generic catalog effect Block direct flat memcpy
            if (inv.blockRegs && inv.blockFloatCount > 0u) {
                const u32 vec4s = min(maxVec4s, (static_cast<u32>(inv.blockFloatCount) + 3u) >> 2);
                std::memcpy(dstTape, inv.blockRegs, static_cast<usize>(vec4s) * 16u);
                return static_cast<u16>(vec4s);
            }
            dstTape[0] = inv.p0; dstTape[1] = inv.p1; dstTape[2] = inv.p2; dstTape[3] = inv.env;
            return 1u;
        }
    }
}

} // namespace

// ============================================================================
// 3. ORDER-SENSITIVE ZOBRIST KEY & EPOCH COMPILER (TIER 1 + TIER 2)
// ============================================================================
Key compute_stage_zobrist(u8 kernelOrdinal, u8 abiStage, u32 stagePos) noexcept {
    const u64 token = (static_cast<u64>(kernelOrdinal) << 24) |
                      (static_cast<u64>(abiStage) << 16) |
                      static_cast<u64>(stagePos & 0xFFFFu);
    return splitmix64(token ^ 0xA0761D6478BD642FULL);
}

void FusedPlanBuffer::compile_chain(
    const EffectInvocation* inputs,
    u32 inputCount,
    u32 width,
    u32 height,
    const TileSummary64& summary
) noexcept {
    reset();
    if (!inputs || inputCount == 0u) return;
    const u32 clampedCount = min(inputCount, FUSION_MAX_INPUT_EFFECTS);
    totalInputCount = clampedCount;

    // Active Epoch builder state (Tier 1 CPU Monoid accumulators + Tier 2 Epoch slice)
    bool epochOpen = false;
    FusedEpochPlan* cur = nullptr;
    u8 curPhase = 0u; // 0=UV, 1=SAMPLE_BOUND, 2=NON_COMMUTATIVE_COLOR
    bool hasNonCommutativeColor = false;
    Transform2D pendingUv = Transform2D::identity();
    bool hasPendingUv = false;
    u16 pendingUvFolds = 0u;
    ColorTransferOp pendingCol = ColorTransferOp::identity();
    bool hasPendingCol = false;
    u16 pendingColFolds = 0u;
    float gaussianVarAcc = 0.0f;

    auto flush_pending_uv = [&]() noexcept {
        if (!hasPendingUv || !cur) return;
        if (!pendingUv.is_identity() && stageCount < FUSION_MAX_STAGES &&
            (cur->tapeVec4Count + 2u) <= FUSION_EPOCH_MAX_VEC4 &&
            (tapeVec4Used + 2u) <= FUSION_TAPE_MAX_VEC4) {
            const u16 relOff = cur->tapeVec4Count;
            float* tp = paramTape.data() + (tapeVec4Used * 4u);
            tp[0] = pendingUv.m00; tp[1] = pendingUv.m01; tp[2] = pendingUv.m10; tp[3] = pendingUv.m11;
            tp[4] = pendingUv.tx;  tp[5] = pendingUv.ty;  tp[6] = pendingUv.pad0; tp[7] = pendingUv.pad1;
            tapeVec4Used += 2u;
            cur->tapeVec4Count += 2u;
            const u32 posInEpoch = cur->stageCount;
            stages[stageCount++] = FusedStageEntry{
                KERNEL_ORD_AFFINE_2D, 0u, relOff, 2u, 0u, 0xAFF12D01u, 0u
            };
            cur->stageCount++;
            cur->pipelineKey = rotl64(cur->pipelineKey, 7u) ^ compute_stage_zobrist(KERNEL_ORD_AFFINE_2D, 0u, posInEpoch);
            cur->aluWeightTotal += 4u;
            if (pendingUvFolds > 1u) {
                const u16 saved = pendingUvFolds - 1u;
                cur->cpuFoldedCount += saved;
                totalCpuFolded += saved;
            }
        } else if (pendingUv.is_identity()) {
            totalIdentitySkip += pendingUvFolds;
        }
        pendingUv = Transform2D::identity();
        hasPendingUv = false;
        pendingUvFolds = 0u;
    };

    auto flush_pending_col = [&]() noexcept {
        if (!hasPendingCol || !cur) return;
        if (!pendingCol.is_identity() && stageCount < FUSION_MAX_STAGES &&
            (cur->tapeVec4Count + 2u) <= FUSION_EPOCH_MAX_VEC4 &&
            (tapeVec4Used + 2u) <= FUSION_TAPE_MAX_VEC4) {
            const u16 relOff = cur->tapeVec4Count;
            float* tp = paramTape.data() + (tapeVec4Used * 4u);
            tp[0] = pendingCol.gainR; tp[1] = pendingCol.gainG; tp[2] = pendingCol.gainB; tp[3] = pendingCol.gainA;
            tp[4] = pendingCol.emitR; tp[5] = pendingCol.emitG; tp[6] = pendingCol.emitB; tp[7] = pendingCol.emitA;
            tapeVec4Used += 2u;
            cur->tapeVec4Count += 2u;
            const u32 posInEpoch = cur->stageCount;
            stages[stageCount++] = FusedStageEntry{
                KERNEL_ORD_AFFINE_COL, 2u, relOff, 2u, 0u, 0xAFF1C002u, 0u
            };
            cur->stageCount++;
            cur->pipelineKey = rotl64(cur->pipelineKey, 7u) ^ compute_stage_zobrist(KERNEL_ORD_AFFINE_COL, 2u, posInEpoch);
            cur->aluWeightTotal += 2u;
            if (pendingColFolds > 1u) {
                const u16 saved = pendingColFolds - 1u;
                cur->cpuFoldedCount += saved;
                totalCpuFolded += saved;
            }
        } else if (pendingCol.is_identity()) {
            totalIdentitySkip += pendingColFolds;
        }
        pendingCol = ColorTransferOp::identity();
        hasPendingCol = false;
        pendingColFolds = 0u;
    };

    auto seal_epoch = [&]() noexcept {
        if (!epochOpen || !cur) return;
        flush_pending_uv();
        flush_pending_col();
        if (cur->stageCount == 0u && !cur->isGeomBarrier) {
            // Everything inside collapsed to identity -> discard empty epoch!
            --epochCount;
        } else {
            cur->isBounded = (cur->tileMask64 != ~0ULL) ? 1u : 0u;
        }
        epochOpen = false;
        cur = nullptr;
        curPhase = 0u;
        hasNonCommutativeColor = false;
    };

    auto open_epoch = [&]() noexcept -> bool {
        if (epochCount >= FUSION_MAX_EPOCHS) return false;
        cur = &epochs[epochCount++];
        cur->pipelineKey       = 0x517CC1B727220A95ULL;
        cur->tileMask64        = 0ULL;
        cur->auxMask           = 0ULL;
        cur->stageBegin        = static_cast<u16>(stageCount);
        cur->stageCount        = 0u;
        cur->tapeVec4Begin     = static_cast<u16>(tapeVec4Used);
        cur->tapeVec4Count     = 0u;
        cur->fusedInputCount   = 0u;
        cur->cpuFoldedCount    = 0u;
        cur->inputFbo          = static_cast<u8>((epochCount - 1u) & 1u);
        cur->outputFbo         = static_cast<u8>(epochCount & 1u);
        cur->hasSampleStage    = 0u;
        cur->isGeomBarrier     = 0u;
        cur->geomKernelOrdinal = 0u;
        cur->isBounded         = 0u;
        cur->aluWeightTotal    = 0u;
        cur->width             = width;
        cur->height            = height;
        cur->geomSourceSlot    = 0u;
        cur->geomTimeSec       = 0.0f;
        cur->geomEnv           = 1.0f;
        epochOpen = true;
        curPhase = 0u;
        hasNonCommutativeColor = false;
        return true;
    };

    for (u32 i = 0; i < clampedCount; ++i) {
        const EffectInvocation& inv = inputs[i];
        const u64 alg = inv.algebraWord;
        if (alg == 0ULL) {
            ++totalIdentitySkip;
            continue;
        }

        // Tier 1a: Identity Gate Bypass (0-cost elimination before touching GPU)
        if ((alg & TRAIT_IDENTITY_GATE) != 0ULL && inv.env <= IDENTITY_EPSILON) {
            ++totalIdentitySkip;
            continue;
        }

        // Tier 1b: Consecutive Gaussian Convolution Variance Folding (sigma^2 = sum sigma_i^2)
        if ((alg & TRAIT_GAUSSIAN_CONV) != 0ULL) {
            const float sigma = max(0.0f, inv.p0);
            if (sigma <= IDENTITY_EPSILON) {
                ++totalIdentitySkip;
                continue;
            }
            if (gaussianVarAcc > 0.0f) {
                ++totalCpuFolded;
            }
            gaussianVarAcc += sigma * sigma;
            foldedGaussianSigma = std::sqrt(gaussianVarAcc);
            continue;
        }

        const u64 abi = abi_bits(alg);
        const u8 kOrd = kernel_ordinal(alg);

        // Compute 64-bit PullbackTileBitboard for this effect
        const u64 itemTileMask = compute_pullback_tile_mask(
            alg, inv.p0, inv.p1, inv.p2,
            static_cast<float>(width), static_cast<float>(height),
            inv.timeSec, summary
        );
        if (itemTileMask == 0ULL) {
            ++totalIdentitySkip;
            continue;
        }

        // Handle ABI_GEOM dedicated barrier (e.g. RainDrop 5-layer, Bloom pyramid)
        if ((abi & ABI_GEOM) != 0ULL) {
            seal_epoch();
            if (!open_epoch()) break;
            cur->isGeomBarrier     = 1u;
            cur->geomKernelOrdinal = kOrd;
            cur->geomSourceSlot    = inv.slot;
            cur->geomTimeSec       = inv.timeSec;
            cur->geomEnv           = inv.env;
            cur->tileMask64        = itemTileMask;
            cur->auxMask           = aux_bits(alg);
            cur->fusedInputCount   = 1u;
            cur->aluWeightTotal    = alu_weight(alg);
            cur->pipelineKey       = compute_stage_zobrist(kOrd, 3u, 0u);
            seal_epoch();
            continue;
        }

        // Tier 1c: CPU Affine2D Monoid Folding (Zoom / Shake / Pan / Mirror -> Transform2D)
        if ((alg & TRAIT_AFFINE_2D) != 0ULL) {
            // Commutativity law: spatially-invariant AffineColor commutes past UV warps!
            // Only seal if a SAMPLE or non-commutative spatially-varying COLOR has run.
            if (epochOpen && (cur->hasSampleStage != 0u || hasNonCommutativeColor)) {
                seal_epoch();
            }
            if (!epochOpen && !open_epoch()) break;
            // inv.p0 = scale, inv.p1 = rotDeg, inv.extra0 = dxNorm, inv.extra1 = dyNorm
            // inv.flags bit0 = tiled (Zoom), bit1 = mirror (Zoom mirror-tile)
            const bool isTiledZoom = ((inv.flags & 1u) != 0u) || (inv.p2 > 0.5f);
            const bool isMirrorTile = ((inv.flags & 2u) != 0u) || (inv.p2 > 0.5f);
            const float fw = static_cast<float>(width > 0u ? width : 1920u);
            const float fh = static_cast<float>(height > 0u ? height : 1080u);
            Transform2D op = isTiledZoom
                ? Transform2D::from_zoom(max(0.05f, inv.p0), inv.p1 * DEG_TO_RAD_F32, isMirrorTile, fw, fh)
                : Transform2D::from_shake(max(0.05f, inv.p0), inv.p1, inv.extra0, inv.extra1, fw, fh);
            // Contravariant UV pullback: applying `op` AFTER `pendingUv` samples `pendingUv(op(uv))`
            pendingUv = op.compose(pendingUv);
            hasPendingUv = true;
            ++pendingUvFolds;
            cur->fusedInputCount++;
            cur->tileMask64 |= itemTileMask;
            continue;
        }

        // Tier 1d: CPU AffineColor Monoid Folding (Brightness / Flicker / Tint / Fade -> ColorTransferOp)
        // Spatially-invariant affine color commutes with UV warps, so it accumulates into pendingCol
        // without sealing subsequent UV warps unless a non-commutative color field follows.
        if ((alg & TRAIT_AFFINE_COLOR) != 0ULL) {
            if (!epochOpen && !open_epoch()) break;
            const float gain = (inv.p0 > 0.0f) ? inv.p0 : 1.0f;
            ColorTransferOp op = ColorTransferOp::from_brightness(gain);
            if (inv.p2 != 0.0f) {
                op.emitR = op.emitG = op.emitB = inv.p2;
            }
            pendingCol = pendingCol.compose(op);
            hasPendingCol = true;
            ++pendingColFolds;
            cur->fusedInputCount++;
            cur->tileMask64 |= itemTileMask;
            continue;
        }

        // Tier 2: General Anonymous ABI_UV / ABI_SAMPLE / ABI_COLOR Partitioning
        u8 targetAbiStage = 2u;
        if (abi == ABI_UV)          targetAbiStage = 0u;
        else if (abi & ABI_SAMPLE)  targetAbiStage = 1u;
        else                        targetAbiStage = 2u;

        // Check true mathematical dependency barrier:
        // - ABI_UV after SAMPLE or non-commutative COLOR -> seal epoch!
        // - Second ABI_SAMPLE or ABI_SAMPLE after non-commutative COLOR -> seal epoch!
        // - Epoch tape capacity check (reserve 10 vec4 safety margin)
        const bool needSeal = epochOpen && (
            (targetAbiStage == 0u && (cur->hasSampleStage != 0u || hasNonCommutativeColor)) ||
            (targetAbiStage == 1u && (cur->hasSampleStage != 0u || hasNonCommutativeColor)) ||
            (cur->tapeVec4Count + 10u > FUSION_EPOCH_MAX_VEC4)
        );
        if (needSeal) {
            seal_epoch();
        }
        if (!epochOpen && !open_epoch()) break;

        if (targetAbiStage == 0u) {
            flush_pending_uv();
        } else if (targetAbiStage == 1u) {
            flush_pending_uv();
            cur->hasSampleStage = 1u;
            curPhase = 1u;
        } else {
            flush_pending_uv();
            flush_pending_col();
            hasNonCommutativeColor = true;
            curPhase = 2u;
        }

        if (stageCount >= FUSION_MAX_STAGES || tapeVec4Used >= FUSION_TAPE_MAX_VEC4) break;
        const u32 availVec4s = min(FUSION_EPOCH_MAX_VEC4 - cur->tapeVec4Count,
                                   FUSION_TAPE_MAX_VEC4 - tapeVec4Used);
        const u16 relOff = cur->tapeVec4Count;
        const u16 writtenVec4s = pack_invocation_to_tape(
            inv, kOrd, width, height, paramTape.data() + (tapeVec4Used * 4u), availVec4s
        );
        if (writtenVec4s == 0u) continue;

        tapeVec4Used += writtenVec4s;
        cur->tapeVec4Count += writtenVec4s;
        const u32 posInEpoch = cur->stageCount;
        stages[stageCount++] = FusedStageEntry{
            kOrd, targetAbiStage, relOff, writtenVec4s,
            static_cast<u16>(inv.slot & 0xFFFFu), inv.effectHash, inv.flags
        };
        cur->stageCount++;
        cur->fusedInputCount++;
        cur->tileMask64 |= itemTileMask;
        cur->auxMask |= aux_bits(alg);
        cur->aluWeightTotal += alu_weight(alg);
        cur->pipelineKey = rotl64(cur->pipelineKey, 7u) ^ compute_stage_zobrist(kOrd, targetAbiStage, posInEpoch);
    }

    seal_epoch();
    totalFboSwitches = (epochCount > 0u) ? epochCount : 0u;
}

// ============================================================================
// 4. TIER 3: ZERO-ALLOCATION GLSL ES 3.00 MEGA-SHADER SYNTHESIZER
// ============================================================================
u32 synthesize_epoch_glsl(
    const FusedPlanBuffer& plan,
    const FusedEpochPlan& epoch,
    char* outBuf,
    u32 outCap
) noexcept {
    if (!outBuf || outCap < 256u) return 0u;
    GlslWriter w{outBuf, outCap, 0u};
    w.buf[0] = '\0';

    w.append(kGlslHeader);

    // 1. Deduplicate and emit ONLY the pure GLSL snippets actually referenced by this epoch
    u32 emittedGroups = 0u;
    const u32 sEnd = epoch.stageBegin + epoch.stageCount;
    for (u32 i = epoch.stageBegin; i < sEnd && i < plan.stageCount; ++i) {
        const u8 kOrd = plan.stages[i].kernelOrdinal;
        if (kOrd >= KERNEL_ORD_COUNT) continue;
        const u8 grp = kSnippetDedupGroup[kOrd];
        if (grp == 0u || (emittedGroups & (1u << grp)) != 0u) continue;
        const char* snip = kKernelSnippets[kOrd];
        if (snip) {
            emittedGroups |= (1u << grp);
            w.append(snip);
        }
    }

    // 2. Emit straight-line zero-branch void main() chaining [UV*] -> [SAMPLE] -> [COLOR*]
    w.append("\nvoid main() {\n    vec2 uv = v_uv;\n");

    // Stage 1: Coordinate Diffeomorphism chain (ABI_UV)
    for (u32 i = epoch.stageBegin; i < sEnd && i < plan.stageCount; ++i) {
        const FusedStageEntry& st = plan.stages[i];
        if (st.abiStage == 0u && st.kernelOrdinal < KERNEL_ORD_COUNT && kKernelSnippets[st.kernelOrdinal]) {
            w.append("    uv = fx_uv_");
            w.append_u32(st.kernelOrdinal);
            w.append("(uv, ");
            w.append_u32(st.tapeVec4Offset);
            w.append(");\n");
        }
    }

    // Stage 2: Anisotropic / Multi-tap Sampler (0 or 1 ABI_SAMPLE)
    bool emittedSample = false;
    for (u32 i = epoch.stageBegin; i < sEnd && i < plan.stageCount; ++i) {
        const FusedStageEntry& st = plan.stages[i];
        if (st.abiStage == 1u && st.kernelOrdinal < KERNEL_ORD_COUNT && kKernelSnippets[st.kernelOrdinal]) {
            w.append("    vec4 col = fx_smp_");
            w.append_u32(st.kernelOrdinal);
            w.append("(u_src, clamp(uv, 0.001, 0.999), ");
            w.append_u32(st.tapeVec4Offset);
            w.append(");\n");
            emittedSample = true;
            break;
        }
    }
    if (!emittedSample) {
        w.append("    vec4 col = texture(u_src, clamp(uv, 0.001, 0.999));\n");
    }

    // Stage 3: Radiometric & Procedural Color Field chain (ABI_COLOR)
    for (u32 i = epoch.stageBegin; i < sEnd && i < plan.stageCount; ++i) {
        const FusedStageEntry& st = plan.stages[i];
        if (st.abiStage == 2u && st.kernelOrdinal < KERNEL_ORD_COUNT && kKernelSnippets[st.kernelOrdinal]) {
            w.append("    col = fx_col_");
            w.append_u32(st.kernelOrdinal);
            w.append("(col, uv, ");
            w.append_u32(st.tapeVec4Offset);
            w.append(");\n");
        }
    }

    w.append("    outColor = col;\n}\n");
    return w.len;
}

RcuDualBank<FusedPlanBuffer>& global_fused_plan_rcu() noexcept {
    static RcuDualBank<FusedPlanBuffer> s_planRcu;
    return s_planRcu;
}

FusedPlanBuffer& global_fused_plan_buffer() noexcept {
    return global_fused_plan_rcu().staging();
}

FusedPipelineTT& global_fused_pipeline_tt() noexcept {
    static FusedPipelineTT s_tt;
    return s_tt;
}

// ============================================================================
// 5. WEBGL2 / GLES3 EXECUTION RUNTIME (UBO + SHARED 256x256 NOISE ATLAS + ZOBRIST TT)
// ============================================================================
#if defined(__EMSCRIPTEN__)

namespace {
GLuint s_paramTapeUbo   = 0u;
GLuint s_noiseAtlasTex  = 0u;
bool   s_fusionInited   = false;
alignas(64) MultiArray<char, FUSION_GLSL_SCRATCH_BYTES> s_glslScratch;

void create_analytical_noise_atlas() noexcept {
    if (s_noiseAtlasTex != 0u) return;
    constexpr u32 kDim = 256u;
    alignas(64) static MultiArray<u8, kDim * kDim * 4u> pixels;
    for (u32 y = 0u; y < kDim; ++y) {
        for (u32 x = 0u; x < kDim; ++x) {
            const u32 h0 = static_cast<u32>(splitmix64((static_cast<u64>(x) << 16) | y));
            const u32 h1 = static_cast<u32>(splitmix64(((static_cast<u64>(x >> 1) << 16) | (y >> 1)) ^ 0x12345ULL));
            const u32 h2 = static_cast<u32>(splitmix64(((static_cast<u64>(x >> 2) << 16) | (y >> 2)) ^ 0x9E377ULL));
            const u32 idx = (y * kDim + x) * 4u;
            pixels[idx + 0u] = static_cast<u8>(h0 & 0xFFu);        // R: High-freq white/hash
            pixels[idx + 1u] = static_cast<u8>((h0 + h1) >> 1);    // G: Smooth value blend
            pixels[idx + 2u] = static_cast<u8>((h1 + h2) >> 1);    // B: Low-freq fBm base
            pixels[idx + 3u] = static_cast<u8>((h0 ^ h2) & 0xFFu); // A: Blue/cellular seed
        }
    }
    glGenTextures(1, &s_noiseAtlasTex);
    if (!s_noiseAtlasTex) return;
    glBindTexture(GL_TEXTURE_2D, s_noiseAtlasTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kDim, kDim, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    render::g_gpuState.invalidate_texture<0>();
}
} // namespace

bool fusion_kernel_init() noexcept {
    if (s_fusionInited) return true;
    glGenBuffers(1, &s_paramTapeUbo);
    if (s_paramTapeUbo != 0u) {
        glBindBuffer(GL_UNIFORM_BUFFER, s_paramTapeUbo);
        glBufferData(GL_UNIFORM_BUFFER, FUSION_EPOCH_MAX_VEC4 * 16u, nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 3u, s_paramTapeUbo);
    }
    create_analytical_noise_atlas();
    raindrop_suite_fx_init();
    s_fusionInited = true;
    return s_paramTapeUbo != 0u;
}

void fusion_register_static_texture(u32 slot, u32 texId, u32 width, u32 height) noexcept {
    register_static_texture(slot, texId, width, height);
}

void fusion_set_current_rain_params(const RainDropParams* params) noexcept {
    raindrop_suite_set_current_params(params);
}

void fusion_execute_epoch(
    const FusedPlanBuffer& plan,
    const FusedEpochPlan& epoch,
    GLuint inTex
) noexcept {
    if (!s_fusionInited) fusion_kernel_init();

    // Dedicated ABI_GEOM path (e.g. NodeVideo 5-layer _RaindropPro)
    if (epoch.isGeomBarrier != 0u) {
        if (epoch.geomKernelOrdinal == KERNEL_ORD_RAINDROP) {
            raindrop_suite_fx_render(inTex, epoch.width, epoch.height, epoch.geomTimeSec, epoch.geomEnv);
        }
        return;
    }

    auto& tt = global_fused_pipeline_tt();
    FusedPipelineEntry* entry = tt.probe_direct(epoch.pipelineKey | 1ULL);
    if (!entry) {
        const u32 glslLen = synthesize_epoch_glsl(plan, epoch, s_glslScratch.data(), FUSION_GLSL_SCRATCH_BYTES);
        if (glslLen == 0u) return;
        const GLuint prog = link_effect_program(kQuadVS, s_glslScratch.data());
        if (!prog) return;
        const GLint locSrc   = glGetUniformLocation(prog, "u_src");
        const GLint locRes   = glGetUniformLocation(prog, "u_res");
        const GLint locNoise = glGetUniformLocation(prog, "u_noiseAtlas");
        const GLuint blockIdx = glGetUniformBlockIndex(prog, "EffectTape");
        if (blockIdx != GL_INVALID_INDEX) {
            glUniformBlockBinding(prog, blockIdx, 3u);
        }
        entry = tt.write(
            epoch.pipelineKey | 1ULL,
            FusedPipelinePayload{
                prog,
                static_cast<i16>(locSrc),
                static_cast<i16>(locRes),
                static_cast<i16>(locNoise),
                static_cast<u16>(blockIdx == GL_INVALID_INDEX ? 0xFFFFu : blockIdx),
                1u
            }
        );
    }

    // 1 single UBO upload for the ENTIRE fused epoch! (Zero per-effect glUniform1f calls)
    if (s_paramTapeUbo != 0u && epoch.tapeVec4Count > 0u) {
        glBindBuffer(GL_UNIFORM_BUFFER, s_paramTapeUbo);
        glBufferSubData(
            GL_UNIFORM_BUFFER, 0,
            static_cast<GLsizeiptr>(epoch.tapeVec4Count) * 16,
            plan.paramTape.data() + (epoch.tapeVec4Begin * 4u)
        );
        glBindBufferBase(GL_UNIFORM_BUFFER, 3u, s_paramTapeUbo);
    }

    bind_effect_program_input(
        entry->payload.glProgram, inTex,
        entry->payload.locSrc, entry->payload.locRes,
        epoch.width, epoch.height
    );

    // Bind shared 256x256 AnalyticalNoiseAtlas on texture unit 4 if needed
    if ((epoch.auxMask & AUX_NOISE_ATLAS) != 0ULL && s_noiseAtlasTex != 0u && entry->payload.locNoise >= 0) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, s_noiseAtlasTex);
        glUniform1i(entry->payload.locNoise, 4);
        glActiveTexture(GL_TEXTURE0);
    }
}

void fusion_execute_single_invocation(
    const EffectInvocation& inv,
    GLuint inTex,
    u32 width,
    u32 height
) noexcept {
    auto& plan = global_fused_plan_buffer();
    plan.compile_chain(&inv, 1u, width, height, TileSummary64::full_active());
    if (plan.epochCount > 0u) {
        fusion_execute_epoch(plan, plan.epochs[0], inTex);
    }
}
#endif

} // namespace effect
} // namespace stockcut
