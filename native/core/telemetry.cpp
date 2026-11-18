// ============================================================================
// CoreEngine - O(1) Sliding-Window Telemetry Implementation (Zero-Branch Hot Path)
// ============================================================================
#include "telemetry.h"

namespace stockcut {

alignas(64) static TelemetryBuffer g_telemetry{};
static MultiArray<double, FPS_WINDOW_SIZE_CPP> s_fpsDeltas{};
static u32    s_fpsHead  = 0;
static u32    s_fpsCount = 0;
static double s_fpsSum   = 0.0;

TelemetryBuffer* telemetry_get_ptr() noexcept {
    return &g_telemetry;
}

void telemetry_reset() noexcept {
    std::memset(&g_telemetry, 0, sizeof(TelemetryBuffer));
    s_fpsDeltas.clear();
    s_fpsHead  = 0;
    s_fpsCount = 0;
    s_fpsSum   = 0.0;
}

// Hot-Path Telemetry Record: Pure branchless O(1) execution
// Input deltaMs is guaranteed positive and validated by the gatekeeper or internal timer.
double telemetry_record_frame_time(double deltaMs) noexcept {
    s_fpsSum -= s_fpsDeltas[s_fpsHead];
    s_fpsDeltas[s_fpsHead] = deltaMs;
    s_fpsSum += deltaMs;
    s_fpsHead = (s_fpsHead + 1u) & (FPS_WINDOW_SIZE_CPP - 1u);
    s_fpsCount += static_cast<u32>(s_fpsCount < FPS_WINDOW_SIZE_CPP);
    g_telemetry.fps = (MS_PER_SECOND * static_cast<double>(s_fpsCount)) / s_fpsSum;
    g_telemetry.frameTimeMs = deltaMs;
    return g_telemetry.fps;
}

} // namespace stockcut

extern "C" {
// The Gatekeeper (Boundary Sanitizer): Guards the external API border
WASM_EXPORT(wasm_telemetry_record_frame_time) double wasm_telemetry_record_frame_time(double deltaMs) {
    if (deltaMs <= 0.0 || deltaMs > stockcut::MAX_FRAME_DELTA_MS) {
        return stockcut::telemetry_get_ptr()->fps;
    }
    return stockcut::telemetry_record_frame_time(deltaMs);
}
} // extern "C"
