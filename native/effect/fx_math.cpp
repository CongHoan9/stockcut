// ============================================================================
// CoreEngine - Effect Math & Trajectory Simulation Implementation
// ============================================================================
#include "fx_math.h"

namespace stockcut {

// ============================================================================
// 1. ZOOM SIMULATION
// ============================================================================
static inline float zoomPrng(float seed) noexcept {
    const float x = fast_sin(seed + 1234.5678f) * 10000.0f;
    return x - static_cast<float>(static_cast<i32>(x));
}

template<bool HasMirror>
void zoom_evaluate_periodic(
    float speed, float amplitude, float phase, float tilt,
    double tMs,
    ZoomEvalResult* outResult
) {
    float p = static_cast<float>((tMs / 1000.0) * static_cast<double>(speed));
    p = p - static_cast<float>(static_cast<i32>(p));
    if (p < 0.0f) p += 1.0f;
    float t = p * ZOOM_PERIODIC_CYCLES + phase;
    t = t - static_cast<float>(static_cast<i32>(t / ZOOM_PERIODIC_CYCLES)) * ZOOM_PERIODIC_CYCLES;
    if (t < 0.0f) t += ZOOM_PERIODIC_CYCLES;
    float g = t + 0.1f;
    g = g - static_cast<float>(static_cast<i32>(g / ZOOM_PERIODIC_CYCLES)) * ZOOM_PERIODIC_CYCLES;
    if (g < 0.0f) g += ZOOM_PERIODIC_CYCLES;
    const i32 c = static_cast<i32>(g);
    float tx = g - static_cast<float>(c);
    const float ampMod = 1.0f + (zoomPrng(static_cast<float>(c * 10)) - 0.5f) * 0.1f;
    const float jitter = (zoomPrng(static_cast<float>(c * 10 + 2)) - 0.5f) * 0.1f;
    tx -= jitter * fast_sin(PI_VAL * tx);
    const float wRaw = -fast_cos(TWO_PI_F32 * tx);
    const float w = __builtin_copysignf(sqrt_f32(abs_val(wRaw)), wRaw);
    const float k = (w + 1.0f) * 0.5f;
    outResult->scale = 1.0f + amplitude * w * (1.0f + (ampMod - 1.0f) * k);
    outResult->rotateDeg = (static_cast<float>(((c & 1) << 1) - 1) * tilt) * k;
    outResult->mirror = static_cast<u32>(HasMirror);
}
template void zoom_evaluate_periodic<false>(float, float, float, float, double, ZoomEvalResult*);
template void zoom_evaluate_periodic<true>(float, float, float, float, double, ZoomEvalResult*);

static inline float peakProfile(float q) noexcept {
    if (q <= 0.0f) return 1.0f;
    if (q >= 1.0f) return 0.0f;
    const float r = fast_cos(PI_VAL * q);
    const float s = (r < 0.0f ? -1.0f : 1.0f) * sqrt_f32(abs_val(r));
    return (s + 1.0f) * 0.5f;
}

void zoom_evaluate_peaks(
    const ZoomPeak* peaks, u32 peakCount,
    float speed, float baseScale,
    double tMs,
    ZoomEvalResult* outResult
) {
    const float half = speed > OPACITY_EPSILON ? (ZOOM_DEFAULT_HALF_MS / speed) : ZOOM_DEFAULT_HALF_MS;
    float scale = baseScale;
    float maxProfile = 0.0f;
    const ZoomPeak* prevPeak = nullptr;
    const ZoomPeak* nextPeak = nullptr;
    for (const ZoomPeak* p = peaks, *const end = peaks + peakCount; p < end; ++p) {
        const float dt = static_cast<float>(tMs) - p->ms;
        const float d = abs_val(dt);
        if (dt >= 0.0f) prevPeak = p;
        else if (!nextPeak) nextPeak = p;
        if (d < half) {
            const float prof = peakProfile(d / half);
            const float s = baseScale + (p->peakScale - baseScale) * prof;
            if (s > scale) scale = s;
            if (prof > maxProfile) maxProfile = prof;
        }
    }
    float tilt = 0.0f;
    if (maxProfile > 0.0f) {
        if (!prevPeak) {
            tilt = nextPeak->tiltDeg;
        } else if (!nextPeak) {
            tilt = prevPeak->tiltDeg;
        } else {
            const float fromPrevious = static_cast<float>(tMs) - prevPeak->ms;
            const float toNext = nextPeak->ms - static_cast<float>(tMs);
            const float previousProfile = fromPrevious < half ? peakProfile(fromPrevious / half) : 0.0f;
            const float nextProfile = toNext < half ? peakProfile(toNext / half) : 0.0f;
            const float previousWeight = nextProfile * (1.0f - previousProfile);
            const float nextWeight = previousProfile * (1.0f - nextProfile);
            const float weightSum = previousWeight + nextWeight;
            const float mix = weightSum > 1.0e-9f ? (previousWeight / weightSum) : 0.5f;
            tilt = prevPeak->tiltDeg * (1.0f - mix) + nextPeak->tiltDeg * mix;
        }
    }
    outResult->scale = scale;
    outResult->rotateDeg = tilt * maxProfile;
    outResult->mirror = 0;
}

float shake_scale_boost(float amplitude, float tiltDeg, float W, float H) {
    const float tiltRad = tiltDeg * DEG_TO_RAD_F32;
    const float diagonal = sqrt_f32(W * W + H * H);
    const float needed = amplitude * W + tiltRad * diagonal * 0.5f;
    const float minDimension = min(W, H);
    return 1.0f + 2.0f * needed / (minDimension > 0.0f ? minDimension : 1.0f) + SHAKE_BOOST_MARGIN;
}

// ============================================================================
// 2. SHAKE SIMULATION
// ============================================================================
template<bool HasFade>
void shake_evaluate(
    float speed, float amplitude, float tilt, float fadeMs,
    double tMs, double durMs,
    float W, float H,
    ShakeEvalResult* outResult
) {
    const float w = TWO_PI_F32 * speed * static_cast<float>(tMs / 1000.0);
    auto n = [w](float a, float p, float c) -> float {
        return 0.6f * fast_sin(w * a + p) + 0.4f * fast_sin(w * c + p * 2.3f + 1.0f);
    };
    float env = 1.0f;
    if constexpr (HasFade) {
        if (fadeMs > OPACITY_EPSILON) {
            const float e0 = clamp(static_cast<float>(tMs) / fadeMs, 0.0f, 1.0f);
            const float e1 = clamp(static_cast<float>(durMs - tMs) / fadeMs, 0.0f, 1.0f);
            env = smoothstep01(min(e0, e1));
        }
    }
    outResult->dx = amplitude * env * n(1.0f, 0.7f, 2.31f);
    outResult->dy = amplitude * env * n(1.13f, 2.1f, 2.77f);
    outResult->rotDeg = tilt * env * n(0.87f, 4.2f, 2.13f);
    outResult->scaleBoost = shake_scale_boost(amplitude, tilt, W, H);
}
template void shake_evaluate<false>(float, float, float, float, double, double, float, float, ShakeEvalResult*);
template void shake_evaluate<true>(float, float, float, float, double, double, float, float, ShakeEvalResult*);

// ============================================================================
// 3. FLICKER SIMULATION
// ============================================================================
template<bool IsSquare>
float flicker_evaluate_periodic(
    float speed, float brightness, float baseVal,
    double tMs
) {
    if (speed <= 0.0f) return baseVal;
    const double period = 1000.0 / static_cast<double>(speed);
    double cycle = tMs / period;
    cycle = cycle - static_cast<double>(static_cast<i64>(cycle));
    cycle += static_cast<double>(cycle < 0.0);
    float w = 0.0f;
    if constexpr (IsSquare) {
        w = static_cast<float>(static_cast<i32>(cycle < 0.5) << 1) - 1.0f;
    } else {
        w = fast_cos(static_cast<float>(TWO_PI_F64 * cycle));
    }
    return max(0.0f, baseVal + brightness * w);
}
template float flicker_evaluate_periodic<false>(float, float, float, double);
template float flicker_evaluate_periodic<true>(float, float, float, double);

// ============================================================================
// 4. INTRO / OUTRO ALPHA FIELD SYNTHESIZER
// ============================================================================
static MultiArray<float, DEFAULT_STAGE_WIDTH_U32> s_arch{};
static MultiArray<float, DEFAULT_STAGE_WIDTH_U32> s_combTooth{};
static MultiArray<float, DEFAULT_STAGE_WIDTH_U32> s_combStripes{};
static MultiArray<u8,    DEFAULT_STAGE_WIDTH_U32> s_topNotch{};
static MultiArray<u8,    DEFAULT_STAGE_WIDTH_U32> s_botNotch{};
static MultiArray<u8,    DEFAULT_STAGE_WIDTH_U32> s_dashOn{};
static MultiArray<float, DEFAULT_STAGE_WIDTH_U32> s_dashBright{};
static u32 s_lastLw = 0;

static void updateColumnLut(u32 lw, float baseW) noexcept {
    if (lw > DEFAULT_STAGE_WIDTH_U32) lw = DEFAULT_STAGE_WIDTH_U32;
    if (s_lastLw == lw) return;
    s_lastLw = lw;
    const float bw = baseW > 0.0f ? baseW : DEFAULT_STAGE_WIDTH_F32;
    const float invLw = 1.0f / static_cast<float>(lw);
    for (u32 x = 0; x < lw; ++x) {
        const float fx = (static_cast<float>(x) + 0.5f) * invLw;
        const float xRel = fx - 0.5f;
        s_arch[x] = xRel * xRel * 0.048f;
        const float X = fx * bw;
        const float cTooth = fast_cos(TWO_PI_F32 * X / 10.53f);
        const float tanhVal = clamp(cTooth * 1.8f, -1.0f, 1.0f);
        const float fadeLeft = smoothstep01((X - 40.0f) / 200.0f);
        const float fadeRight = 1.0f - smoothstep01((X - 1800.0f) / 120.0f);
        const float d1 = (X - 1425.0f) / 5.0f;
        const float s1 = (abs_val(d1) < 4.0f) ? (0.90f * (1.0f - d1 * d1 * 0.06f)) : 0.0f;
        const float d2 = (X - 1458.0f) / 5.0f;
        const float s2 = (abs_val(d2) < 4.0f) ? (0.90f * (1.0f - d2 * d2 * 0.06f)) : 0.0f;
        s_combTooth[x] = tanhVal * fadeLeft * fadeRight;
        s_combStripes[x] = s1 + s2;
        s_topNotch[x] = ((X >= 504.0f && X <= 550.0f) || (X >= 620.0f && X <= 710.0f) || (X >= 1160.0f && X <= 1177.0f) ||
                         (X >= 1183.0f && X <= 1194.0f) || (X >= 1201.0f && X <= 1211.0f) || (X >= 1316.0f && X <= 1355.0f) || (X >= 1363.0f && X <= 1400.0f)) ? 1u : 0u;
        s_botNotch[x] = ((X >= 650.0f && X <= 710.0f) || (X >= 1200.0f && X <= 1220.0f)) ? 1u : 0u;
        const float s1w = fast_sin(X * 0.035f);
        const float s2w = fast_sin(X * 0.095f + 1.2f);
        const float s3w = fast_sin(X * 0.25f + 0.5f);
        const float regZone = 0.5f + 0.5f * fast_sin(X * 0.006f);
        const float wave = (1.0f - regZone) * (s1w * 0.7f + s2w * 0.6f) + regZone * (s3w * 0.9f + 0.2f);
        s_dashOn[x] = (wave > 0.18f) ? 1u : 0u;
        s_dashBright[x] = 0.60f + 0.35f * (fast_cos(X * 0.005f) * 0.5f + 0.5f);
    }
}

u32 effect_render_intro_field_cpp(
    float tau, float m, float flip,
    float ringR,
    float barY0, float barY1, float barPIn, float barPOut, float barTIn, float barTOut, float barSp, u32 barActive,
    float lineYc, float linePlateau, float lineCore, float lineHw1, float lineHw2, u32 lineState, u32 lineActive,
    u32 lw, u32 lh,
    float W, float H,
    float baseW, float baseH,
    u32* outPixelsRGBA
) {
    (void)m; (void)ringR; (void)barPOut; (void)barTOut; (void)barSp;
    if (!outPixelsRGBA || lw == 0 || lh == 0) return 0;
    if (lw > DEFAULT_STAGE_WIDTH_U32) lw = DEFAULT_STAGE_WIDTH_U32;
    updateColumnLut(lw, baseW);
    const float resolvedBaseW = baseW > 0.0f ? baseW : DEFAULT_STAGE_WIDTH_F32;
    const float resolvedBaseH = baseH > 0.0f ? baseH : DEFAULT_STAGE_HEIGHT_F32;
    const float diag = sqrt_f32(W * W + H * H);
    const float bDiag = sqrt_f32(resolvedBaseW * resolvedBaseW + resolvedBaseH * resolvedBaseH);
    const float u = diag / (bDiag > 0.0f ? bDiag : diag);
    const float dyBar = barY1 - barY0;
    const float fe = ((tau >= 4.8f ? 20.0f : 24.0f) * u / (H > 0.0f ? H : 1.0f));
    const float bandLo = barActive ? (barY0 - 0.2f * dyBar - 0.75f * fe - 0.015f) : 0.0f;
    const float bandHi = barActive ? (barY1 + 0.2f * dyBar + 0.75f * fe + 0.015f) : 0.0f;
    const bool archOn = (tau >= 4.47f);
    u32 renderedPixels = 0;
    for (u32 y = 0; y < lh; ++y) {
        float yyBase = (static_cast<float>(y) + 0.5f) / static_cast<float>(lh);
        if (flip > 0.5f) yyBase = 1.0f - yyBase;
        const bool inBar = barActive && (yyBase >= bandLo && yyBase <= bandHi);
        float ln = 0.0f;
        if (lineActive) {
            const float d = abs_val(yyBase - lineYc) * H / u;
            const float plate = linePlateau * (1.0f - smoothstep01((d - lineHw1) / (lineHw2 - lineHw1)));
            const float cd = d * 0.25f;
            const float core = lineCore * (abs_val(cd) < 3.0f ? (1.0f - cd * cd * 0.11f) : 0.0f);
            ln = clamp(plate + core, 0.0f, 1.0f);
        }
        u32* rowPtr = outPixelsRGBA + y * lw;
        if (!inBar && ln <= OPACITY_EPSILON) {
            for (u32* p = rowPtr, *const end = rowPtr + lw; p < end; ++p) {
                *p = INTRO_OUTRO_BASE_PIXEL;
            }
            continue;
        }
        for (u32 x = 0; x < lw; ++x) {
            float lineDashVal = 1.0f;
            if (lineActive) {
                if (lineState == 1u) lineDashVal = s_dashOn[x] ? (0.88f + 0.12f * s_dashBright[x]) : 0.48f;
                else if (lineState == 2u) lineDashVal = s_dashOn[x] ? (0.92f + 0.08f * s_dashBright[x]) : 0.65f;
                else if (lineState == 4u) lineDashVal = 1.0f;
                else lineDashVal = 0.75f;
            }
            float add = (ln > OPACITY_EPSILON) ? (ln * lineDashVal) : 0.0f;
            if (inBar) {
                const float yyEff = yyBase + (archOn ? s_arch[x] : 0.0f);
                const float sh = smoothstep01((yyEff - barY0) / fe + 0.5f) * smoothstep01((barY1 - yyEff) / fe + 0.5f);
                if (sh > DELTA_EPSILON) {
                    const float yNorm = clamp((yyEff - barY0) / (dyBar > IDENTITY_EPSILON ? dyBar : 1.0f), 0.0f, 1.0f);
                    const float vp = fast_sin(PI_VAL * yNorm);
                    const float combPattern = s_combTooth[x] * vp + s_combStripes[x];
                    float v = barPIn + barTIn * combPattern;
                    float edgeDarken = 1.0f;
                    if (yNorm < 0.06f && s_topNotch[x]) edgeDarken -= 0.38f;
                    if (yNorm > 0.94f && s_botNotch[x]) edgeDarken -= 0.38f;
                    v *= max(0.60f, edgeDarken);
                    add = min(1.0f, clamp(v, 0.0f, 1.0f) * sh + add);
                }
            }
            u32 a8 = add > 0.0f ? static_cast<u32>(add * 255.0f + 0.5f) : 0u;
            if (a8 > 255u) a8 = 255u;
            rowPtr[x] = INTRO_OUTRO_BASE_PIXEL | (a8 << 24);
            renderedPixels += static_cast<u32>(a8 > 0u);
        }
    }
    return renderedPixels;
}

// 5. SYMPLECTIC PHYSICAL SPRING EVALUATION (HAMILTONIAN DYNAMICS)
void symplectic_spring_evaluate(
    float targetX, float targetY,
    float k, float damping, float mass,
    float dt,
    float& inOutX, float& inOutVx,
    float& inOutY, float& inOutVy
) noexcept {
    SymplecticPhase<float> phaseX(inOutX, inOutVx * mass, mass, k, damping);
    SymplecticPhase<float> phaseY(inOutY, inOutVy * mass, mass, k, damping);
    phaseX.step_leapfrog(dt, targetX);
    phaseY.step_leapfrog(dt, targetY);
    inOutX = phaseX.position();
    inOutVx = phaseX.velocity();
    inOutY = phaseY.position();
    inOutVy = phaseY.velocity();
}

} // namespace stockcut
