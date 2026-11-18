// ============================================================================
// CoreEngine - Effect Engine & Physical Transition Implementation
// ============================================================================
#include "engine.h"
#include <cmath>
#include <utility>

namespace stockcut {

static constexpr float IO_END     = 8.15f;
static constexpr float IO_OUT_END = 7.883f;
static constexpr float IO_SHIFT   = 0.0f;
static constexpr u32   IO_WARP_NB = 22;
static constexpr u32   IO_RING_NB = 42;
static constexpr u32   IO_M_NB    = 28;
static constexpr u32   IO_BARS_NB = 3;
static constexpr u32   IO_LINES_NB = 6;
static constexpr u32   IO_OUT_WARP_NB = 17;

static constexpr MultiArray<float, IO_WARP_NB, 8> IO_WARP{{
    {{3.902f, 0.8981f, -0.191f,  -0.0752f, -0.0451f, 0.9074f, -0.0648f, -0.1796f}},
    {{4.065f, 0.9144f, -0.191f,  -0.0752f, -0.0451f, 0.9148f, -0.0648f, -0.1796f}},
    {{4.268f, 0.9387f, -0.1887f, -0.0764f, -0.0463f, 0.9352f, -0.0648f, -0.1815f}},
    {{4.471f, 0.9609f, -0.1989f, -0.0751f, -0.0461f, 0.9596f, -0.075f,  -0.1859f}},
    {{4.675f, 0.9884f, -0.2156f, -0.0763f, -0.0501f, 0.9834f, -0.077f,  -0.191f}},
    {{4.878f, 1.0116f, -0.2084f, -0.0846f, -0.0448f, 1.0078f, -0.0798f, -0.188f}},
    {{5.081f, 1.0333f, -0.2045f, -0.0775f, -0.0439f, 1.0329f, -0.0801f, -0.1886f}},
    {{5.284f, 1.0526f, -0.1753f, -0.0647f, -0.0369f, 1.0566f, -0.0779f, -0.1693f}},
    {{5.488f, 1.0602f, -0.1375f, -0.0509f, -0.0282f, 1.0616f, -0.0486f, -0.1434f}},
    {{5.691f, 1.0605f, -0.1084f, -0.0407f, -0.022f,  1.0613f, -0.0401f, -0.111f}},
    {{5.894f, 1.0561f, -0.0841f, -0.0321f, -0.0168f, 1.0557f, -0.0305f, -0.0837f}},
    {{6.097f, 1.0500f, -0.065f,  -0.025f,  -0.0125f, 1.0490f, -0.0233f, -0.0636f}},
    {{6.301f, 1.0422f, -0.0489f, -0.0187f, -0.0095f, 1.0416f, -0.0176f, -0.0477f}},
    {{6.504f, 1.0345f, -0.0365f, -0.0139f, -0.0069f, 1.0341f, -0.0132f, -0.0352f}},
    {{6.707f, 1.0272f, -0.0267f, -0.0102f, -0.0049f, 1.0267f, -0.0096f, -0.0257f}},
    {{6.910f, 1.0204f, -0.019f,  -0.0074f, -0.0032f, 1.0202f, -0.007f,  -0.0187f}},
    {{7.114f, 1.0144f, -0.0132f, -0.0051f, -0.0019f, 1.0143f, -0.0049f, -0.0131f}},
    {{7.317f, 1.0094f, -0.0088f, -0.0033f, -0.0008f, 1.0094f, -0.0035f, -0.0086f}},
    {{7.520f, 1.0052f, -0.0052f, -0.0021f, -0.0001f, 1.0051f, -0.0018f, -0.0052f}},
    {{7.723f, 1.0023f, -0.0027f, -0.001f,   0.0005f, 1.0024f, -0.0012f, -0.0027f}},
    {{7.927f, 1.0003f, -0.0007f, -0.0003f,  0.0006f, 1.0006f, -0.0004f, -0.0009f}},
    {{8.150f, 1.0000f,  0.0f,     0.0f,     0.0f,    1.0000f,  0.0f,     0.0f}}
}};

static constexpr MultiArray<float, IO_RING_NB, 7> IO_RING{{
    {{0.0f, 13.3f, 1.33f, 0.62f, 0.37f, 8.5f, 0.5f}},
    {{0.2f, 13.6f, 1.33f, 0.74f, 0.49f, 8.8f, 0.5f}},
    {{0.4f, 15.0f, 1.30f, 0.73f, 0.63f, 8.6f, 0.5f}},
    {{0.6f, 16.8f, 1.25f, 0.70f, 0.80f, 8.6f, 0.5f}},
    {{0.8f, 19.1f, 1.27f, 0.74f, 0.87f, 8.7f, 0.5f}},
    {{1.0f, 22.9f, 1.20f, 0.65f, 0.91f, 8.4f, 0.5f}},
    {{1.2f, 27.4f, 1.15f, 0.60f, 0.93f, 8.2f, 0.5f}},
    {{1.4f, 32.6f, 1.12f, 0.56f, 0.95f, 8.0f, 0.5f}},
    {{1.6f, 38.6f, 1.09f, 0.54f, 0.95f, 7.9f, 0.5f}},
    {{1.8f, 46.0f, 1.06f, 0.51f, 0.97f, 7.9f, 0.5f}},
    {{2.0f, 54.3f, 1.05f, 0.50f, 0.98f, 7.8f, 0.5f}},
    {{2.2f, 63.9f, 1.04f, 0.49f, 0.98f, 7.8f, 0.5f}},
    {{2.4f, 75.0f, 1.04f, 0.49f, 0.99f, 7.8f, 0.5f}},
    {{2.6f, 87.8f, 1.04f, 0.49f, 0.99f, 7.9f, 0.5f}},
    {{2.8f, 102.8f, 1.03f, 0.48f, 0.99f, 7.8f, 0.5f}},
    {{3.0f, 120.6f, 1.03f, 0.48f, 0.99f, 7.8f, 0.5f}},
    {{3.2f, 141.9f, 1.02f, 0.48f, 0.99f, 7.7f, 0.5f}},
    {{3.4f, 168.6f, 1.02f, 0.48f, 0.99f, 7.7f, 0.5f}},
    {{3.6f, 205.8f, 0.97f, 0.45f, 1.00f, 7.2f, 0.5f}},
    {{3.8f, 290.0f, 0.86f, 0.43f, 1.03f, 6.0f, 0.48f}},
    {{3.9f, 360.0f, 0.87f, 0.44f, 1.06f, 6.0f, 0.54f}},
    {{4.0f, 455.0f, 0.85f, 0.42f, 1.10f, 6.0f, 0.56f}},
    {{4.1f, 574.0f, 0.83f, 0.43f, 1.12f, 6.0f, 0.58f}},
    {{4.2f, 721.0f, 0.79f, 0.41f, 1.16f, 6.0f, 0.59f}},
    {{4.3f, 847.0f, 1.00f, 0.38f, 1.03f, 6.0f, 0.65f}},
    {{4.4f, 1013.0f, 1.00f, 0.60f, 1.00f, 6.0f, 0.67f}},
    {{4.5f, 1120.0f, 1.00f, 0.60f, 0.97f, 6.0f, 0.67f}},
    {{4.6f, 1134.0f, 1.00f, 0.60f, 0.83f, 6.0f, 0.70f}},
    {{4.7f, 1135.0f, 1.00f, 0.60f, 0.70f, 6.0f, 0.73f}},
    {{4.8f, 1132.0f, 1.00f, 0.60f, 0.59f, 6.0f, 0.76f}},
    {{4.9f, 1130.0f, 1.00f, 0.60f, 0.50f, 6.0f, 0.79f}},
    {{5.0f, 1127.0f, 1.00f, 0.60f, 0.42f, 6.0f, 0.809f}},
    {{5.1f, 1128.0f, 1.00f, 0.60f, 0.35f, 6.0f, 0.828f}},
    {{5.2f, 1145.0f, 1.00f, 0.60f, 0.34f, 6.0f, 0.831f}},
    {{5.3f, 1201.0f, 1.00f, 0.60f, 0.44f, 6.0f, 0.803f}},
    {{5.4f, 1339.0f, 1.00f, 0.60f, 0.91f, 6.0f, 0.731f}},
    {{5.5f, 1332.0f, 1.00f, 0.60f, 0.95f, 6.0f, 0.754f}},
    {{5.6f, 1712.0f, 1.00f, 0.60f, 3.00f, 6.0f, 0.586f}},
    {{5.7f, 1823.0f, 1.00f, 0.60f, 3.00f, 6.0f, 0.551f}},
    {{5.8f, 1854.0f, 1.00f, 0.60f, 3.00f, 6.0f, 0.551f}},
    {{6.0f, 2000.0f, 1.00f, 0.60f, 3.00f, 6.0f, 0.56f}},
    {{6.2f, 2000.0f, 1.00f, 0.60f, 0.00f, 6.0f, 0.56f}}
}};

static constexpr MultiArray<float, IO_M_NB, 2> IO_M{{
    {{3.9f, 0.0f}}, {{4.0f, 0.02f}}, {{4.1f, 0.05f}}, {{4.2f, 0.07f}}, {{4.3f, 0.11f}}, {{4.4f, 0.15f}},
    {{4.5f, 0.19f}}, {{4.6f, 0.23f}}, {{4.7f, 0.27f}}, {{4.8f, 0.32f}}, {{4.9f, 0.37f}}, {{5.0f, 0.41f}},
    {{5.1f, 0.46f}}, {{5.2f, 0.51f}}, {{5.3f, 0.56f}}, {{5.4f, 0.61f}}, {{5.5f, 0.67f}}, {{5.6f, 0.71f}},
    {{5.7f, 0.75f}}, {{5.8f, 0.80f}}, {{5.9f, 0.84f}}, {{6.0f, 0.87f}}, {{6.1f, 0.91f}}, {{6.2f, 0.94f}},
    {{6.3f, 0.96f}}, {{6.4f, 0.98f}}, {{6.5f, 0.99f}}, {{6.6f, 1.0f}}
}};

static constexpr MultiArray<float, IO_BARS_NB, 9> IO_BARS{{
    {{4.100f, 4.467f, 0.395f, 0.612f, 0.26f, 0.26f, -0.035f, 0.070f, 0.011f}},
    {{4.467f, 4.833f, 0.360f, 0.635f, 0.92f, 0.96f,  0.070f, 0.050f, 0.023f}},
    {{4.833f, 5.200f, 0.545f, 0.700f, 0.57f, 0.65f,  0.060f, 0.045f, 0.020f}}
}};

static constexpr MultiArray<float, IO_LINES_NB, 8> IO_LINES{{
    {{5.200f, 5.750f, 1.000f, 0.26f, 0.12f, 6.0f, 20.0f, 1.0f}},
    {{6.350f, 6.700f, 1.000f, 0.26f, 0.16f, 6.0f, 20.0f, 2.0f}},
    {{6.700f, 7.080f, 0.754f, 0.30f, 0.20f, 8.0f, 24.0f, 3.0f}},
    {{7.080f, 7.450f, 0.575f, 0.32f, 0.24f, 8.0f, 24.0f, 4.0f}},
    {{7.450f, 7.800f, 0.500f, 0.28f, 0.20f, 6.0f, 18.0f, 5.0f}},
    {{7.800f, 8.150f, 0.500f, 0.22f, 0.15f, 4.0f, 14.0f, 6.0f}}
}};

static constexpr MultiArray<float, IO_OUT_WARP_NB, 2> IO_OUT_WARP{{
    {{0.000f, 8.150f}}, {{0.350f, 7.800f}}, {{0.750f, 7.433f}}, {{1.150f, 7.050f}},
    {{1.533f, 6.683f}}, {{1.783f, 6.450f}}, {{2.783f, 5.500f}}, {{3.166f, 5.200f}},
    {{3.566f, 4.833f}}, {{3.966f, 4.467f}}, {{4.366f, 4.100f}}, {{4.783f, 3.650f}},
    {{5.783f, 2.650f}}, {{6.783f, 1.550f}}, {{7.283f, 0.800f}}, {{7.783f, 0.050f}},
    {{7.883f, 0.000f}}
}};

template <usize N>
static float ioLerp1D(const MultiArray<float, N, 2>& tab, float x) noexcept {
    if (x <= tab[0][0]) return tab[0][1];
    if (x >= tab[N - 1][0]) return tab[N - 1][1];
    usize i = 0;
    while (i < N - 2 && x > tab[i + 1][0]) ++i;
    const float f = (x - tab[i][0]) / (tab[i + 1][0] - tab[i][0]);
    return tab[i][1] + (tab[i + 1][1] - tab[i][1]) * f;
}

static bool ioComputeWarp(float tau, MultiArray<float, 7>& outWarp7) noexcept {
    if (tau >= IO_WARP[IO_WARP_NB - 1][0]) return false;
    u32 i = 0;
    if (tau > IO_WARP[0][0]) {
        while (i < IO_WARP_NB - 2 && tau > IO_WARP[i + 1][0]) ++i;
    }
    const float* a = IO_WARP[i].data();
    const float* b = IO_WARP[i + 1].data();
    const float* pa = (i > 0) ? IO_WARP[i - 1].data() : a;
    const float* pb = (i + 2 < IO_WARP_NB) ? IO_WARP[i + 2].data() : b;
    const float h = b[0] - a[0];
    const float s = clamp((tau - a[0]) / h, 0.0f, 1.0f);
    const float s2 = s * s;
    const float s3 = s2 * s;
    const float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
    const float h10 = s3 - 2.0f * s2 + s;
    const float h01 = -2.0f * s3 + 3.0f * s2;
    const float h11 = s3 - s2;
    for (u32 c = 1; c <= 7; ++c) {
        const float m0 = (b[c] - pa[c]) / (b[0] - pa[0]) * h;
        const float m1 = (pb[c] - a[c]) / (pb[0] - a[0]) * h;
        outWarp7[c - 1] = h00 * a[c] + h10 * m0 + h01 * b[c] + h11 * m1;
    }
    const float dev = abs_val(outWarp7[0] - 1.0f) + abs_val(outWarp7[4] - 1.0f) +
                      abs_val(outWarp7[1]) + abs_val(outWarp7[2]) + abs_val(outWarp7[3]) +
                      abs_val(outWarp7[5]) + abs_val(outWarp7[6]);
    return dev >= 0.004f;
}

static IntroOutroGLState g_introOutroState{};

void effect_engine_init() {
    g_introOutroState = {};
    g_introOutroState.m = 1.0f;
    g_introOutroState.active = 0;
}

template<bool IsOutro>
bool effect_compute_intro_outro_state(float u, IntroOutroGLState* outState) {
    if (!outState) outState = &g_introOutroState;
    u = clamp(u, 0.0f, 1.0f);
    float tau = 0.0f;
    if constexpr (IsOutro) {
        tau = ioLerp1D(IO_OUT_WARP, u * IO_OUT_END);
    } else {
        tau = u * IO_END;
    }
    tau = max(0.0f, tau);
    outState->tau = tau;
    outState->flip = IsOutro ? 1.0f : 0.0f;

    const float ts = tau + IO_SHIFT;
    outState->m = (ts <= IO_M[0][0]) ? 0.0f : ioLerp1D(IO_M, ts);

    MultiArray<float, 7> warp7{};
    if (outState->m > DELTA_EPSILON && ioComputeWarp(tau, warp7)) {
        outState->warpX = {{warp7[0], warp7[1], warp7[2], warp7[3]}};
        outState->warpY = {{warp7[4], warp7[5], warp7[6], 0.0f}};
    } else {
        outState->warpX = {{0.0f, 0.0f, 0.0f, 0.0f}};
        outState->warpY = {{1.0f, 0.0f, 0.0f, 0.0f}};
    }

    if (ts < 6.2f) {
        u32 i = 0;
        while (i < IO_RING_NB - 2 && ts > IO_RING[i + 1][0]) ++i;
        const float f = (ts - IO_RING[i][0]) / (IO_RING[i + 1][0] - IO_RING[i][0]);
        const float rA = IO_RING[i][1];
        const float rB = IO_RING[i + 1][1];
        const float R = (rA > 0.0f && rB > 0.0f)
            ? std::exp(std::log(rA) * (1.0f - f) + std::log(rB) * f)
            : (rA + (rB - rA) * f);
        outState->ring1 = {{
            R,
            IO_RING[i][2] + (IO_RING[i + 1][2] - IO_RING[i][2]) * f,
            IO_RING[i][3] + (IO_RING[i + 1][3] - IO_RING[i][3]) * f,
            IO_RING[i][4] + (IO_RING[i + 1][4] - IO_RING[i][4]) * f
        }};
        outState->ring2 = {{
            IO_RING[i][5] + (IO_RING[i + 1][5] - IO_RING[i][5]) * f,
            IO_RING[i][6] + (IO_RING[i + 1][6] - IO_RING[i][6]) * f,
            0.0f, 0.0f
        }};
    } else {
        outState->ring1 = {};
        outState->ring2 = {};
    }

    outState->bar1 = {};
    outState->bar2 = {};
    for (u32 b = 0; b < IO_BARS_NB; ++b) {
        if (tau >= IO_BARS[b][0] && tau < IO_BARS[b][1]) {
            outState->bar1 = {{IO_BARS[b][2], IO_BARS[b][3], IO_BARS[b][4], IO_BARS[b][5]}};
            outState->bar2 = {{IO_BARS[b][6], IO_BARS[b][7], IO_BARS[b][8], 1.0f}};
            break;
        }
    }

    outState->line1 = {};
    outState->line2 = {};
    for (u32 l = 0; l < IO_LINES_NB; ++l) {
        if (tau >= IO_LINES[l][0] && tau < IO_LINES[l][1]) {
            outState->line1 = {{IO_LINES[l][2], IO_LINES[l][3], IO_LINES[l][4], IO_LINES[l][5]}};
            outState->line2 = {{IO_LINES[l][6], IO_LINES[l][7], 0.0f, 1.0f}};
            break;
        }
    }

    const bool hasVisual = (outState->m < OPACITY_OPAQUE_MIN) ||
                           (outState->ring1[0] > 0.0f && outState->ring1[3] > 0.01f) ||
                           (outState->bar2[3] > 0.5f) ||
                           (outState->line2[3] > 0.5f) ||
                           (abs_val(outState->warpX[0] - 1.0f) > OPACITY_EPSILON);
    outState->active = hasVisual ? 1u : 0u;
    return hasVisual;
}

template bool effect_compute_intro_outro_state<false>(float, IntroOutroGLState*);
template bool effect_compute_intro_outro_state<true>(float, IntroOutroGLState*);

} // namespace stockcut
