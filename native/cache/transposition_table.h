// ============================================================================
// CoreEngine - Stockfish-Grade Universal Transposition Table (Cluster Architecture)
// ============================================================================
#pragma once
#include "core/types.h"
#include "core/data_structures.h"

namespace stockcut::cache {

constexpr u32 TT_FLAG_NONE   = 0u;
constexpr u32 TT_FLAG_VALID  = 1u << 0;
constexpr u32 TT_FLAG_DIRTY  = 1u << 1;
constexpr u32 TT_FLAG_LOCKED = 1u << 2;

// ----------------------------------------------------------------------------
// Universal Entry<PayloadT> (32 bytes when PayloadT <= 16 bytes)
// ----------------------------------------------------------------------------
template <typename PayloadT>
struct alignas(32) Entry {
    Key      key = 0ULL;
    u32      generation = 0u;
    u32      flags = TT_FLAG_NONE;
    PayloadT payload{};

    [[nodiscard]] constexpr bool is_match(Key searchKey) const noexcept {
        return key == searchKey && key != 0ULL;
    }

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return (flags & TT_FLAG_VALID) != 0u && key != 0ULL;
    }

    inline void clear() noexcept {
        key = 0ULL;
        generation = 0u;
        flags = TT_FLAG_NONE;
        payload = PayloadT{};
    }

    inline void invalidate() noexcept {
        key = 0ULL;
        generation = 0u;
        flags = TT_FLAG_NONE;
    }
};

// ----------------------------------------------------------------------------
// Specialized Payloads for Rendering & Presentation
// ----------------------------------------------------------------------------
struct GpuSurface {
    u32 fbo = 0;
    u32 tex = 0;
    u32 width = 0;
    u32 height = 0;

    [[nodiscard]] constexpr bool matches(u32 w, u32 h) const noexcept {
        return width == w && height == h && fbo != 0;
    }
};
static_assert(sizeof(GpuSurface) == 16, "GpuSurface must be exactly 16 bytes");
static_assert(sizeof(Entry<GpuSurface>) == 32, "Entry<GpuSurface> must be exactly 32 bytes");

struct DisplayListPayload {
    u32 cacheSlot = 0;
    u8  tier = 0;
    u8  pad[3]{};
};
static_assert(sizeof(DisplayListPayload) == 8, "DisplayListPayload must be 8 bytes");
static_assert(sizeof(Entry<DisplayListPayload>) == 32, "Entry<DisplayListPayload> must be 32 bytes");

// ----------------------------------------------------------------------------
// Universal TranspositionTable<PayloadT, ClusterCount, Ways>
// Stockfish 4-Way Set-Associative Cluster Cacheline Architecture
// ----------------------------------------------------------------------------
template <typename PayloadT, usize ClusterCount = 256, usize Ways = 4>
class alignas(64) TranspositionTable {
    static_assert((ClusterCount & (ClusterCount - 1)) == 0, "ClusterCount must be a power of 2");
public:
    using EntryType   = Entry<PayloadT>;
    using ClusterType = Cluster<EntryType, Ways>;
    static constexpr usize CLUSTERS = ClusterCount;
    static constexpr usize WAYS     = Ways;
    static constexpr usize CAPACITY = ClusterCount * Ways;

    constexpr TranspositionTable() noexcept = default;

    void clear() noexcept {
        for (ClusterType* c = clusters_.begin(); c < clusters_.end(); ++c) {
            for (EntryType* e = c->entries.begin(); e < c->entries.end(); ++e) {
                e->clear();
            }
        }
        currentGeneration_ = 1;
        hitCount_ = 0;
        missCount_ = 0;
    }

    void invalidate_entries() noexcept {
        for (ClusterType* c = clusters_.begin(); c < clusters_.end(); ++c) {
            for (EntryType* e = c->entries.begin(); e < c->entries.end(); ++e) {
                e->invalidate();
            }
        }
        currentGeneration_ = 1;
        hitCount_ = 0;
        missCount_ = 0;
    }

    [[nodiscard]] constexpr usize cluster_count() const noexcept { return ClusterCount; }
    [[nodiscard]] constexpr usize entry_count() const noexcept { return CAPACITY; }
    [[nodiscard]] constexpr usize capacity() const noexcept { return CAPACITY; }
    [[nodiscard]] u32 current_generation() const noexcept { return currentGeneration_; }
    inline void advance_generation() noexcept { ++currentGeneration_; }
    inline void reset_stats() noexcept { hitCount_ = 0; missCount_ = 0; }

    [[nodiscard]] u32 hits() const noexcept { return hitCount_; }
    [[nodiscard]] u32 misses() const noexcept { return missCount_; }
    [[nodiscard]] double hit_ratio() const noexcept {
        const u32 total = hitCount_ + missCount_;
        return (total > 0) ? (static_cast<double>(hitCount_) / static_cast<double>(total)) : 0.0;
    }

    [[nodiscard]] constexpr usize get_cluster_index(Key key) const noexcept {
        return static_cast<usize>(key & (ClusterCount - 1u));
    }

    // Direct O(1) probe by contract-guaranteed non-zero key (0 defensive if)
    [[nodiscard]] [[gnu::always_inline]] inline EntryType* probe_direct(Key nonZeroKey) noexcept {
        ClusterType& cluster = clusters_[get_cluster_index(nonZeroKey)];
        #pragma unroll
        for (usize i = 0; i < Ways; ++i) {
            if (cluster.entries[i].key == nonZeroKey) {
                cluster.entries[i].generation = currentGeneration_;
                ++hitCount_;
                return &cluster.entries[i];
            }
        }
        ++missCount_;
        return nullptr;
    }

    // Direct O(1) probe by key
    [[nodiscard]] EntryType* probe(Key key) noexcept {
        if (key == 0ULL) return nullptr;
        return probe_direct(key);
    }

    // Conditional O(1) probe with custom matching predicate
    template <typename MatchPredicate>
    [[nodiscard]] EntryType* probe_if(Key key, MatchPredicate&& pred) noexcept {
        if (key == 0ULL) return nullptr;
        ClusterType& cluster = clusters_[get_cluster_index(key)];
        #pragma unroll
        for (usize i = 0; i < Ways; ++i) {
            if (cluster.entries[i].is_match(key) && pred(cluster.entries[i].payload)) {
                cluster.entries[i].generation = currentGeneration_;
                ++hitCount_;
                return &cluster.entries[i];
            }
        }
        ++missCount_;
        return nullptr;
    }

    // Allocate / Replace Entry using single-pass Stockfish Aging LRU Policy
    EntryType* allocate(Key key) noexcept {
        ClusterType& cluster = clusters_[get_cluster_index(key)];
        usize bestIdx = 0;
        u32 oldestGen = 0xFFFFFFFFu;
        #pragma unroll
        for (usize i = 0; i < Ways; ++i) {
            const Key k = cluster.entries[i].key;
            if (k == 0ULL || k == key) {
                cluster.entries[i].key = key;
                cluster.entries[i].generation = currentGeneration_;
                cluster.entries[i].flags = TT_FLAG_VALID;
                return &cluster.entries[i];
            }
            const bool isOlder = (cluster.entries[i].generation < oldestGen);
            oldestGen = isOlder ? cluster.entries[i].generation : oldestGen;
            bestIdx   = isOlder ? i : bestIdx;
        }
        cluster.entries[bestIdx].key = key;
        cluster.entries[bestIdx].generation = currentGeneration_;
        cluster.entries[bestIdx].flags = TT_FLAG_VALID;
        return &cluster.entries[bestIdx];
    }

    // Contract: allocate() always returns a valid EntryType* (0 defensive null check)
    EntryType* write(Key key, const PayloadT& payload, u32 flags = TT_FLAG_VALID) noexcept {
        EntryType* slot = allocate(key);
        slot->payload = payload;
        slot->flags = flags;
        return slot;
    }

private:
    alignas(64) MultiArray<ClusterType, ClusterCount> clusters_{};
    u32 currentGeneration_ = 1;
    u32 hitCount_ = 0;
    u32 missCount_ = 0;
};

// ============================================================================
// SpacetimeTileMMU<TileCount, ClusterCount, Ways>: VideoOS Kernel Tile CoW MMU
// Reuses TranspositionTable<TilePagePayload> directly (ZERO duplicate TT code!)
// Maps 64 virtual tiles per frame -> Physical VRAM Tile Page IDs via Copy-on-Write.
// ============================================================================
struct TilePagePayload {
    u16 physPageId = 0xFFFFu;
    u16 refCount   = 0u;
    u32 tileIndex  = 0u;
    u32 lastFrame  = 0u;
    u32 flags      = 0u;
};
static_assert(sizeof(TilePagePayload) == 16, "TilePagePayload must be 16 bytes");
static_assert(sizeof(Entry<TilePagePayload>) == 32, "Entry<TilePagePayload> must be 32 bytes");

struct alignas(64) TilePageTable64 {
    MultiArray<u16, 64> physPageIds; // 128 bytes = 2 L1 Cachelines per frame!
    u64 cowSharedMask = 0ULL;        // Bit t == 1: Tile shared CoW from VRAM pool (0 render cost)
    u64 cowDirtyMask  = 0ULL;        // Bit t == 1: Fresh physical tile allocated (needs render)
    u32 sharedCount   = 0u;
    u32 allocatedCount = 0u;
};

template <u32 PoolPages = 1024u, usize ClusterCount = 256u, usize Ways = 4u>
struct alignas(64) SpacetimeTileMMU {
    TranspositionTable<TilePagePayload, ClusterCount, Ways> pageTT{};
    u32 nextPhysPage = 0u;
    u32 frameCounter = 0u;
    u64 totalCowHits = 0ULL;
    u64 totalCowAllocs = 0ULL;

    inline void clear() noexcept {
        pageTT.clear();
        nextPhysPage = 0u;
        frameCounter = 0u;
        totalCowHits = 0ULL;
        totalCowAllocs = 0ULL;
    }

    // Resolve all 64 tiles of a frame into CoW shared pages vs dirty allocated pages
    inline void resolve_frame_cow(
        const u64* __restrict tileZobrist64,
        TilePageTable64& outTable
    ) noexcept {
        ++frameCounter;
        pageTT.advance_generation();
        u64 sharedBB = 0ULL;
        u64 dirtyBB  = 0ULL;
        for (u32 t = 0u; t < 64u; ++t) {
            const Key tk = tileZobrist64[t] | 1ULL; // Contract: non-zero key for probe_direct
            auto* hit = pageTT.probe_direct(tk);
            if (hit != nullptr) {
                ++hit->payload.refCount;
                hit->payload.lastFrame = frameCounter;
                outTable.physPageIds[t] = hit->payload.physPageId;
                sharedBB |= (1ULL << t);
            } else {
                const u16 physId = static_cast<u16>(nextPhysPage++ & (PoolPages - 1u));
                pageTT.write(tk, TilePagePayload{physId, 1u, t, frameCounter, 1u});
                outTable.physPageIds[t] = physId;
                dirtyBB |= (1ULL << t);
            }
        }
        outTable.cowSharedMask  = sharedBB;
        outTable.cowDirtyMask   = dirtyBB;
        outTable.sharedCount    = BitOps<u64>::popcount64(sharedBB);
        outTable.allocatedCount = BitOps<u64>::popcount64(dirtyBB);
        totalCowHits   += outTable.sharedCount;
        totalCowAllocs += outTable.allocatedCount;
    }
};

// Global DisplayList Transposition Table (2048 clusters x 8 ways = 16384 entries, 512KB)
using DisplayListTT = TranspositionTable<DisplayListPayload, 2048, 8>;
DisplayListTT& get_global_display_list_tt() noexcept;

void tt_clear() noexcept;
void tt_next_generation() noexcept;
usize tt_memory_bytes() noexcept;

} // namespace stockcut::cache

namespace stockcut {

void tt_clear() noexcept;
void tt_next_generation() noexcept;
usize tt_memory_bytes() noexcept;

} // namespace stockcut
