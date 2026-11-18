#pragma once
// ============================================================================
// STOCKCUT STREAMING RANDOM-ACCESS 1:1 PIPELINE
// ============================================================================
#include "types.h"
#include "data_structures.h"
#include "nnue_project.h"

namespace stockcut {
constexpr u32 STREAM_CHUNK_BYTES   = 65536u;
constexpr u32 STREAM_CHUNK_SHIFT   = 16u;
constexpr u32 STREAM_RING_PAGES    = 16u;
constexpr u32 MAX_STREAM_RESOURCES = 64u;
constexpr u32 MAX_STREAM_REQUESTS  = 16u;

constexpr u8 STREAM_HEADER_UNPROBED = 0u;
constexpr u8 STREAM_HEADER_INFLIGHT = 1u;
constexpr u8 STREAM_HEADER_READY    = 2u;

constexpr u8 STREAM_PAGE_FREE    = 0u;
constexpr u8 STREAM_PAGE_LOADING = 1u;
constexpr u8 STREAM_PAGE_READY   = 2u;

struct alignas(16) ByteRangeCmd {
    u32 resourceId;
    u32 slot;
    u32 ringPageIdx;
    u32 wasmDestPtr;
    double byteStart;
    u32 byteLength;
    u32 flags;
};
static_assert(sizeof(ByteRangeCmd) == 32, "ByteRangeCmd must be 32 bytes");

struct alignas(64) StreamResourceDesc {
    u32 resourceId;
    u32 mediaType;
    u64 fileSize;
    u32 durationMs;
    u32 byteRate;
    u32 dataStartOffset;
    u32 timescale;
    u8  headerState;
    u8  containerKind;
    u16 gopCount;
    u32 lastRequestedPage;
    MultiArray<u64, 3> reserved;
};
static_assert(sizeof(StreamResourceDesc) == 64, "StreamResourceDesc must be 64 bytes");

struct alignas(64) StreamRingPage {
    u64 key;
    u32 resourceId;
    u32 chunkIdx;
    u32 validBytes;
    u32 lruTick;
    u8  state;
    MultiArray<u8, 39> pad;
    alignas(64) MultiArray<u8, STREAM_CHUNK_BYTES> data;
};

class DemuxPipeline {
public:
    DemuxPipeline() noexcept { reset(); }
    void reset() noexcept;
    u32 register_resource(u32 resourceId, u32 mediaType, u64 fileSize) noexcept;

    template <bool IsPlaying>
    u32 schedule_at(TimePoint timeMs) noexcept;

    bool request_byte_range(u32 resourceId, u32 slot, u64 byteStart, u32 byteLen, u32 flags) noexcept;
    void commit_chunk(u32 resourceId, u32 ringPageIdx, u64 byteStart, u32 actualBytes) noexcept;
    u32 pending_request_count() const noexcept { return static_cast<u32>(pendingCmds_.size()); }
    const ByteRangeCmd* pending_requests_ptr() const noexcept { return pendingCmds_.begin(); }
    void clear_pending_requests() noexcept { pendingCmds_.clear(); }
    u32 get_resource_duration_ms(u32 resourceId) const noexcept;
    u32 get_resource_byte_rate(u32 resourceId) const noexcept;
    u32 hit_count() const noexcept { return hits_; }
    u32 miss_count() const noexcept { return misses_; }

private:
    StreamResourceDesc* find_or_create_resource(u32 resourceId, u32 mediaType) noexcept;
    const StreamResourceDesc* find_resource(u32 resourceId) const noexcept;
    i32 find_ring_page(u32 resourceId, u32 chunkIdx) noexcept;
    u32 allocate_ring_page(u32 resourceId, u32 chunkIdx) noexcept;
    void parse_header_slice(StreamResourceDesc& desc, const u8* buf, u32 len) noexcept;

    MultiArray<StreamResourceDesc, MAX_STREAM_RESOURCES> resources_{};
    u32 resourceCount_ = 0;
    MultiArray<StreamRingPage, STREAM_RING_PAGES> ringPages_{};
    ValueList<ByteRangeCmd, MAX_STREAM_REQUESTS> pendingCmds_{};
    u32 lruCounter_ = 1;
    u32 hits_ = 0;
    u32 misses_ = 0;
};

DemuxPipeline& stream_pipeline() noexcept;
} // namespace stockcut
