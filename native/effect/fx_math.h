// ============================================================================
// CoreEngine - Effect Math & Trajectory Simulation Subsystem
// ============================================================================
#pragma once
#include "types.h"
#include "math_utils.h"
#include "data_structures.h"

namespace stockcut {

constexpr float ZOOM_PERIODIC_CYCLES     = 10.0f;
constexpr float ZOOM_DEFAULT_HALF_MS     = 500.0f;
constexpr float SHAKE_BOOST_MARGIN       = 0.002f;
constexpr u32   INTRO_OUTRO_BASE_PIXEL   = 0x00FFFFFFu;

struct alignas(16) ZoomEvalResult {
    float scale;
    float rotateDeg;
    u32   mirror;
    float pad;
};

struct ZoomPeak {
    float ms;
    float peakScale;
    float tiltDeg;
};

struct alignas(16) ShakeEvalResult {
    float dx;
    float dy;
    float rotDeg;
    float scaleBoost;
};

// 1. ZOOM TRAJECTORY EVALUATION
template<bool HasMirror>
void zoom_evaluate_periodic(
    float speed, float amplitude, float phase, float tilt,
    double tMs,
    ZoomEvalResult* outResult
);

void zoom_evaluate_peaks(
    const ZoomPeak* peaks, u32 peakCount,
    float speed, float baseScale,
    double tMs,
    ZoomEvalResult* outResult
);

// 2. SHAKE HARMONIC SYNTHESIS
template<bool HasFade>
void shake_evaluate(
    float speed, float amplitude, float tilt, float fadeMs,
    double tMs, double durMs,
    float W, float H,
    ShakeEvalResult* outResult
);

float shake_scale_boost(float amplitude, float tiltDeg, float W, float H);

// 3. FLICKER BRIGHTNESS EVALUATION
template<bool IsSquare>
float flicker_evaluate_periodic(
    float speed, float brightness, float baseVal,
    double tMs
);

// 4. INTRO / OUTRO ALPHA FIELD SYNTHESIS
u32 effect_render_intro_field_cpp(
    float tau, float m, float flip,
    float ringR,
    float barY0, float barY1, float barPIn, float barPOut, float barTIn, float barTOut, float barSp, u32 barActive,
    float lineYc, float linePlateau, float lineCore, float lineHw1, float lineHw2, u32 lineState, u32 lineActive,
    u32 lw, u32 lh,
    float W, float H,
    float baseW, float baseH,
    u32* outPixelsRGBA
);

// 5. SYMPLECTIC PHYSICAL SPRING EVALUATION (HAMILTONIAN DYNAMICS)
// Tích phân Leapfrog bảo toàn năng lượng và thể tích không gian pha Liouville
void symplectic_spring_evaluate(
    float targetX, float targetY,
    float k, float damping, float mass,
    float dt,
    float& inOutX, float& inOutVx,
    float& inOutY, float& inOutVy
) noexcept;

} // namespace stockcut
