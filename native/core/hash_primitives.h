// ============================================================================
// CoreEngine - Single-Source Hash, PRNG & Checksum Primitives
// ============================================================================
#pragma once
#include "types.h"
#include <cstring>

namespace stockcut
{
    // ------------------------------------------------------------------------
    // 1. Canonical 64-bit & 32-bit Hash Constants (Declared Once)
    // ------------------------------------------------------------------------
    constexpr u64 HASH_GOLDEN_64   = 0x9e3779b97f4a7c15ULL;
    constexpr u64 SPLITMIX_MUL1    = 0xbf58476d1ce4e5b9ULL;
    constexpr u64 SPLITMIX_MUL2    = 0x94d049bb133111ebULL;
    constexpr u64 ZOBRIST_MUL_A    = 0x517cc1b727220a95ULL;
    constexpr u64 ZOBRIST_MUL_B    = 0x6c62272e07bb0142ULL;
    constexpr u64 ZOBRIST_MUL_C    = 0xd6e8feb86659fd93ULL;
    constexpr u64 ZOBRIST_MUL_D    = 0x2545f4914f6cdd1dULL;
    constexpr u64 ZOBRIST_MUL_E    = 0x85ebca6b27d4eb2fULL;
    constexpr u64 MURMUR64_MUL1    = 0xff51afd7ed558ccdULL;

    constexpr u64 FNV1A_64_OFFSET  = 0xcbf29ce484222325ULL; // 14695981039346656037ULL
    constexpr u64 FNV1A_64_PRIME   = 0x100000001b3ULL;      // 1099511628211ULL
    constexpr u32 FNV1A_32_OFFSET  = 0x811c9dc5u;           // 2166136261u
    constexpr u32 FNV1A_32_PRIME   = 0x01000193u;           // 16777619u

    constexpr u32 MURMUR3_32_C0    = 0x9e3779b9u;
    constexpr u32 MURMUR3_32_M1    = 0x85ebca6bu;
    constexpr u32 MURMUR3_32_M2    = 0xc2b2ae35u;
    constexpr u32 XXHASH_32_P3     = 0x27d4eb2fu;
    constexpr u32 XXHASH_32_P5     = 0x165667b1u;

    // ------------------------------------------------------------------------
    // 2. Bijective SplitMix64 Mixer, Boost+SplitMix64 Combiner & PRNG
    // ------------------------------------------------------------------------
    [[nodiscard]] constexpr u64 splitmix64_mix(u64 z) noexcept
    {
        z = (z ^ (z >> 30)) * SPLITMIX_MUL1;
        z = (z ^ (z >> 27)) * SPLITMIX_MUL2;
        return z ^ (z >> 31);
    }

    [[nodiscard]] constexpr u64 hash_mix64(u64 h, u64 v) noexcept
    {
        h ^= v + HASH_GOLDEN_64 + (h << 6) + (h >> 2);
        return splitmix64_mix(h);
    }

    [[nodiscard]] constexpr u64 splitmix64_next(u64& state) noexcept
    {
        state += HASH_GOLDEN_64;
        return splitmix64_mix(state);
    }

    // ------------------------------------------------------------------------
    // 3. MurmurHash3 32-bit Finalizer
    // ------------------------------------------------------------------------
    [[nodiscard]] constexpr u32 murmur3_fmix32(u32 h) noexcept
    {
        h ^= h >> 16;
        h *= MURMUR3_32_M1;
        h ^= h >> 13;
        h *= MURMUR3_32_M2;
        return h ^ (h >> 16);
    }

    // ------------------------------------------------------------------------
    // 4. Unified Base-31 Resource & Font ID Hash (ABI-synced with JS bridge)
    // ------------------------------------------------------------------------
    [[nodiscard]] constexpr u32 hash31_str(const char* str) noexcept
    {
        if (!str || !*str) return 0u;
        u32 hash = 0u;
        for (const char* p = str; *p; ++p) {
            hash = (hash << 5) - hash + static_cast<u8>(*p);
        }
        return hash;
    }

    [[nodiscard]] constexpr u32 hash31_bytes(const char* data, usize len) noexcept
    {
        if (!data || !len) return 0u;
        u32 hash = 0u;
        for (const char* p = data, *const end = data + len; p < end; ++p) {
            hash = (hash << 5) - hash + static_cast<u8>(*p);
        }
        return hash;
    }

    // ------------------------------------------------------------------------
    // 5. Unified FNV-1a 32-bit & 64-bit
    // ------------------------------------------------------------------------
    [[nodiscard]] constexpr u32 fnv1a_32_step(u32 hash, u8 byte) noexcept
    {
        return (hash ^ static_cast<u32>(byte)) * FNV1A_32_PRIME;
    }

    [[nodiscard]] inline u32 fnv1a_32_u64(u32 hash, u64 value) noexcept
    {
        for (u32 byte = 0; byte < 8; ++byte) {
            hash = fnv1a_32_step(hash, static_cast<u8>(value & 0xFFu));
            value >>= 8;
        }
        return hash;
    }

    [[nodiscard]] inline u32 fnv1a_32_bytes(const void* data, usize len, u32 seed = FNV1A_32_OFFSET) noexcept
    {
        const u8* p = static_cast<const u8*>(data);
        u32 h = seed;
        for (const u8* const end = p + len; p < end; ++p) {
            h = fnv1a_32_step(h, *p);
        }
        return h;
    }

    [[nodiscard]] constexpr u32 fnv1a_32_str(const char* str, u32 seed = FNV1A_32_OFFSET) noexcept
    {
        u32 h = seed;
        if (str) {
            for (const char* p = str; *p; ++p) {
                h = fnv1a_32_step(h, static_cast<u8>(*p));
            }
        }
        return h;
    }

    [[nodiscard]] constexpr u64 fnv1a_64_step(u64 hash, u8 byte) noexcept
    {
        return (hash ^ static_cast<u64>(byte)) * FNV1A_64_PRIME;
    }

    [[nodiscard]] constexpr u64 fnv1a_64_u64(u64 hash, u64 value) noexcept
    {
        for (u32 byte = 0; byte < 8; ++byte) {
            hash = fnv1a_64_step(hash, static_cast<u8>(value & 0xFFu));
            value >>= 8;
        }
        return hash;
    }

    [[nodiscard]] inline u64 fnv1a_64_bytes(const void* data, usize len, u64 seed = FNV1A_64_OFFSET) noexcept
    {
        const u8* p = static_cast<const u8*>(data);
        u64 h = seed;
        for (const u8* const end = p + len; p < end; ++p) {
            h = fnv1a_64_step(h, *p);
        }
        return h;
    }

    [[nodiscard]] constexpr u64 fnv1a_64_str(const char* str, u64 seed = FNV1A_64_OFFSET) noexcept
    {
        u64 h = seed;
        if (str) {
            for (const char* p = str; *p; ++p) {
                h = fnv1a_64_step(h, static_cast<u8>(*p));
            }
        }
        return h;
    }

    // ------------------------------------------------------------------------
    // 6. Unified 64-bit Cacheline Struct Word Hasher (Style & Filter Arenas)
    // ------------------------------------------------------------------------
    template <typename T>
    [[nodiscard]] inline u64 hash_struct_words64(const T& obj) noexcept
    {
        static_assert((sizeof(T) % sizeof(u64)) == 0, "Struct size must be a multiple of 8 bytes");
        constexpr usize WORD_COUNT = sizeof(T) / sizeof(u64);
        const u64* p = reinterpret_cast<const u64*>(&obj);
        u64 h = HASH_GOLDEN_64;
        for (usize i = 0; i < WORD_COUNT; ++i) {
            h ^= p[i] * ZOBRIST_MUL_A;
            h = (h << 31) | (h >> 33);
        }
        return h == 0ULL ? 1ULL : h;
    }

    // ------------------------------------------------------------------------
    // 7. Unified IEEE 802.3 CRC32 (Zero-Allocation Compile-Time Table)
    // ------------------------------------------------------------------------
    namespace detail
    {
        struct Crc32Table
        {
            u32 entries[256]{};
            constexpr Crc32Table() noexcept
            {
                for (u32 i = 0; i < 256; ++i) {
                    u32 c = i;
                    for (u32 k = 0; k < 8; ++k) {
                        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                    }
                    entries[i] = c;
                }
            }
        };
        inline constexpr Crc32Table kCrc32Table{};
    } // namespace detail

    [[nodiscard]] inline u32 crc32_update(u32 crc, const void* data, usize len) noexcept
    {
        const u8* p = static_cast<const u8*>(data);
        u32 state = crc ^ INVALID_U32;
        for (const u8* const end = p + len; p < end; ++p) {
            state = detail::kCrc32Table.entries[(state ^ *p) & 0xFFu] ^ (state >> 8);
        }
        return state ^ INVALID_U32;
    }

    [[nodiscard]] inline u32 crc32_compute(const void* data, usize len) noexcept
    {
        return crc32_update(0u, data, len);
    }
} // namespace stockcut
