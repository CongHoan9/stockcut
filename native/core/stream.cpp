#include "stream.h"
#include "hash_primitives.h"
#include "timeline/position.h"
#include <cstring>

namespace stockcut {
namespace {
constexpr u32 STREAM_VIDEO_DEFAULT_BYTERATE = 384000u;
constexpr u32 STREAM_AUDIO_DEFAULT_BYTERATE = 24000u;
constexpr u64 STREAM_FALLBACK_BYTERATE      = 256000ULL;
constexpr u32 STREAM_FLAG_HEADER_PROBE      = 1u;

alignas(64) DemuxPipeline g_streamPipeline;

inline u32 read_be32(const u8* p) noexcept {
    return (static_cast<u32>(p[0]) << 24) |
           (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8)  |
           static_cast<u32>(p[3]);
}
inline u64 read_be64(const u8* p) noexcept {
    return (static_cast<u64>(read_be32(p)) << 32) | static_cast<u64>(read_be32(p + 4));
}
inline u32 read_le32(const u8* p) noexcept {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}
inline u64 make_page_key(u32 resourceId, u32 chunkIdx) noexcept {
    u64 k = (static_cast<u64>(resourceId) << 32) | static_cast<u64>(chunkIdx);
    k ^= k >> 30;
    k *= SPLITMIX_MUL1;
    k ^= k >> 27;
    return k ? k : 1ULL;
}
} // namespace

DemuxPipeline& stream_pipeline() noexcept {
    return g_streamPipeline;
}

void DemuxPipeline::reset() noexcept {
    resources_.clear();
    resourceCount_ = 0;
    for (StreamRingPage* p = ringPages_.begin(); p < ringPages_.end(); ++p) {
        p->key = 0ULL;
        p->resourceId = 0u;
        p->chunkIdx = 0u;
        p->validBytes = 0u;
        p->lruTick = 0u;
        p->state = STREAM_PAGE_FREE;
    }
    pendingCmds_.clear();
    lruCounter_ = 1u;
    hits_ = 0u;
    misses_ = 0u;
}

StreamResourceDesc* DemuxPipeline::find_or_create_resource(u32 resourceId, u32 mediaType) noexcept {
    if (!resourceId) return nullptr;
    for (u32 i = 0; i < resourceCount_; ++i) {
        if (resources_[i].resourceId == resourceId) {
            if (mediaType && !resources_[i].mediaType) resources_[i].mediaType = mediaType;
            return &resources_[i];
        }
    }
    if (resourceCount_ >= MAX_STREAM_RESOURCES) return &resources_[resourceId & (MAX_STREAM_RESOURCES - 1u)];
    StreamResourceDesc& desc = resources_[resourceCount_++];
    std::memset(&desc, 0, sizeof(StreamResourceDesc));
    desc.resourceId = resourceId;
    desc.mediaType = mediaType;
    desc.byteRate = (mediaType == static_cast<u32>(MediaType::Video))
        ? STREAM_VIDEO_DEFAULT_BYTERATE
        : STREAM_AUDIO_DEFAULT_BYTERATE;
    desc.lastRequestedPage = INVALID_U32;
    return &desc;
}

const StreamResourceDesc* DemuxPipeline::find_resource(u32 resourceId) const noexcept {
    for (u32 i = 0; i < resourceCount_; ++i) {
        if (resources_[i].resourceId == resourceId) return &resources_[i];
    }
    return nullptr;
}

u32 DemuxPipeline::register_resource(u32 resourceId, u32 mediaType, u64 fileSize) noexcept {
    StreamResourceDesc* desc = find_or_create_resource(resourceId, mediaType);
    if (!desc) return 0u;
    if (fileSize > 0ULL) desc->fileSize = fileSize;
    if (desc->headerState == STREAM_HEADER_UNPROBED) {
        request_byte_range(resourceId, INVALID_U16, 0ULL, STREAM_CHUNK_BYTES, STREAM_FLAG_HEADER_PROBE);
    }
    return resourceId;
}

i32 DemuxPipeline::find_ring_page(u32 resourceId, u32 chunkIdx) noexcept {
    const u64 key = make_page_key(resourceId, chunkIdx);
    for (u32 i = 0; i < STREAM_RING_PAGES; ++i) {
        StreamRingPage& page = ringPages_[i];
        if (page.key == key &&
            page.resourceId == resourceId &&
            page.chunkIdx == chunkIdx &&
            page.state != STREAM_PAGE_FREE) {
            page.lruTick = ++lruCounter_;
            return static_cast<i32>(i);
        }
    }
    return -1;
}

u32 DemuxPipeline::allocate_ring_page(u32 resourceId, u32 chunkIdx) noexcept {
    u32 victim = 0u;
    u32 bestTick = INVALID_U32;
    for (u32 i = 0; i < STREAM_RING_PAGES; ++i) {
        if (ringPages_[i].state == STREAM_PAGE_FREE) {
            victim = i;
            break;
        }
        if (ringPages_[i].lruTick < bestTick) {
            bestTick = ringPages_[i].lruTick;
            victim = i;
        }
    }
    StreamRingPage& page = ringPages_[victim];
    page.key = make_page_key(resourceId, chunkIdx);
    page.resourceId = resourceId;
    page.chunkIdx = chunkIdx;
    page.validBytes = 0u;
    page.lruTick = ++lruCounter_;
    page.state = STREAM_PAGE_LOADING;
    return victim;
}

bool DemuxPipeline::request_byte_range(u32 resourceId, u32 slot, u64 byteStart, u32 byteLen, u32 flags) noexcept {
    if (!resourceId) return false;
    const u32 chunkIdx = static_cast<u32>(byteStart >> STREAM_CHUNK_SHIFT);
    if (find_ring_page(resourceId, chunkIdx) >= 0) {
        ++hits_;
        return true;
    }
    if (pendingCmds_.size() >= MAX_STREAM_REQUESTS) return false;
    ++misses_;
    const u32 pageIdx = allocate_ring_page(resourceId, chunkIdx);
    StreamResourceDesc* desc = find_or_create_resource(resourceId, 0u);
    if (desc) {
        if (chunkIdx == 0u && desc->headerState == STREAM_HEADER_UNPROBED)
            desc->headerState = STREAM_HEADER_INFLIGHT;
        desc->lastRequestedPage = chunkIdx;
    }
    const u64 alignedStart = static_cast<u64>(chunkIdx) << STREAM_CHUNK_SHIFT;
    u32 clampedLen = (byteLen > 0u && byteLen <= STREAM_CHUNK_BYTES) ? byteLen : STREAM_CHUNK_BYTES;
    if (desc && desc->fileSize > 0ULL) {
        if (alignedStart >= desc->fileSize) return false;
        const u64 remain = desc->fileSize - alignedStart;
        if (remain < clampedLen) clampedLen = static_cast<u32>(remain);
    }
    ByteRangeCmd cmd{};
    cmd.resourceId  = resourceId;
    cmd.slot        = slot;
    cmd.ringPageIdx = pageIdx;
    cmd.wasmDestPtr = static_cast<u32>(reinterpret_cast<usize>(ringPages_[pageIdx].data.data()));
    cmd.byteStart   = static_cast<double>(alignedStart);
    cmd.byteLength  = clampedLen;
    cmd.flags       = flags;
    pendingCmds_.push_back(cmd);
    return true;
}

template <bool IsPlaying>
u32 DemuxPipeline::schedule_at(TimePoint timeMs) noexcept {
    Position<>* pos = timeline_position_get_instance();
    if (!pos) return 0u;
    Bitboard mediaBB = pos->active_media_items();
    while (mediaBB) {
        const u32 slot = mediaBB.pop_lsb();
        const TimelineItem& it = pos->item(slot);
        if (!it.resourceId) continue;
        StreamResourceDesc* desc = find_or_create_resource(it.resourceId, static_cast<u32>(it.type));
        if (!desc) continue;
        if (desc->headerState == STREAM_HEADER_UNPROBED) {
            request_byte_range(it.resourceId, slot, 0ULL, STREAM_CHUNK_BYTES, STREAM_FLAG_HEADER_PROBE);
        }
        const i64 relMs = stockcut::max<i64>(0LL, timeMs - it.startMs);
        const u64 byteRate = desc->byteRate > 0u ? desc->byteRate : STREAM_FALLBACK_BYTERATE;
        const u64 targetOffset = static_cast<u64>(desc->dataStartOffset) + (static_cast<u64>(relMs) * byteRate) / 1000ULL;
        request_byte_range(it.resourceId, slot, targetOffset, STREAM_CHUNK_BYTES, 0u);
        if constexpr (IsPlaying) {
            request_byte_range(it.resourceId, slot, targetOffset + STREAM_CHUNK_BYTES, STREAM_CHUNK_BYTES, 0u);
        }
    }
    return static_cast<u32>(pendingCmds_.size());
}
template u32 DemuxPipeline::schedule_at<false>(TimePoint) noexcept;
template u32 DemuxPipeline::schedule_at<true>(TimePoint) noexcept;

void DemuxPipeline::parse_header_slice(StreamResourceDesc& desc, const u8* buf, u32 len) noexcept {
    if (!buf || len < 16u) return;
    if (buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F' &&
        buf[8] == 'W' && buf[9] == 'A' && buf[10] == 'V' && buf[11] == 'E') {
        desc.containerKind = 2u;
        u32 pos = 12u;
        while (pos + 8u <= len) {
            const u32 chunkSize = read_le32(buf + pos + 4u);
            if (buf[pos] == 'f' && buf[pos + 1] == 'm' && buf[pos + 2] == 't' && buf[pos + 3] == ' ' && pos + 20u <= len) {
                desc.timescale = read_le32(buf + pos + 12u);
                desc.byteRate  = read_le32(buf + pos + 16u);
            } else if (buf[pos] == 'd' && buf[pos + 1] == 'a' && buf[pos + 2] == 't' && buf[pos + 3] == 'a') {
                desc.dataStartOffset = pos + 8u;
                if (desc.byteRate > 0u) {
                    desc.durationMs = static_cast<u32>((static_cast<u64>(chunkSize) * 1000ULL) / desc.byteRate);
                }
                break;
            }
            pos += 8u + ((chunkSize + 1u) & ~1u);
        }
        desc.headerState = STREAM_HEADER_READY;
        return;
    }
    if ((buf[4] == 'f' && buf[5] == 't' && buf[6] == 'y' && buf[7] == 'p') ||
        (buf[4] == 'm' && buf[5] == 'o' && buf[6] == 'o' && buf[7] == 'v') ||
        (buf[4] == 'm' && buf[5] == 'd' && buf[6] == 'a' && buf[7] == 't')) {
        desc.containerKind = 1u;
        for (u32 i = 0; i + 32u <= len; ++i) {
            if (buf[i] == 'm' && buf[i + 1] == 'v' && buf[i + 2] == 'h' && buf[i + 3] == 'd') {
                const u8 version = buf[i + 4u];
                if (version == 0u && i + 24u <= len) {
                    const u32 timescale = read_be32(buf + i + 16u);
                    const u32 duration  = read_be32(buf + i + 20u);
                    if (timescale > 0u && duration > 0u) {
                        desc.timescale  = timescale;
                        desc.durationMs = static_cast<u32>((static_cast<u64>(duration) * 1000ULL) / timescale);
                    }
                } else if (version == 1u && i + 36u <= len) {
                    const u32 timescale = read_be32(buf + i + 24u);
                    const u64 duration  = read_be64(buf + i + 28u);
                    if (timescale > 0u && duration > 0ULL) {
                        desc.timescale  = timescale;
                        desc.durationMs = static_cast<u32>((duration * 1000ULL) / timescale);
                    }
                }
                break;
            }
        }
        if (desc.durationMs > 0u && desc.fileSize > 0ULL) {
            desc.byteRate = static_cast<u32>((desc.fileSize * 1000ULL) / desc.durationMs);
        }
        desc.headerState = STREAM_HEADER_READY;
        return;
    }
    u32 mp3Start = 0u;
    if (buf[0] == 'I' && buf[1] == 'D' && buf[2] == '3' && len >= 10u) {
        const u32 tagSize = ((buf[6] & 0x7Fu) << 21) |
                            ((buf[7] & 0x7Fu) << 14) |
                            ((buf[8] & 0x7Fu) << 7)  |
                            (buf[9] & 0x7Fu);
        mp3Start = 10u + tagSize;
    }
    if (mp3Start + 4u < len) {
        static constexpr MultiArray<u16, 16> kBitratesKbps{ {
            0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
        } };
        for (u32 i = mp3Start; i + 4u < len; ++i) {
            if (buf[i] == 0xFFu && (buf[i + 1] & 0xE0u) == 0xE0u) {
                const u32 brIdx = (buf[i + 2] >> 4) & 0x0Fu;
                const u32 kbps = kBitratesKbps[brIdx];
                if (kbps > 0u) {
                    desc.containerKind   = 3u;
                    desc.dataStartOffset = i;
                    desc.byteRate        = (kbps * 1000u) >> 3;
                    if (desc.fileSize > i && desc.byteRate > 0u) {
                        desc.durationMs = static_cast<u32>(((desc.fileSize - i) * 1000ULL) / desc.byteRate);
                    }
                    break;
                }
            }
        }
    }
    desc.headerState = STREAM_HEADER_READY;
}

void DemuxPipeline::commit_chunk(u32 resourceId, u32 ringPageIdx, u64 byteStart, u32 actualBytes) noexcept {
    if (ringPageIdx >= STREAM_RING_PAGES) return;
    StreamRingPage& page = ringPages_[ringPageIdx];
    page.validBytes = stockcut::min(actualBytes, STREAM_CHUNK_BYTES);
    page.state = STREAM_PAGE_READY;
    page.lruTick = ++lruCounter_;
    StreamResourceDesc* desc = find_or_create_resource(resourceId, 0u);
    if (desc && byteStart == 0ULL && actualBytes >= 16u) {
        parse_header_slice(*desc, page.data.data(), page.validBytes);
    }
}

u32 DemuxPipeline::get_resource_duration_ms(u32 resourceId) const noexcept {
    const StreamResourceDesc* desc = find_resource(resourceId);
    return desc ? desc->durationMs : 0u;
}

u32 DemuxPipeline::get_resource_byte_rate(u32 resourceId) const noexcept {
    const StreamResourceDesc* desc = find_resource(resourceId);
    return desc ? desc->byteRate : 0u;
}
} // namespace stockcut

extern "C" {
stockcut::u32 wasm_stream_register_resource(stockcut::u32 resourceId, stockcut::u32 mediaType, double fileSize) {
    return stockcut::stream_pipeline().register_resource(
        resourceId, mediaType, fileSize > 0.0 ? static_cast<stockcut::u64>(fileSize) : 0ULL
    );
}
stockcut::u32 wasm_stream_schedule_at(double timeMs, stockcut::u32 isPlaying) {
    const stockcut::TimePoint tp = static_cast<stockcut::TimePoint>(timeMs);
    return (isPlaying != 0u)
        ? stockcut::stream_pipeline().schedule_at<true>(tp)
        : stockcut::stream_pipeline().schedule_at<false>(tp);
}
stockcut::u32 wasm_stream_get_request_count() {
    return stockcut::stream_pipeline().pending_request_count();
}
const stockcut::ByteRangeCmd* wasm_stream_get_requests_ptr() {
    return stockcut::stream_pipeline().pending_requests_ptr();
}
void wasm_stream_clear_requests() {
    stockcut::stream_pipeline().clear_pending_requests();
}
void wasm_stream_commit_chunk(stockcut::u32 resourceId, stockcut::u32 ringPageIdx, double byteStart, stockcut::u32 actualBytes) {
    stockcut::stream_pipeline().commit_chunk(
        resourceId, ringPageIdx, byteStart > 0.0 ? static_cast<stockcut::u64>(byteStart) : 0ULL, actualBytes
    );
}
stockcut::u32 wasm_stream_get_resource_duration_ms(stockcut::u32 resourceId) {
    return stockcut::stream_pipeline().get_resource_duration_ms(resourceId);
}
stockcut::u32 wasm_stream_get_hit_count() {
    return stockcut::stream_pipeline().hit_count();
}
stockcut::u32 wasm_stream_get_miss_count() {
    return stockcut::stream_pipeline().miss_count();
}
} // extern "C"
