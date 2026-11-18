// ============================================================================
// CoreEngine - Deterministic Computation Cache (Compile-Time Domain Specialization)
// ============================================================================
#pragma once
#include "types.h"

namespace stockcut {
enum class ComputationCacheDomain : u32 {
    ZoomPeriodic    = 1,
    Shake           = 2,
    FlickerPeriodic = 3,
    IntroOutroState = 4,
    EffectRoi       = 5
};

constexpr u32 COMPUTATION_CACHE_MAX_PAYLOAD = 160;

struct ComputationCacheStatus {
    u64 key;
    u32 domain;
    u32 size;
    u32 checksum;
    u32 hit;
    u32 persist;
};

template <ComputationCacheDomain Domain>
[[nodiscard]] constexpr u32 computation_cache_payload_size_T() noexcept {
    if constexpr (Domain == ComputationCacheDomain::ZoomPeriodic)         return 3u * sizeof(float);
    else if constexpr (Domain == ComputationCacheDomain::Shake)           return 4u * sizeof(float);
    else if constexpr (Domain == ComputationCacheDomain::FlickerPeriodic) return sizeof(float);
    else if constexpr (Domain == ComputationCacheDomain::IntroOutroState) return 37u * sizeof(float);
    else if constexpr (Domain == ComputationCacheDomain::EffectRoi)       return 4u * sizeof(float);
    else return 0u;
}

u64 computation_cache_key(u32 domain, const u64* inputs, u32 inputCount);
bool computation_cache_find(u64 key, u32 domain, void* outPayload, u32 size, bool* outPersist);
bool computation_cache_store(u64 key, u32 domain, const void* payload, u32 size);
bool computation_cache_import(u64 key, u32 domain, u32 size, u32 expectedChecksum, const void* payload);
u32 computation_cache_checksum(u64 key, u32 domain, u32 size, const void* payload);
u32 computation_cache_payload_size(u32 domain);
u8* computation_cache_transfer_buffer();
const u8* computation_cache_last_payload();
const ComputationCacheStatus& computation_cache_last_status();

template <bool Hit, bool Persist>
void computation_cache_set_last_status_T(u64 key, u32 domain, u32 size, const void* payload);

template <bool Succeeded>
void computation_cache_persist_result_T(u64 key, u32 domain);

// Unified compile-time cached evaluation wrapper (eliminates boilerplate across engine.cpp)
template <ComputationCacheDomain Domain, typename ComputeFn>
inline void computation_cache_eval_T(const u64* inputs, u32 inputCount, void* outPayload, ComputeFn&& computeFn) {
    constexpr u32 kDomain = static_cast<u32>(Domain);
    constexpr u32 kSize   = computation_cache_payload_size_T<Domain>();
    const u64 key = computation_cache_key(kDomain, inputs, inputCount);
    bool persist = false;
    if (computation_cache_find(key, kDomain, outPayload, kSize, &persist)) {
        if (persist) {
            computation_cache_set_last_status_T<true, true>(key, kDomain, kSize, outPayload);
        } else {
            computation_cache_set_last_status_T<true, false>(key, kDomain, kSize, outPayload);
        }
        return;
    }
    computeFn();
    const bool cacheable = computation_cache_store(key, kDomain, outPayload, kSize);
    computation_cache_set_last_status_T<false, false>(key, kDomain, cacheable ? kSize : 0u, outPayload);
}

void computation_cache_clear();
usize computation_cache_memory_bytes();
} // namespace stockcut
