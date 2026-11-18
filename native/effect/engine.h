// ============================================================================
// CoreEngine - Physical Transition Analytical LUT Evaluator (Zero-Enum)
// ----------------------------------------------------------------------------
// Refactored: Legacy enum class EffectType & EffectType templates DELETED.
// All effect identity / RoI / spatial support queries live in effect/algebra.h
// driven purely by 64-bit anonymous algebraWord.
// ============================================================================
#pragma once
#include "types.h"
#include "math_utils.h"
#include "data_structures.h"

namespace stockcut {

struct alignas(16) IntroOutroGLState {
    float m;
    float flip;
    float tau;
    float pad;
    MultiArray<float, 4> warpX;
    MultiArray<float, 4> warpY;
    MultiArray<float, 4> ring1;
    MultiArray<float, 4> ring2;
    MultiArray<float, 4> bar1;
    MultiArray<float, 4> bar2;
    MultiArray<float, 4> line1;
    MultiArray<float, 4> line2;
    u32   active;
};

void effect_engine_init();

// Compile-time specialized analytical transition evaluator
template<bool IsOutro>
bool effect_compute_intro_outro_state(float u, IntroOutroGLState* outState);

} // namespace stockcut
