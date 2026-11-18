// ============================================================================
// STOCKCUT C++ RESOURCE REGISTRY IMPLEMENTATION (DATA-STRUCTURE-CENTRIC)
// ============================================================================
#include "registry.h"
#include "hash_primitives.h"
#include "string_parse.h"
#include <cstring>

namespace stockcut {

static ResourceRegistry g_resourceRegistry;

ResourceRegistry& resource_registry() noexcept {
    return g_resourceRegistry;
}

void ResourceEntry::init(
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
) noexcept {
    std::memset(this, 0, sizeof(ResourceEntry));
    mediaType = inMediaType;
    fileSize = inFileSize;
    durationMs = inDurationMs;
    byteRate = (durationMs > 0u && fileSize > 0ULL) ? static_cast<u32>((fileSize * 1000ULL) / durationMs) : 0u;
    width = inWidth;
    height = inHeight;
    flags = inFlags;
    lruGeneration = 1u;

    if (inSrc && *inSrc) {
        copy_cstr_bounded(src.data(), src.size(), inSrc);
        srcHash = ResourceRegistry::zobrist_str(src.data());
        resourceId = ResourceRegistry::hash_str(src.data());
    }
    if (inName && *inName) {
        copy_cstr_bounded(name.data(), name.size(), inName);
        nameHash = ResourceRegistry::zobrist_str(name.data());
    }
    if (inTitle && *inTitle) copy_cstr_bounded(title.data(), title.size(), inTitle);
    if (inArtist && *inArtist) copy_cstr_bounded(artist.data(), artist.size(), inArtist);
    if (inGenre && *inGenre) copy_cstr_bounded(genre.data(), genre.size(), inGenre);
}

void ResourceRegistry::reset() noexcept {
    entries_.clear();
    count_ = 0;
    clusters_.clear();
    typeIndices_.clear();
    typeCounts_.clear();
    eventQueue_.clear();
    eventHead_ = 0;
    eventTail_ = 0;
    eventCount_ = 0;
    currentGeneration_ = 1;
    currentMemoryBytes_ = 0;
}

u32 ResourceRegistry::hash_str(const char* str) noexcept {
    return hash31_str(str);
}

u64 ResourceRegistry::zobrist_str(const char* str) noexcept {
    return (!str || !*str) ? 0ULL : fnv1a_64_str(str);
}

void ResourceRegistry::insert_cluster_slot(u64 keyHash, u32 resId, u16 entryIdx) noexcept {
    if (!keyHash && !resId) return;
    const usize clusterIdx = static_cast<usize>(
        keyHash ? (keyHash & (RESOURCE_CLUSTERS_NB - 1u)) : (resId & (RESOURCE_CLUSTERS_NB - 1u)));
    ResourceCluster& cluster = clusters_[clusterIdx];

    for (usize i = 0; i < RESOURCE_CLUSTER_WAYS; ++i) {
        ClusterSlot& s = cluster.slots[i];
        if (!s.flags || s.keyHash == keyHash || (resId && s.resourceId == resId)) {
            s.keyHash = keyHash;
            s.resourceId = resId;
            s.entryIndex = entryIdx;
            s.flags = 1u;
            return;
        }
    }
    cluster.slots[0] = {keyHash, resId, entryIdx, 1u};
}

void ResourceRegistry::rebuild_cluster_index() noexcept {
    clusters_.clear();
    for (u32 i = 0; i < count_; ++i) {
        const u16 idx = static_cast<u16>(i);
        const ResourceEntry& e = entries_[i];
        if (e.srcHash)    insert_cluster_slot(e.srcHash, e.resourceId, idx);
        if (e.nameHash)   insert_cluster_slot(e.nameHash, e.resourceId, idx);
        if (e.resourceId) insert_cluster_slot(static_cast<u64>(e.resourceId), e.resourceId, idx);
    }
}

ResourceEntry* ResourceRegistry::probe(u64 keyHash) noexcept {
    return const_cast<ResourceEntry*>(static_cast<const ResourceRegistry*>(this)->probe(keyHash));
}

const ResourceEntry* ResourceRegistry::probe(u64 keyHash) const noexcept {
    if (!keyHash) return nullptr;
    const ResourceCluster& cluster = clusters_[static_cast<usize>(keyHash & (RESOURCE_CLUSTERS_NB - 1u))];
    for (usize i = 0; i < RESOURCE_CLUSTER_WAYS; ++i) {
        const ClusterSlot& s = cluster.slots[i];
        if (s.flags && s.keyHash == keyHash && s.entryIndex < count_) {
            return &entries_[s.entryIndex];
        }
    }
    return nullptr;
}

ResourceEntry* ResourceRegistry::probe_id(u32 resourceId) noexcept {
    return const_cast<ResourceEntry*>(static_cast<const ResourceRegistry*>(this)->probe_id(resourceId));
}

const ResourceEntry* ResourceRegistry::probe_id(u32 resourceId) const noexcept {
    if (!resourceId) return nullptr;
    const ResourceCluster& cluster = clusters_[static_cast<usize>(resourceId & (RESOURCE_CLUSTERS_NB - 1u))];
    for (usize i = 0; i < RESOURCE_CLUSTER_WAYS; ++i) {
        const ClusterSlot& s = cluster.slots[i];
        if (s.flags && s.resourceId == resourceId && s.entryIndex < count_ && entries_[s.entryIndex].match_id(resourceId)) {
            return &entries_[s.entryIndex];
        }
    }
    return nullptr;
}

ResourceEntry* ResourceRegistry::find_by_src(const char* inSrc) noexcept {
    return (!inSrc || !*inSrc) ? nullptr : probe(zobrist_str(inSrc));
}

const ResourceEntry* ResourceRegistry::find_by_src(const char* inSrc) const noexcept {
    return (!inSrc || !*inSrc) ? nullptr : probe(zobrist_str(inSrc));
}

ResourceEntry* ResourceRegistry::find_by_name(const char* inName) noexcept {
    return (!inName || !*inName) ? nullptr : probe(zobrist_str(inName));
}

const ResourceEntry* ResourceRegistry::find_by_name(const char* inName) const noexcept {
    return (!inName || !*inName) ? nullptr : probe(zobrist_str(inName));
}

ResourceEntry* ResourceRegistry::find(const char* identifier) noexcept {
    return const_cast<ResourceEntry*>(static_cast<const ResourceRegistry*>(this)->find(identifier));
}

const ResourceEntry* ResourceRegistry::find(const char* identifier) const noexcept {
    if (!identifier || !*identifier) return nullptr;
    if (const ResourceEntry* e = probe(zobrist_str(identifier))) return e;
    return probe_id(hash_str(identifier));
}

u32 ResourceRegistry::register_resource(
    u32 mediaType,
    const char* inSrc,
    const char* inName,
    const char* title,
    const char* artist,
    const char* genre,
    u64 fileSize,
    u32 durationMs,
    u16 width,
    u16 height,
    u16 flags
) noexcept {
    if (!inSrc || !*inSrc || count_ >= MAX_REGISTRY_RESOURCES) return 0u;

    if (ResourceEntry* existing = find_by_src(inSrc)) {
        if (fileSize > 0ULL && existing->fileSize == 0ULL) existing->fileSize = fileSize;
        if (durationMs > 0u && existing->durationMs == 0u) existing->durationMs = durationMs;
        if (width > 0u && existing->width == 0u) existing->width = width;
        if (height > 0u && existing->height == 0u) existing->height = height;
        existing->flags |= flags;
        push_event(RES_EVENT_UPDATE, existing->resourceId, existing->mediaType);
        return existing->resourceId;
    }

    const u32 index = count_++;
    const u16 entryIdx = static_cast<u16>(index);
    ResourceEntry& entry = entries_[index];

    entry.init(mediaType, inSrc, inName, title, artist, genre, fileSize, durationMs, width, height, flags);
    entry.lruGeneration = currentGeneration_;

    if (entry.srcHash)    insert_cluster_slot(entry.srcHash, entry.resourceId, entryIdx);
    if (entry.nameHash)   insert_cluster_slot(entry.nameHash, entry.resourceId, entryIdx);
    if (entry.resourceId) insert_cluster_slot(static_cast<u64>(entry.resourceId), entry.resourceId, entryIdx);

    if (mediaType < static_cast<u32>(RegistryMediaType::Count)) {
        u32& tCount = typeCounts_[mediaType];
        if (tCount < MAX_REGISTRY_RESOURCES) {
            typeIndices_[mediaType][tCount++] = entryIdx;
        }
    }

    stream_pipeline().register_resource(entry.resourceId, mediaType, fileSize);
    push_event(RES_EVENT_ADD, entry.resourceId, mediaType);
    return entry.resourceId;
}

bool ResourceRegistry::unregister_resource(u32 resourceId) noexcept {
    if (!resourceId) return false;
    for (u32 i = 0; i < count_; ++i) {
        if (!entries_[i].match_id(resourceId)) continue;
        const u32 mediaType = entries_[i].mediaType;
        push_event(RES_EVENT_REMOVE, resourceId, mediaType);

        if (entries_[i].memoryBytes > 0u) {
            currentMemoryBytes_ -= entries_[i].memoryBytes;
            entries_[i].memoryBytes = 0u;
        }

        if (mediaType < static_cast<u32>(RegistryMediaType::Count)) {
            u32& tCount = typeCounts_[mediaType];
            for (u32 j = 0; j < tCount; ++j) {
                if (typeIndices_[mediaType][j] == i) {
                    for (u32 k = j; k + 1 < tCount; ++k) {
                        typeIndices_[mediaType][k] = typeIndices_[mediaType][k + 1];
                    }
                    --tCount;
                    break;
                }
            }
        }

        if (i + 1 < count_) {
            entries_[i] = entries_[count_ - 1];
            const u32 movedType = entries_[i].mediaType;
            if (movedType < static_cast<u32>(RegistryMediaType::Count)) {
                u32& tCount = typeCounts_[movedType];
                for (u32 j = 0; j < tCount; ++j) {
                    if (typeIndices_[movedType][j] == count_ - 1) {
                        typeIndices_[movedType][j] = static_cast<u16>(i);
                        break;
                    }
                }
            }
        }
        --count_;
        rebuild_cluster_index();
        return true;
    }
    return false;
}

u32 ResourceRegistry::count_by_type(u32 mediaType) const noexcept {
    return (mediaType < static_cast<u32>(RegistryMediaType::Count)) ? typeCounts_[mediaType] : 0u;
}

u32 ResourceRegistry::get_id_by_type_index(u32 mediaType, u32 index) const noexcept {
    if (mediaType >= static_cast<u32>(RegistryMediaType::Count) || index >= typeCounts_[mediaType]) return 0u;
    return entries_[typeIndices_[mediaType][index]].resourceId;
}

void ResourceRegistry::touch(u32 resourceId) noexcept {
    if (ResourceEntry* entry = probe_id(resourceId)) {
        entry->touch(++currentGeneration_);
    }
}

void ResourceRegistry::set_memory_loaded(u32 resourceId, u32 bytes) noexcept {
    if (ResourceEntry* entry = probe_id(resourceId)) {
        currentMemoryBytes_ += (bytes - entry->memoryBytes);
        entry->load_memory(bytes);
        entry->touch(++currentGeneration_);
        if (currentMemoryBytes_ > maxMemoryBytes_) {
            enforce_budget(static_cast<u32>(maxMemoryBytes_));
        }
    }
}

void ResourceRegistry::set_memory_unloaded(u32 resourceId) noexcept {
    if (ResourceEntry* entry = probe_id(resourceId)) {
        if (entry->memoryBytes > 0u) {
            currentMemoryBytes_ -= entry->memoryBytes;
        }
        entry->unload_memory();
        push_event(RES_EVENT_EVICT, resourceId, entry->mediaType);
    }
}

u32 ResourceRegistry::enforce_budget(u32 maxBytes) noexcept {
    u32 evictedCount = 0;
    while (currentMemoryBytes_ > maxBytes && count_ > 0u) {
        u32 oldestIdx = INVALID_U32;
        u32 oldestGen = INVALID_U32;
        for (u32 i = 0; i < count_; ++i) {
            if (entries_[i].is_loaded() && entries_[i].lruGeneration < oldestGen) {
                oldestGen = entries_[i].lruGeneration;
                oldestIdx = i;
            }
        }
        if (oldestIdx == INVALID_U32) break;
        ResourceEntry& victim = entries_[oldestIdx];
        currentMemoryBytes_ -= victim.memoryBytes;
        victim.unload_memory();
        push_event(RES_EVENT_EVICT, victim.resourceId, victim.mediaType);
        ++evictedCount;
    }
    return evictedCount;
}

u32 ResourceRegistry::clear_project_files() noexcept {
    u32 cleared = 0;
    u32 i = 0;
    while (i < count_) {
        if (entries_[i].flags & RES_FLAG_IS_PROJECT) {
            unregister_resource(entries_[i].resourceId);
            ++cleared;
        } else {
            ++i;
        }
    }
    return cleared;
}

void ResourceRegistry::push_event(u32 type, u32 resourceId, u32 mediaType) noexcept {
    if (eventCount_ >= MAX_RESOURCE_EVENTS) {
        eventHead_ = (eventHead_ + 1u) & (MAX_RESOURCE_EVENTS - 1u);
        --eventCount_;
    }
    eventQueue_[eventTail_] = {type, resourceId, mediaType, 0u};
    eventTail_ = (eventTail_ + 1u) & (MAX_RESOURCE_EVENTS - 1u);
    ++eventCount_;
}

bool ResourceRegistry::poll_event(RegistryEvent& outEvent) noexcept {
    if (eventCount_ == 0u) return false;
    outEvent = eventQueue_[eventHead_];
    eventHead_ = (eventHead_ + 1u) & (MAX_RESOURCE_EVENTS - 1u);
    --eventCount_;
    return true;
}

} // namespace stockcut

extern "C" {

static stockcut::MultiArray<char, stockcut::RES_WASM_INPUT_CAPACITY> g_wasmInputBuffer{};

char* wasm_res_get_input_buffer_ptr() {
    return g_wasmInputBuffer.data();
}

stockcut::u32 wasm_res_register(
    stockcut::u32 mediaType,
    const char* src,
    const char* name,
    const char* title,
    const char* artist,
    const char* genre,
    double fileSize,
    stockcut::u32 durationMs,
    stockcut::u32 width,
    stockcut::u32 height,
    stockcut::u32 flags
) {
    return stockcut::resource_registry().register_resource(
        mediaType,
        src,
        name,
        title,
        artist,
        genre,
        fileSize > 0.0 ? static_cast<stockcut::u64>(fileSize) : 0ULL,
        durationMs,
        static_cast<stockcut::u16>(width),
        static_cast<stockcut::u16>(height),
        static_cast<stockcut::u16>(flags)
    );
}

stockcut::u32 wasm_res_unregister(stockcut::u32 resourceId) {
    return stockcut::resource_registry().unregister_resource(resourceId) ? 1u : 0u;
}

stockcut::u32 wasm_res_get_count() {
    return stockcut::resource_registry().total_count();
}

stockcut::u32 wasm_res_get_count_by_type(stockcut::u32 mediaType) {
    return stockcut::resource_registry().count_by_type(mediaType);
}

stockcut::u32 wasm_res_get_id_by_type_index(stockcut::u32 mediaType, stockcut::u32 index) {
    return stockcut::resource_registry().get_id_by_type_index(mediaType, index);
}

stockcut::u32 wasm_res_find_by_path(const char* path) {
    stockcut::ResourceEntry* e = stockcut::resource_registry().find_by_src(path);
    return e ? e->resourceId : 0u;
}

stockcut::u32 wasm_res_find_by_name(const char* name) {
    stockcut::ResourceEntry* e = stockcut::resource_registry().find_by_name(name);
    return e ? e->resourceId : 0u;
}

const stockcut::ResourceEntry* wasm_res_get_entry_ptr(stockcut::u32 resourceId) {
    return stockcut::resource_registry().find_by_id(resourceId);
}

const char* wasm_res_get_entry_path(stockcut::u32 resourceId) {
    const stockcut::ResourceEntry* e = stockcut::resource_registry().find_by_id(resourceId);
    return e ? e->src.data() : "";
}

const char* wasm_res_get_entry_name(stockcut::u32 resourceId) {
    const stockcut::ResourceEntry* e = stockcut::resource_registry().find_by_id(resourceId);
    return e ? e->name.data() : "";
}

const char* wasm_res_get_entry_title(stockcut::u32 resourceId) {
    const stockcut::ResourceEntry* e = stockcut::resource_registry().find_by_id(resourceId);
    return e ? e->title.data() : "";
}

const char* wasm_res_get_entry_artist(stockcut::u32 resourceId) {
    const stockcut::ResourceEntry* e = stockcut::resource_registry().find_by_id(resourceId);
    return e ? e->artist.data() : "";
}

const char* wasm_res_get_entry_genre(stockcut::u32 resourceId) {
    const stockcut::ResourceEntry* e = stockcut::resource_registry().find_by_id(resourceId);
    return e ? e->genre.data() : "";
}

double wasm_res_get_entry_file_size(stockcut::u32 resourceId) {
    const stockcut::ResourceEntry* e = stockcut::resource_registry().find_by_id(resourceId);
    return e ? static_cast<double>(e->fileSize) : 0.0;
}

stockcut::u32 wasm_res_get_entry_duration_ms(stockcut::u32 resourceId) {
    const stockcut::ResourceEntry* e = stockcut::resource_registry().find_by_id(resourceId);
    return e ? e->durationMs : 0u;
}

stockcut::u32 wasm_res_get_entry_flags(stockcut::u32 resourceId) {
    const stockcut::ResourceEntry* e = stockcut::resource_registry().find_by_id(resourceId);
    return e ? static_cast<stockcut::u32>(e->flags) : 0u;
}

void wasm_res_touch(stockcut::u32 resourceId) {
    stockcut::resource_registry().touch(resourceId);
}

void wasm_res_set_memory_loaded(stockcut::u32 resourceId, stockcut::u32 bytes) {
    stockcut::resource_registry().set_memory_loaded(resourceId, bytes);
}

void wasm_res_set_memory_unloaded(stockcut::u32 resourceId) {
    stockcut::resource_registry().set_memory_unloaded(resourceId);
}

stockcut::u32 wasm_res_enforce_budget(stockcut::u32 maxBytes) {
    return stockcut::resource_registry().enforce_budget(maxBytes);
}

stockcut::u32 wasm_res_clear_project_files() {
    return stockcut::resource_registry().clear_project_files();
}

stockcut::u32 wasm_res_poll_event(stockcut::u32* outType, stockcut::u32* outResourceId, stockcut::u32* outMediaType) {
    stockcut::RegistryEvent ev{};
    if (stockcut::resource_registry().poll_event(ev)) {
        if (outType) *outType = ev.eventType;
        if (outResourceId) *outResourceId = ev.resourceId;
        if (outMediaType) *outMediaType = ev.mediaType;
        return 1u;
    }
    return 0u;
}

} // extern "C"
