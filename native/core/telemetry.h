// ============================================================================
// CoreEngine - Lock-Free 64-Byte Aligned Telemetry Buffer
// ============================================================================
#pragma once
#include "types.h"
#include "data_structures.h"

namespace stockcut {

constexpr u32    TELEMETRY_PAD_WORDS   = 30u;
constexpr u32    FPS_WINDOW_SIZE_CPP   = 32u;
constexpr double MAX_FRAME_DELTA_MS    = 1000.0;
constexpr double MS_PER_SECOND         = 1000.0;
constexpr double DEFAULT_TELEMETRY_FPS = 60.0;

enum TelemetryRenderMode : u32 {
    TELEMETRY_MODE_DIRTY  = 0u,
    TELEMETRY_MODE_FULL   = 1u,
    TELEMETRY_MODE_TT_HIT = 3u,
    TELEMETRY_MODE_NONE   = 4u
};

struct alignas(64) TelemetryBuffer {
    double fps;               // [0..1] offset 0
    double frameTimeMs;       // [2..3] offset 8
    double cpuTimeMs;         // [4..5] offset 16
    double nowMs;             // [6..7] offset 24
    u64    currentHash;       // [8..9] offset 32
    u32    frameIndex;        // [10]   offset 40
    u32    renderMode;        // [11]   offset 44
    u32    dirtyN;            // [12]   offset 48
    u32    culledCount;       // [13]   offset 52
    u32    activeFxCount;     // [14]   offset 56
    u32    activeLayersCount; // [15]   offset 60
    u32    styleEpoch;        // [16]   offset 64
    u32    fontsVer;          // [17]   offset 68
    u32    mediaVer;          // [18]   offset 72
    u32    baseW;             // [19]   offset 76
    u32    baseH;             // [20]   offset 80
    u32    previewW;          // [21]   offset 84
    u32    previewH;          // [22]   offset 88
    float  scale;             // [23]   offset 92
    float  dpr;               // [24]   offset 96
    u32    ttHits;            // [25]   offset 100
    u32    ttMisses;          // [26]   offset 104
    u32    zobristBreakers;   // [27]   offset 108
    u32    dirtyAreaPx;       // [28]   offset 112
    u32    flags;             // [29]   offset 116
    u32    heartbeat;         // [30]   offset 120
    u32    fullCount;         // [31]   offset 124
    u32    dirtyCount;        // [32]   offset 128
    u32    noneCount;         // [33]   offset 132
    MultiArray<u32, TELEMETRY_PAD_WORDS> pad;
};
static_assert(sizeof(TelemetryBuffer) == 256, "TelemetryBuffer must be 256 bytes");
static_assert(alignof(TelemetryBuffer) == 64, "TelemetryBuffer must be 64-byte aligned");

TelemetryBuffer* telemetry_get_ptr() noexcept;
void             telemetry_reset() noexcept;
double           telemetry_record_frame_time(double deltaMs) noexcept;

} // namespace stockcut
