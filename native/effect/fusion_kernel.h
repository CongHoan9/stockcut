// ============================================================================
// native/effect/fusion_kernel.h
// ANONYMOUS 3-TIER EFFECT FUSION KERNEL (CPU FOLD -> EPOCH PARTITION -> ZOBRIST TT)
// ----------------------------------------------------------------------------
// Replaces legacy dispatch.h/.cpp and 10 standalone per-effect GLSL program files.
// - Tier 1 (CPU Algebraic Pre-Folding): Collapses consecutive Affine2D, AffineColor,
//   Gaussian variance, and Identity-Gate effects in O(N) before GPU sees them.
// - Tier 2 (Anonymous Single-Pass Epoch Partitioner): Driven strictly by 4-bit ABI
//   (ABI_UV | ABI_SAMPLE | ABI_COLOR | ABI_GEOM) in 64-bit algebraWord. Packs up to
//   256 effects into minimal FusedEpochPlans + 1 flat std140 u_paramTape UBO.
// - Tier 3 (Zobrist FusedPipelineTT + Pure GLSL Snippet Synthesizer + NoiseAtlas):
//   Concatenates pure GLSL ES 3.00 snippets into straight-line Mega-Shaders cached
//   by order-sensitive 64-bit Zobrist key. 0 per-effect glUniform1f calls.
// ============================================================================
#pragma once

#include "../core/types.h"
#include "../core/memory.h"
#include "../core/data_structures.h"
#include "../cache/transposition_table.h"
#include "algebra.h"
#include "monoid_operator.h"
#include "engine.h"

#if defined(__EMSCRIPTEN__)
#include <GLES3/gl3.h>
#endif

namespace stockcut {
namespace effect {

constexpr u32 FUSION_MAX_INPUT_EFFECTS  = 256u;
constexpr u32 FUSION_MAX_EPOCHS         = 32u;
constexpr u32 FUSION_MAX_STAGES         = 256u;
constexpr u32 FUSION_TAPE_MAX_VEC4      = 1024u; // 16 KB flat std140 tape (4096 floats)
constexpr u32 FUSION_EPOCH_MAX_VEC4     = 256u;  // 4 KB per-epoch UBO window (256 vec4)
constexpr u32 FUSION_GLSL_SCRATCH_BYTES = 32768u;
constexpr u32 FUSION_PIPELINE_TT_SETS   = 64u;
constexpr u32 FUSION_PIPELINE_TT_WAYS   = 2u;

// ============================================================================
// 1. ANONYMOUS EFFECT INVOCATION (64 BYTES = 1 L1 CACHELINE)
// ============================================================================
struct alignas(64) EffectInvocation {
    u64          algebraWord     = 0ULL;    // 0..7   (8B: Zero-Enum ABI/Domain/Trait/Aux)
    u32          effectHash      = 0u;      // 8..11  (4B: FNV-1a anonymous identity)
    u32          slot            = 0u;      // 12..15 (4B: Source timeline slot)
    float        timeSec         = 0.0f;    // 16..19 (4B)
    float        env             = 1.0f;    // 20..23 (4B: Envelope 0..1)
    float        p0              = 0.0f;    // 24..27 (4B: Intensity / angle / scale)
    float        p1              = 0.0f;    // 28..31 (4B: Speed / rot / duration)
    float        p2              = 0.0f;    // 32..35 (4B: Radius / brightness / param2)
    float        p3              = 0.0f;    // 36..39 (4B: Param3 / borderWidth)
    float        extra0          = 0.0f;    // 40..43 (4B: Shake dx / mirror flag / mode)
    float        extra1          = 0.0f;    // 44..47 (4B: Shake dy / extra)
    const float* blockRegs       = nullptr; // 48..51 or 48..55 (optional Arena Block ptr)
    u16          blockFloatCount = 0u;
    u16          flags           = 0u;
    u32          padTail[2]      = {};
};
static_assert(sizeof(EffectInvocation) == 64, "EffectInvocation must be exactly 64 bytes");

// ============================================================================
// 2. FUSED STAGE ENTRY (16 BYTES) & FUSED EPOCH PLAN (64 BYTES = 1 L1 CACHELINE)
// ============================================================================
struct alignas(16) FusedStageEntry {
    u8  kernelOrdinal;  // 0     (1..24 opaque pure snippet ordinal)
    u8  abiStage;       // 1     (0=UV, 1=SAMPLE, 2=COLOR, 3=GEOM)
    u16 tapeVec4Offset; // 2..3  (Relative vec4 offset inside this epoch's UBO)
    u16 tapeVec4Count;  // 4..5  (Number of vec4s occupied in u_tape[])
    u16 sourceSlot;     // 6..7  (Timeline slot)
    u32 effectHash;     // 8..11 (Anonymous hash)
    u32 flags;          // 12..15
};
static_assert(sizeof(FusedStageEntry) == 16, "FusedStageEntry must be exactly 16 bytes");

struct alignas(64) FusedEpochPlan {
    Key pipelineKey;       // 0..7   (8B: Order-sensitive Zobrist key for FusedPipelineTT)
    u64 tileMask64;        // 8..15  (8B: Exact 8x8 PullbackTileBitboard)
    u64 auxMask;           // 16..23 (8B: Required shared auxiliary textures)
    u16 stageBegin;        // 24..25
    u16 stageCount;        // 26..27
    u16 tapeVec4Begin;     // 28..29 (Start offset in FusedPlanBuffer::paramTape)
    u16 tapeVec4Count;     // 30..31 (Total vec4s to upload in 1 glBufferSubData)
    u16 fusedInputCount;   // 32..33 (Number of input effects collapsed into this epoch)
    u16 cpuFoldedCount;    // 34..35 (Number of effects collapsed purely on CPU)
    u8  inputFbo;          // 36
    u8  outputFbo;         // 37
    u8  hasSampleStage;    // 38
    u8  isGeomBarrier;     // 39
    u8  geomKernelOrdinal; // 40
    u8  isBounded;         // 41 (1 if tileMask64 != ~0ULL -> scissor eligible!)
    u16 aluWeightTotal;    // 42..43
    u32 width;             // 44..47
    u32 height;            // 48..51
    u32 geomSourceSlot;    // 52..55
    float geomTimeSec;     // 56..59
    float geomEnv;         // 60..63

    [[nodiscard]] constexpr u32 active_tile_count() const noexcept {
        return BitOps<u64>::popcount64(tileMask64);
    }
    [[nodiscard]] constexpr float pixel_reduction_ratio() const noexcept {
        return 1.0f - (static_cast<float>(active_tile_count()) * (1.0f / 64.0f));
    }
};
static_assert(sizeof(FusedEpochPlan) == 64, "FusedEpochPlan must be exactly 64 bytes");

// ============================================================================
// 3. ZERO-ALLOCATION FUSED PLAN BUFFER & COMPILER
// ============================================================================
struct alignas(64) FusedPlanBuffer {
    MultiArray<FusedEpochPlan, FUSION_MAX_EPOCHS>      epochs;
    MultiArray<FusedStageEntry, FUSION_MAX_STAGES>     stages;
    MultiArray<float, FUSION_TAPE_MAX_VEC4 * 4u>       paramTape;

    u32 epochCount       = 0u;
    u32 stageCount       = 0u;
    u32 tapeVec4Used     = 0u;
    u32 totalInputCount  = 0u;
    u32 totalCpuFolded   = 0u;
    u32 totalIdentitySkip= 0u;
    u32 totalFboSwitches = 0u;
    float foldedGaussianSigma = 0.0f;

    inline void reset() noexcept {
        epochCount        = 0u;
        stageCount        = 0u;
        tapeVec4Used      = 0u;
        totalInputCount   = 0u;
        totalCpuFolded    = 0u;
        totalIdentitySkip = 0u;
        totalFboSwitches  = 0u;
        foldedGaussianSigma = 0.0f;
    }

    // Compile arbitrary chain of anonymous EffectInvocations into minimal FusedEpochPlans
    void compile_chain(
        const EffectInvocation* inputs,
        u32 inputCount,
        u32 width,
        u32 height,
        const effect_algebra::TileSummary64& summary
    ) noexcept;

    // Estimated intermediate VRAM bandwidth (bytes/frame) vs naive ping-pong (1080p RGBA8 = 8.29 MB/pass R+W = 16.59 MB)
    [[nodiscard]] constexpr u64 estimate_vram_bytes(u32 width, u32 height) const noexcept {
        // Each epoch reads 1x (active tiles) + writes 1x (active tiles) = 8 bytes/pixel * active_ratio
        u64 total = 0ULL;
        const u64 fullFrameRW = static_cast<u64>(width) * static_cast<u64>(height) * 8ULL;
        for (u32 i = 0; i < epochCount; ++i) {
            const u64 tiles = static_cast<u64>(epochs[i].active_tile_count());
            total += (fullFrameRW * tiles) / 64ULL;
        }
        return total;
    }

    [[nodiscard]] static constexpr u64 estimate_naive_vram_bytes(
        u32 inputCount, u32 width, u32 height
    ) noexcept {
        return static_cast<u64>(inputCount) * static_cast<u64>(width) * static_cast<u64>(height) * 8ULL;
    }
};

// ============================================================================
// 4. TIER 3: ZOBRIST FUSED PIPELINE TT (REUSES cache::TranspositionTable!)
// Zero duplicate TT struct — 16B payload fits cleanly into 32B cache::Entry.
// ============================================================================
struct FusedPipelinePayload {
    u32 glProgram   = 0u;
    i16 locSrc      = -1;
    i16 locRes      = -1;
    i16 locNoise    = -1;
    u16 uboBlockIdx = 0xFFFFu;
    u32 flags       = 0u;
};
static_assert(sizeof(FusedPipelinePayload) == 16, "FusedPipelinePayload must be 16 bytes");
static_assert(sizeof(cache::Entry<FusedPipelinePayload>) == 32, "Entry<FusedPipelinePayload> must be 32 bytes");

using FusedPipelineEntry = cache::Entry<FusedPipelinePayload>;
using FusedPipelineTT    = cache::TranspositionTable<FusedPipelinePayload, FUSION_PIPELINE_TT_SETS, FUSION_PIPELINE_TT_WAYS>;

// Pure GLSL ES 3.00 Mega-Shader Source Synthesizer (Zero Heap Allocation, Host + WASM)
u32 synthesize_epoch_glsl(
    const FusedPlanBuffer& plan,
    const FusedEpochPlan& epoch,
    char* outBuf,
    u32 outCap
) noexcept;

// Order-sensitive 64-bit Zobrist hash for a stage sequence
[[nodiscard]] Key compute_stage_zobrist(u8 kernelOrdinal, u8 abiStage, u32 stagePos) noexcept;

// Global zero-allocation Fusion Runtime instances (Pha 5 Lock-Free RcuDualBank!)
RcuDualBank<FusedPlanBuffer>& global_fused_plan_rcu() noexcept;
FusedPlanBuffer& global_fused_plan_buffer() noexcept;
FusedPipelineTT& global_fused_pipeline_tt() noexcept;

// ============================================================================
// 5. WEBGL2 / GLES3 EXECUTION INTERFACE (ZERO LEGACY dispatch.cpp!)
// ============================================================================
#if defined(__EMSCRIPTEN__)
bool fusion_kernel_init() noexcept;
void fusion_register_static_texture(u32 slot, u32 texId, u32 width, u32 height) noexcept;
void fusion_set_current_rain_params(const RainDropParams* params) noexcept;

// Execute a single FusedEpochPlan on WebGL2 (1 UBO upload + 1 draw setup)
void fusion_execute_epoch(
    const FusedPlanBuffer& plan,
    const FusedEpochPlan& epoch,
    GLuint inTex
) noexcept;

// Direct single-effect convenience bridge (compiles 1-element FusedEpochPlan & executes via Zobrist TT)
void fusion_execute_single_invocation(
    const EffectInvocation& inv,
    GLuint inTex,
    u32 width,
    u32 height
) noexcept;
#endif

} // namespace effect
} // namespace stockcut
