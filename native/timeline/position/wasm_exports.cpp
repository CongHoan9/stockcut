// ============================================================================
// [ĐÁNH GIÁ ĐIỂM NGHẼN HỆ THỐNG]:
// 1. Duyệt cây phân cấp đệ quy trong Hot Path 60 FPS:
//    - Duyệt qua lại các node timeline làm phát sinh cache miss và chi phí con trỏ.
// 2. Kế thừa template lặp lại ở mỗi khung hình:
//    - Hỏi quan hệ cha-con liên tục làm giảm hiệu suất xử lý khung hình.
//
// [HƯỚNG KHẮC PHỤC TRIỆT ĐỂ — KHÔNG VÁ]:
// 1. Kế thừa 1 lần duy nhất ở Cold Path & Bộ nhớ chia sẻ:
//    - Con trỏ trực tiếp vào bộ nhớ phẳng của cha, đọc O(1) không duyệt cây.
// 2. Bitboard Layer Masking & Ngăn xếp lịch sử StateInfo:
//    - Lọc trạng thái layer bằng bitmask 64-bit và lưu vết vi sai kiểu Stockfish.
// ============================================================================
// Bottleneck: scalar C ABI exports marshal values one call at a time.
// Optimization path: batch project setup into shared-memory records only after the
// existing layout is versioned; keep the per-frame API allocation-free.
#include "position.h"
#include "core/string_parse.h"
#include "text/font_map.h"
#include "text/font_engine.h"
#include "renderer.h"
#include "config.h"
#include "undo.h"
#include "cut_project.h"
#include "manager.h"
#include "outline.h"
#include "effect/param_arena.h"
namespace stockcut {
namespace {
constexpr usize FONT_FAMILY_BUF_CAP    = 64;
constexpr usize CONFIG_SCRATCH_CAP     = 4096;
constexpr i32   INVALID_KF_OFFSET_MS   = -999999;
constexpr u32   INVALID_SLOT_MODE_SENT = 999u;
constexpr u32   INVALID_DEBUG_EVAL     = 9999u;
MultiArray<ItemPresentation, MAX_TIMELINE_SLOTS> g_itemPresentation{};
}
// Clears the position engine and its rewind-history cursor at project replacement.
WASM_EXPORT(wasm_pos_clear) void wasm_pos_clear() {
    timeline_position_reset();
}
// Copies one already-normalized JS timeline item into its fixed native slot.
WASM_EXPORT(wasm_pos_put_item) void wasm_pos_put_item(
    u32 slot, u32 uid, u8 type, u8 tier, u8 flags,
    i64 startMs, i64 endMs, float baseW, float baseH,
    float spinSpeed, float maskHole, float volume, i32 zIndex, u32 resourceId,
    float baseX, float baseY, float rotateDeg
) {
    if (slot >= MAX_TIMELINE_SLOTS) return;
    TimelineItem item{};
    item.uid = uid;
    item.type = static_cast<MediaType>(type);
    item.tier = tier;
    item.flags = flags;
    item.startMs = startMs;
    item.endMs = endMs;
    item.baseW = baseW;
    item.baseH = baseH;
    item.spinSpeed = spinSpeed;
    item.maskHole = maskHole;
    item.volume = volume;
    item.zIndex = zIndex;
    item.resourceId = resourceId;
    item.baseX = baseX;
    item.baseY = baseY;
    item.rotateDeg = rotateDeg;
    timeline_position_get_instance()->put_item(slot, item);
}
// Writes one normalized keyframe into the preallocated slot track.
WASM_EXPORT(wasm_pos_set_keyframe) void wasm_pos_set_keyframe(
    u32 slot, u32 keyframeIndex, i32 offsetMs, float opacity,
    float x, float y, float w, float h, float rotateDeg, float scale
) {
    if (slot >= MAX_TIMELINE_SLOTS || keyframeIndex >= MAX_KEYFRAMES_PER_SLOT) return;
    timeline_position_get_instance()->set_keyframe(
        slot, keyframeIndex, offsetMs, opacity, x, y, w, h, rotateDeg, scale);
}
// Configures keyframe metadata used to specialize a slot's runtime track mode.
WASM_EXPORT(wasm_pos_set_track_info) void wasm_pos_set_track_info(
    u32 slot, u32 count, u32 easing, u32 contentMask, u32 hasSpatial
) {
    if (slot >= MAX_TIMELINE_SLOTS) return;
    timeline_position_get_instance()->set_track_info(slot, count, easing, contentMask, hasSpatial);
}
// Removes a slot and its memberships from the native timeline bitboards.
WASM_EXPORT(wasm_pos_remove_item) void wasm_pos_remove_item(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return;
    timeline_position_get_instance()->remove_item(slot);
}
// Sets the known project duration used by timeline evaluation.
WASM_EXPORT(wasm_pos_set_duration) void wasm_pos_set_duration(i64 durationMs) {
    timeline_position_get_instance()->set_duration(durationMs);
}
// Advances to a requested timestamp and returns the resulting display-command count.
WASM_EXPORT(wasm_pos_step_to) u32 wasm_pos_step_to(i64 nextTimeMs) {
    return timeline_position_display_list(timeline_position_advance(nextTimeMs));
}
// Rewinds one valid history step and regenerates its display list.
WASM_EXPORT(wasm_pos_undo_step) u32 wasm_pos_undo_step() {
    Position<>* position = timeline_position_get_instance();
    if (!position->undo_step()) return 0;
    return position->generate_display_list(position->state());
}
// Sets canonical stage dimensions and rebuilds geometry for existing items.
WASM_EXPORT(wasm_pos_set_stage_dimensions) void wasm_pos_set_stage_dimensions(float w, float h) {
    timeline_position_get_instance()->set_stage_dimensions(w, h);
}
// Returns the active-slot bitboard for all media types.
WASM_EXPORT(wasm_pos_get_active_mask) u64 wasm_pos_get_active_mask() {
    return timeline_position_get_instance()->active_items();
}
// Returns the active audio/video-slot bitboard for Web Audio scheduling.
WASM_EXPORT(wasm_pos_get_active_audio_mask) u64 wasm_pos_get_active_audio_mask() {
    return timeline_position_get_instance()->active_media_items();
}
// Returns the active video/audio-slot bitboard for media synchronization.
WASM_EXPORT(wasm_pos_get_active_media_mask) u64 wasm_pos_get_active_media_mask() {
    return timeline_position_get_instance()->active_media_items();
}
// Returns the current timeline time in milliseconds.
WASM_EXPORT(wasm_pos_get_time) i64 wasm_pos_get_time() {
    return timeline_position_get_instance()->time();
}
// Reports whether any valid native timeline item is configured.
WASM_EXPORT(wasm_pos_has_items) u32 wasm_pos_has_items() {
    return timeline_position_get_instance()->has_items() ? 1 : 0;
}
WASM_EXPORT(wasm_pos_get_item_presentation_ptr) u32
wasm_pos_get_item_presentation_ptr(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) slot = 0;
    const TimelineItem& item = timeline_position_get_instance()->item(slot);
    const bool isNeutral = item.type == MediaType::Audio;
    constexpr u32 MAX_SAFE_TIER = static_cast<u32>(MediaTier::Continuous);
    const u32 safeTier = item.tier <= MAX_SAFE_TIER ? static_cast<u32>(item.tier) : MAX_SAFE_TIER;
    ItemPresentation& presentation = g_itemPresentation[slot];
    presentation.renderTier = isNeutral ? 0u :
        safeTier + 1u;
    presentation.flags = item.flags;
    presentation.maskHole = item.maskHole;
    presentation.rotateDeg = item.rotateDeg;
    presentation.spinSpeed = item.spinSpeed;
    presentation.volume = item.volume;
    const Position<>* position = timeline_position_get_instance();
    presentation.revealMode = position->slot_block(slot).get_value<u8, text_bit::REVEAL_MODE_0, 4>();
    presentation.revealDurationMs =
        position->get_text_reveal_duration_ms(slot);
    presentation.revealStaggerMs =
        position->get_text_reveal_stagger_ms(slot);
    return static_cast<u32>(reinterpret_cast<usize>(&presentation));
}
// Writes browser-normalized text style data into the native per-slot text arena.
WASM_EXPORT(wasm_pos_set_text_params) void wasm_pos_set_text_params(
    u32 slot,
    float fontSize, float lineHeight, float letterSpacing, float strokeWidth,
    float anchorX, float anchorY, u32 color, u32 strokeColor,
    u32 shadowColor, float shadowBlur, float shadowOffsetX, float shadowOffsetY,
    u32 fontWeight, u32 textAlign, u32 fontCategory,
    u32 revealDurationMs, u32 revealMode, u32 textTransform, u32 fontHash,
    u32 lineCount, u32 styleBits
) {
    if (slot >= MAX_TIMELINE_SLOTS) return;
    // styleBits: bit 0 = italic, bit 1-3 = TextDecoration, bit 4 = inheritColor
    Position<>* pos = timeline_position_get_instance();
    TextBlock tb = pos->slot_block(slot);
    tb.set_value<text_reg::FONT_SIZE>(fontSize);
    tb.set_value<text_reg::LINE_HEIGHT>(lineHeight);
    tb.set_value<text_reg::LETTER_SPACING>(letterSpacing);
    tb.set_value<text_reg::STROKE_WIDTH>(strokeWidth);
    tb.set_value<text_reg::ANCHOR_X>(anchorX);
    tb.set_value<text_reg::ANCHOR_Y>(anchorY);
    tb.set_value<text_reg::COLOR>(color);
    tb.set_value<text_reg::STROKE_COLOR>(strokeColor);
    tb.set_value<text_reg::SHADOW_COLOR>(shadowColor);
    tb.set_value<text_reg::SHADOW_BLUR>(shadowBlur);
    tb.set_value<text_reg::SHADOW_OFFSET_X>(shadowOffsetX);
    tb.set_value<text_reg::SHADOW_OFFSET_Y>(shadowOffsetY);
    tb.set_value<text_reg::FONT_WEIGHT>(fontWeight);
    tb.set_value<text_bit::TEXT_ALIGN_LO, 2>(static_cast<u8>(textAlign));
    tb.set_value<text_bit::FONT_CATEGORY_LO, 3>(static_cast<u8>(fontCategory));
    tb.set_value<text_reg::REVEAL_DURATION_MS>(static_cast<float>(revealDurationMs));
    tb.set_value<text_bit::REVEAL_MODE_0, 4>(static_cast<u8>(revealMode));
    tb.set_value<text_bit::TEXT_TRANSFORM_LO, 2>(static_cast<u8>(textTransform));
    tb.set_value<text_reg::FONT_HASH>(fontHash);
    tb.set_value<text_bit::LINE_COUNT_BASE, 8>(static_cast<u16>(lineCount > 1 ? lineCount : 1));
    const bool isItalic = (styleBits & 1u) != 0;
    tb.set_value<text_bit::IS_ITALIC, 1>(isItalic);
    tb.set_value<text_bit::DECORATION_UNDERLINE, 3>(static_cast<u8>((styleBits >> 1) & TextDecoration::Mask));
    tb.set_value<text_bit::INHERIT_COLOR, 1>(((styleBits >> 4) & 1u) != 0);
    const u16 resolvedFontFileId = isItalic
        ? font::FontMapRegistry::instance().resolveFontFileIdByHash<true>(fontHash, static_cast<u16>(fontWeight))
        : font::FontMapRegistry::instance().resolveFontFileIdByHash<false>(fontHash, static_cast<u16>(fontWeight));
    tb.set_value<text_reg::RESOLVED_FONT_ID>(static_cast<u32>(resolvedFontFileId));
    pos->set_slot_text_block(slot, tb);
}
// Exposes the slot_blocks_arena for zero-copy JS reads of Text & Media blocks.
WASM_EXPORT(wasm_pos_get_text_params_ptr) u32 wasm_pos_get_text_params_ptr() {
    return static_cast<u32>(reinterpret_cast<usize>(
        timeline_position_get_instance()->text_params_arena()));
}
// Exposes the slot_blocks_arena for zero-copy JS reads of Path & Media blocks.
WASM_EXPORT(wasm_pos_get_path_params_ptr) u32 wasm_pos_get_path_params_ptr() {
    return static_cast<u32>(reinterpret_cast<usize>(
        timeline_position_get_instance()->slot_blocks_arena()));
}
// Exposes the SVG Path d string for a given slot.
WASM_EXPORT(wasm_pos_get_path_string_ptr) u32 wasm_pos_get_path_string_ptr(u32 slot) {
    return static_cast<u32>(reinterpret_cast<usize>(
        path::PathStringArena::instance().get_path(slot)));
}
// Configures per-slot bass-reactive timing and color count.
WASM_EXPORT(wasm_pos_set_bass_params) void wasm_pos_set_bass_params(
    u32 slot, float hop, u32 colorCount, u32 isBassActive
) {
    if (slot >= MAX_TIMELINE_SLOTS) return;
    timeline_position_get_instance()->set_bass_params(slot, hop, colorCount, isBassActive);
}
// Writes one theme color pair into the fixed bass color table.
WASM_EXPORT(wasm_pos_set_bass_color) void wasm_pos_set_bass_color(
    u32 slot, u32 colorIdx, u32 bgRgba, u32 textRgba
) {
    if (slot >= MAX_TIMELINE_SLOTS || colorIdx >= MAX_BASS_COLORS) return;
    timeline_position_get_instance()->set_bass_color(slot, colorIdx, bgRgba, textRgba);
}
// Copies externally prepared hit indices into the preallocated native hit buffer.
WASM_EXPORT(wasm_pos_set_bass_hits) void wasm_pos_set_bass_hits(
    u32 slot, const u16* hits, u32 count
) {
    if (slot >= MAX_TIMELINE_SLOTS || !hits) return;
    timeline_position_get_instance()->set_bass_hit_data(slot, hits, count);
}
// Resolves bass colors/hit index into optional caller-provided output cells.
WASM_EXPORT(wasm_pos_resolve_bass_colors) void wasm_pos_resolve_bass_colors(
    u32 slot, double nowMs, u32* outBg, u32* outText, u32* outHitIdx
) {
    if (slot >= MAX_TIMELINE_SLOTS) return;
    Position<>* position = timeline_position_get_instance();
    const TimePoint timeMs = static_cast<TimePoint>(nowMs);
    if (outBg) *outBg = position->resolve_bass_bg_color(slot, timeMs);
    if (outText) *outText = position->resolve_bass_text_color(slot, timeMs);
    if (outHitIdx) *outHitIdx = position->resolve_bass_hit_index(slot, timeMs);
}
// Exposes the contiguous 4 KB Evaluated arena for zero-copy JS/VBO reads.
WASM_EXPORT(wasm_pos_get_evaluated_slots_ptr) u32 wasm_pos_get_evaluated_slots_ptr() {
    return static_cast<u32>(reinterpret_cast<usize>(
        timeline_position_get_instance()->evaluated_slots_arena()));
}
WASM_EXPORT(wasm_pos_update_slot_text_bounds) void wasm_pos_update_slot_text_bounds(u32 slot, float realW, float realH) {
    if (slot >= MAX_TIMELINE_SLOTS || realW <= 0.0f || realH <= 0.0f) return;
    timeline_position_get_instance()->update_slot_text_bounds(slot, realW, realH);
}
WASM_EXPORT(wasm_pos_get_item_start_ms) i64 wasm_pos_get_item_start_ms(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return 0;
    return timeline_position_get_instance()->item(slot).startMs;
}
WASM_EXPORT(wasm_pos_get_item_end_ms) i64 wasm_pos_get_item_end_ms(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return 0;
    return timeline_position_get_instance()->item(slot).endMs;
}
// Font Variant Mapping Table (Bản đồ hóa Font Variant) WASM Exports
WASM_EXPORT(wasm_font_map_load_manifest) u32 wasm_font_map_load_manifest(const char* jsonPtr, u32 jsonLen) {
    if (!jsonPtr || jsonLen == 0) return 0;
    return font::FontMapRegistry::instance().loadManifestJson(jsonPtr, jsonLen) ? 1 : 0;
}
WASM_EXPORT(wasm_font_map_get_upload_ptr) u32 wasm_font_map_get_upload_ptr() {
    return static_cast<u32>(reinterpret_cast<usize>(
        font::FontMapRegistry::instance().getUploadBuffer()));
}
WASM_EXPORT(wasm_font_map_get_upload_capacity) u32 wasm_font_map_get_upload_capacity() {
    return font::FontMapRegistry::instance().getUploadCapacity();
}
WASM_EXPORT(wasm_font_map_commit_file) u32 wasm_font_map_commit_file(const char* pathPtr, u32 pathLen, u32 byteCount) {
    if (!pathPtr || pathLen == 0 || byteCount == 0) return 0;
    const u32 fid = font::FontMapRegistry::instance().commitUpload(pathPtr, pathLen, byteCount);
    if (fid != 0) Render::invalidate_all_text_caches();
    return fid;
}
WASM_EXPORT(wasm_font_map_resolve_file) u32 wasm_font_map_resolve_file(const char* familyPtr, u32 familyLen, u32 weight, u32 isItalic) {
    MultiArray<char, FONT_FAMILY_BUF_CAP> famBuf{};
    if (familyPtr && familyLen > 0) {
        const u32 len = min(familyLen, static_cast<u32>(FONT_FAMILY_BUF_CAP - 1u));
        std::memcpy(famBuf.data(), familyPtr, len);
        famBuf[len] = '\0';
    }
    return (isItalic != 0)
        ? font::FontMapRegistry::instance().resolveFontFileId<true>(famBuf.data(), static_cast<u16>(weight))
        : font::FontMapRegistry::instance().resolveFontFileId<false>(famBuf.data(), static_cast<u16>(weight));
}
WASM_EXPORT(wasm_font_map_get_file_buffer_ptr) u32 wasm_font_map_get_file_buffer_ptr(u32 fileId) {
    const u8* buf = font::FontMapRegistry::instance().getFileBuffer(static_cast<u16>(fileId));
    return static_cast<u32>(reinterpret_cast<usize>(buf));
}
WASM_EXPORT(wasm_font_map_get_file_buffer_size) u32 wasm_font_map_get_file_buffer_size(u32 fileId) {
    return font::FontMapRegistry::instance().getFileSize(static_cast<u16>(fileId));
}
WASM_EXPORT(wasm_font_map_get_file_key) u32 wasm_font_map_get_file_key(u32 fileId) {
    return font::FontMapRegistry::instance().getFileKey(static_cast<u16>(fileId));
}
WASM_EXPORT(wasm_font_map_get_file_path) u32 wasm_font_map_get_file_path(u32 fileId, char* outBuf, u32 maxLen) {
    if (!outBuf || maxLen == 0) return 0;
    const font::FontFileRecord* rec = font::FontMapRegistry::instance().getFileRecord(static_cast<u16>(fileId));
    if (!rec) return 0;
    return static_cast<u32>(copy_cstr_bounded(outBuf, maxLen, rec->path));
}
WASM_EXPORT(wasm_font_map_get_family_count) u32 wasm_font_map_get_family_count() {
    return font::FontMapRegistry::instance().familyCount();
}
WASM_EXPORT(wasm_font_map_get_file_count) u32 wasm_font_map_get_file_count() {
    return font::FontMapRegistry::instance().fileCount();
}
WASM_EXPORT(wasm_font_map_load_manifest_file) u32 wasm_font_map_load_manifest_file(const char* pathPtr) {
    const char* path = (pathPtr && pathPtr[0]) ? pathPtr : DEFAULT_FONT_MANIFEST_PATH;
    return font::FontMapRegistry::instance().loadManifestFile(path) ? 1 : 0;
}
WASM_EXPORT(wasm_font_map_load_file_direct) u32 wasm_font_map_load_file_direct(const char* pathPtr) {
    if (!pathPtr || !pathPtr[0]) return 0;
    return font::FontMapRegistry::instance().loadFontFileDirect(pathPtr);
}
WASM_EXPORT(wasm_font_map_load_all_fonts) u32 wasm_font_map_load_all_fonts() {
    return font::FontMapRegistry::instance().loadAllManifestFonts();
}
WASM_EXPORT(wasm_text_font_load_by_file_id) u32 wasm_text_font_load_by_file_id(u32 fileId) {
    return text_font_load_by_file_id(static_cast<u16>(fileId));
}
WASM_EXPORT(wasm_pos_get_slot_resource_id) u32 wasm_pos_get_slot_resource_id(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return 0;
    return timeline_position_get_instance()->item(slot).resourceId;
}
WASM_EXPORT(wasm_pos_get_defined_props) u64 wasm_pos_get_defined_props(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return 0ULL;
    return timeline_position_get_instance()->defined_props(slot);
}
WASM_EXPORT(wasm_pos_get_keyframed_props) u64 wasm_pos_get_keyframed_props(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return 0ULL;
    return timeline_position_get_instance()->keyframed_props(slot);
}
WASM_EXPORT(wasm_pos_get_static_zobrist_key) Key wasm_pos_get_static_zobrist_key(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return 0ULL;
    return timeline_position_get_instance()->static_zobrist_key(slot);
}
WASM_EXPORT(wasm_pos_get_defined_props_word) u64 wasm_pos_get_defined_props_word(u32 slot, u32 wordIdx) {
    if (slot >= MAX_TIMELINE_SLOTS || wordIdx != 0u) return 0ULL;
    return timeline_position_get_instance()->defined_props(slot);
}
WASM_EXPORT(wasm_pos_get_keyframed_props_word) u64 wasm_pos_get_keyframed_props_word(u32 slot, u32 wordIdx) {
    if (slot >= MAX_TIMELINE_SLOTS || wordIdx != 0u) return 0ULL;
    return timeline_position_get_instance()->keyframed_props(slot);
}
WASM_EXPORT(wasm_pos_get_total_defined_count) u32 wasm_pos_get_total_defined_count() {
    Position<>* pos = timeline_position_get_instance();
    u32 total = 0;
    for (Bitboard occ = pos->populated_slots(); occ;) {
        total += BitOps<u64>::popcount64(pos->defined_props(occ.pop_lsb()));
    }
    return total;
}
WASM_EXPORT(wasm_pos_get_total_keyframed_count) u32 wasm_pos_get_total_keyframed_count() {
    Position<>* pos = timeline_position_get_instance();
    u32 total = 0;
    for (Bitboard occ = pos->populated_slots(); occ;) {
        total += BitOps<u64>::popcount64(pos->keyframed_props(occ.pop_lsb()));
    }
    return total;
}
WASM_EXPORT(wasm_pos_get_static_slot_count) u32 wasm_pos_get_static_slot_count() {
    Position<>* pos = timeline_position_get_instance();
    u32 count = 0;
    for (Bitboard occ = pos->populated_slots(); occ;) {
        count += (pos->keyframed_props(occ.pop_lsb()) == 0ULL) ? 1u : 0u;
    }
    return count;
}
WASM_EXPORT(wasm_pos_get_slot_media_type) u32 wasm_pos_get_slot_media_type(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return 0;
    return static_cast<u32>(timeline_position_get_instance()->item(slot).type);
}
WASM_EXPORT(wasm_renderer_invalidate_text_caches) void wasm_renderer_invalidate_text_caches() {
    Render::invalidate_all_text_caches();
}
WASM_EXPORT(wasm_pos_get_track_count) u32 wasm_pos_get_track_count(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return 0;
    return timeline_position_get_instance()->get_track(slot).count;
}
WASM_EXPORT(wasm_pos_get_track_content_mask) u32 wasm_pos_get_track_content_mask(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return 0;
    return timeline_position_get_instance()->get_track(slot).contentMask;
}
WASM_EXPORT(wasm_pos_get_track_frame_offset) i32 wasm_pos_get_track_frame_offset(u32 slot, u32 kfIdx) {
    if (slot >= MAX_TIMELINE_SLOTS || kfIdx >= MAX_KEYFRAMES_PER_SLOT) return INVALID_KF_OFFSET_MS;
    return timeline_position_get_instance()->keyframe_offset_ms(slot, kfIdx);
}
WASM_EXPORT(wasm_pos_get_track_active_steps) u32 wasm_pos_get_track_active_steps(u32 slot, i64 nowMs) {
    if (slot >= MAX_TIMELINE_SLOTS) return 0;
    return timeline_position_get_instance()->resolve_active_kf_content_steps(slot, static_cast<TimePoint>(nowMs));
}
WASM_EXPORT(wasm_pos_get_slot_mode) u32 wasm_pos_get_slot_mode(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return INVALID_SLOT_MODE_SENT;
    return static_cast<u32>(timeline_position_get_instance()->mode_of(slot));
}
WASM_EXPORT(wasm_pos_get_by_mode_bb) u64 wasm_pos_get_by_mode_bb(u32 mode) {
    if (mode >= TRACK_MODE_NB) return 0;
    return timeline_position_get_instance()->by_mode_bb_array()[mode];
}
WASM_EXPORT(wasm_pos_get_state_dirty_bb) u64 wasm_pos_get_state_dirty_bb() {
    StateInfo<>* st = timeline_position_get_instance()->state();
    return st ? static_cast<u64>(st->dirtyBB) : 0ULL;
}

WASM_EXPORT(wasm_pos_get_state_occluded_bb) u64 wasm_pos_get_state_occluded_bb() {
    StateInfo<>* st = timeline_position_get_instance()->state();
    return st ? static_cast<u64>(st->occludedBB) : 0ULL;
}

WASM_EXPORT(wasm_pos_get_state_occluded_slots_bb) u64 wasm_pos_get_state_occluded_slots_bb() {
    StateInfo<>* st = timeline_position_get_instance()->state();
    return st ? static_cast<u64>(st->occludedSlotsBB) : 0ULL;
}

WASM_EXPORT(wasm_pos_get_static_occluded_slots_bb) u64 wasm_pos_get_static_occluded_slots_bb() {
    Position<>* pos = timeline_position_get_instance();
    return pos ? pos->static_occluded_slots_bb().word(0) : 0ULL;
}

WASM_EXPORT(wasm_pos_is_slot_fully_occluded) u32 wasm_pos_is_slot_fully_occluded(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return 0;
    Position<>* pos = timeline_position_get_instance();
    return (pos && pos->is_slot_fully_occluded(slot)) ? 1 : 0;
}

WASM_EXPORT(wasm_pos_is_slot_partially_occluded) u32 wasm_pos_is_slot_partially_occluded(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return 0;
    Position<>* pos = timeline_position_get_instance();
    return (pos && pos->is_slot_partially_occluded(slot)) ? 1 : 0;
}

WASM_EXPORT(wasm_pos_get_slot_visible_tiles) u64 wasm_pos_get_slot_visible_tiles(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return 0ULL;
    Position<>* pos = timeline_position_get_instance();
    return pos ? pos->slot_visible_tiles(slot).value : 0ULL;
}

WASM_EXPORT(wasm_pos_get_slot_scissor_ptr) u32 wasm_pos_get_slot_scissor_ptr(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return 0;
    Position<>* pos = timeline_position_get_instance();
    return pos ? static_cast<u32>(reinterpret_cast<usize>(&pos->slot_scissor_box(slot))) : 0;
}

WASM_EXPORT(wasm_pos_get_slot_scissor_box) void wasm_pos_get_slot_scissor_box(u32 slot, float* outBox) {
    if (slot >= MAX_TIMELINE_SLOTS || !outBox) return;
    Position<>* pos = timeline_position_get_instance();
    if (!pos) return;
    const Rect<float>& box = pos->slot_scissor_box(slot);
    outBox[0] = box.x0;
    outBox[1] = box.y0;
    outBox[2] = box.x1;
    outBox[3] = box.y1;
}

WASM_EXPORT(wasm_pos_get_z_shadow_bb) u64 wasm_pos_get_z_shadow_bb(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return 0ULL;
    Position<>* pos = timeline_position_get_instance();
    return pos ? pos->z_shadow_bb(slot).word(0) : 0ULL;
}

WASM_EXPORT(wasm_pos_blockers_bb) u64 wasm_pos_blockers_bb() {
    Position<>* pos = timeline_position_get_instance();
    return pos ? pos->static_fullscreen_blockers_bb().word(0) : 0ULL;
}
WASM_EXPORT(wasm_pos_get_static_fullscreen_blockers_bb) u64 wasm_pos_get_static_fullscreen_blockers_bb() {
    return wasm_pos_blockers_bb();
}
WASM_EXPORT(wasm_pos_debug_eval) u32 wasm_pos_debug_eval(u32 slot, i64 nowMs) {
    if (slot >= MAX_TIMELINE_SLOTS) return INVALID_DEBUG_EVAL;
    Position<>* pos = timeline_position_get_instance();
    u16 flags = 0;
    u16 filter = 0;
    pos->evaluate_text_state(slot, static_cast<TimePoint>(nowMs), flags, filter);
    return (static_cast<u32>(flags) << 16) | filter;
}
WASM_EXPORT(wasm_pos_debug_step_info) u32 wasm_pos_debug_step_info(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return 0;
    Position<>* pos = timeline_position_get_instance();
    StateInfo<>* st = pos->state();
    if (!st) return 0;
    const u32 inActive = (st->activeBB & (1ULL << slot)) ? 1 : 0;
    const u32 inDirty = (st->dirtyBB & (1ULL << slot)) ? 1 : 0;
    const u32 mode = static_cast<u32>(pos->mode_of(slot));
    return (inActive << 0) | (inDirty << 1) | (mode << 4);
}
WASM_EXPORT(wasm_pos_debug_slot_filter) u32 wasm_pos_debug_slot_filter(u32 slot) {
    Position<>* pos = timeline_position_get_instance();
    if (slot >= MAX_TIMELINE_SLOTS) return 0;
    return pos->evaluated_slots_arena()[slot].filter;
}
WASM_EXPORT(wasm_pos_debug_slot_flags) u32 wasm_pos_debug_slot_flags(u32 slot) {
    Position<>* pos = timeline_position_get_instance();
    if (slot >= MAX_TIMELINE_SLOTS) return 0;
    return pos->evaluated_slots_arena()[slot].flags;
}
WASM_EXPORT(wasm_pos_debug_check4) u32 wasm_pos_debug_check4(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return INVALID_U8;
    Position<>* pos = timeline_position_get_instance();
    StateInfo<>* st = pos->state();
    if (!st) return INVALID_U8;
    const u32 a = (st->dirtyBB & (1ULL << slot)) ? 1u : 0u;
    const u32 b = (st->activeBB & (1ULL << slot)) ? 1u : 0u;
    const u32 c = (pos->by_type_bb<MediaType::Lyric>() & (1ULL << slot)) ? 1u : 0u;
    const u32 d = (pos->by_mode_bb<TrackMode::Procedural>() & (1ULL << slot)) ? 1u : 0u;
    return a | (b << 1) | (c << 2) | (d << 3);
}
WASM_EXPORT(wasm_pos_debug_pointers) u32 wasm_pos_debug_pointers(u32 idx) {
    Position<>* pos = timeline_position_get_instance();
    if (idx == 0) return static_cast<u32>(reinterpret_cast<usize>(pos));
    if (idx == 1) return static_cast<u32>(reinterpret_cast<usize>(pos->state()));
    return 0;
}
WASM_EXPORT(wasm_pos_get_by_nature_bb) u64 wasm_pos_get_by_nature_bb(u32 nature) {
    if (nature >= DYNAMIC_NATURE_NB) return 0;
    return timeline_position_get_instance()->by_nature_bb_array()[nature];
}
WASM_EXPORT(wasm_pos_get_by_dynamic_axis_bb) u64 wasm_pos_get_by_dynamic_axis_bb(u32 axis) {
    if (axis >= DYNAMIC_AXIS_NB) return 0;
    return timeline_position_get_instance()->by_dynamic_axis_bb_array()[axis];
}
WASM_EXPORT(wasm_pos_get_varying_props_mask) u32 wasm_pos_get_varying_props_mask(u32 slot) {
    if (slot >= MAX_TIMELINE_SLOTS) return 0;
    return timeline_position_get_instance()->varying_props_mask(slot);
}
WASM_EXPORT(wasm_pos_get_chunk_kf_active_bb) u64 wasm_pos_get_chunk_kf_active_bb() {
    return timeline_position_get_instance()->chunk_kf_active_bb();
}
WASM_EXPORT(wasm_pos_get_chunk_base_zobrist_lo) u32 wasm_pos_get_chunk_base_zobrist_lo() {
    return static_cast<u32>(timeline_position_get_instance()->chunk_base_zobrist() & INVALID_U32);
}
WASM_EXPORT(wasm_pos_get_chunk_base_zobrist_hi) u32 wasm_pos_get_chunk_base_zobrist_hi() {
    return static_cast<u32>((timeline_position_get_instance()->chunk_base_zobrist() >> 32) & INVALID_U32);
}
WASM_EXPORT(wasm_pos_get_running_dynamic_zobrist_lo) u32 wasm_pos_get_running_dynamic_zobrist_lo() {
    return static_cast<u32>(timeline_position_get_instance()->running_dynamic_zobrist() & INVALID_U32);
}
WASM_EXPORT(wasm_pos_get_running_dynamic_zobrist_hi) u32 wasm_pos_get_running_dynamic_zobrist_hi() {
    return static_cast<u32>((timeline_position_get_instance()->running_dynamic_zobrist() >> 32) & INVALID_U32);
}
WASM_EXPORT(wasm_pos_get_state_zobrist_lo) u32 wasm_pos_get_state_zobrist_lo() {
    StateInfo<>* st = timeline_position_get_instance()->state();
    return st ? static_cast<u32>(st->key & INVALID_U32) : 0U;
}
WASM_EXPORT(wasm_pos_get_state_zobrist_hi) u32 wasm_pos_get_state_zobrist_hi() {
    StateInfo<>* st = timeline_position_get_instance()->state();
    return st ? static_cast<u32>((st->key >> 32) & INVALID_U32) : 0U;
}

// ============================================================================
// Stockfish-Style ConfigOptions & OptionsMap WASM Exports
// ============================================================================
static MultiArray<char, CONFIG_SCRATCH_CAP> g_configScratch{};

WASM_EXPORT(wasm_config_get_scratch_ptr) char* wasm_config_get_scratch_ptr() {
    return g_configScratch.data();
}

WASM_EXPORT(wasm_config_get_scratch_capacity) u32 wasm_config_get_scratch_capacity() {
    return static_cast<u32>(CONFIG_SCRATCH_CAP);
}

WASM_EXPORT(wasm_config_init) void wasm_config_init() {
    engineManager().config_options().init_defaults();
}

WASM_EXPORT(wasm_config_get_json) u32 wasm_config_get_json(char* outBuf, u32 maxLen) {
    if (!outBuf || maxLen == 0) {
        outBuf = g_configScratch.data();
        maxLen = static_cast<u32>(CONFIG_SCRATCH_CAP);
    }
    return engineManager().config_options().to_json(outBuf, maxLen);
}

WASM_EXPORT(wasm_config_set_json) u32 wasm_config_set_json(const char* jsonStr, u32 len) {
    if (!jsonStr || len == 0) return 0;
    const bool ok = cutProject().do_edit<CONFIG>(jsonStr, len);
    return ok ? 1U : 0U;
}

WASM_EXPORT(wasm_config_get_str) u32 wasm_config_get_str(const char* key, char* outBuf, u32 maxLen) {
    if (!key) return 0;
    if (!outBuf || maxLen == 0) {
        outBuf = g_configScratch.data();
        maxLen = static_cast<u32>(CONFIG_SCRATCH_CAP);
    }
    const ConfigOption* opt = engineManager().config_options().find(key);
    if (!opt) return 0;
    return static_cast<u32>(copy_cstr_bounded(outBuf, maxLen, opt->string_val()));
}

WASM_EXPORT(wasm_config_set_str) u32 wasm_config_set_str(const char* key, const char* val) {
    if (!key || !val) return 0;
    return engineManager().config_options().set(key, val) ? 1U : 0U;
}

WASM_EXPORT(wasm_config_get_double) double wasm_config_get_double(const char* key, double defaultVal) {
    if (!key) return defaultVal;
    const ConfigOption* opt = engineManager().config_options().find(key);
    return opt ? opt->double_val() : defaultVal;
}

WASM_EXPORT(wasm_config_set_double) void wasm_config_set_double(const char* key, double val) {
    if (key) engineManager().config_options().set(key, val);
}

// ============================================================================
// Unified do_edit<Action> & Stockfish StateInfo Undo/Redo WASM Exports
// ============================================================================
static HistoryDirties g_lastEditDirty{};

WASM_EXPORT(wasm_project_do_edit) u32 wasm_project_do_edit(u32 actionType, const char* jsonStr, u32 len) {
    if (!jsonStr || len == 0) return 0;
    const bool ok = dispatch_do_edit(actionType, jsonStr, len, &g_lastEditDirty);
    return ok ? 1U : 0U;
}

WASM_EXPORT(wasm_project_undo) u32 wasm_project_undo() {
    const bool ok = cutProject().step_edit<false>(&g_lastEditDirty);
    return ok ? 1U : 0U;
}

WASM_EXPORT(wasm_project_redo) u32 wasm_project_redo() {
    const bool ok = cutProject().step_edit<true>(&g_lastEditDirty);
    return ok ? 1U : 0U;
}

WASM_EXPORT(wasm_project_can_undo) u32 wasm_project_can_undo() {
    return cutProject().can_undo() ? 1U : 0U;
}

WASM_EXPORT(wasm_project_can_redo) u32 wasm_project_can_redo() {
    return cutProject().can_redo() ? 1U : 0U;
}

WASM_EXPORT(wasm_project_get_last_dirty_bb) u64 wasm_project_get_last_dirty_bb() {
    return g_lastEditDirty.all_dirty_bb();
}

WASM_EXPORT(wasm_project_get_step_count) u32 wasm_project_get_step_count() {
    return ProjectHistory::instance().step_count();
}

// ============================================================================
// Selection Outline Engine WASM Exports (GEMINI.md Rule 9: JS Zero-Business)
// ============================================================================
static timeline::OutlineEngine s_selectionEngine;

WASM_EXPORT(wasm_selection_get_input_indices_ptr) u32* wasm_selection_get_input_indices_ptr() {
    return s_selectionEngine.inputIndices.data();
}

WASM_EXPORT(wasm_selection_compute_outlines) u32 wasm_selection_compute_outlines(u32 count) {
    if (count == 0) {
        s_selectionEngine.segmentCount = 0;
        return 0;
    }
    if (count > timeline::OutlineEngine::MAX_SELECTED_INPUT) count = timeline::OutlineEngine::MAX_SELECTED_INPUT;
    Position<>* pos = timeline_position_get_instance();
    return s_selectionEngine.compute_outlines(pos, count);
}

WASM_EXPORT(wasm_selection_get_segments_ptr) const timeline::SelectionSegment* wasm_selection_get_segments_ptr() {
    return s_selectionEngine.segments.data();
}

WASM_EXPORT(wasm_selection_get_segment_count) u32 wasm_selection_get_segment_count() {
    return s_selectionEngine.segmentCount;
}

WASM_EXPORT(wasm_selection_hit_test) i32 wasm_selection_hit_test(float stageX, float stageY) {
    Position<>* pos = timeline_position_get_instance();
    return s_selectionEngine.hit_test(pos, stageX, stageY);
}

WASM_EXPORT(wasm_selection_marquee_hit_test) u32 wasm_selection_marquee_hit_test(float x0, float y0, float x1, float y1) {
    Position<>* pos = timeline_position_get_instance();
    return s_selectionEngine.marquee_hit_test(pos, x0, y0, x1, y1);
}

WASM_EXPORT(wasm_selection_get_hit_indices_ptr) const u32* wasm_selection_get_hit_indices_ptr() {
    return s_selectionEngine.hitIndices.data();
}

WASM_EXPORT(wasm_pos_get_top_level_count) u32 wasm_pos_get_top_level_count() {
    Position<>* pos = timeline_position_get_instance();
    return pos ? pos->get_top_level_count() : 0u;
}

WASM_EXPORT(wasm_render_selection_overlay) void wasm_render_selection_overlay(
    float stageW, float stageH, float dx, float dy, float lineThick
) {
    Render::render_selection_overlay(
        stageW, stageH,
        s_selectionEngine.segments.data(), s_selectionEngine.segmentCount,
        dx, dy,
        lineThick
    );
}

WASM_EXPORT(wasm_clear_selection_overlay) void wasm_clear_selection_overlay(float stageW, float stageH) {
    s_selectionEngine.segmentCount = 0;
    s_selectionEngine.hitCount = 0;
    Render::render_selection_overlay(
        stageW, stageH,
        nullptr, 0, 0.0f, 0.0f,
        2.0f
    );
}

WASM_EXPORT(wasm_renderer_ensure_slot_text_shaped) void wasm_renderer_ensure_slot_text_shaped(u32 slot, float sx) {
    Render::ensure_slot_text_shaped(slot, sx);
}

WASM_EXPORT(wasm_renderer_get_slot_glyph_cache) u32 wasm_renderer_get_slot_glyph_cache(u32 slot) {
    return Render::get_slot_glyph_cache(slot);
}

WASM_EXPORT(wasm_renderer_get_glyph_cache_count) u32 wasm_renderer_get_glyph_cache_count(u32 cacheSlot) {
    return Render::get_glyph_cache_count(cacheSlot);
}

// ============================================================================
// 215-Effect Catalog & EffectParamArena WASM Exports (Zero-Enum / Direct Block)
// ============================================================================
WASM_EXPORT(wasm_effect_catalog_clear) void wasm_effect_catalog_clear() {
    effect_catalog::global_effect_param_arena().clear();
}

WASM_EXPORT(wasm_effect_catalog_find_id) u32 wasm_effect_catalog_find_id(const char* nameOrPath) {
    return effect_catalog::EffectParamArena<256>::find_effect_blob_offset(nameOrPath);
}

WASM_EXPORT(wasm_effect_catalog_instantiate) u32 wasm_effect_catalog_instantiate(u32 ordinalOrBlobOff) {
    if (ordinalOrBlobOff >= effect_catalog::kCatalogRecordsStartOff) {
        return static_cast<u32>(effect_catalog::global_effect_param_arena().instantiate_by_blob_offset(ordinalOrBlobOff));
    }
    return static_cast<u32>(effect_catalog::global_effect_param_arena().instantiate_by_ordinal(ordinalOrBlobOff));
}

WASM_EXPORT(wasm_effect_catalog_instantiate_by_name) u32 wasm_effect_catalog_instantiate_by_name(const char* nameOrPath) {
    return static_cast<u32>(effect_catalog::global_effect_param_arena().instantiate_by_name(nameOrPath));
}

WASM_EXPORT(wasm_effect_catalog_reset_defaults) u32 wasm_effect_catalog_reset_defaults(u32 slot) {
    return effect_catalog::global_effect_param_arena().reset_instance_defaults(static_cast<u16>(slot)) ? 1u : 0u;
}

WASM_EXPORT(wasm_effect_catalog_get_float_ptr) const float* wasm_effect_catalog_get_float_ptr(u32 slot) {
    const auto& arena = effect_catalog::global_effect_param_arena();
    if (slot >= arena.instanceCount) return nullptr;
    return arena.block(static_cast<u16>(slot)).reg.data();
}

WASM_EXPORT(wasm_effect_catalog_get_block_ptr) const EffectBlock* wasm_effect_catalog_get_block_ptr(u32 slot) {
    const auto& arena = effect_catalog::global_effect_param_arena();
    if (slot >= arena.instanceCount) return nullptr;
    return &arena.block(static_cast<u16>(slot));
}

WASM_EXPORT(wasm_effect_catalog_get_handles_ptr) const effect_catalog::EffectSlotHandle* wasm_effect_catalog_get_handles_ptr() {
    return effect_catalog::global_effect_param_arena().handles.data();
}

// ============================================================================
// WARM PATH: DIRECT ATOMIC IN-PLACE MUTATION API (< 100ns, Zero JSON)
// ============================================================================
WASM_EXPORT(wasm_pos_mutate_reg_f32) void wasm_pos_mutate_reg_f32(u32 slot, u32 regIdx, float val) {
    Position<>* pos = timeline_position_get_instance();
    if (pos && slot < MAX_TIMELINE_SLOTS) {
        pos->mutate_slot_reg_f32(slot, regIdx, val);
    }
}

WASM_EXPORT(wasm_pos_mutate_reg_u32) void wasm_pos_mutate_reg_u32(u32 slot, u32 regIdx, u32 val) {
    Position<>* pos = timeline_position_get_instance();
    if (pos && slot < MAX_TIMELINE_SLOTS) {
        pos->mutate_slot_reg_u32(slot, regIdx, val);
    }
}

WASM_EXPORT(wasm_pos_mutate_bit) void wasm_pos_mutate_bit(u32 slot, u32 bitIdx, u32 val) {
    Position<>* pos = timeline_position_get_instance();
    if (pos && slot < MAX_TIMELINE_SLOTS) {
        pos->mutate_slot_bit(slot, bitIdx, val);
    }
}

WASM_EXPORT(wasm_pos_get_slot_reg_f32) float wasm_pos_get_slot_reg_f32(u32 slot, u32 regIdx) {
    Position<>* pos = timeline_position_get_instance();
    return (pos && slot < MAX_TIMELINE_SLOTS) ? pos->get_slot_reg_f32(slot, regIdx) : 0.0f;
}

WASM_EXPORT(wasm_pos_get_slot_reg_u32) u32 wasm_pos_get_slot_reg_u32(u32 slot, u32 regIdx) {
    Position<>* pos = timeline_position_get_instance();
    return (pos && slot < MAX_TIMELINE_SLOTS) ? pos->get_slot_reg_u32(slot, regIdx) : 0u;
}

WASM_EXPORT(wasm_pos_get_slot_bit) u32 wasm_pos_get_slot_bit(u32 slot, u32 bitIdx) {
    Position<>* pos = timeline_position_get_instance();
    return (pos && slot < MAX_TIMELINE_SLOTS && pos->get_slot_bit(slot, bitIdx)) ? 1u : 0u;
}

WASM_EXPORT(wasm_fx_mutate_param_f32) void wasm_fx_mutate_param_f32(u32 instanceSlot, u32 regIdx, float val) {
    auto& arena = effect_catalog::global_effect_param_arena();
    if (instanceSlot < arena.instanceCount) {
        arena.block(static_cast<u16>(instanceSlot)).set_value(regIdx, val);
        arena.mark_dirty(static_cast<u16>(instanceSlot));
    }
}

WASM_EXPORT(wasm_fx_mutate_bit) void wasm_fx_mutate_bit(u32 instanceSlot, u32 bitIdx, u32 val) {
    auto& arena = effect_catalog::global_effect_param_arena();
    if (instanceSlot < arena.instanceCount) {
        arena.block(static_cast<u16>(instanceSlot)).set_value(bitIdx, val != 0);
        arena.mark_dirty(static_cast<u16>(instanceSlot));
    }
}

// ============================================================================
// GPU-DRIVEN PIPELINE WASM EXPORTS (ZERO-COPY WEBGPU / WEBGL2 BRIDGE)
// ============================================================================
WASM_EXPORT(wasm_gpu_init_grid) void wasm_gpu_init_grid(float stageW, float stageH, u32 tileSize) {
    gpu_driven::get_gpu_pipeline_manager().init(stageW, stageH, tileSize);
}

WASM_EXPORT(wasm_gpu_bin_scene) void wasm_gpu_bin_scene(double nowMs, u32 dirtyBB_low, u32 dirtyBB_high) {
    Position<>* pos = timeline_position_get_instance();
    if (!pos) return;
    const u64 dirtyBB = (static_cast<u64>(dirtyBB_high) << 32) | static_cast<u64>(dirtyBB_low);
    gpu_driven::get_gpu_pipeline_manager().execute_cpu_binning(*pos, dirtyBB, static_cast<float>(nowMs));
}

WASM_EXPORT(wasm_gpu_prepare_frame) void wasm_gpu_prepare_frame(double nowMs, u32 dirtyBB_low, u32 dirtyBB_high) {
    Position<>* pos = timeline_position_get_instance();
    if (!pos) return;
    const u64 dirtyBB = (static_cast<u64>(dirtyBB_high) << 32) | static_cast<u64>(dirtyBB_low);
    gpu_driven::get_gpu_pipeline_manager().execute_gpu_pseudo_prep(*pos, dirtyBB, static_cast<float>(nowMs));
}

WASM_EXPORT(wasm_gpu_get_grid_config_ptr) u32 wasm_gpu_get_grid_config_ptr() {
    return static_cast<u32>(reinterpret_cast<usize>(
        gpu_driven::get_gpu_pipeline_manager().get_grid_config_ptr()));
}

WASM_EXPORT(wasm_gpu_get_tile_headers_ptr) u32 wasm_gpu_get_tile_headers_ptr() {
    return static_cast<u32>(reinterpret_cast<usize>(
        gpu_driven::get_gpu_pipeline_manager().get_tile_headers_ptr()));
}

WASM_EXPORT(wasm_gpu_get_tile_indices_ptr) u32 wasm_gpu_get_tile_indices_ptr() {
    return static_cast<u32>(reinterpret_cast<usize>(
        gpu_driven::get_gpu_pipeline_manager().get_tile_indices_ptr()));
}

WASM_EXPORT(wasm_gpu_get_upload_ranges_ptr) u32 wasm_gpu_get_upload_ranges_ptr() {
    return static_cast<u32>(reinterpret_cast<usize>(
        gpu_driven::get_gpu_pipeline_manager().get_upload_ranges_ptr()));
}

WASM_EXPORT(wasm_gpu_get_upload_range_count) u32 wasm_gpu_get_upload_range_count() {
    return gpu_driven::get_gpu_pipeline_manager().get_upload_range_count();
}

WASM_EXPORT(wasm_gpu_get_tile_count) u32 wasm_gpu_get_tile_count() {
    return gpu_driven::get_gpu_pipeline_manager().get_tile_count();
}

WASM_EXPORT(wasm_gpu_get_active_tile_count) u32 wasm_gpu_get_active_tile_count() {
    return gpu_driven::get_gpu_pipeline_manager().get_active_tile_count();
}

WASM_EXPORT(wasm_gpu_get_total_indices_count) u32 wasm_gpu_get_total_indices_count() {
    return gpu_driven::get_gpu_pipeline_manager().get_total_indices_count();
}

WASM_EXPORT(wasm_gpu_get_tile_bit_matrix_ptr) u32 wasm_gpu_get_tile_bit_matrix_ptr() {
    return static_cast<u32>(reinterpret_cast<usize>(
        gpu_driven::get_gpu_pipeline_manager().get_tile_bit_matrix_ptr()));
}

WASM_EXPORT(wasm_gpu_get_bit_matrix_active_tile_count) u32 wasm_gpu_get_bit_matrix_active_tile_count() {
    return gpu_driven::get_gpu_pipeline_manager().get_bit_matrix_active_tile_count();
}

WASM_EXPORT(wasm_gpu_get_context_ptr) u32 wasm_gpu_get_context_ptr() {
    return static_cast<u32>(reinterpret_cast<usize>(
        gpu_driven::get_gpu_pipeline_manager().get_hardware_context_ptr()));
}

WASM_EXPORT(wasm_gpu_get_spatial_cells_ptr) u32 wasm_gpu_get_spatial_cells_ptr() {
    return static_cast<u32>(reinterpret_cast<usize>(
        gpu_driven::get_gpu_pipeline_manager().get_spatial_cells_ptr()));
}

WASM_EXPORT(wasm_gpu_get_ring_ptr) u32 wasm_gpu_get_ring_ptr() {
    return static_cast<u32>(reinterpret_cast<usize>(
        gpu_driven::get_gpu_pipeline_manager().get_frame_ring_ptr()));
}
} // namespace stockcut
