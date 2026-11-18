// ============================================================================
// CoreEngine - Music & Audio Analysis Script Implementation
// ============================================================================
#include "script.h"

namespace stockcut {
namespace {
alignas(64) MusicAnalysisScript g_musicScript{};
} // namespace

MusicAnalysisScript* get_active_music_script() noexcept {
    return &g_musicScript;
}

void MusicAnalysisScript::reset() noexcept {
    durationMs   = 0.0;
    hopSec       = DEFAULT_MUSIC_HOP_SEC;
    totalSamples = 0;
    totalHits    = 0;
    chunkCount   = 0;
    levels.clear();
    hitIndices.clear();
    chunks.clear();
}

bool MusicAnalysisScript::bake(
    const float* inLevels,
    const u16* inHits,
    u32 sampleCount,
    double inHopSec,
    double inDurationMs
) noexcept {
    if (!inLevels || sampleCount == 0 || inHopSec <= MIN_MUSIC_HOP_SEC) {
        reset();
        return false;
    }
    const u32 count = min(sampleCount, MAX_SCRIPT_SAMPLES);
    totalSamples = count;
    hopSec       = inHopSec;
    durationMs   = inDurationMs > 0.0 ? inDurationMs : (count * inHopSec * MUSIC_MS_PER_SEC);

    for (u32 i = 0; i < count; ++i) {
        levels[i] = clamp(inLevels[i], 0.0f, 1.0f);
        hitIndices[i] = inHits ? inHits[i] : 0u;
    }
    totalHits = (count > 0 && inHits) ? hitIndices[count - 1] : 0u;

    const u32 neededChunks = min(
        MAX_MUSIC_CHUNKS,
        static_cast<u32>((durationMs + MUSIC_CHUNK_SIZE_MS - 1) / MUSIC_CHUNK_SIZE_MS)
    );
    chunkCount = neededChunks;

    for (u32 c = 0; c < neededChunks; ++c) {
        MusicChunk& chunk = chunks[c];
        const double chunkStartMs = static_cast<double>(c * MUSIC_CHUNK_SIZE_MS);
        const double chunkEndMs   = chunkStartMs + static_cast<double>(MUSIC_CHUNK_SIZE_MS);
        const u32 sampleStart = min(count - 1u, static_cast<u32>((chunkStartMs / MUSIC_MS_PER_SEC) / hopSec));
        const u32 sampleEnd   = min(count - 1u, static_cast<u32>((chunkEndMs / MUSIC_MS_PER_SEC) / hopSec));
        chunk.sampleOffset    = sampleStart;
        chunk.accumulatedHits = (sampleStart < count) ? hitIndices[sampleStart] : static_cast<u16>(totalHits);

        float maxVal = 0.0f;
        for (u32 s = sampleStart; s <= sampleEnd; ++s) {
            if (levels[s] > maxVal) maxVal = levels[s];
        }
        chunk.peakEnergyU8 = static_cast<u8>(clamp(maxVal * 255.0f, 0.0f, 255.0f));
        chunk.hasBass      = (chunk.peakEnergyU8 > MUSIC_BASS_ENERGY_MIN_U8) ? 1u : 0u;

        u64 bitboard = 0ULL;
        if (inHits && count > 0u) {
            u16 prevHit = (sampleStart > 0u) ? hitIndices[sampleStart - 1u] : 0u;
            for (u32 bit = 0; bit < MUSIC_BITS_PER_CHUNK; ++bit) {
                const double sliceTimeMs = chunkStartMs + static_cast<double>(bit * MUSIC_BIT_RESOLUTION_MS);
                const u32 s = min(count - 1u, static_cast<u32>((sliceTimeMs / MUSIC_MS_PER_SEC) / hopSec));
                const u16 currHit = hitIndices[s];
                if (currHit > prevHit) {
                    bitboard |= (1ULL << bit);
                    prevHit = currHit;
                }
            }
        }
        chunk.hitBitboard = bitboard;
    }
    return true;
}

} // namespace stockcut

extern "C" {

WASM_EXPORT(wasm_music_script_bake)
stockcut::u32 wasm_music_script_bake(
    const float* levels,
    const stockcut::u16* hits,
    stockcut::u32 sampleCount,
    double hopSec,
    double durationMs
) {
    auto* script = stockcut::get_active_music_script();
    return (script && script->bake(levels, hits, sampleCount, hopSec, durationMs)) ? 1u : 0u;
}

WASM_EXPORT(wasm_music_script_get_levels_ptr)
float* wasm_music_script_get_levels_ptr() {
    auto* script = stockcut::get_active_music_script();
    return script ? script->levels.data() : nullptr;
}

WASM_EXPORT(wasm_music_script_get_hits_ptr)
stockcut::u16* wasm_music_script_get_hits_ptr() {
    auto* script = stockcut::get_active_music_script();
    return script ? script->hitIndices.data() : nullptr;
}

WASM_EXPORT(wasm_music_script_get_capacity)
stockcut::u32 wasm_music_script_get_capacity() {
    return stockcut::MAX_SCRIPT_SAMPLES;
}

WASM_EXPORT(wasm_music_script_bake_inplace)
stockcut::u32 wasm_music_script_bake_inplace(
    stockcut::u32 sampleCount,
    double hopSec,
    double durationMs
) {
    auto* script = stockcut::get_active_music_script();
    return (script && script->bake(script->levels.data(), script->hitIndices.data(), sampleCount, hopSec, durationMs)) ? 1u : 0u;
}

WASM_EXPORT(wasm_music_script_reset)
void wasm_music_script_reset() {
    auto* script = stockcut::get_active_music_script();
    if (script) script->reset();
}

WASM_EXPORT(wasm_music_script_sample_level)
float wasm_music_script_sample_level(double nowMs) {
    const auto* script = stockcut::get_active_music_script();
    return script ? script->sample_level(nowMs) : 0.0f;
}

WASM_EXPORT(wasm_music_script_sample_hit)
stockcut::u32 wasm_music_script_sample_hit(double nowMs) {
    const auto* script = stockcut::get_active_music_script();
    return script ? script->sample_hit_index(nowMs) : 0u;
}

WASM_EXPORT(wasm_music_script_has_hit)
stockcut::u32 wasm_music_script_has_hit(double nowMs) {
    const auto* script = stockcut::get_active_music_script();
    return (script && script->has_hit_at(nowMs)) ? 1u : 0u;
}

WASM_EXPORT(wasm_music_script_get_chunk_count)
stockcut::u32 wasm_music_script_get_chunk_count() {
    const auto* script = stockcut::get_active_music_script();
    return script ? script->chunkCount : 0u;
}

} // extern "C"
