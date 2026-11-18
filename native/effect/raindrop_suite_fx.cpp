// ============================================================================
// NodeVideo Unified _RaindropPro ("rain drop") Engine
// Architecture:
//   - Zero-Allocation Stockfish Data-Oriented Design (fixed MultiArray pools)
//   - Clean ViewRaindrop + Stained_BumpDistort implementation (rewritten from scratch)
//   - Unified Pass 1: _static1 + _static2 + _dynamic + _rainTrailTex composite + _blur (_gaussian/_glass)
//   - Optional Pass 2/3/4 (_trail2): 8x8 RainStreaksTextureSheet + layer_style_glass bevel
// ============================================================================
#include "raindrop_suite_fx.h"
#include "gl_shader_util.h"
#include "math_utils.h"
#include "render/state.h"

#if defined(__EMSCRIPTEN__)
#include <GLES3/gl3.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace stockcut {
namespace effect {
namespace {

using stockcut::render::g_gpuState;

constexpr u32 kMaxTrailParticles = 512;
constexpr u32 kTrailHistSamples  = 24;
constexpr u32 kMaxTrailVerts     = kMaxTrailParticles * ((kTrailHistSamples - 1u) * 6u);

struct TrailPoint {
    float x = 0.0f;
    float y = 0.0f;
    float alpha = 1.0f;
};

struct TrailVertex {
    float x;
    float y;
    float u;
    float v;
    float a;
};
static_assert(sizeof(TrailVertex) == 20);

[[nodiscard]] inline u32 hash_u32(u32 x) noexcept {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

[[nodiscard]] inline float rand01(u32 seed, u32 particleIdx, u32 salt) noexcept {
    const u32 h = hash_u32(seed * 0x9E3779B1u ^ particleIdx * 0x85EBCA77u ^ salt * 0xC2B2AE3Du);
    return static_cast<float>(h & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

[[nodiscard]] inline float lerp_f32(float a, float b, float t) noexcept {
    return a + (b - a) * t;
}

[[nodiscard]] inline float hash2f(i32 ix, i32 iy, u32 seed) noexcept {
    const u32 h = hash_u32(static_cast<u32>(ix) * 374761393u +
                           static_cast<u32>(iy) * 668265263u +
                           seed * 1442695041u);
    return static_cast<float>(h & 0x00FFFFFFu) * (2.0f / 16777216.0f) - 1.0f;
}

[[nodiscard]] inline float value_noise2(float x, float y, u32 seed) noexcept {
    const i32 ix = static_cast<i32>(std::floor(x));
    const i32 iy = static_cast<i32>(std::floor(y));
    const float fx = x - static_cast<float>(ix);
    const float fy = y - static_cast<float>(iy);
    const float ux = fx * fx * (3.0f - 2.0f * fx);
    const float uy = fy * fy * (3.0f - 2.0f * fy);
    const float n00 = hash2f(ix,     iy,     seed);
    const float n10 = hash2f(ix + 1, iy,     seed);
    const float n01 = hash2f(ix,     iy + 1, seed);
    const float n11 = hash2f(ix + 1, iy + 1, seed);
    return lerp_f32(lerp_f32(n00, n10, ux), lerp_f32(n01, n11, ux), uy);
}

// ============================================================================
// Stained_BumpDistort Vertex & Fragment Shaders (ViewRaindrop Pass 1A & 1B)
// ============================================================================
static const char* const kTrailBumpVS = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in float aAlpha;
uniform vec2 uOrthoHalf;
out vec2 vScreenUV;
out vec2 vTexUV;
out float vAlpha;
void main() {
    vec2 ndc = aPos / max(uOrthoHalf, vec2(1e-4));
    gl_Position = vec4(ndc, 0.0, 1.0);
    vScreenUV = ndc * 0.5 + 0.5;
    vTexUV = aUV;
    vAlpha = aAlpha;
}
)";

// Pass 1A -> _rainTrailTex: exact Stained_BumpDistort refraction with silhouette alpha
static const char* const kTrailBumpFS = R"(#version 300 es
precision highp float;
in vec2 vScreenUV;
in vec2 vTexUV;
in float vAlpha;
uniform sampler2D uSourceTex;
uniform sampler2D uBumpMap;
uniform vec2 uGrabTexelSize;
uniform float uDistortion;
uniform int uIsRibbon;
out vec4 fragColor;
void main() {
    vec2 uvBump = (uIsRibbon != 0) ? vec2(vTexUV.x, fract(vTexUV.y * 2.5)) : vTexUV;
    vec2 bump = texture(uBumpMap, uvBump).xy * 2.0 - 1.0;
    float bumpMag = length(bump);
    float centerMask = smoothstep(0.46, 0.14, abs(vTexUV.x - 0.5));
    float endFade = smoothstep(0.0, 0.22, vTexUV.y) * smoothstep(1.0, 0.90, vTexUV.y);
    float shapeMask = max(centerMask, smoothstep(0.04, 0.16, bumpMag)) * endFade;
    float cov = clamp(shapeMask * vAlpha, 0.0, 1.0);
    if (cov < 0.04) discard;
    vec2 uv = clamp(vScreenUV + bump * (uDistortion * vAlpha) * uGrabTexelSize, vec2(0.001), vec2(0.999));
    fragColor = vec4(texture(uSourceTex, uv).rgb, 1.0);
}
)";

// Pass 1B -> _rainTrailDistortMap: accumulated trail coverage via GL_MAX blend (trail-only, zero round head)
static const char* const kTrailMaskFS = R"(#version 300 es
precision highp float;
in vec2 vScreenUV;
in vec2 vTexUV;
in float vAlpha;
uniform sampler2D uBumpMap;
uniform int uIsRibbon;
out vec4 fragColor;
void main() {
    vec2 uvBump = (uIsRibbon != 0) ? vec2(vTexUV.x, fract(vTexUV.y * 2.5)) : vTexUV;
    vec2 bump = texture(uBumpMap, uvBump).xy * 2.0 - 1.0;
    float bumpMag = length(bump);
    float centerMask = smoothstep(0.46, 0.14, abs(vTexUV.x - 0.5));
    float endFade = smoothstep(0.0, 0.22, vTexUV.y) * smoothstep(1.0, 0.90, vTexUV.y);
    float shapeMask = max(centerMask, smoothstep(0.04, 0.16, bumpMag)) * endFade;
    float cov = clamp(shapeMask * vAlpha, 0.0, 1.0);
    if (cov < 0.01) discard;
    fragColor = vec4(cov, cov, cov, 1.0);
}
)";

// ============================================================================
// Unified Main Pass Shader: rain_drop_pro.shader.glsl (SubProgram #1/#3)
// Implements _static1 + _static2 + _dynamic + _trail composite + _blur_type/_blur
// ============================================================================
static const char* const kMainRainProFS = R"(#version 300 es
precision highp float;
in vec2 v_uv;
uniform sampler2D uSourceTex;
uniform sampler2D uNoiseTex;
uniform sampler2D uTrailTex;
uniform sampler2D uTrailMaskTex;
uniform vec2 uResolution;
uniform float uTime;
uniform float uStrength;
uniform int uBlurType;
uniform float uBlur;
uniform int uEnableStatic1;
uniform float uAmount1;
uniform float uIntensity1;
uniform float uScale1;
uniform float uNoise1;
uniform float uSpeed1;
uniform int uEnableStatic2;
uniform float uAmount2;
uniform float uIntensity2;
uniform float uScale2;
uniform float uNoise2;
uniform float uSpeed2;
uniform int uEnableDynamic;
uniform float uAmount3;
uniform float uIntensity3;
uniform float uScale3;
uniform float uSpeed3;
uniform int uEnableTrail;
out vec4 fragColor;

vec2 evalStaticOctave(
    vec2 uv, vec2 resScaled, float freqMult, vec2 uvPi, vec2 noiseOff,
    float curTime, float speed, float amountThresh, float intensity, out bool hit
) {
    vec2 cellFreq = resScaled * freqMult;
    vec2 p = uvPi * cellFreq + noiseOff;
    vec2 sp = sin(p);
    vec2 cp = cos(p);
    vec2 cellUV = roundEven(uv * cellFreq - vec2(0.25)) / cellFreq;
    vec3 nSample = textureLod(uNoiseTex, cellUV, 0.0).xyz;
    float sumSin = sp.x + sp.y;
    float phase = fract(curTime * speed * (nSample.z + 0.1) + nSample.y);
    float fade = max(1.0 - phase * 2.0, 0.0);
    float dropShape = sumSin * fade;
    hit = (nSample.x < amountThresh) && (dropShape > 0.5);
    vec3 nrm = vec3(-cp.x, -cp.y, (dropShape - 0.5) * 1.8 + 0.2);
    float invLen = inversesqrt(max(dot(nrm, nrm), 1e-6));
    return (nrm.xy * invLen) * (-intensity) * 0.25;
}

vec2 evalStaticLayer(
    vec2 uv, float noiseParam, float scaleParam, float amountParam, float intensityParam, float speedParam
) {
    vec2 noiseOff = (texture(uNoiseTex, uv * noiseParam * 0.1).xy - 0.5) * 2.0;
    vec2 resScaled = uResolution * scaleParam;
    vec2 uvPi = uv * 6.28;
    float tSpeed = uTime * speedParam;

    bool h0 = false, h1 = false, h2 = false, h3 = false;
    vec2 off = evalStaticOctave(uv, resScaled, 0.06, uvPi, noiseOff, tSpeed, 1.0, amountParam * 0.08, intensityParam, h0);
    if (h0) return off;
    off = evalStaticOctave(uv, resScaled, 0.045, uvPi, noiseOff, tSpeed, 1.0, amountParam * 0.16, intensityParam, h1);
    if (h1) return off;
    off = evalStaticOctave(uv, resScaled, 0.03, uvPi, noiseOff, tSpeed, 1.0, amountParam * 0.24, intensityParam, h2);
    if (h2) return off;
    off = evalStaticOctave(uv, resScaled, 0.015, uvPi, noiseOff, tSpeed, 1.0, amountParam * 0.32, intensityParam, h3);
    if (h3) return off;
    return vec2(0.0);
}

vec2 evalDynamicOctave(vec2 aspectUV, vec2 gridScale, float curTime, float amount, float tOffset) {
    vec2 gridUV = aspectUV * gridScale;
    gridUV.y += curTime * 0.25;
    vec2 id = floor(gridUV);
    float colRand = fract(sin(id.x * 76.34 + tOffset) * 768.34);
    gridUV.y += colRand;
    id = floor(gridUV);
    float cellRand = fract(sin(dot(id, vec2(23.76, 567.2))) * 2345.0);
    if (cellRand < (1.0 - amount)) return vec2(0.0);

    vec2 st = fract(gridUV) - vec2(0.5, 0.0);
    float tCycle = curTime + cellRand * 6.28318;
    float yPos = -sin(tCycle + sin(tCycle + 0.5 * sin(tCycle))) * 0.42;
    float xShift = (cellRand - 0.5) * 0.45;
    float xWiggle = sin(aspectUV.y * 20.0 + colRand * 6.28318 + sin(aspectUV.y * 20.0) * 0.5) * 0.14 * (0.5 - abs(st.x));
    float centerX = xShift + xWiggle * (sin(cellRand * 6.28318) * 0.5 + 0.5);

    vec2 dPos = st - vec2(centerX, yPos + 0.5);
    float cellAspect = gridScale.x / max(gridScale.y, 0.01);
    float rMain = length(vec2(dPos.x, dPos.y * (cellAspect * 0.72)));
    float dropMask = smoothstep(0.22, 0.04, rMain);

    vec2 trailSt = vec2(st.x - centerX, fract(aspectUV.y * gridScale.y * 4.0) - 0.5);
    float beadFade = smoothstep(0.95, yPos + 0.5, st.y) * step(yPos + 0.5, st.y);
    float rBead = length(vec2(trailSt.x, trailSt.y * (cellAspect * 0.22)));
    float beadMask = smoothstep(0.14, 0.03, rBead) * beadFade;

    vec2 mainOff = -vec2(dPos.x, dPos.y * 2.2) * dropMask * 2.0;
    vec2 beadOff = -vec2(trailSt.x, trailSt.y * 0.8) * beadMask * 1.1;
    return mainOff + beadOff;
}

vec2 evalDynamicLayer(vec2 uv) {
    float aspect = uResolution.x / max(uResolution.y, 1.0);
    vec2 aspectUV = (uv - 0.5) * vec2(aspect, 1.0) + 0.5;
    float curTime = uTime * uSpeed3;
    float amount = clamp(uAmount3, 0.0, 1.0);
    float sc = max(uScale3, 0.01);
    vec2 o1 = evalDynamicOctave(aspectUV, vec2(6.0, 1.0) * sc, curTime, amount, 0.0);
    vec2 o2 = evalDynamicOctave(aspectUV, vec2(12.0, 2.0) * sc * 0.85, curTime * 0.82, amount * 0.85, 19.37);
    return (o1 + o2 * 0.65) * uIntensity3 * 0.18;
}

vec2 mirrorZoomUV(vec2 uv) {
    vec2 z = (uv - 0.5) * 1.01 + 0.5;
    if (z.x < 0.0) z.x = -z.x;
    else if (z.x > 1.0) z.x = 1.0 - fract(z.x);
    if (z.y < 0.0) z.y = -z.y;
    else if (z.y > 1.0) z.y = 1.0 - fract(z.y);
    return (z - 0.5) * 0.990099013 + 0.5;
}

void main() {
    vec2 dynOff = (uEnableDynamic != 0) ? evalDynamicLayer(v_uv) : vec2(0.0);
    vec2 st1Off = (uEnableStatic1 != 0) ? evalStaticLayer(v_uv, uNoise1, uScale1, uAmount1, uIntensity1, uSpeed1) : vec2(0.0);
    vec2 st2Off = (uEnableStatic2 != 0) ? evalStaticLayer(v_uv, uNoise2, uScale2, uAmount2, uIntensity2, uSpeed2) : vec2(0.0);

    bool hasDistort = (uEnableDynamic != 0) || (uEnableStatic1 != 0) || (uEnableStatic2 != 0);
    vec2 combinedOff = (dynOff + st1Off + st2Off) * uStrength;
    vec2 sampleUV = hasDistort ? clamp(mirrorZoomUV(v_uv) + combinedOff * 0.5, vec2(0.001), vec2(0.999)) : v_uv;

    vec4 trailTex = (uEnableTrail != 0) ? texture(uTrailTex, v_uv) : vec4(0.0);
    float trailAlpha = smoothstep(0.4, 0.95, trailTex.w);
    float distortMap = (uEnableTrail != 0) ? texture(uTrailMaskTex, v_uv).x : 0.0;
    float trailBlend = clamp(trailAlpha * smoothstep(0.02, 0.20, distortMap), 0.0, 1.0);

    vec4 sceneCol;
    if (uBlur <= 0.0005) {
        sceneCol = texture(uSourceTex, sampleUV);
    } else if (uBlurType == 0) {
        float bRad = uBlur * 0.02 * (1.0 - trailTex.w);
        vec4 accum = vec4(0.0);
        for (int i = 0; i < 64; ++i) {
            float fi = float(i * 2);
            float ang = fi * 161.144379;
            vec2 dir = vec2(cos(ang), sin(ang));
            float r0 = fract(sin(dot(vec2(fi, sampleUV.x + sampleUV.y), vec2(12.9898, 78.233))) * 43758.5469) + bRad;
            float r1 = fract(sin(dot(vec2(fi + 1.0, sampleUV.x + sampleUV.y + 24.0), vec2(12.9898, 78.233))) * 43758.5469) + bRad;
            accum += 0.5 * texture(uSourceTex, clamp(sampleUV + dir * (r0 * bRad), 0.001, 0.999));
            accum += 0.5 * texture(uSourceTex, clamp(sampleUV + dir * (r1 * bRad), 0.001, 0.999));
        }
        sceneCol = accum * (1.0 / 64.0);
    } else {
        float rnd = fract(sin(dot(sampleUV, vec2(92.0, 80.0))) + cos(dot(sampleUV, vec2(41.0, 62.0))) * 51.0);
        vec2 glassUV = sampleUV + vec2(uBlur * 0.5 * rnd * 0.05);
        sceneCol = texture(uSourceTex, clamp(glassUV, 0.001, 0.999));
    }

    fragColor = mix(sceneCol, trailTex, trailBlend);
}
)";

// ============================================================================
// Pass 2..4 (_trail2): 8x8 RainStreaksTextureSheet + layer_style_glass
// ============================================================================
static const char* const kTrail2MaskFS = R"(#version 300 es
precision highp float;
in vec2 v_uv;
uniform sampler2D uSourceTex;
uniform sampler2D uStreaksTex;
uniform vec2 uResolution;
uniform float uTime;
uniform float uStrength;
uniform float uAmount4;
uniform float uSpeedMin;
uniform float uSpeedMax;
uniform float uScaleMin;
uniform float uScaleMax;
uniform float uIntensityMin;
uniform float uIntensityMax;
uniform float uRotation;
uniform float uRandRotation;
uniform float uRandPos;
uniform float uSeed2;
uniform vec4 uTint;
uniform int uHasCustomTint;
out vec4 fragColor;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    float aspect = uResolution.x / max(uResolution.y, 1.0);
    vec2 centered = (v_uv - 0.5) * vec2(aspect, 1.0);
    float maskAlpha = 0.0;
    vec3 maskTint = vec3(0.0);
    int count = clamp(int(floor(uAmount4 + 0.5)), 1, 64);

    for (int i = 0; i < 64; ++i) {
        if (i >= count) break;
        vec2 sid = vec2(float(i) + uSeed2 * 13.7, uSeed2 * 3.1 + float(i) * 1.93);
        float r0 = hash12(sid + 1.1);
        float r1 = hash12(sid + 2.3);
        float r2 = hash12(sid + 3.7);
        float r3 = hash12(sid + 5.1);
        float r4 = hash12(sid + 7.9);
        float r5 = hash12(sid + 11.3);

        float spd = mix(uSpeedMin, uSpeedMax, r0) * 0.18;
        float sc  = mix(uScaleMin, uScaleMax, r1) * 0.42;
        float inten = mix(uIntensityMin, uIntensityMax, r2);
        float rotDeg = uRotation + (r3 * 2.0 - 1.0) * 180.0 * uRandRotation;
        float rad = rotDeg * 0.0174532925;
        float c = cos(rad), s = sin(rad);

        vec2 origin = vec2((r4 - 0.5) * aspect * 1.1, (r5 - 0.5) * 1.1) * mix(0.25, 1.0, uRandPos);
        float loopT = fract(uTime * spd * 0.25 + r2 * 1.7 + r0);
        vec2 pos = origin + vec2(0.0, 0.7 - loopT * 1.4);

        vec2 d = centered - pos;
        vec2 rPos = vec2(d.x * c - d.y * s, d.x * s + d.y * c) / max(sc, 0.04);
        vec2 tileUV = rPos + 0.5;
        if (tileUV.x >= 0.0 && tileUV.x <= 1.0 && tileUV.y >= 0.0 && tileUV.y <= 1.0) {
            float frameIdx = floor(mod(loopT * 64.0, 64.0));
            float tx = mod(frameIdx, 8.0);
            float ty = 7.0 - floor(frameIdx / 8.0);
            float e = 0.012;
            vec2 uv0 = (vec2(tx, ty) + clamp(tileUV, vec2(0.002), vec2(0.998))) / 8.0;
            vec2 uv1 = (vec2(tx, ty) + clamp(tileUV + vec2( e, 0.0), vec2(0.002), vec2(0.998))) / 8.0;
            vec2 uv2 = (vec2(tx, ty) + clamp(tileUV + vec2(-e, 0.0), vec2(0.002), vec2(0.998))) / 8.0;
            vec2 uv3 = (vec2(tx, ty) + clamp(tileUV + vec2(0.0,  e), vec2(0.002), vec2(0.998))) / 8.0;
            vec2 uv4 = (vec2(tx, ty) + clamp(tileUV + vec2(0.0, -e), vec2(0.002), vec2(0.998))) / 8.0;
            float rawA = max(
                texture(uStreaksTex, uv0).a,
                max(
                    max(texture(uStreaksTex, uv1).a, texture(uStreaksTex, uv2).a),
                    max(texture(uStreaksTex, uv3).a, texture(uStreaksTex, uv4).a)
                ) * 0.85
            );
            float edgeMask = smoothstep(0.0, 0.08, tileUV.x) * smoothstep(1.0, 0.92, tileUV.x)
                           * smoothstep(0.0, 0.08, tileUV.y) * smoothstep(1.0, 0.92, tileUV.y);
            float a = smoothstep(0.03, 0.30, rawA) * edgeMask * inten * uStrength;
            if (a > maskAlpha) {
                maskAlpha = a;
                maskTint = (uHasCustomTint != 0) ? uTint.rgb : texture(uSourceTex, v_uv).rgb;
            }
        }
    }
    maskAlpha = clamp(maskAlpha * 1.4, 0.0, 1.0);
    fragColor = vec4(maskTint, maskAlpha);
}
)";

static const char* const kGlassSdfFS = R"(#version 300 es
precision highp float;
in vec2 v_uv;
uniform sampler2D uMaskTex;
uniform vec2 uTexelSize;
uniform float uRadius;
out vec4 fragColor;
void main() {
    float centerA = texture(uMaskTex, v_uv).a;
    float minDistSq = 100.0;
    const int kRad = 5;
    for (int j = -kRad; j <= kRad; ++j) {
        for (int i = -kRad; i <= kRad; ++i) {
            vec2 off = vec2(float(i), float(j));
            float a = texture(uMaskTex, clamp(v_uv + off * uTexelSize * uRadius, 0.001, 0.999)).a;
            bool diff = (a >= 0.15) != (centerA >= 0.15);
            if (diff) minDistSq = min(minDistSq, dot(off, off));
        }
    }
    float dist = clamp(sqrt(minDistSq) / float(kRad), 0.0, 1.0);
    float signedVal = (centerA >= 0.15) ? (0.5 + dist * 0.5) : (0.5 - dist * 0.5);
    fragColor = vec4(signedVal, centerA, 0.0, 1.0);
}
)";

static const char* const kGlassCompositeFS = R"(#version 300 es
precision highp float;
in vec2 v_uv;
uniform sampler2D uSourceTex;
uniform sampler2D uSdfTex;
uniform sampler2D uMaskTex;
uniform vec2 uTexelSize;
uniform float uGlassIntensity;
out vec4 fragColor;
void main() {
    float maskA = texture(uMaskTex, v_uv).a;
    if (maskA < 0.02) {
        fragColor = texture(uSourceTex, v_uv);
        return;
    }
    vec2 stepUV = uTexelSize * 2.0;
    float dx = texture(uSdfTex, v_uv + vec2(stepUV.x, 0.0)).x - texture(uSdfTex, v_uv - vec2(stepUV.x, 0.0)).x;
    float dy = texture(uSdfTex, v_uv + vec2(0.0, stepUV.y)).x - texture(uSdfTex, v_uv - vec2(0.0, stepUV.y)).x;
    vec2 nrm = vec2(dx, dy) * 6.0;
    vec2 refrUV = clamp(v_uv - nrm * uGlassIntensity, vec2(0.001), vec2(0.999));
    vec4 base = texture(uSourceTex, refrUV);
    vec2 lightDir = normalize(vec2(-0.6, 0.8));
    float spec = pow(max(dot(normalize(nrm + vec2(0.0, 1e-4)), lightDir), 0.0), 6.0);
    vec4 maskSample = texture(uMaskTex, v_uv);
    base.rgb = mix(base.rgb, maskSample.rgb * 0.15 + base.rgb * 0.90, maskA * 0.25);
    base.rgb += vec3(spec * maskA * 0.35);
    fragColor = base;
}
)";

// ============================================================================
// Persistent Zero-Allocation RainDrop GPU + Simulation State
// ============================================================================
struct RainDropResources {
    bool initialized = false;

    GLuint progTrailBump     = 0;
    GLuint progTrailMask     = 0;
    GLuint progMainRainPro   = 0;
    GLuint progTrail2Mask    = 0;
    GLuint progGlassSdf      = 0;
    GLuint progGlassComp     = 0;

    GLint locTB_OrthoHalf    = -1;
    GLint locTB_SourceTex    = -1;
    GLint locTB_BumpMap      = -1;
    GLint locTB_GrabTexel    = -1;
    GLint locTB_Distortion   = -1;
    GLint locTB_IsRibbon     = -1;

    GLint locTM_OrthoHalf    = -1;
    GLint locTM_BumpMap      = -1;
    GLint locTM_IsRibbon     = -1;

    GLint locMain_SourceTex     = -1;
    GLint locMain_NoiseTex      = -1;
    GLint locMain_TrailTex      = -1;
    GLint locMain_TrailMaskTex  = -1;
    GLint locMain_Resolution    = -1;
    GLint locMain_Time          = -1;
    GLint locMain_Strength      = -1;
    GLint locMain_BlurType      = -1;
    GLint locMain_Blur          = -1;
    GLint locMain_EnableStatic1 = -1;
    GLint locMain_Amount1       = -1;
    GLint locMain_Intensity1    = -1;
    GLint locMain_Scale1        = -1;
    GLint locMain_Noise1        = -1;
    GLint locMain_Speed1        = -1;
    GLint locMain_EnableStatic2 = -1;
    GLint locMain_Amount2       = -1;
    GLint locMain_Intensity2    = -1;
    GLint locMain_Scale2        = -1;
    GLint locMain_Noise2        = -1;
    GLint locMain_Speed2        = -1;
    GLint locMain_EnableDynamic = -1;
    GLint locMain_Amount3       = -1;
    GLint locMain_Intensity3    = -1;
    GLint locMain_Scale3        = -1;
    GLint locMain_Speed3        = -1;
    GLint locMain_EnableTrail   = -1;

    GLint locT2_SourceTex     = -1;
    GLint locT2_StreaksTex    = -1;
    GLint locT2_Resolution    = -1;
    GLint locT2_Time          = -1;
    GLint locT2_Strength      = -1;
    GLint locT2_Amount4       = -1;
    GLint locT2_SpeedMin      = -1;
    GLint locT2_SpeedMax      = -1;
    GLint locT2_ScaleMin      = -1;
    GLint locT2_ScaleMax      = -1;
    GLint locT2_IntensityMin  = -1;
    GLint locT2_IntensityMax  = -1;
    GLint locT2_Rotation      = -1;
    GLint locT2_RandRotation  = -1;
    GLint locT2_RandPos       = -1;
    GLint locT2_Seed2         = -1;
    GLint locT2_Tint          = -1;
    GLint locT2_HasCustomTint = -1;

    GLint locGS_MaskTex      = -1;
    GLint locGS_TexelSize    = -1;
    GLint locGS_Radius       = -1;
    GLint locGC_SourceTex    = -1;
    GLint locGC_SdfTex       = -1;
    GLint locGC_MaskTex      = -1;
    GLint locGC_TexelSize    = -1;
    GLint locGC_GlassInt     = -1;

    // Dedicated VAOs/VBOs for internal scratch passes
    GLuint fsQuadVao         = 0;
    GLuint fsQuadVbo         = 0;
    GLuint trailVao          = 0;
    GLuint trailVbo          = 0;

    u32    rtWidth           = 0;
    u32    rtHeight          = 0;
    GLuint fboTrailColor     = 0;
    GLuint texTrailColor     = 0;
    GLuint fboTrailMask      = 0;
    GLuint texTrailMask      = 0;
    GLuint fboStage          = 0;
    GLuint texStage          = 0;
    GLuint fboStreakMask     = 0;
    GLuint texStreakMask     = 0;
    GLuint fboGlassSdf       = 0;
    GLuint texGlassSdf       = 0;

    MultiArray<TrailPoint,  kTrailHistSamples> histBuf;
    MultiArray<TrailVertex, kMaxTrailVerts>    ribbonVerts;
};

static RainDropResources s_rain;
static RainDropParams    s_activeParams{};
static bool              s_hasActiveParams = false;

void alloc_rt(GLuint& fbo, GLuint& tex, u32 w, u32 h) noexcept {
    if (fbo == 0) glGenFramebuffers(1, &fbo);
    if (tex == 0) glGenTextures(1, &tex);
    g_gpuState.bind_texture<0>(tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_gpuState.bind_framebuffer(fbo, w, h);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
}

void ensure_rain_targets(u32 w, u32 h, GLuint prevFbo) noexcept {
    if (s_rain.rtWidth == w && s_rain.rtHeight == h && s_rain.texTrailColor != 0) return;
    s_rain.rtWidth  = w;
    s_rain.rtHeight = h;
    alloc_rt(s_rain.fboTrailColor, s_rain.texTrailColor, w, h);
    alloc_rt(s_rain.fboTrailMask,  s_rain.texTrailMask,  w, h);
    alloc_rt(s_rain.fboStage,      s_rain.texStage,      w, h);
    alloc_rt(s_rain.fboStreakMask, s_rain.texStreakMask, w, h);
    alloc_rt(s_rain.fboGlassSdf,   s_rain.texGlassSdf,   w, h);
    g_gpuState.bind_texture<0>(0);
    g_gpuState.bind_framebuffer(prevFbo, w, h);
}

// ============================================================================
// Rewritten ViewRaindrop Geometry Builder (Pure Stockfish Static Staging Buffers)
// ============================================================================
void build_view_raindrop_meshes(
    const RainDropParams& p,
    float timeSec,
    float orthoW,
    float orthoH,
    u32& outRibbonVertCount
) noexcept {
    outRibbonVertCount = 0u;

    const float rate = std::max(0.0f, p.trailAmount);
    const float speedScale = std::max(0.0f, p.trailSpeed);
    if (rate <= 0.01f) return;

    const float simTime = timeSec * speedScale;
    const float lifeLo  = std::max(0.1f, std::min(p.trailLifeMin, p.trailLifeMax));
    const float lifeHi  = std::max(lifeLo, std::max(p.trailLifeMin, p.trailLifeMax));
    const float spdLo   = std::min(p.trailSpeedMin, p.trailSpeedMax);
    const float spdHi   = std::max(p.trailSpeedMin, p.trailSpeedMax);
    const float sizeLo  = std::max(0.01f, std::min(p.trailStartSizeMin, p.trailStartSizeMax));
    const float sizeHi  = std::max(sizeLo, std::max(p.trailStartSizeMin, p.trailStartSizeMax));
    const float spreadEndScale = 0.75f + std::clamp(p.trailSpread, 0.0f, 1.0f) * 0.85f;
    const u32   seed    = static_cast<u32>(std::max(0, p.trailSeed));
    const bool  prewarm = (p.layerMask & RAIN_FLAG_PREWARM) != 0u;

    const float minSpawnTime = prewarm ? (simTime - lifeHi) : std::max(0.0f, simTime - lifeHi);
    const i32 kStart = static_cast<i32>(std::ceil(minSpawnTime * rate));
    const i32 kEnd   = static_cast<i32>(std::floor(simTime * rate));
    if (kEnd < kStart) return;

    const float halfW = orthoW * 0.5f;
    const float halfH = orthoH * 0.5f;
    const float trailSpanSec = 0.62f;

    u32 drawnParticles = 0u;
    for (i32 k = kStart; k <= kEnd && drawnParticles < kMaxTrailParticles; ++k) {
        const u32 idx = static_cast<u32>(k);
        const float spawnTime = static_cast<float>(k) / rate;
        const float age = simTime - spawnTime;
        const float life = lerp_f32(lifeLo, lifeHi, rand01(seed, idx, 1u));
        if (age < 0.0f || age >= life) continue;

        const float lifeNorm = age / life;
        const float alphaIn  = std::min(1.0f, lifeNorm / 0.08f);
        const float alphaOut = std::clamp((1.0f - lifeNorm) / 0.22f, 0.0f, 1.0f);
        const float pAlpha   = std::min(alphaIn, alphaOut);
        if (pAlpha <= 0.01f) continue;

        const float sizeRand  = std::pow(rand01(seed, idx, 2u), 1.35f);
        const float startSize = lerp_f32(sizeLo, sizeHi, sizeRand);
        const float sizeCurve = lerp_f32(0.35f, spreadEndScale, lifeNorm);
        const float headSize  = std::max(0.04f, startSize * sizeCurve);

        const float x0 = (rand01(seed, idx, 3u) - 0.5f) * orthoW * 1.04f;
        const float y0 = (rand01(seed, idx, 4u) * 1.35f - 0.55f) * orthoH + p.trailOffset;

        const float spdRand   = std::pow(rand01(seed, idx, 5u), 1.4f);
        const float fallSpeed = lerp_f32(spdLo, spdHi, spdRand);
        const float effFall   = std::max(fallSpeed, 0.25f);
        const float dirVelX   = p.trailDirection * rand01(seed, idx, 6u);
        const float invRad    = 1.0f / std::sqrt(x0 * x0 + y0 * y0 + 1e-4f);
        const float radVelX   = p.trailRadial * x0 * invRad;
        const float radVelY   = p.trailRadial * y0 * invRad;

        const float vxBase = dirVelX + radVelX;
        const float vyBase = -effFall + radVelY;

        const float segWindow = std::max(0.08f, std::min(age, trailSpanSec));
        const float tWindowStart = std::max(0.0f, age - segWindow);

        float cx = x0;
        float cy = y0;
        const u32 preSteps = (tWindowStart > 1e-4f) ? 8u : 0u;
        if (preSteps > 0u) {
            const float preDt = tWindowStart / static_cast<float>(preSteps);
            for (u32 s = 0; s < preSteps; ++s) {
                const float st = (static_cast<float>(s) + 0.5f) * preDt;
                const float nx = value_noise2(cx * 0.85f + st * 0.35f, cy * 0.85f, seed + 17u);
                const float ny = value_noise2(cx * 0.85f, cy * 0.85f + st * 0.35f, seed + 53u);
                cx += (vxBase + nx * p.trailNoise * 2.0f) * preDt;
                cy += (vyBase + ny * p.trailNoise * 0.25f) * preDt;
            }
        }

        const float histDt = segWindow / static_cast<float>(kTrailHistSamples - 1u);
        for (u32 s = 0; s < kTrailHistSamples; ++s) {
            const float frac = static_cast<float>(s) / static_cast<float>(kTrailHistSamples - 1u);
            s_rain.histBuf[s] = { cx, cy, pAlpha * (0.18f + 0.82f * frac) };
            if (s + 1u < kTrailHistSamples && histDt > 1e-5f) {
                const float st = tWindowStart + (static_cast<float>(s) + 0.5f) * histDt;
                const float nx = value_noise2(cx * 0.85f + st * 0.35f, cy * 0.85f, seed + 17u);
                const float ny = value_noise2(cx * 0.85f, cy * 0.85f + st * 0.35f, seed + 53u);
                cx += (vxBase + nx * p.trailNoise * 2.0f) * histDt;
                cy += (vyBase + ny * p.trailNoise * 0.25f) * histDt;
            }
        }

        const float headX = s_rain.histBuf[kTrailHistSamples - 1u].x;
        const float headY = s_rain.histBuf[kTrailHistSamples - 1u].y;
        const float tailY = s_rain.histBuf[0].y;
        const float halfHead = headSize * 0.5f;

        const float minY = std::min(headY, tailY) - halfHead;
        const float maxY = std::max(headY, tailY) + halfHead;
        if (headX + halfHead < -halfW || headX - halfHead > halfW ||
            maxY < -halfH || minY > halfH) {
            continue;
        }

        const float totalDx = headX - s_rain.histBuf[0].x;
        const float totalDy = headY - tailY;
        const float movedDist = std::sqrt(totalDx * totalDx + totalDy * totalDy);
        if (movedDist > 1e-4f &&
            outRibbonVertCount + (kTrailHistSamples - 1u) * 6u <= kMaxTrailVerts) {
            for (u32 s = 0; s + 1u < kTrailHistSamples; ++s) {
                const TrailPoint& p0 = s_rain.histBuf[s];
                const TrailPoint& p1 = s_rain.histBuf[s + 1u];
                const float frac0 = static_cast<float>(s) / static_cast<float>(kTrailHistSamples - 1u);
                const float frac1 = static_cast<float>(s + 1u) / static_cast<float>(kTrailHistSamples - 1u);
                float dx = p1.x - p0.x;
                float dy = p1.y - p0.y;
                float segLen = std::sqrt(dx * dx + dy * dy);
                if (segLen < 1e-5f) { dx = 0.0f; dy = -1.0f; segLen = 1.0f; }
                const float px = -dy / segLen;
                const float py =  dx / segLen;
                const float tipTaper0 = (frac0 > 0.88f) ? lerp_f32(1.0f, 0.35f, (frac0 - 0.88f) / 0.12f) : 1.0f;
                const float tipTaper1 = (frac1 > 0.88f) ? lerp_f32(1.0f, 0.35f, (frac1 - 0.88f) / 0.12f) : 1.0f;
                const float hw0 = halfHead * (0.06f + 0.24f * std::pow(frac0, 0.7f)) * tipTaper0;
                const float hw1 = halfHead * (0.06f + 0.24f * std::pow(frac1, 0.7f)) * tipTaper1;

                const TrailVertex a0{ p0.x - px * hw0, p0.y - py * hw0, 0.0f, frac0, p0.alpha };
                const TrailVertex b0{ p0.x + px * hw0, p0.y + py * hw0, 1.0f, frac0, p0.alpha };
                const TrailVertex a1{ p1.x - px * hw1, p1.y - py * hw1, 0.0f, frac1, p1.alpha };
                const TrailVertex b1{ p1.x + px * hw1, p1.y + py * hw1, 1.0f, frac1, p1.alpha };

                s_rain.ribbonVerts[outRibbonVertCount++] = a0;
                s_rain.ribbonVerts[outRibbonVertCount++] = b0;
                s_rain.ribbonVerts[outRibbonVertCount++] = a1;
                s_rain.ribbonVerts[outRibbonVertCount++] = b0;
                s_rain.ribbonVerts[outRibbonVertCount++] = b1;
                s_rain.ribbonVerts[outRibbonVertCount++] = a1;
            }
        }

        ++drawnParticles;
    }
}

void draw_mesh_pass(
    GLint locOrthoHalf, float orthoW, float orthoH,
    const TrailVertex* verts, u32 vertCount
) noexcept {
    if (vertCount == 0u) return;
    glUniform2f(locOrthoHalf, orthoW * 0.5f, orthoH * 0.5f);
    g_gpuState.bind_vao(s_rain.trailVao);
    glBindBuffer(GL_ARRAY_BUFFER, s_rain.trailVbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertCount * sizeof(TrailVertex)),
                 verts, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertCount));
}

void draw_scratch_quad() noexcept {
    g_gpuState.bind_vao(s_rain.fsQuadVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void render_rain_trail_buffers(
    const RainDropParams& p,
    GLuint inTex,
    u32 width,
    u32 height,
    float timeSec,
    GLuint prevFbo
) noexcept {
    const float wF = static_cast<float>(width);
    const float hF = static_cast<float>(height);
    const float aspectHOverW = hF / std::max(wF, 1.0f);
    const float orthoW = std::max(p.trailScale, 0.01f) * 10.0f;
    const float orthoH = (aspectHOverW <= 1.0f) ? (orthoW * aspectHOverW) : (orthoW / aspectHOverW);

    u32 ribbonVertCount = 0u;
    build_view_raindrop_meshes(p, timeSec, orthoW, orthoH, ribbonVertCount);

    const GLuint trailBumpTex = get_static_texture(3);
    const float  distortion   = p.trailIntensity * p.strength * 0.16f * std::clamp(p.trailScale, 0.12f, 1.5f);

    g_gpuState.bind_framebuffer(s_rain.fboTrailColor, width, height);
    g_gpuState.set_blend<false>();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (ribbonVertCount > 0u && trailBumpTex != 0) {
        g_gpuState.use_program(s_rain.progTrailBump);
        g_gpuState.bind_texture<0>(inTex);
        glUniform1i(s_rain.locTB_SourceTex, 0);
        glUniform1i(s_rain.locTB_BumpMap, 1);
        glUniform2f(s_rain.locTB_GrabTexel, 1.0f / orthoW, 1.0f / orthoH);
        glUniform1f(s_rain.locTB_Distortion, distortion);

        g_gpuState.bind_texture<1>(trailBumpTex);
        glUniform1i(s_rain.locTB_IsRibbon, 1);
        draw_mesh_pass(s_rain.locTB_OrthoHalf, orthoW, orthoH,
                       s_rain.ribbonVerts.data(), ribbonVertCount);

        // Pass 1B -> s_fboTrailMask (_rainTrailDistortMap): trail ribbons ONLY (matches ViewRaindrop)
        g_gpuState.bind_framebuffer(s_rain.fboTrailMask, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        g_gpuState.set_blend<true>();
        glBlendEquation(GL_MAX);
        g_gpuState.set_blend_func_separate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);

        g_gpuState.use_program(s_rain.progTrailMask);
        glUniform1i(s_rain.locTM_BumpMap, 0);
        g_gpuState.bind_texture<0>(trailBumpTex);
        glUniform1i(s_rain.locTM_IsRibbon, 1);
        draw_mesh_pass(s_rain.locTM_OrthoHalf, orthoW, orthoH,
                       s_rain.ribbonVerts.data(), ribbonVertCount);

        glBlendEquation(GL_FUNC_ADD);
        g_gpuState.set_blend<false>();
        g_gpuState.bind_texture<1>(0);
        g_gpuState.bind_texture<0>(0);
    } else {
        g_gpuState.bind_framebuffer(s_rain.fboTrailMask, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    g_gpuState.bind_framebuffer(prevFbo, width, height);
}

} // namespace

void raindrop_suite_set_current_params(const RainDropParams* params) noexcept {
    if (params) {
        s_activeParams = *params;
        s_hasActiveParams = true;
    } else {
        s_hasActiveParams = false;
    }
}

bool raindrop_suite_fx_init() noexcept {
    if (s_rain.initialized) return true;

    s_rain.progTrailBump   = link_effect_program(kTrailBumpVS, kTrailBumpFS);
    s_rain.progTrailMask   = link_effect_program(kTrailBumpVS, kTrailMaskFS);
    s_rain.progMainRainPro = link_effect_program(kQuadVS, kMainRainProFS);
    s_rain.progTrail2Mask  = link_effect_program(kQuadVS, kTrail2MaskFS);
    s_rain.progGlassSdf    = link_effect_program(kQuadVS, kGlassSdfFS);
    s_rain.progGlassComp   = link_effect_program(kQuadVS, kGlassCompositeFS);

    if (!s_rain.progTrailBump || !s_rain.progTrailMask || !s_rain.progMainRainPro ||
        !s_rain.progTrail2Mask || !s_rain.progGlassSdf || !s_rain.progGlassComp) {
        return false;
    }

    static constexpr float kFsQuadVerts[8] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f
    };
    glGenVertexArrays(1, &s_rain.fsQuadVao);
    glGenBuffers(1, &s_rain.fsQuadVbo);
    g_gpuState.bind_vao(s_rain.fsQuadVao);
    glBindBuffer(GL_ARRAY_BUFFER, s_rain.fsQuadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kFsQuadVerts), kFsQuadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);

    glGenVertexArrays(1, &s_rain.trailVao);
    glGenBuffers(1, &s_rain.trailVbo);
    g_gpuState.bind_vao(s_rain.trailVao);
    glBindBuffer(GL_ARRAY_BUFFER, s_rain.trailVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(TrailVertex), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(TrailVertex), reinterpret_cast<void*>(8));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(TrailVertex), reinterpret_cast<void*>(16));
    g_gpuState.bind_vao(0);

    s_rain.locTB_OrthoHalf   = glGetUniformLocation(s_rain.progTrailBump, "uOrthoHalf");
    s_rain.locTB_SourceTex   = glGetUniformLocation(s_rain.progTrailBump, "uSourceTex");
    s_rain.locTB_BumpMap     = glGetUniformLocation(s_rain.progTrailBump, "uBumpMap");
    s_rain.locTB_GrabTexel   = glGetUniformLocation(s_rain.progTrailBump, "uGrabTexelSize");
    s_rain.locTB_Distortion  = glGetUniformLocation(s_rain.progTrailBump, "uDistortion");
    s_rain.locTB_IsRibbon    = glGetUniformLocation(s_rain.progTrailBump, "uIsRibbon");

    s_rain.locTM_OrthoHalf   = glGetUniformLocation(s_rain.progTrailMask, "uOrthoHalf");
    s_rain.locTM_BumpMap     = glGetUniformLocation(s_rain.progTrailMask, "uBumpMap");
    s_rain.locTM_IsRibbon    = glGetUniformLocation(s_rain.progTrailMask, "uIsRibbon");

    s_rain.locMain_SourceTex     = glGetUniformLocation(s_rain.progMainRainPro, "uSourceTex");
    s_rain.locMain_NoiseTex      = glGetUniformLocation(s_rain.progMainRainPro, "uNoiseTex");
    s_rain.locMain_TrailTex      = glGetUniformLocation(s_rain.progMainRainPro, "uTrailTex");
    s_rain.locMain_TrailMaskTex  = glGetUniformLocation(s_rain.progMainRainPro, "uTrailMaskTex");
    s_rain.locMain_Resolution    = glGetUniformLocation(s_rain.progMainRainPro, "uResolution");
    s_rain.locMain_Time          = glGetUniformLocation(s_rain.progMainRainPro, "uTime");
    s_rain.locMain_Strength      = glGetUniformLocation(s_rain.progMainRainPro, "uStrength");
    s_rain.locMain_BlurType      = glGetUniformLocation(s_rain.progMainRainPro, "uBlurType");
    s_rain.locMain_Blur          = glGetUniformLocation(s_rain.progMainRainPro, "uBlur");
    s_rain.locMain_EnableStatic1 = glGetUniformLocation(s_rain.progMainRainPro, "uEnableStatic1");
    s_rain.locMain_Amount1       = glGetUniformLocation(s_rain.progMainRainPro, "uAmount1");
    s_rain.locMain_Intensity1    = glGetUniformLocation(s_rain.progMainRainPro, "uIntensity1");
    s_rain.locMain_Scale1        = glGetUniformLocation(s_rain.progMainRainPro, "uScale1");
    s_rain.locMain_Noise1        = glGetUniformLocation(s_rain.progMainRainPro, "uNoise1");
    s_rain.locMain_Speed1        = glGetUniformLocation(s_rain.progMainRainPro, "uSpeed1");
    s_rain.locMain_EnableStatic2 = glGetUniformLocation(s_rain.progMainRainPro, "uEnableStatic2");
    s_rain.locMain_Amount2       = glGetUniformLocation(s_rain.progMainRainPro, "uAmount2");
    s_rain.locMain_Intensity2    = glGetUniformLocation(s_rain.progMainRainPro, "uIntensity2");
    s_rain.locMain_Scale2        = glGetUniformLocation(s_rain.progMainRainPro, "uScale2");
    s_rain.locMain_Noise2        = glGetUniformLocation(s_rain.progMainRainPro, "uNoise2");
    s_rain.locMain_Speed2        = glGetUniformLocation(s_rain.progMainRainPro, "uSpeed2");
    s_rain.locMain_EnableDynamic = glGetUniformLocation(s_rain.progMainRainPro, "uEnableDynamic");
    s_rain.locMain_Amount3       = glGetUniformLocation(s_rain.progMainRainPro, "uAmount3");
    s_rain.locMain_Intensity3    = glGetUniformLocation(s_rain.progMainRainPro, "uIntensity3");
    s_rain.locMain_Scale3        = glGetUniformLocation(s_rain.progMainRainPro, "uScale3");
    s_rain.locMain_Speed3        = glGetUniformLocation(s_rain.progMainRainPro, "uSpeed3");
    s_rain.locMain_EnableTrail   = glGetUniformLocation(s_rain.progMainRainPro, "uEnableTrail");

    s_rain.locT2_SourceTex     = glGetUniformLocation(s_rain.progTrail2Mask, "uSourceTex");
    s_rain.locT2_StreaksTex    = glGetUniformLocation(s_rain.progTrail2Mask, "uStreaksTex");
    s_rain.locT2_Resolution    = glGetUniformLocation(s_rain.progTrail2Mask, "uResolution");
    s_rain.locT2_Time          = glGetUniformLocation(s_rain.progTrail2Mask, "uTime");
    s_rain.locT2_Strength      = glGetUniformLocation(s_rain.progTrail2Mask, "uStrength");
    s_rain.locT2_Amount4       = glGetUniformLocation(s_rain.progTrail2Mask, "uAmount4");
    s_rain.locT2_SpeedMin      = glGetUniformLocation(s_rain.progTrail2Mask, "uSpeedMin");
    s_rain.locT2_SpeedMax      = glGetUniformLocation(s_rain.progTrail2Mask, "uSpeedMax");
    s_rain.locT2_ScaleMin      = glGetUniformLocation(s_rain.progTrail2Mask, "uScaleMin");
    s_rain.locT2_ScaleMax      = glGetUniformLocation(s_rain.progTrail2Mask, "uScaleMax");
    s_rain.locT2_IntensityMin  = glGetUniformLocation(s_rain.progTrail2Mask, "uIntensityMin");
    s_rain.locT2_IntensityMax  = glGetUniformLocation(s_rain.progTrail2Mask, "uIntensityMax");
    s_rain.locT2_Rotation      = glGetUniformLocation(s_rain.progTrail2Mask, "uRotation");
    s_rain.locT2_RandRotation  = glGetUniformLocation(s_rain.progTrail2Mask, "uRandRotation");
    s_rain.locT2_RandPos       = glGetUniformLocation(s_rain.progTrail2Mask, "uRandPos");
    s_rain.locT2_Seed2         = glGetUniformLocation(s_rain.progTrail2Mask, "uSeed2");
    s_rain.locT2_Tint          = glGetUniformLocation(s_rain.progTrail2Mask, "uTint");
    s_rain.locT2_HasCustomTint = glGetUniformLocation(s_rain.progTrail2Mask, "uHasCustomTint");

    s_rain.locGS_MaskTex   = glGetUniformLocation(s_rain.progGlassSdf, "uMaskTex");
    s_rain.locGS_TexelSize = glGetUniformLocation(s_rain.progGlassSdf, "uTexelSize");
    s_rain.locGS_Radius    = glGetUniformLocation(s_rain.progGlassSdf, "uRadius");

    s_rain.locGC_SourceTex = glGetUniformLocation(s_rain.progGlassComp, "uSourceTex");
    s_rain.locGC_SdfTex    = glGetUniformLocation(s_rain.progGlassComp, "uSdfTex");
    s_rain.locGC_MaskTex   = glGetUniformLocation(s_rain.progGlassComp, "uMaskTex");
    s_rain.locGC_TexelSize = glGetUniformLocation(s_rain.progGlassComp, "uTexelSize");
    s_rain.locGC_GlassInt  = glGetUniformLocation(s_rain.progGlassComp, "uGlassIntensity");

    s_rain.initialized = true;
    return true;
}

void raindrop_suite_fx_render(
    GLuint inTex, u32 width, u32 height, float time, float env
) noexcept {
    if (width == 0u || height == 0u) return;
    if (!s_rain.initialized && !raindrop_suite_fx_init()) return;

    const RainDropParams p = s_hasActiveParams ? s_activeParams : RainDropParams{};
    const float effStrength = p.strength * env;

    const bool hasStatic1 = (p.layerMask & RAIN_LAYER_STATIC1) != 0u;
    const bool hasStatic2 = (p.layerMask & RAIN_LAYER_STATIC2) != 0u;
    const bool hasDynamic = (p.layerMask & RAIN_LAYER_DYNAMIC) != 0u;
    const bool hasTrail   = (p.layerMask & RAIN_LAYER_TRAIL)   != 0u;
    const bool hasTrail2  = (p.layerMask & RAIN_LAYER_TRAIL2)  != 0u;
    const bool runMainPass = hasStatic1 || hasStatic2 || hasDynamic || hasTrail || (p.blur > 0.0005f) || !hasTrail2;

    GLint prevFboRaw = 0;
    GLint prevVaoRaw = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFboRaw);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVaoRaw);
    const GLuint outFbo  = static_cast<GLuint>(prevFboRaw);
    const GLuint quadVao = static_cast<GLuint>(prevVaoRaw);

    ensure_rain_targets(width, height, outFbo);

    // Stage A: ViewRaindrop particle + trail ribbon simulation -> s_texTrailColor & s_texTrailMask
    if (hasTrail) {
        render_rain_trail_buffers(p, inTex, width, height, time, outFbo);
    }

    // Stage B: Unified main pass (_static1 + _static2 + _dynamic + _trail composite + _blur)
    GLuint curInputTex = inTex;
    if (runMainPass) {
        if (hasTrail2) {
            g_gpuState.bind_framebuffer(s_rain.fboStage, width, height);
        } else {
            g_gpuState.bind_framebuffer(outFbo, width, height);
        }

        const GLuint noiseTex = get_static_texture(0);
        g_gpuState.use_program(s_rain.progMainRainPro);
        g_gpuState.bind_texture<0>(inTex);
        g_gpuState.bind_texture<1>(noiseTex != 0 ? noiseTex : inTex);
        g_gpuState.bind_texture<2>(hasTrail ? s_rain.texTrailColor : inTex);
        g_gpuState.bind_texture<3>(hasTrail ? s_rain.texTrailMask  : inTex);
        glUniform1i(s_rain.locMain_SourceTex, 0);
        glUniform1i(s_rain.locMain_NoiseTex, 1);
        glUniform1i(s_rain.locMain_TrailTex, 2);
        glUniform1i(s_rain.locMain_TrailMaskTex, 3);

        glUniform2f(s_rain.locMain_Resolution, static_cast<float>(width), static_cast<float>(height));
        glUniform1f(s_rain.locMain_Time, time);
        glUniform1f(s_rain.locMain_Strength, effStrength);
        glUniform1i(s_rain.locMain_BlurType, static_cast<GLint>(p.blurType));
        glUniform1f(s_rain.locMain_Blur, p.blur);

        glUniform1i(s_rain.locMain_EnableStatic1, hasStatic1 ? 1 : 0);
        glUniform1f(s_rain.locMain_Amount1, p.amount1);
        glUniform1f(s_rain.locMain_Intensity1, p.intensity1);
        glUniform1f(s_rain.locMain_Scale1, p.scale1);
        glUniform1f(s_rain.locMain_Noise1, p.noise1);
        glUniform1f(s_rain.locMain_Speed1, p.speed1);

        glUniform1i(s_rain.locMain_EnableStatic2, hasStatic2 ? 1 : 0);
        glUniform1f(s_rain.locMain_Amount2, p.amount2);
        glUniform1f(s_rain.locMain_Intensity2, p.intensity2);
        glUniform1f(s_rain.locMain_Scale2, p.scale2);
        glUniform1f(s_rain.locMain_Noise2, p.noise2);
        glUniform1f(s_rain.locMain_Speed2, p.speed2);

        glUniform1i(s_rain.locMain_EnableDynamic, hasDynamic ? 1 : 0);
        glUniform1f(s_rain.locMain_Amount3, p.amount3);
        glUniform1f(s_rain.locMain_Intensity3, p.intensity3);
        glUniform1f(s_rain.locMain_Scale3, p.scale3);
        glUniform1f(s_rain.locMain_Speed3, p.speed3);

        glUniform1i(s_rain.locMain_EnableTrail, hasTrail ? 1 : 0);

        if (hasTrail2) {
            draw_scratch_quad();
            curInputTex = s_rain.texStage;
        } else {
            // Final pass: ShaderEffectPassPolicy::finish_fullscreen_pass issues glDrawArrays
            g_gpuState.bind_vao(quadVao);
            return;
        }
    }

    // Stage C: _trail2 (8x8 RainStreaksTextureSheet + layer_style_glass SDF + composite -> outFbo)
    if (hasTrail2) {
        const GLuint streaksTex = get_static_texture(1);

        // C.1 -> Streak Mask FBO
        g_gpuState.bind_framebuffer(s_rain.fboStreakMask, width, height);
        g_gpuState.use_program(s_rain.progTrail2Mask);
        g_gpuState.bind_texture<0>(curInputTex);
        g_gpuState.bind_texture<1>(streaksTex != 0 ? streaksTex : curInputTex);
        glUniform1i(s_rain.locT2_SourceTex, 0);
        glUniform1i(s_rain.locT2_StreaksTex, 1);
        glUniform2f(s_rain.locT2_Resolution, static_cast<float>(width), static_cast<float>(height));
        glUniform1f(s_rain.locT2_Time, time);
        glUniform1f(s_rain.locT2_Strength, effStrength);
        glUniform1f(s_rain.locT2_Amount4, p.trail2Amount);
        glUniform1f(s_rain.locT2_SpeedMin, p.trail2SpeedMin);
        glUniform1f(s_rain.locT2_SpeedMax, p.trail2SpeedMax);
        glUniform1f(s_rain.locT2_ScaleMin, p.trail2ScaleMin);
        glUniform1f(s_rain.locT2_ScaleMax, p.trail2ScaleMax);
        glUniform1f(s_rain.locT2_IntensityMin, p.trail2IntensityMin);
        glUniform1f(s_rain.locT2_IntensityMax, p.trail2IntensityMax);
        glUniform1f(s_rain.locT2_Rotation, p.trail2Rotation);
        glUniform1f(s_rain.locT2_RandRotation, p.trail2RandRotation);
        glUniform1f(s_rain.locT2_RandPos, p.trail2RandPos);
        glUniform1f(s_rain.locT2_Seed2, static_cast<float>(p.trail2Seed));
        glUniform4f(s_rain.locT2_Tint, p.trail2TintR, p.trail2TintG, p.trail2TintB, p.trail2TintA);
        glUniform1i(s_rain.locT2_HasCustomTint, (p.layerMask & RAIN_FLAG_CUSTOM_TINT) ? 1 : 0);
        draw_scratch_quad();

        // C.2 -> Glass SDF FBO
        g_gpuState.bind_framebuffer(s_rain.fboGlassSdf, width, height);
        g_gpuState.use_program(s_rain.progGlassSdf);
        g_gpuState.bind_texture<0>(s_rain.texStreakMask);
        glUniform1i(s_rain.locGS_MaskTex, 0);
        glUniform2f(s_rain.locGS_TexelSize, 1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
        glUniform1f(s_rain.locGS_Radius, 1.8f);
        draw_scratch_quad();

        // C.3 -> Final Glass Bevel Composite (drawn by ShaderEffectPassPolicy::finish_fullscreen_pass)
        g_gpuState.bind_framebuffer(outFbo, width, height);
        g_gpuState.use_program(s_rain.progGlassComp);
        g_gpuState.bind_texture<0>(curInputTex);
        g_gpuState.bind_texture<1>(s_rain.texGlassSdf);
        g_gpuState.bind_texture<2>(s_rain.texStreakMask);
        glUniform1i(s_rain.locGC_SourceTex, 0);
        glUniform1i(s_rain.locGC_SdfTex, 1);
        glUniform1i(s_rain.locGC_MaskTex, 2);
        glUniform2f(s_rain.locGC_TexelSize, 1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
        glUniform1f(s_rain.locGC_GlassInt, p.trail2Strength * 0.015f * env);
        g_gpuState.bind_vao(quadVao);
    }
}

} // namespace effect
} // namespace stockcut
#endif
