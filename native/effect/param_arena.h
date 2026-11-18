// ============================================================================
// CoreEngine - Absolute Anonymous Effect Block Arena (Zero-Decoder Blob Loader)
// ----------------------------------------------------------------------------
// Upgraded for Anonymous 3-Tier Effect Kernel (V2.0):
// 1. EffectSlotHandle (24B -> 32B aligned): {word0, word1, algebraWord, pad}.
// 2. load_effect_block_from_blob(): Reads headerWord (+0), defaultBits0 (+8),
//    algebraWord (+16), and default floats directly from kCatalogBlob.
// 3. pack_block_ubo_std140(): Direct memcpy of Block.bits + Block.reg -> u_paramTape.
// 4. EffectParamArena<MaxInstances>: Stores Effect<EffectBlock> with algebraWord.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include "../core/types.h"
#include "../core/memory.h"
#include "../core/data_structures.h"
#include "../property/block.h"
#include "../property/media.h"
#include "../property/effect.h"
#include "catalog.h"
#include "algebra.h"

namespace stockcut {
namespace effect_catalog {

// ============================================================================
// 1. PURE 32-BYTE BINARY EFFECT HANDLE (1/2 L1 CACHELINE, ZERO ENUMS)
//   word0[0..31]  : blobRecOff (Byte offset in kCatalogBlob, 0 = built-in/unallocated)
//   word0[32..47] : reserved / u16 arena offset
//   word0[48..55] : floatCount (Aligned float count 4..88)
//   word0[56]     : active (1 = enabled, 0 = bypassed)
//   word0[57]     : dirtyUbo (1 = UBO needs upload)
//   word0[58..59] : tierShift (0..3)
//   word0[60..62] : execMode (0..4)
//   word1[0..15]  : shaderSlot (0..181 or 0xFFFF)
//   word1[16..31] : instanceSlot (0..MaxInstances-1)
//   word1[32..47] : uboByteOffset
//   word1[48..55] : paramCount (0..69)
//   algebraWord   : 64-bit Anonymous Fusion Signature (4-bit ABI | 7-bit Domain | Traits | Aux)
//   effectHash    : 32-bit FNV-1a canonical effect hash
// ============================================================================
struct alignas(32) EffectSlotHandle {
    u64 word0;
    u64 word1;
    u64 algebraWord;
    u32 effectHash;
    u32 pad;
};
static_assert(sizeof(EffectSlotHandle) == 32, "EffectSlotHandle must be exactly 32 bytes");

// ============================================================================
// 2. STANDALONE RAW BLOB-TO-BLOCK LOADER & STD140 UBO PACKER
// ============================================================================
template <typename BlockT>
inline u64 load_effect_block_from_blob(u32 recOff, BlockT& blk, u64* outAlgebraWord = nullptr) noexcept {
    if (recOff == 0u || recOff + 24u > kCatalogBlobSizeBytes) {
        if (outAlgebraWord) *outAlgebraWord = 0ULL;
        return 0ULL;
    }
    const u64 hdr          = blob_read_u64(recOff);
    const u64 defaultBits0 = blob_read_u64(recOff + 8u);
    const u64 algebraWord  = blob_read_u64(recOff + 16u);
    if (outAlgebraWord) *outAlgebraWord = algebraWord;

    const u32 floatCount   = static_cast<u32>(hdr & 0xFFULL);
    const u32 defFloatsRel = static_cast<u32>((hdr >> 48) & 0xFFFFULL);
    const u32 copyCount    = (floatCount <= BlockT::kFloatCount)
                             ? floatCount
                             : static_cast<u32>(BlockT::kFloatCount);

    blk.clear();
    blk.bits.words[0] = defaultBits0;
    if (copyCount > 0u) {
        std::memcpy(
            blk.reg.data(),
            kCatalogBlob + recOff + defFloatsRel,
            static_cast<usize>(copyCount) * sizeof(float)
        );
        blk.active.words[0] = (copyCount < 64u) ? ((1ULL << copyCount) - 1ULL) : ~0ULL;
        if constexpr (BlockT::kRegBlocks > 1) {
            if (copyCount > 64u) {
                const u32 rem = copyCount - 64u;
                blk.active.words[1] = (rem < 64u) ? ((1ULL << rem) - 1ULL) : ~0ULL;
            }
        }
    }
    return hdr;
}

template <typename BlockT>
inline u32 pack_block_ubo_std140(
    const BlockT& blk,
    u32 floatCount,
    float* outFloats,
    u32 maxOutFloats = 256u
) noexcept {
    const u32 copyFloats = min(
        min(floatCount, static_cast<u32>(BlockT::kFloatCount)),
        (maxOutFloats > 4u) ? (maxOutFloats - 4u) : 0u
    );
    const u32 totalFloats = 4u + copyFloats;

    const u64 w0 = blk.bits.words[0];
    const u64 w1 = (BlockT::kBitBlocks >= 2) ? blk.bits.words[1] : 0ULL;
    std::memcpy(&outFloats[0], &w0, sizeof(u64));
    std::memcpy(&outFloats[2], &w1, sizeof(u64));
    if (copyFloats > 0u) {
        std::memcpy(
            &outFloats[4],
            blk.reg.data(),
            static_cast<usize>(copyFloats) * sizeof(float)
        );
    }
    return totalFloats;
}

// ============================================================================
// 3. PURE BLOCK EFFECT ARENA (ZERO SPEC-DECODING WRAPPERS, FULL ALGEBRA WORD)
// ============================================================================
template <usize MaxInstances = 256>
struct alignas(64) EffectParamArena {
    static constexpr usize kMaxInstances = MaxInstances;
    static constexpr u16   kInvalidSlot  = 0xFFFFu;
    static constexpr usize kSlotMaskBlocks = (MaxInstances + 63) / 64;

    MultiArray<EffectSlotHandle, MaxInstances> handles;
    MultiArray<Effect<EffectBlock>, MaxInstances> effectInstances;
    Mask<kSlotMaskBlocks>                      activeSlots;
    u32 instanceCount = 0;

    inline void clear() noexcept {
        activeSlots.for_each_active([&](u32 i) noexcept {
            handles[i].word0 = 0ULL;
            handles[i].word1 = static_cast<u64>(i) << 16;
            handles[i].algebraWord = 0ULL;
            handles[i].effectHash = 0u;
            effectInstances[i].clear();
        });
        activeSlots.clear();
        instanceCount = 0;
    }

    [[nodiscard]] [[gnu::always_inline]] static inline constexpr u32 find_effect_blob_offset(
        const char* nameOrResourcePath
    ) noexcept {
        return blob_find_effect_offset_by_hash(fnv1a_lower(nameOrResourcePath));
    }

    [[nodiscard]] inline u16 instantiate_by_blob_offset(u32 recOff, u32 effectHash = 0u) noexcept {
        if (instanceCount >= MaxInstances) return kInvalidSlot;
        const u16 slot = static_cast<u16>(instanceCount);
        effectInstances[slot].clear();
        u64 algebraWord = 0ULL;
        const u64 hdr = load_effect_block_from_blob(recOff, effectInstances[slot], &algebraWord);
        if (hdr == 0ULL) return kInvalidSlot;

        ++instanceCount;
        activeSlots.set_bit(slot, true);
        const u64 floatCount = hdr & 0xFFULL;
        const u64 paramCount = (hdr >> 8) & 0xFFULL;
        const u64 tierShift  = (hdr >> 16) & 0x3ULL;
        const u64 execMode   = (hdr >> 18) & 0x7ULL;
        const u64 shaderSlot = (hdr >> 32) & 0xFFFFULL;

        auto& eff = effectInstances[slot];
        eff.algebraWord = algebraWord;
        eff.foldFn = nullptr;

        const u64 stateFlags = 1ULL | (1ULL << 1) | (tierShift << 2) | (execMode << 4);
        handles[slot].word0 = static_cast<u64>(recOff) |
                              (floatCount << 48) |
                              (stateFlags << 56);
        handles[slot].word1 = shaderSlot |
                              (static_cast<u64>(slot) << 16) |
                              (paramCount << 48);
        handles[slot].algebraWord = algebraWord;
        handles[slot].effectHash  = effectHash;
        handles[slot].pad         = 0u;
        return slot;
    }

    [[nodiscard]] inline u16 instantiate_by_ordinal(u32 ordinalIdx) noexcept {
        return instantiate_by_blob_offset(blob_effect_offset_by_ordinal(ordinalIdx), ordinalIdx + 1u);
    }

    [[nodiscard]] inline u16 instantiate_by_hash(u32 hash) noexcept {
        constexpr u32 kHashZoom       = fnv1a_lower("zoom");
        constexpr u32 kHashShake      = fnv1a_lower("shake");
        constexpr u32 kHashBrightness = fnv1a_lower("brightness");
        constexpr u32 kHashFlicker    = fnv1a_lower("flicker");

        const u32 recOff = blob_find_effect_offset_by_hash(hash);
        u16 slot = (recOff != 0u) ? instantiate_by_blob_offset(recOff, hash) : kInvalidSlot;
        const u64 nativeAlg = effect_algebra::resolve_algebra_word_by_hash(hash);

        if (slot == kInvalidSlot && nativeAlg != 0ULL) {
            if (instanceCount >= MaxInstances) return kInvalidSlot;
            slot = static_cast<u16>(instanceCount++);
            activeSlots.set_bit(slot, true);
            effectInstances[slot].clear();
            effectInstances[slot].algebraWord = nativeAlg;
            handles[slot].word0       = (16ULL << 48) | (1ULL << 56);
            handles[slot].word1       = static_cast<u64>(slot) << 16;
            handles[slot].algebraWord = nativeAlg;
            handles[slot].effectHash  = hash;
            handles[slot].pad         = 0u;
        }
        if (slot != kInvalidSlot) {
            auto& eff = effectInstances[slot];
            if (nativeAlg != 0ULL) {
                eff.algebraWord = nativeAlg;
                handles[slot].algebraWord = nativeAlg;
            }
            if (hash == kHashZoom) {
                eff.template set_value<0>(hash);
                eff.template set_value<1>(0xFFFFu);
                eff.template set_value<3>(1.0f);
                eff.template set_value<0>(true);
                eff.template set_value<1>(true);
                eff.foldFn = &fold_zoom<EffectBlock>;
            } else if (hash == kHashShake) {
                eff.template set_value<0>(hash);
                eff.template set_value<1>(0xFFFFu);
                eff.template set_value<3>(1.0f);
                eff.template set_value<0>(true);
                eff.template set_value<1>(true);
                eff.foldFn = &fold_shake<EffectBlock>;
            } else if (hash == kHashBrightness || hash == kHashFlicker) {
                eff.template set_value<0>(hash);
                eff.template set_value<1>(0xFFFFu);
                eff.template set_value<3>(1.0f);
                eff.template set_value<0>(true);
                eff.template set_value<1>(true);
                eff.foldFn = &fold_brightness<EffectBlock>;
            }
        }
        return slot;
    }

    [[nodiscard]] inline u16 instantiate_by_name(const char* nameOrPath) noexcept {
        return instantiate_by_hash(fnv1a_lower(nameOrPath));
    }

    inline bool reset_instance_defaults(u16 slot) noexcept {
        const u32 recOff = static_cast<u32>(handles[slot].word0 & 0xFFFFFFFFULL);
        u64 alg = 0ULL;
        load_effect_block_from_blob(recOff, effectInstances[slot], &alg);
        if (alg != 0ULL) {
            effectInstances[slot].algebraWord = alg;
            handles[slot].algebraWord = alg;
        }
        handles[slot].word0 |= (1ULL << 57);
        return true;
    }

    inline void mark_dirty(u16 slot) noexcept {
        handles[slot].word0 |= (1ULL << 57);
    }

    [[nodiscard]] [[gnu::always_inline]] inline u64 algebra_word(u16 slot) const noexcept {
        return handles[slot].algebraWord;
    }

    [[nodiscard]] [[gnu::always_inline]] inline EffectBlock& block(u16 slot) noexcept {
        return effectInstances[slot];
    }

    [[nodiscard]] [[gnu::always_inline]] inline const EffectBlock& block(u16 slot) const noexcept {
        return effectInstances[slot];
    }

    [[nodiscard]] [[gnu::always_inline]] inline Effect<EffectBlock>& effect(u16 slot) noexcept {
        return effectInstances[slot];
    }

    [[nodiscard]] [[gnu::always_inline]] inline const Effect<EffectBlock>& effect(u16 slot) const noexcept {
        return effectInstances[slot];
    }

    inline void fold_instance(u16 slot, FusedUberEffect& acc) const noexcept {
        effectInstances[slot].fold_into(acc);
    }

    inline u32 pack_ubo_std140(u16 slot, float* outFloats, u32 maxOutFloats = 256u) noexcept {
        const u32 floatCount = static_cast<u32>((handles[slot].word0 >> 48) & 0xFFULL);
        const u32 written = pack_block_ubo_std140(effectInstances[slot], floatCount, outFloats, maxOutFloats);
        handles[slot].word0 &= ~(1ULL << 57);
        return written;
    }
};

inline EffectParamArena<256>& global_effect_param_arena() noexcept {
    static EffectParamArena<256> s_arena;
    return s_arena;
}

} // namespace effect_catalog
} // namespace stockcut
