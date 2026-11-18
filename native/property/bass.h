// ============================================================================
// native/property/bass.h
// BASS-REACTIVE AUDIO PARAMETERS & HIT ARENA
// ----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <cstring>
#include "../core/types.h"
#include "../core/data_structures.h"

namespace stockcut {

constexpr u32 MAX_BASS_COLORS = 16;
constexpr u32 MAX_BASS_HITS = 8192;

struct alignas(16) BassBeatParams {
    float hop;                       // Hop duration (s), default 0.05f
    u32 hitCount;                    // Sample count in hit array (0..MAX_BASS_HITS)
    u32 colorCount;                  // Theme color count (0..MAX_BASS_COLORS)
    u32 isBassActive;                // 1 if slot is bass-reactive
    MultiArray<u32, MAX_BASS_COLORS> colorsBg;   // RGBA packed 0xAABBGGRR
    MultiArray<u32, MAX_BASS_COLORS> colorsText; // RGBA packed 0xAABBGGRR

    inline void clear() noexcept {
        hop = DEFAULT_BASS_HOP_F32;
        hitCount = 0;
        colorCount = 0;
        isBassActive = 0;
        colorsBg.fill(0u);
        colorsText.fill(0u);
    }
};

extern MultiArray<u16, MAX_TIMELINE_SLOTS, MAX_BASS_HITS> g_bassHits;

} // namespace stockcut
