// ============================================================================
// CoreEngine - Bijective 128-bit Canonical Slot State & Galois XOR Zobrist
// ============================================================================
#pragma once
#include "types.h"
#include "hash_primitives.h"
#include "data_structures.h"
#include <cmath>

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

namespace stockcut
{
    constexpr u32 ZOBRIST_ITEM_KEY_CAPACITY = 512;

    void zobrist_init(u64 seed);
    Key *zobrist_get_item_keys_buf();
    u32 zobrist_get_item_keys_capacity();
    Key zobrist_compute(u32 W, u32 H, u32 styleEpoch, u32 fontsVer, u32 styleVer, u32 mediaVer, u32 bgHash, u32 count);
    Key zobrist_item_key(u32 idx);
    Key zobrist_tier_key(u32 tier);
    Key zobrist_spin_key(u32 ang10);
    Key zobrist_fx_key(u32 idx);
    Key zobrist_color_key(u32 idx);

    [[nodiscard]] inline i32 hzobrist_quantize_midtread(float val, float scale) noexcept
    {
        return static_cast<i32>(__builtin_floorf(val * scale + 0.5f));
    }

    [[nodiscard]] inline u16 hzobrist_quantize_u16(float val, float scale) noexcept
    {
        return static_cast<u16>(static_cast<u32>(hzobrist_quantize_midtread(val, scale)) & INVALID_U16);
    }

    enum class HZobristChannel : u32
    {
        Identity = 0,
        GeomXY   = 1,
        GeomWH   = 2,
        Rotate   = 3,
        Opacity  = 4,
        Resource = 5,
        Reveal   = 6,
        Effect   = 7,
        ChannelNb = 8
    };

    [[nodiscard]] constexpr Key hzobrist_mix64(u64 x) noexcept
    {
        return splitmix64_mix(x);
    }

    template <HZobristChannel Ch>
    [[nodiscard]] constexpr Key hzobrist_channel_token_T(u32 slot, u64 payload) noexcept
    {
        constexpr MultiArray<u64, static_cast<usize>(HZobristChannel::ChannelNb)> kChannelPrimes = {{
            HASH_GOLDEN_64,
            ZOBRIST_MUL_A,
            ZOBRIST_MUL_B,
            ZOBRIST_MUL_C,
            SPLITMIX_MUL1,
            SPLITMIX_MUL2,
            ZOBRIST_MUL_D,
            ZOBRIST_MUL_E
        }};
        const u64 domainSeed = kChannelPrimes[static_cast<usize>(Ch)] ^
                               (static_cast<u64>(slot + 1u) * ZOBRIST_MUL_C);
        return hzobrist_mix64(payload ^ domainSeed);
    }

    [[nodiscard]] constexpr Key hzobrist_bijective128_to_64(u32 slot, u64 lo, u64 hi) noexcept
    {
        const u64 slotSeed = static_cast<u64>(slot + 1u) * HASH_GOLDEN_64;
        const u64 L = hzobrist_mix64(lo ^ slotSeed);
        const u64 R = hzobrist_mix64(hi ^ (slotSeed * ZOBRIST_MUL_A));
        return hzobrist_mix64(L ^ ((R << 23) | (R >> 41)));
    }

    [[nodiscard]] inline Key hzobrist_identity(u32 slot, u16 type, i32 zIndex, u8 tier, u8 flags) noexcept
    {
        const u64 packed = (static_cast<u64>(type) << 48) |
                           (static_cast<u64>(static_cast<u32>(zIndex)) << 16) |
                           (static_cast<u64>(tier) << 8) |
                           static_cast<u64>(flags);
        return hzobrist_channel_token_T<HZobristChannel::Identity>(slot, packed);
    }

    [[nodiscard]] inline u64 hzobrist_pack_geom64(float x0, float y0, float x1, float y1) noexcept
    {
#if defined(__wasm_simd128__)
        const v128_t geom = wasm_f32x4_make(x0, y0, x1, y1);
        const v128_t scaled = wasm_f32x4_add(wasm_f32x4_mul(geom, wasm_f32x4_splat(8.0f)), wasm_f32x4_splat(0.5f));
        const v128_t floored = wasm_f32x4_floor(scaled);
        const v128_t i32s = wasm_i32x4_trunc_sat_f32x4(floored);
        const u64 q0 = static_cast<u64>(static_cast<u16>(wasm_i32x4_extract_lane(i32s, 0)));
        const u64 q1 = static_cast<u64>(static_cast<u16>(wasm_i32x4_extract_lane(i32s, 1)));
        const u64 q2 = static_cast<u64>(static_cast<u16>(wasm_i32x4_extract_lane(i32s, 2)));
        const u64 q3 = static_cast<u64>(static_cast<u16>(wasm_i32x4_extract_lane(i32s, 3)));
        return q0 | (q1 << 16) | (q2 << 32) | (q3 << 48);
#else
        const u64 qx0 = static_cast<u64>(hzobrist_quantize_u16(x0, 8.0f));
        const u64 qy0 = static_cast<u64>(hzobrist_quantize_u16(y0, 8.0f));
        const u64 qx1 = static_cast<u64>(hzobrist_quantize_u16(x1, 8.0f));
        const u64 qy1 = static_cast<u64>(hzobrist_quantize_u16(y1, 8.0f));
        return qx0 | (qy0 << 16) | (qx1 << 32) | (qy1 << 48);
#endif
    }

    [[nodiscard]] inline Key hzobrist_geom(u32 slot, float x0, float y0, float x1, float y1) noexcept
    {
        return hzobrist_channel_token_T<HZobristChannel::GeomXY>(slot, hzobrist_pack_geom64(x0, y0, x1, y1));
    }

    [[nodiscard]] inline u16 hzobrist_pack_rot16(float rotateDeg) noexcept
    {
        const float q = rotateDeg * (1.0f / 360.0f);
        const float norm = rotateDeg - __builtin_floorf(q) * 360.0f;
        return static_cast<u16>(static_cast<u32>(hzobrist_quantize_midtread(norm, 65536.0f / 360.0f)) & INVALID_U16);
    }

    [[nodiscard]] inline Key hzobrist_rot(u32 slot, float rotateDeg) noexcept
    {
        return hzobrist_channel_token_T<HZobristChannel::Rotate>(slot, static_cast<u64>(hzobrist_pack_rot16(rotateDeg)));
    }

    [[nodiscard]] inline u16 hzobrist_pack_opacity16(float opacity) noexcept
    {
        const float clamped = clamp(opacity, 0.0f, 1.0f);
        return static_cast<u16>(hzobrist_quantize_midtread(clamped, 4096.0f) & 0x7FFFu);
    }

    [[nodiscard]] inline Key hzobrist_opacity(u32 slot, float opacity) noexcept
    {
        return hzobrist_channel_token_T<HZobristChannel::Opacity>(slot, static_cast<u64>(hzobrist_pack_opacity16(opacity)));
    }

    [[nodiscard]] inline Key hzobrist_resource(u32 slot, u32 resourceId, u32 extraColor = 0) noexcept
    {
        const u64 packed = (static_cast<u64>(extraColor) << 32) | static_cast<u64>(resourceId);
        return hzobrist_channel_token_T<HZobristChannel::Resource>(slot, packed);
    }

    [[nodiscard]] inline Key hzobrist_reveal(u32 slot, u16 progressPermille, u32 filterPacked) noexcept
    {
        const u64 packed = (static_cast<u64>(progressPermille) << 32) | static_cast<u64>(filterPacked);
        return hzobrist_channel_token_T<HZobristChannel::Reveal>(slot, packed);
    }

    [[nodiscard]] inline Key hzobrist_effect(u32 slot, i64 startMs, i64 endMs, u32 fxId, i32 zIndex) noexcept
    {
        const u64 p0 = (static_cast<u64>(fxId) << 32) ^ static_cast<u64>(static_cast<u32>(zIndex));
        const u64 p1 = (static_cast<u64>(startMs) * ZOBRIST_MUL_A) ^ static_cast<u64>(endMs);
        return hzobrist_channel_token_T<HZobristChannel::Effect>(slot, p0 ^ p1);
    }

    [[nodiscard]] inline Key hzobrist_viewport(u32 W, u32 H, u32 passCount = 0) noexcept
    {
        const u64 packed = (static_cast<u64>(W) << 32) ^ (static_cast<u64>(H) << 16) ^ static_cast<u64>(passCount);
        return hzobrist_mix64(packed ^ ZOBRIST_MUL_A);
    }

    // Unified Viewport + Environment Zobrist Mixer (shared by zobrist_compute & Position)
    [[nodiscard]] constexpr Key hzobrist_mix_env_fields(
        Key seed, u32 styleEpoch, u32 fontsVer, u32 styleVer, u32 mediaVer, u32 bgHash) noexcept
    {
        Key h = seed;
        h ^= static_cast<Key>(styleEpoch) * HASH_GOLDEN_64;
        h ^= static_cast<Key>(fontsVer)   * static_cast<Key>(MURMUR3_32_M1);
        h ^= static_cast<Key>(styleVer)   * static_cast<Key>(MURMUR3_32_M2);
        h ^= static_cast<Key>(mediaVer)   * static_cast<Key>(XXHASH_32_P3);
        h ^= static_cast<Key>(bgHash)     * static_cast<Key>(XXHASH_32_P5);
        return h;
    }

    [[nodiscard]] inline Key hzobrist_viewport_env(
        u32 W, u32 H, u32 styleEpoch, u32 fontsVer, u32 styleVer, u32 mediaVer, u32 bgHash) noexcept
    {
        const Key base = (static_cast<Key>(W) << 32) ^ static_cast<Key>(H);
        return hzobrist_mix64(hzobrist_mix_env_fields(base, styleEpoch, fontsVer, styleVer, mediaVer, bgHash));
    }

} // namespace stockcut

namespace stockcut::cache {

    struct ViewportEnv {
        u32 width = 0;
        u32 height = 0;
        u32 styleEpoch = 0;
        u32 fontsVer = 0;
        u32 styleVer = 0;
        u32 mediaVer = 0;
        u32 bgHash = 0;
    };

    template <typename T>
    struct Zobrist;

    template <>
    struct Zobrist<ViewportEnv> {
        [[nodiscard]] static inline Key token(const ViewportEnv& env) noexcept {
            return hzobrist_viewport_env(env.width, env.height, env.styleEpoch, env.fontsVer, env.styleVer, env.mediaVer, env.bgHash);
        }
    };

} // namespace stockcut::cache
