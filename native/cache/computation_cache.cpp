// ============================================================================
// CoreEngine - Deterministic Computation Cache Implementation
// ============================================================================
#include "computation_cache.h"
#include "hash_primitives.h"
#include "data_structures.h"

namespace stockcut {
namespace {
constexpr u32 CACHE_SLOT_COUNT = 512;
constexpr u32 CACHE_SCHEMA_VERSION = 0x00010001u;
static_assert((CACHE_SLOT_COUNT & (CACHE_SLOT_COUNT - 1)) == 0);

struct CacheEntry {
    u64 key;
    u32 domain;
    u32 size;
    u32 checksum;
    MultiArray<u8, COMPUTATION_CACHE_MAX_PAYLOAD> payload;
    u32 accessCount;
    u32 coldPersistState;
};

alignas(16) MultiArray<CacheEntry, CACHE_SLOT_COUNT> g_entries;
alignas(16) MultiArray<u8, COMPUTATION_CACHE_MAX_PAYLOAD> g_transferBuffer;
alignas(16) MultiArray<u8, COMPUTATION_CACHE_MAX_PAYLOAD> g_lastPayload;
ComputationCacheStatus g_lastStatus{};

bool validatePayload(u32 domain, u32 size, const void* payload) {
    if (!payload || size != computation_cache_payload_size(domain) ||
        size > COMPUTATION_CACHE_MAX_PAYLOAD || (size & 3u) != 0) {
        return false;
    }
    const float* values = static_cast<const float*>(payload);
    const u32 valueCount = size >> 2;
    for (u32 index = 0; index < valueCount; ++index) {
        if (domain == static_cast<u32>(ComputationCacheDomain::IntroOutroState) &&
            index == 36) {
            continue;
        }
        const float value = values[index];
        if (!(value == value) || value > FLT_MAX_SAFE || value < -FLT_MAX_SAFE) {
            return false;
        }
    }
    if (domain == static_cast<u32>(ComputationCacheDomain::ZoomPeriodic) &&
        values[2] != 0.0f && values[2] != 1.0f) {
        return false;
    }
    if (domain == static_cast<u32>(ComputationCacheDomain::IntroOutroState) &&
        static_cast<const u32*>(payload)[36] > 1u) {
        return false;
    }
    return true;
}

u32 slotFor(u64 key, u32 domain) {
    const u64 mixed = key ^ (key >> 32) ^ (static_cast<u64>(domain) * HASH_GOLDEN_64);
    return static_cast<u32>(mixed) & (CACHE_SLOT_COUNT - 1);
}

void publish_last_status_impl(
    u64 key, u32 domain, u32 size, u32 hitFlag, u32 persistFlag, const void* payload
) {
    g_lastStatus.key = key;
    g_lastStatus.domain = domain;
    g_lastStatus.size = size;
    g_lastStatus.checksum = computation_cache_checksum(key, domain, size, payload);
    g_lastStatus.hit = hitFlag;
    g_lastStatus.persist = persistFlag;
    if (size <= COMPUTATION_CACHE_MAX_PAYLOAD && payload) {
        std::memcpy(g_lastPayload.data(), payload, size);
    }
}
} // namespace

u64 computation_cache_key(u32 domain, const u64* inputs, u32 inputCount) {
    u64 hash = FNV1A_64_OFFSET ^
        (static_cast<u64>(CACHE_SCHEMA_VERSION) << 32) ^ domain;
    for (const u64* p = inputs, *const end = inputs + inputCount; p < end; ++p) {
        hash = fnv1a_64_u64(hash, *p);
    }
    return hash | static_cast<u64>(hash == 0ULL);
}

bool computation_cache_find(
    u64 key, u32 domain, void* outPayload, u32 size, bool* outPersist
) {
    if (outPersist) *outPersist = false;
    if (!outPayload || size != computation_cache_payload_size(domain) ||
        size > COMPUTATION_CACHE_MAX_PAYLOAD) {
        return false;
    }
    CacheEntry& entry = g_entries[slotFor(key, domain)];
    if (entry.key != key || entry.domain != domain || entry.size != size ||
        entry.checksum != computation_cache_checksum(key, domain, size, entry.payload.data()) ||
        !validatePayload(domain, size, entry.payload.data())) {
        return false;
    }
    std::memcpy(outPayload, entry.payload.data(), size);
    if (entry.accessCount < 2u) ++entry.accessCount;
    if (entry.accessCount >= 2u && entry.coldPersistState == 0) {
        entry.coldPersistState = 1;
        if (outPersist) *outPersist = true;
    }
    return true;
}

bool computation_cache_store(u64 key, u32 domain, const void* payload, u32 size) {
    if (!key || !validatePayload(domain, size, payload)) return false;
    CacheEntry& entry = g_entries[slotFor(key, domain)];
    // Ap dung dinh luat Nang luong Tu do Helmholtz F = U - T * S:
    // Bao ve ban ghi dang cho cold persistence khong bi ghi de boi collision
    if (entry.key != 0 && entry.key != key && entry.coldPersistState == 1) {
        return false;
    }
    std::memcpy(entry.payload.data(), payload, size);
    entry.key = key;
    entry.domain = domain;
    entry.size = size;
    entry.checksum = computation_cache_checksum(key, domain, size, entry.payload.data());
    entry.accessCount = 1;
    entry.coldPersistState = 0;
    return true;
}

bool computation_cache_import(
    u64 key, u32 domain, u32 size, u32 expectedChecksum, const void* payload
) {
    if (!validatePayload(domain, size, payload) ||
        computation_cache_checksum(key, domain, size, payload) != expectedChecksum) {
        return false;
    }
    if (!computation_cache_store(key, domain, payload, size)) return false;
    g_entries[slotFor(key, domain)].coldPersistState = 2;
    return true;
}

u32 computation_cache_checksum(u64 key, u32 domain, u32 size, const void* payload) {
    if (!payload) return 0;
    const u32 seed = fnv1a_32_u64(FNV1A_32_OFFSET ^ domain ^ size ^ CACHE_SCHEMA_VERSION, key);
    return fnv1a_32_bytes(payload, size, seed);
}

static constexpr MultiArray<u32, 6> DOMAIN_PAYLOAD_SIZES = {{
    0u,
    computation_cache_payload_size_T<ComputationCacheDomain::ZoomPeriodic>(),
    computation_cache_payload_size_T<ComputationCacheDomain::Shake>(),
    computation_cache_payload_size_T<ComputationCacheDomain::FlickerPeriodic>(),
    computation_cache_payload_size_T<ComputationCacheDomain::IntroOutroState>(),
    computation_cache_payload_size_T<ComputationCacheDomain::EffectRoi>()
}};

u32 computation_cache_payload_size(u32 domain) {
    return (domain < 6u) ? DOMAIN_PAYLOAD_SIZES[domain] : 0u;
}

u8* computation_cache_transfer_buffer() { return g_transferBuffer.data(); }
const u8* computation_cache_last_payload() { return g_lastPayload.data(); }
const ComputationCacheStatus& computation_cache_last_status() { return g_lastStatus; }

template <bool Hit, bool Persist>
void computation_cache_set_last_status_T(u64 key, u32 domain, u32 size, const void* payload) {
    publish_last_status_impl(key, domain, size, Hit ? 1u : 0u, Persist ? 1u : 0u, payload);
}
template void computation_cache_set_last_status_T<false, false>(u64, u32, u32, const void*);
template void computation_cache_set_last_status_T<false, true>(u64, u32, u32, const void*);
template void computation_cache_set_last_status_T<true, false>(u64, u32, u32, const void*);
template void computation_cache_set_last_status_T<true, true>(u64, u32, u32, const void*);

template <bool Succeeded>
void computation_cache_persist_result_T(u64 key, u32 domain) {
    CacheEntry& entry = g_entries[slotFor(key, domain)];
    if (entry.key == key && entry.domain == domain && entry.coldPersistState == 1) {
        entry.coldPersistState = Succeeded ? 2u : 0u;
    }
}
template void computation_cache_persist_result_T<false>(u64, u32);
template void computation_cache_persist_result_T<true>(u64, u32);

void computation_cache_clear() {
    for (CacheEntry* p = g_entries.begin(); p < g_entries.end(); ++p) {
        p->key = 0;
        p->domain = 0;
        p->size = 0;
        p->checksum = 0;
        p->accessCount = 0;
        p->coldPersistState = 0;
    }
    g_lastStatus = {};
}

usize computation_cache_memory_bytes() {
    return sizeof(g_entries) + sizeof(g_transferBuffer) +
        sizeof(g_lastPayload) + sizeof(g_lastStatus);
}
} // namespace stockcut
