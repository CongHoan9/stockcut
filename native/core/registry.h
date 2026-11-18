#pragma once
// ============================================================================
// STOCKCUT C++ RESOURCE REGISTRY (CENTRAL ASSET HUB & ZERO-BULK-LOAD ENGINE)
// ============================================================================
#include "types.h"
#include "data_structures.h"
#include "stream.h"

namespace stockcut {

constexpr u32   MAX_REGISTRY_RESOURCES     = 512u;
constexpr u32   MAX_RESOURCE_EVENTS        = 64u;
constexpr usize RESOURCE_CLUSTERS_NB       = 256u;
constexpr usize RESOURCE_CLUSTER_WAYS      = 4u;
constexpr u64   DEFAULT_RESOURCE_BUDGET_B  = 256ULL * 1024ULL * 1024ULL;
constexpr u32   RES_WASM_INPUT_CAPACITY    = 2048u;

enum class RegistryMediaType : u32 {
    Video = 0,
    Image = 1,
    Audio = 2,
    Font  = 3,
    Count = 4
};

enum ResourceEventType : u32 {
    RES_EVENT_ADD    = 1u,
    RES_EVENT_REMOVE = 2u,
    RES_EVENT_UPDATE = 3u,
    RES_EVENT_EVICT  = 4u
};

constexpr u16 RES_FLAG_NONE          = 0u;
constexpr u16 RES_FLAG_IS_ASSET      = 1u << 0;
constexpr u16 RES_FLAG_IS_PROJECT    = 1u << 1;
constexpr u16 RES_FLAG_MEMORY_LOADED = 1u << 2;
constexpr u16 RES_FLAG_STREAMABLE    = 1u << 3;

struct alignas(16) RegistryEvent {
    u32 eventType;
    u32 resourceId;
    u32 mediaType;
    u32 pad;
};
static_assert(sizeof(RegistryEvent) == 16, "RegistryEvent must be 16 bytes");

struct alignas(64) ResourceEntry {
    u32 resourceId;
    u32 mediaType;
    u64 srcHash;
    u64 fileSize;
    u32 durationMs;
    u32 byteRate;
    u16 width;
    u16 height;
    u16 flags;
    u16 genreId;
    u32 lruGeneration;
    u32 memoryBytes;
    MultiArray<char, 256> src;
    MultiArray<char, 64>  name;
    MultiArray<char, 64>  title;
    MultiArray<char, 64>  artist;
    MultiArray<char, 32>  genre;
    u64 nameHash;
    u32 textureId;
    MultiArray<u32, 9>    reserved;

    void init(
        u32 inMediaType,
        const char* inSrc,
        const char* inName,
        const char* inTitle,
        const char* inArtist,
        const char* inGenre,
        u64 inFileSize,
        u32 inDurationMs,
        u16 inWidth,
        u16 inHeight,
        u16 inFlags
    ) noexcept;

    constexpr bool match_id(u32 id) const noexcept {
        return resourceId != 0u && resourceId == id;
    }

    constexpr bool match_key(u64 hash) const noexcept {
        return (srcHash != 0ULL && srcHash == hash) || (nameHash != 0ULL && nameHash == hash);
    }

    constexpr bool match_src_hash(u64 hash) const noexcept {
        return srcHash != 0ULL && srcHash == hash;
    }

    constexpr bool match_name_hash(u64 hash) const noexcept {
        return nameHash != 0ULL && nameHash == hash;
    }

    void touch(u32 gen) noexcept { lruGeneration = gen; }

    void load_memory(u32 bytes) noexcept {
        memoryBytes = bytes;
        flags |= RES_FLAG_MEMORY_LOADED;
    }

    void unload_memory() noexcept {
        memoryBytes = 0u;
        flags &= ~RES_FLAG_MEMORY_LOADED;
    }

    constexpr bool is_loaded() const noexcept { return (flags & RES_FLAG_MEMORY_LOADED) != 0u; }
    constexpr bool is_streamable() const noexcept { return (flags & RES_FLAG_STREAMABLE) != 0u; }

    u64 byte_offset_at(TimePoint t) const noexcept {
        if (durationMs == 0u || fileSize == 0ULL) return 0ULL;
        const u64 offset = (static_cast<u64>(t) * fileSize) / durationMs;
        return (offset < fileSize) ? offset : fileSize;
    }
};
static_assert(sizeof(ResourceEntry) == 576, "ResourceEntry must be 576 bytes (aligned 64B)");

struct alignas(16) ClusterSlot {
    u64 keyHash = 0;
    u32 resourceId = 0;
    u16 entryIndex = 0;
    u16 flags = 0;
};
static_assert(sizeof(ClusterSlot) == 16, "ClusterSlot must be 16 bytes");

using ResourceCluster = Cluster<ClusterSlot, RESOURCE_CLUSTER_WAYS>;
static_assert(sizeof(ResourceCluster) == 64, "ResourceCluster must be 64 bytes (1 L1 Cacheline)");

class ResourceRegistry {
public:
    ResourceRegistry() noexcept { reset(); }

    void reset() noexcept;

    u32 register_resource(
        u32 mediaType,
        const char* src,
        const char* name,
        const char* title,
        const char* artist,
        const char* genre,
        u64 fileSize,
        u32 durationMs,
        u16 width,
        u16 height,
        u16 flags
    ) noexcept;

    bool unregister_resource(u32 resourceId) noexcept;

    ResourceEntry* probe(u64 keyHash) noexcept;
    const ResourceEntry* probe(u64 keyHash) const noexcept;
    ResourceEntry* probe_id(u32 resourceId) noexcept;
    const ResourceEntry* probe_id(u32 resourceId) const noexcept;

    ResourceEntry* find_by_src(const char* src) noexcept;
    const ResourceEntry* find_by_src(const char* src) const noexcept;
    ResourceEntry* find_by_name(const char* name) noexcept;
    const ResourceEntry* find_by_name(const char* name) const noexcept;
    ResourceEntry* find_by_id(u32 resourceId) noexcept { return probe_id(resourceId); }
    const ResourceEntry* find_by_id(u32 resourceId) const noexcept { return probe_id(resourceId); }

    ResourceEntry* find(const char* identifier) noexcept;
    const ResourceEntry* find(const char* identifier) const noexcept;

    u32 total_count() const noexcept { return count_; }
    u32 count_by_type(u32 mediaType) const noexcept;
    u32 get_id_by_type_index(u32 mediaType, u32 index) const noexcept;

    void touch(u32 resourceId) noexcept;
    void set_memory_loaded(u32 resourceId, u32 bytes) noexcept;
    void set_memory_unloaded(u32 resourceId) noexcept;
    u32 enforce_budget(u32 maxBytes) noexcept;
    u32 clear_project_files() noexcept;

    bool poll_event(RegistryEvent& outEvent) noexcept;
    u32 pending_event_count() const noexcept { return eventCount_; }
    u64 current_memory_bytes() const noexcept { return currentMemoryBytes_; }

    static u32 hash_str(const char* str) noexcept;
    static u64 zobrist_str(const char* str) noexcept;
    static u32 compute_resource_id(const char* str) noexcept { return hash_str(str); }

private:
    void push_event(u32 type, u32 resourceId, u32 mediaType) noexcept;
    void insert_cluster_slot(u64 keyHash, u32 resourceId, u16 entryIdx) noexcept;
    void rebuild_cluster_index() noexcept;

    MultiArray<ResourceEntry, MAX_REGISTRY_RESOURCES> entries_{};
    u32 count_ = 0;
    MultiArray<ResourceCluster, RESOURCE_CLUSTERS_NB> clusters_{};
    MultiArray<u16, static_cast<usize>(RegistryMediaType::Count), MAX_REGISTRY_RESOURCES> typeIndices_{};
    MultiArray<u32, static_cast<usize>(RegistryMediaType::Count)> typeCounts_{};
    MultiArray<RegistryEvent, MAX_RESOURCE_EVENTS> eventQueue_{};
    u32 eventHead_ = 0;
    u32 eventTail_ = 0;
    u32 eventCount_ = 0;
    u32 currentGeneration_ = 1;
    u64 currentMemoryBytes_ = 0;
    u64 maxMemoryBytes_ = DEFAULT_RESOURCE_BUDGET_B;
};

ResourceRegistry& resource_registry() noexcept;

} // namespace stockcut
