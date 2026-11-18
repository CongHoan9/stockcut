// ============================================================================
// [ĐÁNH GIÁ ĐIỂM NGHẼN HỆ THỐNG]:
// 1. Phân mảnh dữ liệu bộ nhớ và truy cập L1 Cache rời rạc:
//    - Cấu trúc đối tượng rời rạc và việc kiểm tra phòng thủ lặp lại trong Hot Path 60 FPS
//      làm tăng tỉ lệ trượt L1 Cache và gây lãng phí chu kỳ vi xử lý.
// 2. Chi phí cấp phát heap động (Heap Thrashing):
//    - Cấp phát heap trong vòng lặp chính làm phát sinh áp lực dọn rác và phân mảnh RAM.
//
// [HƯỚNG KHẮC PHỤC TRIỆT ĐỂ — KHÔNG VÁ]:
// 1. Kiến trúc mảng phẳng DOD (Data-Oriented Design) & Căn lề Cacheline:
//    - Căn lề dữ liệu 64-byte (alignas(64)), nạp dữ liệu liên tục theo đường truyền SIMD.
// 2. Straight-line Execution & Zero-Allocation:
//    - Loại bỏ kiểm tra hợp lệ phòng thủ trong Hot Path; 0 malloc/free ở tần số 60 FPS.
// ============================================================================
// ============================================================================
// CoreEngine - Display List Implementation (Pha 5)
// ----------------------------------------------------------------------------
// [KỸ THUẬT STOCKFISH ÁP DỤNG]:
// 1. Static Command Buffer (Zero Dynamic Allocations)
//    - Kiểu bài toán: Tái sử dụng g_displayList 8KB không sinh Garbage Collector
//    - Tham chiếu Stockfish: Stockfish/src/movegen.cpp (MoveList static buffer)
// 2. Bitwise Occlusion Testing via Spatial Bitboard
//    - Kiểu bài toán: So sánh bitmask nhanh chóng để loại bỏ lệnh vẽ dư thừa
//    - Tham chiếu Stockfish: Stockfish/src/bitboard.cpp
// 3. MoveList-Style Topological Traversal
//    - Kiểu bài toán: Lặp mảng SceneNode phẳng và phát chuỗi lệnh tuần tự
//    - Tham chiếu Stockfish: Stockfish/src/movepick.cpp
// Tra cứu thêm tại: native/phan_loai_nghiep_vu_stockfish.txt (Mục 1, Mục 4 & Mục 10)
// ============================================================================
#include "display_list.h"
#include "data_structures.h"
#include "timeline/bitboard.h"
#include "geometry.h"
#include "style.h"
#include "filter.h"

namespace stockcut {

static DisplayList g_displayListInstance{};

DisplayList& DisplayList::instance() noexcept {
    return g_displayListInstance;
}

u32 DisplayList::cull(u64 occludedMask, double W, double H) noexcept {
    const u32 n = count();
    if (n == 0 || occludedMask == 0ULL || W <= 0.0 || H <= 0.0) return 0;
    u32 culled = 0;
    FastSceneBitboard<64> occluderState;
    occluderState.bits[0] = occludedMask;

    for (DisplayCommand* p = stream_.data(), *const end = p + n; p < end; ++p) {
        if (p->opCode == CMD_NOP) continue;
        if (p->opCode == CMD_PUSH_CLIP || p->opCode == CMD_POP_CLIP ||
            p->opCode == CMD_PUSH_FILTER || p->opCode == CMD_POP_FILTER) continue;

        const u64 mask = Bitboard::FromAABBT<SpatialCoverage::ExactAABB>(p->x0, p->y0, p->x1, p->y1, W, H);
        if (mask != 0ULL) {
            FastSceneBitboard<64> cmdState;
            cmdState.bits[0] = mask;
            cmdState.cull(occluderState);
            if (cmdState.bits[0] == 0ULL) {
                p->opCode = CMD_NOP;
                p->flags |= NODE_FLAG_CULLED;
                culled++;
            }
        }
    }
    if (culled > 0 && ++generation_ == 0) ++generation_;
    return culled;
}

u32* display_list_get_ptr() {
    return g_displayListInstance.u32_data();
}

u32 display_list_get_count() {
    return g_displayListInstance.count();
}

u32 display_list_generation() {
    return g_displayListInstance.generation();
}

void display_list_set_count(u32 count) {
    g_displayListInstance.set_count(count);
}

void display_list_reset() {
    g_displayListInstance.reset();
}

void display_list_terminate_sentinel() {
    g_displayListInstance.terminate_sentinel();
}

bool display_list_push(
    u16 opCode, u16 flags, u32 resourceId,
    float x0, float y0, float x1, float y1,
    float opacity, u16 styleSlot, u16 filterSlot
) {
    DisplayCommand cmd;
    cmd.opCode = opCode;
    cmd.flags = flags;
    cmd.resourceId = resourceId;
    cmd.x0 = x0;
    cmd.y0 = y0;
    cmd.x1 = x1;
    cmd.y1 = y1;
    cmd.opacity = opacity;
    cmd.styleSlot = styleSlot;
    cmd.filterSlot = filterSlot;
    return g_displayListInstance.push_next(cmd);
}

// Chèn không rẽ nhánh theo mô hình Stockfish LinearStream::push_next_if (0 JMP)
bool display_list_push_if(
    bool cond,
    u16 opCode, u16 flags, u32 resourceId,
    float x0, float y0, float x1, float y1,
    float opacity, u16 styleSlot, u16 filterSlot
) {
    DisplayCommand cmd;
    cmd.opCode = opCode;
    cmd.flags = flags;
    cmd.resourceId = resourceId;
    cmd.x0 = x0;
    cmd.y0 = y0;
    cmd.x1 = x1;
    cmd.y1 = y1;
    cmd.opacity = opacity;
    cmd.styleSlot = styleSlot;
    cmd.filterSlot = filterSlot;
    return g_displayListInstance.push_if(cond, cmd);
}

DisplayCommand* display_list_get_command(u32 idx) {
    return g_displayListInstance.get_command(idx);
}

u32 display_list_cull(u64 occludedMask, double W, double H) {
    return g_displayListInstance.cull(occludedMask, W, H);
}

u32 display_list_generate_from_scene(u32 nodeCount, float viewportW, float viewportH) {
    display_list_reset();
    if (nodeCount == 0) return 0;
    const u32 n = (nodeCount > MAX_SCENE_NODES) ? MAX_SCENE_NODES : nodeCount;
    SceneNode* nodes = geometry_get_nodes_ptr();
    PackedStyle* styles = style_get_arena();

    // Duyệt phẳng mảng SceneNode 0 -> n-1 (Topological Painter's Algorithm)
    for (u32 i = 0; i < n; ++i) {
        const SceneNode& node = nodes[i];
        if (!(node.flags & NODE_FLAG_VISIBLE)) continue;
        if (node.flags & NODE_FLAG_CULLED) continue;

        const u16 sSlot = (node.styleSlot < STYLE_ARENA_CAPACITY) ? node.styleSlot : 0u;
        const PackedStyle& style = styles[sSlot];
        const float opacity = style.opacity;
        if (opacity <= OPACITY_EPSILON) continue;

        const float x0 = node.aabbMinX;
        const float y0 = node.aabbMinY;
        const float x1 = node.aabbMaxX;
        const float y1 = node.aabbMaxY;

        const bool hasClip = (node.flags & NODE_FLAG_HAS_CLIP) != 0;
        const bool hasBox  = (style.activeFlags & (STYLE_BIT_BACKGROUND | STYLE_BIT_BORDER | STYLE_BIT_RADIUS)) != 0;
        const bool hasText = (style.activeFlags & STYLE_BIT_TYPOGRAPHY) != 0;
        const bool hasImg  = !(hasBox || hasText);

        display_list_push_if(hasClip, CMD_PUSH_CLIP, node.flags, node.clipNode, x0, y0, x1, y1, opacity, sSlot, 0);
        display_list_push_if(hasBox,  CMD_DRAW_RECT, node.flags, i,             x0, y0, x1, y1, opacity, sSlot, 0);
        display_list_push_if(hasText, CMD_DRAW_TEXT, node.flags, i,             x0, y0, x1, y1, opacity, sSlot, 0);
        display_list_push_if(hasImg,  CMD_DRAW_IMAGE, node.flags, i,            x0, y0, x1, y1, opacity, sSlot, 0);
    }

    return g_displayListInstance.count();
}

// ----------------------------------------------------------------------------
// COMPACT COMMAND RING ARENA (512 KB, Zero DisplayListSlot Header Overhead, O(1) Clear)
// ----------------------------------------------------------------------------
constexpr u32 DL_ARENA_SLOT_CAPACITY = 64u;
constexpr u32 DL_RING_CMD_CAPACITY   = 16384u; // 16384 * 32B = 512 KB

alignas(64) static MultiArray<DisplayCommand, DL_RING_CMD_CAPACITY> g_dlCmdRing{};
static u32 g_dlRingCursor = 0u;
static u32 g_dlRingEpoch  = 1u;

[[nodiscard]] static inline u32 compute_dl_key_tag(u64 key) noexcept {
    const u32 folded = static_cast<u32>(key ^ (key >> 32)) ^ (g_dlRingEpoch * 0x9E3779B9u);
    const u32 tag = (folded ^ (folded >> 11)) & 0x7FFu;
    return tag != 0u ? tag : 1u;
}

// Reports fixed storage reserved by the display-list cache for capacity audits.
usize display_list_cache_memory_bytes()
{
    return sizeof(g_dlCmdRing);
}

u32 display_list_cache_save(u64 key, u8 tier)
{
    (void)tier;
    const u32 cnt = g_displayListInstance.count();
    if (cnt > DL_ARENA_SLOT_CAPACITY)
        return DL_CACHE_INVALID;
    if (g_dlRingCursor + cnt > DL_RING_CMD_CAPACITY) {
        g_dlRingCursor = 0u;
        ++g_dlRingEpoch;
    }
    const u32 offset = g_dlRingCursor;
    g_dlRingCursor += cnt;
    const DisplayCommand* src = g_displayListInstance.data();
    DisplayCommand* dst = g_dlCmdRing.data() + offset;
    for (const DisplayCommand* p = src, *const end = src + cnt; p < end; ++p, ++dst) {
        *dst = *p;
    }
    const u32 tag = compute_dl_key_tag(key);
    return offset | (cnt << 14u) | (tag << 21u);
}

bool display_list_cache_restore(u32 slot, u64 expectedKey) {
    if (slot == DL_CACHE_INVALID) return false;
    const u32 offset = slot & 0x3FFFu;
    const u32 cnt    = (slot >> 14u) & 0x7Fu;
    const u32 tag    = (slot >> 21u) & 0x7FFu;
    if (tag == 0u || cnt > DL_ARENA_SLOT_CAPACITY || offset + cnt > DL_RING_CMD_CAPACITY)
        return false;
    if (expectedKey != 0ULL && tag != compute_dl_key_tag(expectedKey)) {
        return false;
    }
    g_displayListInstance.reset();
    const DisplayCommand* src = g_dlCmdRing.data() + offset;
    for (const DisplayCommand* p = src, *const end = src + cnt; p < end; ++p) {
        g_displayListInstance.push_next_fast(*p);
    }
    return true;
}

void display_list_cache_clear() {
    g_dlRingCursor = 0u;
    ++g_dlRingEpoch;
}

} // namespace stockcut

