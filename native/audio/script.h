// ============================================================================
// CoreEngine - Music & Audio Analysis Script (Stockfish Temporal Bitboard Architecture)
// ============================================================================
#pragma once

#include "types.h"
#include "data_structures.h"
#include "quantum/quantum_wave.h"
#include <cmath>

namespace stockcut {

constexpr u32    MUSIC_CHUNK_SHIFT_MS          = 8u;                         // 256ms = 1 << 8
constexpr u32    MUSIC_CHUNK_SIZE_MS           = 1u << MUSIC_CHUNK_SHIFT_MS; // 256ms
constexpr u32    MUSIC_BIT_RESOLUTION_MS       = 4u;                         // 256ms / 64 = 4ms
constexpr u32    MUSIC_BITS_PER_CHUNK          = 64u;
constexpr u32    MAX_MUSIC_CHUNKS              = 4096u;
constexpr u32    MAX_SCRIPT_SAMPLES            = 65536u;
constexpr double DEFAULT_MUSIC_HOP_SEC         = 0.010;
constexpr double MIN_MUSIC_HOP_SEC             = 0.0001;
constexpr double MUSIC_MS_PER_SEC              = 1000.0;
constexpr u8     MUSIC_BASS_ENERGY_MIN_U8      = 12u;

struct alignas(16) MusicChunk {
    u64 hitBitboard;
    u32 sampleOffset;
    u16 accumulatedHits;
    u8  peakEnergyU8;
    u8  hasBass;

    [[nodiscard]] inline u32 hit_count() const noexcept {
        return BitOps<u64>::popcount64(hitBitboard);
    }
};
static_assert(sizeof(MusicChunk) == 16, "MusicChunk must be exactly 16 bytes");

class alignas(64) MusicAnalysisScript {
public:
    double durationMs   = 0.0;
    double hopSec       = DEFAULT_MUSIC_HOP_SEC;
    u32    totalSamples = 0;
    u32    totalHits    = 0;
    u32    chunkCount   = 0;

    alignas(16) MultiArray<float, MAX_SCRIPT_SAMPLES>     levels;
    alignas(16) MultiArray<u16, MAX_SCRIPT_SAMPLES>       hitIndices;
    alignas(64) MultiArray<MusicChunk, MAX_MUSIC_CHUNKS>  chunks;

    void reset() noexcept;

    bool bake(
        const float* inLevels,
        const u16* inHits,
        u32 sampleCount,
        double inHopSec,
        double inDurationMs
    ) noexcept;

    [[nodiscard]] [[gnu::always_inline]] static inline double wrap_loop_time_ms(
        double nowMs, double maxMs, u32* outLoopCount = nullptr
    ) noexcept {
        if (outLoopCount) {
            *outLoopCount = (maxMs > 0.0) ? static_cast<u32>(nowMs / maxMs) : 0u;
        }
        return (maxMs > 0.0 && nowMs >= maxMs) ? std::fmod(nowMs, maxMs) : nowMs;
    }

    [[nodiscard]] [[gnu::always_inline]] inline float sample_level(double nowMs) const noexcept {
        if (totalSamples == 0 || nowMs < 0.0 || hopSec <= MIN_MUSIC_HOP_SEC) return 0.0f;
        const double maxMs = static_cast<double>(totalSamples) * hopSec * MUSIC_MS_PER_SEC;
        const double effectiveMs = wrap_loop_time_ms(nowMs, maxMs);
        const double samplePos = (effectiveMs / MUSIC_MS_PER_SEC) / hopSec;
        const u32 idx = static_cast<u32>(samplePos) % totalSamples;
        const u32 nextIdx = (idx + 1u) % totalSamples;
        const float fr = static_cast<float>(samplePos - static_cast<double>(static_cast<u32>(samplePos)));
        return levels[idx] + (levels[nextIdx] - levels[idx]) * fr;
    }

    [[nodiscard]] [[gnu::always_inline]] inline u32 sample_hit_index(double nowMs) const noexcept {
        if (totalSamples == 0 || nowMs < 0.0 || hopSec <= MIN_MUSIC_HOP_SEC) return 0u;
        const double maxMs = static_cast<double>(totalSamples) * hopSec * MUSIC_MS_PER_SEC;
        u32 loopCount = 0u;
        const double effectiveMs = wrap_loop_time_ms(nowMs, maxMs, &loopCount);
        const u32 idx = static_cast<u32>((effectiveMs / MUSIC_MS_PER_SEC) / hopSec) % totalSamples;
        return (loopCount * (totalHits > 0u ? totalHits : 1u)) + hitIndices[idx];
    }

    [[nodiscard]] [[gnu::always_inline]] inline bool has_hit_at(double nowMs) const noexcept {
        if (chunkCount == 0 || nowMs < 0.0) return false;
        const double maxMs = static_cast<double>(chunkCount * MUSIC_CHUNK_SIZE_MS);
        const double effectiveMs = wrap_loop_time_ms(nowMs, maxMs);
        const u32 chunkIdx = static_cast<u32>(effectiveMs) >> MUSIC_CHUNK_SHIFT_MS;
        if (chunkIdx >= chunkCount) return false;
        const MusicChunk& chunk = chunks[chunkIdx];
        if (chunk.peakEnergyU8 == 0u) return false;
        const u32 localMs = static_cast<u32>(effectiveMs) & (MUSIC_CHUNK_SIZE_MS - 1u);
        const u32 bitIdx = localMs / MUSIC_BIT_RESOLUTION_MS;
        return (chunk.hitBitboard & (1ULL << bitIdx)) != 0ULL;
    }

    static constexpr float rotation_sign(u32 hit) noexcept {
        return static_cast<float>(static_cast<i32>((hit & 1u) << 1) - 1);
    }

    [[nodiscard]] [[gnu::always_inline]] inline float sample_rotation_sign(double nowMs) const noexcept {
        return rotation_sign(sample_hit_index(nowMs));
    }

    [[nodiscard]] [[gnu::always_inline]] inline u8 sample_quantum_phase(double nowMs) const noexcept {
        const u32 hit = sample_hit_index(nowMs);
        return static_cast<u8>(hit & 3u);
    }

    [[nodiscard]] inline float sample_pulse(double nowMs, float, float) const noexcept {
        const float lvl = sample_level(nowMs);
        if (lvl <= OPACITY_EPSILON) return 0.0f;
        return has_hit_at(nowMs) ? 1.0f : lvl;
    }
};

MusicAnalysisScript* get_active_music_script() noexcept;

} // namespace stockcut
