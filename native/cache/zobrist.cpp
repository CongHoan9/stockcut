// ============================================================================
// CoreEngine - Zobrist 64-bit Hashing Implementation
// ============================================================================
#include "zobrist.h"
#include "math_utils.h"
#include "data_structures.h"

namespace stockcut {
    template <usize ItemCapacity = 1024, usize SpinAngles = 3600>
    class ZobristEngineInternal
    {
    public:
        alignas(64) MultiArray<Key, ItemCapacity> itemsTable;
        alignas(64) MultiArray<Key, 4> tiersTable;
        alignas(64) MultiArray<Key, SpinAngles> spinTable;
        alignas(64) MultiArray<Key, MAX_TIMELINE_SLOTS> fxTable;
        alignas(64) MultiArray<Key, 256> colorTable;

        void init(u64 seed = HASH_GOLDEN_64)
        {
            u64 smState = seed;
            for (Key* k = itemsTable.begin(); k < itemsTable.end(); ++k) *k = splitmix64(smState);
            for (Key* k = tiersTable.begin(); k < tiersTable.end(); ++k) *k = splitmix64(smState);
            for (Key* k = spinTable.begin(); k < spinTable.end(); ++k)   *k = splitmix64(smState);
            for (Key* k = fxTable.begin(); k < fxTable.end(); ++k)       *k = splitmix64(smState);
            for (Key* k = colorTable.begin(); k < colorTable.end(); ++k) *k = splitmix64(smState);
        }

        template <usize UNROLL = 4>
        inline Key computeFrameHash(
            u32 W, u32 H, u32 styleEpoch, u32 fontsVer,
            u32 styleVer, u32 mediaVer, u32 bgHash,
            const Key *__restrict itemKeys,
            usize count) const noexcept
        {
            Key h = HASH_GOLDEN_64;
            h ^= static_cast<Key>(W) * ZOBRIST_MUL_A;
            h ^= static_cast<Key>(H) * ZOBRIST_MUL_B;
            h = hzobrist_mix_env_fields(h, styleEpoch, fontsVer, styleVer, mediaVer, bgHash);
            h ^= static_cast<Key>(count) * SPLITMIX_MUL2;
            usize i = 0;
            for (; i + UNROLL <= count; i += UNROLL)
            {
                #pragma unroll
                for (usize u = 0; u < UNROLL; ++u) {
                    const usize idx = i + u;
                    h ^= itemKeys[idx] ^ (static_cast<Key>(idx + 1) * ZOBRIST_MUL_A);
                }
            }
            for (; i < count; ++i) {
                h ^= itemKeys[i] ^ (static_cast<Key>(i + 1) * ZOBRIST_MUL_A);
            }
            return h == 0ULL ? 1ULL : h;
        }
    };

alignas(64) static ZobristEngineInternal<1024, 3600> g_zobrist;
alignas(64) static MultiArray<Key, ZOBRIST_ITEM_KEY_CAPACITY> g_itemKeys;

void zobrist_init(u64 seed) { g_zobrist.init(seed); }
Key* zobrist_get_item_keys_buf() { return g_itemKeys.data(); }
u32 zobrist_get_item_keys_capacity() { return ZOBRIST_ITEM_KEY_CAPACITY; }

Key zobrist_compute(u32 W, u32 H, u32 styleEpoch, u32 fontsVer, u32 styleVer, u32 mediaVer, u32 bgHash, u32 count) {
    count = min(count, ZOBRIST_ITEM_KEY_CAPACITY);
    return g_zobrist.computeFrameHash(W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash, g_itemKeys.data(), count);
}
Key zobrist_item_key(u32 idx) { return g_zobrist.itemsTable[idx & 1023u]; }
Key zobrist_tier_key(u32 tier) { return g_zobrist.tiersTable[tier & 3u]; }
Key zobrist_spin_key(u32 ang10) { return g_zobrist.spinTable[ang10 % 3600u]; }
Key zobrist_fx_key(u32 idx) { return g_zobrist.fxTable[idx & (MAX_TIMELINE_SLOTS - 1u)]; }
Key zobrist_color_key(u32 idx) { return g_zobrist.colorTable[idx & 255u]; }
} // namespace stockcut
