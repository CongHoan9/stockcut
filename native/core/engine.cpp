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
// CoreEngine - WebAssembly Export Dispatcher & ABI Boundary
// ----------------------------------------------------------------------------
// [KỸ THUẬT STOCKFISH ÁP DỤNG]:
// 1. C-ABI Freestanding WASM Export Boundary
//    - Kiểu bài toán: Giao tiếp JS/WASM trực tiếp qua con trỏ bộ nhớ tuyến tính phẳng (0-copy)
//    - Tham chiếu Stockfish: Stockfish/src/uci.cpp (UCI string/command interface)
// 2. Zero-Allocation C++ Dispatcher
//    - Kiểu bài toán: Mọi hàm export chỉ đọc/ghi trực tiếp vào buffers căn chỉnh 64-bit
//    - Tham chiếu Stockfish: Stockfish/src/types.h
// Tra cứu thêm tại: native/phan_loai_nghiep_vu_stockfish.txt (Mục 1 đến Mục 8)
// ============================================================================
#include "types.h"
#include "math_utils.h"
#include "transposition_table.h"
#include "zobrist.h"
#include "algebra.h"
#include "fusion_kernel.h"
#include "dirty_rect.h"
#include "dsp.h"
#include "timeline/bitboard.h"
#include "telemetry.h"
#include "display_list.h"
#include "pipeline.h"
#include "style.h"
#include "layout.h"
#include "geometry.h"
#include "filter.h"
#include "engine.h"
#include "fx_math.h"
#include "computation_cache.h"
#include "obs_matrix.h"
#include "roi.h"
#include "scale.h"
#include "render/renderer.h"
#include "timeline/transfer.h"
#include "timeline/effect.h"
#include "position.h"
#include "hash_primitives.h"
#include "data_structures.h"
using namespace stockcut;
constexpr u32 EFFECT_SCRATCH_FLOATS       = 4u;
constexpr u32 COMPUTATION_CACHE_INFO_WORDS = 7u;
constexpr u32 BATCH_EVAL_MAX_FRAMES       = 16384u;
// Preserves exact float/double inputs in content-addressed computation keys.
static u64 effectFloatBits(float value) {
    return __builtin_bit_cast(u32, value);
}
static u64 effectDoubleBits(double value) {
    return __builtin_bit_cast(u64, value);
}

// 1. Zobrist
WASM_EXPORT(wasm_zobrist_init) void wasm_zobrist_init(u64 seed) {
    zobrist_init(seed);
    tt_clear();
}
WASM_EXPORT(wasm_get_item_keys_buf) Key* wasm_get_item_keys_buf() {
    return zobrist_get_item_keys_buf();
}
WASM_EXPORT(wasm_get_item_keys_capacity) u32 wasm_get_item_keys_capacity() {
    return zobrist_get_item_keys_capacity();
}
WASM_EXPORT(wasm_compute_zobrist) u64 wasm_compute_zobrist(
    u32 W, u32 H, u32 styleEpoch, u32 fontsVer,
    u32 styleVer, u32 mediaVer, u32 bgHash, u32 count
) {
    return zobrist_compute(W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash, count);
}
// 2. Transposition Table & Command Buffer Cache
WASM_EXPORT(wasm_tt_clear) void wasm_tt_clear() {
    cache::get_global_display_list_tt().clear();
    display_list_cache_clear();
}
WASM_EXPORT(wasm_tt_next_gen) void wasm_tt_next_gen() {
    cache::get_global_display_list_tt().advance_generation();
}
WASM_EXPORT(wasm_tt_probe) u32 wasm_tt_probe(u64 key, u32* outData, u32* outTier) {
    auto* entry = cache::get_global_display_list_tt().probe(key);
    if (entry && entry->is_valid()) {
        if (outData) *outData = entry->payload.cacheSlot;
        if (outTier) *outTier = static_cast<u32>(entry->payload.tier);
        return 1;
    }
    return 0;
}
WASM_EXPORT(wasm_tt_write) void wasm_tt_write(u64 key, u32 data, u32 tier) {
    cache::get_global_display_list_tt().write(key, {data, static_cast<u8>(tier & 0xFF), {}});
}
WASM_EXPORT(wasm_tt_save_display_list) u32 wasm_tt_save_display_list(u64 key, u32 tier)
{
    const u32 slot = display_list_cache_save(key, static_cast<u8>(tier & 0xFF));
    if (slot != DL_CACHE_INVALID)
        cache::get_global_display_list_tt().write(key, {slot, static_cast<u8>(tier & 0xFF), {}});
    return slot;
}
WASM_EXPORT(wasm_tt_restore_display_list) u32 wasm_tt_restore_display_list(u32 slot, u64 expectedKey) {
    return display_list_cache_restore(slot, expectedKey) ? 1 : 0;
}
// 3. Dirty Coalescer
WASM_EXPORT(wasm_get_dirty_buf) Rect<double>* wasm_get_dirty_buf() {
    return dirty_get_buf();
}
WASM_EXPORT(wasm_get_area_ptr) double* wasm_get_area_ptr() {
    return dirty_get_area_ptr();
}
WASM_EXPORT(wasm_coalesce_dirty) usize wasm_coalesce_dirty(usize count, double W, double H) {
    return dirty_coalesce(count, W, H);
}
// 4. Audio DSP & Native Audio Clock Engine
WASM_EXPORT(wasm_fft_init) void wasm_fft_init() {
    audio_fft_init();
    audio_tracks_reset();
}
WASM_EXPORT(wasm_get_sample_buf) float* wasm_get_sample_buf() {
    return audio_get_sample_buf();
}
WASM_EXPORT(wasm_get_mag_buf) float* wasm_get_mag_buf() {
    return audio_get_mag_buf();
}
WASM_EXPORT(wasm_fft_transform_frame) void wasm_fft_transform_frame(u32 validCount) {
    audio_fft_transform_frame(validCount);
}
WASM_EXPORT(wasm_compute_superflux) void wasm_compute_superflux(const float* currBands, const float* prevBands, u32 numBands, float* outDiff) {
    audio_compute_superflux(currBands, prevBands, numBands, outDiff);
}
WASM_EXPORT(wasm_audio_get_volume_envelopes_ptr) AudioVolumeEnvelope* wasm_audio_get_volume_envelopes_ptr() {
    return audio_get_volume_envelopes_ptr();
}
WASM_EXPORT(wasm_audio_get_sync_result_ptr) AudioSyncResult* wasm_audio_get_sync_result_ptr() {
    return audio_get_sync_result_ptr();
}
WASM_EXPORT(wasm_audio_sync_track) u32 wasm_audio_sync_track(
    u32 slot, double timelineNowMs, double mediaCurrentSec, double wallNowMs, u32 isPlaying, u32 isPaused, u32 isSeeking
) {
    return audio_track_sync(slot, timelineNowMs, mediaCurrentSec, wallNowMs, isPlaying, isPaused, isSeeking);
}
// 5. Spatial Bitboard
WASM_EXPORT(wasm_spatial_aabb_mask) u64 wasm_spatial_aabb_mask(double x0, double y0, double x1, double y1, double W, double H) {
    return Bitboard::FromAABBT<SpatialCoverage::ExactAABB>(x0, y0, x1, y1, W, H);
}
// 6. Telemetry
WASM_EXPORT(wasm_get_telemetry_ptr) TelemetryBuffer* wasm_get_telemetry_ptr() {
    return telemetry_get_ptr();
}
WASM_EXPORT(wasm_telemetry_reset) void wasm_telemetry_reset() {
    telemetry_reset();
}
// 7. Display List
WASM_EXPORT(wasm_get_display_list_ptr) u32* wasm_get_display_list_ptr() {
    return display_list_get_ptr();
}
WASM_EXPORT(wasm_get_display_list_count) u32 wasm_get_display_list_count() {
    return display_list_get_count();
}
WASM_EXPORT(wasm_display_list_get_count) u32 wasm_display_list_get_count() {
    return display_list_get_count();
}
WASM_EXPORT(wasm_set_display_list_count) void wasm_set_display_list_count(u32 count) {
    display_list_set_count(count);
}
WASM_EXPORT(wasm_display_list_reset) void wasm_display_list_reset() {
    display_list_reset();
}
WASM_EXPORT(wasm_display_list_push) u32 wasm_display_list_push(
    u32 opCode, u32 flags, u32 resourceId,
    float x0, float y0, float x1, float y1,
    float opacity, u32 styleSlot, u32 filterSlot
) {
    return display_list_push(
        (u16)opCode, (u16)flags, resourceId,
        x0, y0, x1, y1, opacity,
        (u16)styleSlot, (u16)filterSlot
    ) ? 1 : 0;
}
WASM_EXPORT(wasm_display_list_cull) u32 wasm_display_list_cull(u64 occludedMask, double W, double H) {
    return display_list_cull(occludedMask, W, H);
}
WASM_EXPORT(wasm_display_list_generate_from_scene) u32 wasm_display_list_generate_from_scene(
    u32 nodeCount, float viewportW, float viewportH
) {
    return display_list_generate_from_scene(nodeCount, viewportW, viewportH);
}
// 8. Pipeline Master Evaluation
WASM_EXPORT(wasm_get_pipeline_result_ptr) PipelineResult* wasm_get_pipeline_result_ptr() {
    return pipeline_get_result_ptr();
}
WASM_EXPORT(wasm_render_pipeline_evaluate) PipelineResult* wasm_render_pipeline_evaluate(
    u32 W, u32 H,
    u32 styleEpoch, u32 fontsVer, u32 styleVer, u32 mediaVer, u32 bgHash,
    u32 count, u64 lastFrameHash, u64 occludedMask,
    double nowMs, u32 frameIndex, u32 isPlaying
) {
    return (isPlaying != 0u)
        ? pipeline_evaluate_T<true>(W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash, count, lastFrameHash, occludedMask, nowMs, frameIndex)
        : pipeline_evaluate_T<false>(W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash, count, lastFrameHash, occludedMask, nowMs, frameIndex);
}
WASM_EXPORT(wasm_pipeline_evaluate_position) PipelineResult* wasm_pipeline_evaluate_position(
    u32 W, u32 H,
    u32 styleEpoch, u32 fontsVer, u32 styleVer, u32 mediaVer, u32 bgHash,
    u64 lastFrameHash, u64 occludedMask,
    double nowMs, u32 frameIndex, u32 isPlaying
) {
    return (isPlaying != 0u)
        ? pipeline_evaluate_position_T<true>(W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash, lastFrameHash, occludedMask, nowMs, frameIndex)
        : pipeline_evaluate_position_T<false>(W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash, lastFrameHash, occludedMask, nowMs, frameIndex);
}
// 9. Style Engine
WASM_EXPORT(wasm_get_styles_ptr) PackedStyle* wasm_get_styles_ptr() {
    return style_get_arena();
}
WASM_EXPORT(wasm_style_reset) void wasm_style_reset(u32 idx) {
    style_reset(idx);
}
WASM_EXPORT(wasm_style_compute_affine_2d) void wasm_style_compute_affine_2d(u32 idx, float* out6Floats) {
    style_compute_affine_2d(idx, out6Floats);
}
WASM_EXPORT(wasm_style_interpolate) void wasm_style_interpolate(u32 fromIdx, u32 toIdx, float t, u32 outIdx) {
    style_interpolate(fromIdx, toIdx, t, outIdx);
}
WASM_EXPORT(wasm_style_hash) u64 wasm_style_hash(u32 idx) {
    return style_hash(idx);
}
// 10. Text Layout & Typography Engine
WASM_EXPORT(wasm_text_get_input_buf) u32* wasm_text_get_input_buf() {
    return text_get_input_buf();
}
WASM_EXPORT(wasm_text_get_glyphs_ptr) LayoutGlyph* wasm_text_get_glyphs_ptr() {
    return text_get_glyphs_ptr();
}
WASM_EXPORT(wasm_text_get_result_ptr) TextLayoutResult* wasm_text_get_result_ptr() {
    return text_get_result_ptr();
}
WASM_EXPORT(wasm_text_layout) void wasm_text_layout(
    u32 charCount,
    float fontSize,
    float maxWidth,
    float letterSpacing,
    float lineHeight,
    u32 textAlign,
    u32 fontCategory,
    u32 textTransform
) {
    text_layout(charCount, fontSize, maxWidth, letterSpacing, lineHeight, textAlign, fontCategory, textTransform);
}
WASM_EXPORT(wasm_text_register_glyph_metric) void wasm_text_register_glyph_metric(u32 fontCategory, u32 unicode, float advance) {
    text_register_glyph_metric(fontCategory, unicode, advance);
}
WASM_EXPORT(wasm_text_layout_hash) u64 wasm_text_layout_hash(
    u32 charCount,
    float fontSize,
    float maxWidth,
    float letterSpacing,
    float lineHeight,
    u32 textAlign
) {
    return text_layout_hash(charCount, fontSize, maxWidth, letterSpacing, lineHeight, textAlign);
}
// 11. Geometry & Scene Graph Engine (SIMD128 AABB)
WASM_EXPORT(wasm_geometry_get_nodes_ptr) SceneNode* wasm_geometry_get_nodes_ptr() {
    return geometry_get_nodes_ptr();
}
WASM_EXPORT(wasm_geometry_get_result_ptr) GeometryResult* wasm_geometry_get_result_ptr() {
    return geometry_get_result_ptr();
}
WASM_EXPORT(wasm_geometry_reset_node) void wasm_geometry_reset_node(u32 idx) {
    geometry_reset_node(idx);
}
WASM_EXPORT(wasm_geometry_set_node_local) void wasm_geometry_set_node_local(
    u32 idx,
    float x, float y, float w, float h,
    u16 parentIdx, u16 styleSlot, u16 clipNode, u16 flags
) {
    geometry_set_node_local(idx, x, y, w, h, parentIdx, styleSlot, clipNode, flags);
}
WASM_EXPORT(wasm_geometry_evaluate_scene) GeometryResult* wasm_geometry_evaluate_scene(u32 nodeCount, float viewportW, float viewportH) {
    return geometry_evaluate_scene(nodeCount, viewportW, viewportH);
}
WASM_EXPORT(wasm_geometry_compute_aabb_simd) void wasm_geometry_compute_aabb_simd(
    float a, float b, float c, float d, float tx, float ty,
    float lx, float ly, float lw, float lh,
    float* outAABB4Floats
) {
    geometry_compute_aabb_simd(a, b, c, d, tx, ty, lx, ly, lw, lh, outAABB4Floats);
}
// 12. Shader Pipeline & CSS Filter/Blend Engine (Pha 4)
WASM_EXPORT(wasm_filter_get_arena) FilterUniforms* wasm_filter_get_arena() {
    return filter_get_arena();
}
WASM_EXPORT(wasm_filter_reset) void wasm_filter_reset(u32 idx) {
    filter_reset(idx);
}
WASM_EXPORT(wasm_filter_set_blur) void wasm_filter_set_blur(u32 idx, float radius) {
    filter_set_blur(idx, radius);
}
WASM_EXPORT(wasm_filter_set_shadow) void wasm_filter_set_shadow(
    u32 idx, float ox, float oy, float blur, float spread, u32 colorRgba
) {
    filter_set_shadow(idx, ox, oy, blur, spread, colorRgba);
}
WASM_EXPORT(wasm_filter_set_blend_mode) void wasm_filter_set_blend_mode(u32 idx, u32 blendMode) {
    filter_set_blend_mode(idx, blendMode);
}
WASM_EXPORT(wasm_filter_set_color_adjustments) void wasm_filter_set_color_adjustments(
    u32 idx, float bright, float contrast, float sat, float hueDeg, float inv, float sep
) {
    filter_set_color_adjustments(idx, bright, contrast, sat, hueDeg, inv, sep);
}
WASM_EXPORT(wasm_filter_compute_color_matrix) void wasm_filter_compute_color_matrix(u32 idx, float* out16Floats) {
    filter_compute_color_matrix(idx, out16Floats);
}
WASM_EXPORT(wasm_color_matrix_compute) void wasm_color_matrix_compute(
    float bright, float contrast, float sat, float hueDeg, float inv, float sep, float opacity,
    ColorMatrix64B* outMat
) {
    compute_color_matrix_64b(bright, contrast, sat, hueDeg, inv, sep, opacity, *outMat);
}
WASM_EXPORT(wasm_filter_get_kawase_passes_buf) KawasePassInfo* wasm_filter_get_kawase_passes_buf() {
    return filter_get_kawase_passes_buf();
}
WASM_EXPORT(wasm_filter_compute_kawase_passes) u32 wasm_filter_compute_kawase_passes(
    float blurRadius, u32 srcW, u32 srcH, KawasePassInfo* outPasses, u32 maxPasses
) {
    return filter_compute_kawase_passes(blurRadius, srcW, srcH, outPasses, maxPasses);
}
WASM_EXPORT(wasm_filter_compute_kawase_passes_static) u32 wasm_filter_compute_kawase_passes_static(
    float blurRadius, u32 srcW, u32 srcH
) {
    return filter_compute_kawase_passes_static(blurRadius, srcW, srcH);
}
WASM_EXPORT(wasm_filter_hash) u64 wasm_filter_hash(u32 idx) {
    return filter_hash(idx);
}
// 12. Effect Engine & Physical Transitions (Natron RoI & Analytical State)
static IntroOutroGLState g_wasmIntroOutroState;
alignas(16) static MultiArray<float, EFFECT_SCRATCH_FLOATS> g_wasmEffectScratch;
alignas(16) static MultiArray<u32, COMPUTATION_CACHE_INFO_WORDS> g_wasmComputationCacheInfo;
WASM_EXPORT(wasm_effect_engine_init) void wasm_effect_engine_init() {
    effect_engine_init();
}
WASM_EXPORT(wasm_get_intro_outro_state_buf) IntroOutroGLState* wasm_get_intro_outro_state_buf() {
    return &g_wasmIntroOutroState;
}
// Returns dedicated aligned storage for small effect results, never the dirty-area scalar.
WASM_EXPORT(wasm_get_effect_scratch_ptr) float* wasm_get_effect_scratch_ptr() {
    return g_wasmEffectScratch.data();
}
// Returns dedicated shared storage for the complete computation-cache status record.
WASM_EXPORT(wasm_get_computation_cache_info_buf) u32* wasm_get_computation_cache_info_buf() {
    return g_wasmComputationCacheInfo.data();
}
// Reuses the deterministic transition-state POD and publishes its cold-cache metadata.
WASM_EXPORT(wasm_compute_intro_outro_state) u32 wasm_compute_intro_outro_state(u32 isOutro, float u) {
    const MultiArray<u64, 2> inputs{ { isOutro ? 1ULL : 0ULL, effectFloatBits(u) } };
    computation_cache_eval_T<ComputationCacheDomain::IntroOutroState>(
        inputs.data(), 2u, &g_wasmIntroOutroState,
        [&]() noexcept {
            if (isOutro) {
                effect_compute_intro_outro_state<true>(u, &g_wasmIntroOutroState);
            } else {
                effect_compute_intro_outro_state<false>(u, &g_wasmIntroOutroState);
            }
        }
    );
    return g_wasmIntroOutroState.active ? 1u : 0u;
}
WASM_EXPORT(wasm_effect_resolve_algebra_word) u64 wasm_effect_resolve_algebra_word(u32 effectHash) {
    return effect_algebra::resolve_algebra_word_by_hash(effectHash);
}
WASM_EXPORT(wasm_effect_is_identity) u32 wasm_effect_is_identity(
    u64 algebraWord, double nowMs, double startMs, double endMs, float param0, float param1
) {
    return effect_algebra::effect_algebra_is_identity(algebraWord, nowMs, startMs, endMs, param0, param1) ? 1u : 0u;
}
// Computes or reuses the RoI for a complete anonymous algebraWord/time/geometry signature.
WASM_EXPORT(wasm_effect_compute_roi) void wasm_effect_compute_roi(
    u64 algebraWord, double nowMs, double startMs, double endMs,
    float W, float H, float inX0, float inY0, float inX1, float inY1,
    float* outRoI4Floats
) {
    const MultiArray<u64, 10> inputs{ {
        algebraWord, effectDoubleBits(nowMs), effectDoubleBits(startMs),
        effectDoubleBits(endMs), effectFloatBits(W), effectFloatBits(H),
        effectFloatBits(inX0), effectFloatBits(inY0),
        effectFloatBits(inX1), effectFloatBits(inY1)
    } };
    MultiArray<float, 4> result{};
    computation_cache_eval_T<ComputationCacheDomain::EffectRoi>(
        inputs.data(), 10u, result.data(),
        [&]() noexcept {
            Rect<float> roi;
            effect_algebra::effect_algebra_compute_roi(
                algebraWord,
                nowMs, startMs, endMs, W, H,
                inX0, inY0, inX1, inY1,
                roi
            );
            result[0] = roi.x0;
            result[1] = roi.y0;
            result[2] = roi.x1;
            result[3] = roi.y1;
        }
    );
    if (outRoI4Floats) {
        outRoI4Floats[0] = result[0];
        outRoI4Floats[1] = result[1];
        outRoI4Floats[2] = result[2];
        outRoI4Floats[3] = result[3];
    }
}
// 13. Natron Proxy RenderScale
WASM_EXPORT(wasm_render_scale_set) void wasm_render_scale_set(float sx, float sy) {
    render_scale_set(sx, sy);
}
WASM_EXPORT(wasm_render_scale_set_level) void wasm_render_scale_set_level(u32 level) {
    render_scale_set_level(level);
}
WASM_EXPORT(wasm_render_scale_get) void wasm_render_scale_get(float* outScale2Floats) {
    RenderScale s = render_scale_get();
    if (outScale2Floats) {
        outScale2Floats[0] = s.x;
        outScale2Floats[1] = s.y;
    }
}
WASM_EXPORT(wasm_render_scale_is_identity) u32 wasm_render_scale_is_identity() {
    return render_scale_is_identity() ? 1u : 0u;
}
// 14. Heavy Mathematical Effect Simulations (Ported from JS to C++)
// Evaluates a periodic zoom state and shares cache status for the async cold tier.
WASM_EXPORT(wasm_zoom_evaluate_periodic) void wasm_zoom_evaluate_periodic(
    float speed, float amplitude, float phase, float tilt, u32 mirror,
    double tMs, float* outScaleRotMirror3
) {
    const MultiArray<u64, 6> inputs{ {
        effectFloatBits(speed), effectFloatBits(amplitude), effectFloatBits(phase),
        effectFloatBits(tilt), mirror ? 1ULL : 0ULL, effectDoubleBits(tMs)
    } };
    MultiArray<float, 3> result{};
    computation_cache_eval_T<ComputationCacheDomain::ZoomPeriodic>(
        inputs.data(), 6u, result.data(),
        [&]() noexcept {
            ZoomEvalResult res{};
            if (mirror) {
                zoom_evaluate_periodic<true>(speed, amplitude, phase, tilt, tMs, &res);
            } else {
                zoom_evaluate_periodic<false>(speed, amplitude, phase, tilt, tMs, &res);
            }
            result[0] = res.scale;
            result[1] = res.rotateDeg;
            result[2] = static_cast<float>(res.mirror);
        }
    );
    if (outScaleRotMirror3) {
        outScaleRotMirror3[0] = result[0];
        outScaleRotMirror3[1] = result[1];
        outScaleRotMirror3[2] = result[2];
    }
}
WASM_EXPORT(wasm_zoom_evaluate_peaks) void wasm_zoom_evaluate_peaks(
    const ZoomPeak* peaks, u32 peakCount, float speed, float baseScale,
    double tMs, float* outScaleRot2
) {
    ZoomEvalResult res;
    zoom_evaluate_peaks(peaks, peakCount, speed, baseScale, tMs, &res);
    if (outScaleRot2) {
        outScaleRot2[0] = res.scale;
        outScaleRot2[1] = res.rotateDeg;
    }
}
// Evaluates all shake harmonics from its full deterministic parameter signature.
WASM_EXPORT(wasm_shake_evaluate) void wasm_shake_evaluate(
    float speed, float amplitude, float tilt, float fadeMs,
    double tMs, double durMs, float W, float H,
    float* outDxDyRotScale4
) {
    const MultiArray<u64, 8> inputs{ {
        effectFloatBits(speed), effectFloatBits(amplitude), effectFloatBits(tilt),
        effectFloatBits(fadeMs), effectDoubleBits(tMs), effectDoubleBits(durMs),
        effectFloatBits(W), effectFloatBits(H)
    } };
    MultiArray<float, 4> result{};
    computation_cache_eval_T<ComputationCacheDomain::Shake>(
        inputs.data(), 8u, result.data(),
        [&]() noexcept {
            ShakeEvalResult res{};
            if (fadeMs > OPACITY_EPSILON) {
                shake_evaluate<true>(speed, amplitude, tilt, fadeMs, tMs, durMs, W, H, &res);
            } else {
                shake_evaluate<false>(speed, amplitude, tilt, fadeMs, tMs, durMs, W, H, &res);
            }
            result[0] = res.dx;
            result[1] = res.dy;
            result[2] = res.rotDeg;
            result[3] = res.scaleBoost;
        }
    );
    if (outDxDyRotScale4) {
        outDxDyRotScale4[0] = result[0];
        outDxDyRotScale4[1] = result[1];
        outDxDyRotScale4[2] = result[2];
        outDxDyRotScale4[3] = result[3];
    }
}
WASM_EXPORT(wasm_shake_scale_boost) float wasm_shake_scale_boost(
    float amplitude, float tiltDeg, float W, float H
) {
    return shake_scale_boost(amplitude, tiltDeg, W, H);
}
// Evaluates periodic flicker while caching only its current numeric result.
WASM_EXPORT(wasm_flicker_evaluate_periodic) float wasm_flicker_evaluate_periodic(
    float speed, float brightness, float baseVal, u32 isSquare, double tMs
) {
    const MultiArray<u64, 5> inputs{ {
        effectFloatBits(speed), effectFloatBits(brightness),
        effectFloatBits(baseVal), isSquare ? 1ULL : 0ULL, effectDoubleBits(tMs)
    } };
    float result = 0.0f;
    computation_cache_eval_T<ComputationCacheDomain::FlickerPeriodic>(
        inputs.data(), 5u, &result,
        [&]() noexcept {
            result = isSquare
                ? flicker_evaluate_periodic<true>(speed, brightness, baseVal, tMs)
                : flicker_evaluate_periodic<false>(speed, brightness, baseVal, tMs);
        }
    );
    return result;
}
// Exposes the latest effect-cache record metadata without crossing 64-bit ABI boundaries.
WASM_EXPORT(wasm_computation_cache_last_info) void wasm_computation_cache_last_info(u32* outInfo) {
    const ComputationCacheStatus& status = computation_cache_last_status();
    g_wasmComputationCacheInfo[0] = static_cast<u32>(status.key);
    g_wasmComputationCacheInfo[1] = static_cast<u32>(status.key >> 32);
    g_wasmComputationCacheInfo[2] = status.domain;
    g_wasmComputationCacheInfo[3] = status.size;
    g_wasmComputationCacheInfo[4] = status.checksum;
    g_wasmComputationCacheInfo[5] = status.hit;
    g_wasmComputationCacheInfo[6] = status.persist;
    if (!outInfo) return;
    for (u32 index = 0; index < COMPUTATION_CACHE_INFO_WORDS; ++index) {
        outInfo[index] = g_wasmComputationCacheInfo[index];
    }
}
// Imports a validated cold-cache record into the fixed native hot cache.
WASM_EXPORT(wasm_computation_cache_import) u32 wasm_computation_cache_import(
    u32 keyLow, u32 keyHigh, u32 domain, u32 size, u32 checksum, const void* payload
) {
    const u64 key = (static_cast<u64>(keyHigh) << 32) | keyLow;
    return computation_cache_import(key, domain, size, checksum, payload) ? 1u : 0u;
}
// Completes or retries the cold-store request for one still-resident native entry.
WASM_EXPORT(wasm_computation_cache_persist_result) void wasm_computation_cache_persist_result(
    u32 keyLow, u32 keyHigh, u32 domain, u32 succeeded
) {
    const u64 key = (static_cast<u64>(keyHigh) << 32) | keyLow;
    if (succeeded == 1u) {
        computation_cache_persist_result_T<true>(key, domain);
    } else {
        computation_cache_persist_result_T<false>(key, domain);
    }
}
// Returns the preallocated transfer buffer used by the IndexedDB adapter.
WASM_EXPORT(wasm_computation_cache_transfer_buffer) u8* wasm_computation_cache_transfer_buffer() {
    return computation_cache_transfer_buffer();
}
// Returns the stable snapshot of the last result for asynchronous cold-tier writes.
WASM_EXPORT(wasm_computation_cache_last_payload) const u8* wasm_computation_cache_last_payload() {
    return computation_cache_last_payload();
}
// Exposes the fixed memory cost for benchmark and runtime audit reporting.
WASM_EXPORT(wasm_computation_cache_memory_bytes) u32 wasm_computation_cache_memory_bytes() {
    return static_cast<u32>(
        computation_cache_memory_bytes() +
        sizeof(g_wasmEffectScratch) + sizeof(g_wasmComputationCacheInfo)
    );
}
// Clears only computation records; display-list and timeline state remain unchanged.
WASM_EXPORT(wasm_computation_cache_clear) void wasm_computation_cache_clear() {
    computation_cache_clear();
}
WASM_EXPORT(wasm_effect_render_intro_field) u32 wasm_effect_render_intro_field(
    float tau, float m, float flip,
    float ringR,
    float barY0, float barY1, float barPIn, float barPOut, float barTIn, float barTOut, float barSp, u32 barActive,
    float lineYc, float linePlateau, float lineCore, float lineHw1, float lineHw2, u32 lineState, u32 lineActive,
    u32 lw, u32 lh,
    float W, float H,
    float baseW, float baseH,
    u32* outPixelsRGBA
) {
    return effect_render_intro_field_cpp(
        tau, m, flip,
        ringR,
        barY0, barY1, barPIn, barPOut, barTIn, barTOut, barSp, barActive,
        lineYc, linePlateau, lineCore, lineHw1, lineHw2, lineState, lineActive,
        lw, lh,
        W, H,
        baseW, baseH,
        outPixelsRGBA
    );
}
// 15. OBS Studio Matrix Pipeline & Stack
WASM_EXPORT(wasm_matrix_stack_reset) void wasm_matrix_stack_reset() {
    g_matrixStack.reset();
}
WASM_EXPORT(wasm_matrix_stack_push) void wasm_matrix_stack_push() {
    g_matrixStack.push();
}
WASM_EXPORT(wasm_matrix_stack_pop) void wasm_matrix_stack_pop() {
    g_matrixStack.pop();
}
WASM_EXPORT(wasm_matrix_stack_translate) void wasm_matrix_stack_translate(float tx, float ty, float tz) {
    g_matrixStack.translate(tx, ty, tz);
}
WASM_EXPORT(wasm_matrix_stack_scale) void wasm_matrix_stack_scale(float sx, float sy, float sz) {
    g_matrixStack.scale(sx, sy, sz);
}
WASM_EXPORT(wasm_matrix_stack_rotate_z) void wasm_matrix_stack_rotate_z(float rad) {
    g_matrixStack.rotate_z(rad);
}
WASM_EXPORT(wasm_matrix_stack_get_current) const float* wasm_matrix_stack_get_current() {
    return reinterpret_cast<const float*>(&g_matrixStack.current());
}
WASM_EXPORT(wasm_matrix4_ortho) void wasm_matrix4_ortho(
    float left, float right, float bottom, float top, float zNear, float zFar,
    float* out16Floats
) {
    if (!out16Floats) return;
    Matrix4 m;
    Matrix4::ortho(m, left, right, bottom, top, zNear, zFar);
    for (u32 i = 0; i < 16; i++) {
        out16Floats[i] = reinterpret_cast<const float*>(&m)[i];
    }
}
WASM_EXPORT(wasm_matrix4_perspective) void wasm_matrix4_perspective(
    float fovYDeg, float aspect, float zNear, float zFar,
    float* out16Floats
) {
    if (!out16Floats) return;
    Matrix4 m;
    Matrix4::perspective(m, fovYDeg, aspect, zNear, zFar);
    for (u32 i = 0; i < 16; i++) {
        out16Floats[i] = reinterpret_cast<const float*>(&m)[i];
    }
}
// 16. Natron Reverse-DAG RoD / RoI Optimization Engine
static NatronRenderPlan g_wasmNatronPlan;
WASM_EXPORT(wasm_natron_optimize_render_plan) const NatronRenderPlan* wasm_natron_optimize_render_plan(
    u64 algebraWord,
    float reqX1, float reqY1, float reqX2, float reqY2,
    float rodX1, float rodY1, float rodX2, float rodY2,
    double nowMs, double startMs, double endMs,
    float param0, float param1,
    float W, float H
) {
    Rect<float> req(reqX1, reqY1, reqX2, reqY2);
    Rect<float> rod(rodX1, rodY1, rodX2, rodY2);
    natron_optimize_render_plan_algebra(
        algebraWord,
        req, rod,
        nowMs, startMs, endMs,
        param0, param1,
        W, H,
        g_wasmNatronPlan
    );
    return &g_wasmNatronPlan;
}
WASM_EXPORT(wasm_get_tier_enum) u32 wasm_get_tier_enum()
{
    return static_cast<u32>(MediaTier::Static) | (static_cast<u32>(MediaTier::Finite) << 8) | (static_cast<u32>(MediaTier::Continuous) << 16);
}
// 17. Native OpenGL ES 3.0 / WebGL2 Direct Renderer & GPU Transfer Queue
WASM_EXPORT(wasm_render_init) u32 wasm_render_init() {
    if (Render::init("#engine-canvas")) return 1u;
    return Render::init("#native-gl-canvas") ? 1u : 0u;
}
WASM_EXPORT(wasm_render_has_context) u32 wasm_render_has_context() {
    return Render::has_context() ? 1u : 0u;
}
WASM_EXPORT(wasm_render_sync_glyph_atlas) void wasm_render_sync_glyph_atlas() {
    Render::sync_glyph_atlas();
}
WASM_EXPORT(wasm_render_alloc_texture) u32 wasm_render_alloc_texture() {
    return Render::alloc_texture();
}
WASM_EXPORT(wasm_render_bind_texture) void wasm_render_bind_texture(u32 texId) {
    Render::bind_texture(texId);
}
WASM_EXPORT(wasm_render_free_texture) void wasm_render_free_texture(u32 texId) {
    Render::free_texture(texId);
}
WASM_EXPORT(wasm_fx_register_static_texture) void wasm_fx_register_static_texture(u32 slot, u32 texId, u32 width, u32 height) {
#if defined(__EMSCRIPTEN__)
    effect::fusion_register_static_texture(slot, texId, width, height);
#else
    (void)slot; (void)texId; (void)width; (void)height;
#endif
}
WASM_EXPORT(wasm_render_get_glyph_atlas_tex_id) u32 wasm_render_get_glyph_atlas_tex_id() {
    return Render::get_glyph_atlas_tex_id();
}
WASM_EXPORT(wasm_render_get_scene_fbo_id) u32 wasm_render_get_scene_fbo_id() {
    return Render::get_scene_fbo_id();
}
WASM_EXPORT(wasm_render_get_scene_tex_id) u32 wasm_render_get_scene_tex_id() {
    return Render::get_scene_tex_id();
}
WASM_EXPORT(wasm_render_get_glyph_upload_scratch) float* wasm_render_get_glyph_upload_scratch() {
    return Render::get_glyph_upload_scratch();
}
WASM_EXPORT(wasm_render_get_line_widths_scratch) float* wasm_render_get_line_widths_scratch() {
    return Render::get_line_widths_scratch();
}
WASM_EXPORT(wasm_render_store_glyph_cache) void wasm_render_store_glyph_cache(
    u32 cacheSlot,
    u32 glyphCount,
    u32 lineCount,
    float lineHeight,
    u32 alignCode,
    float baselineOffset
) {
    Render::store_glyph_cache(
        cacheSlot,
        Render::get_glyph_upload_scratch(),
        glyphCount,
        Render::get_line_widths_scratch(),
        lineCount,
        lineHeight,
        alignCode,
        baselineOffset
    );
}
WASM_EXPORT(wasm_render_execute_frame) u32 wasm_render_execute_frame(
    u32 width, u32 height,
    u32 compositeMode,
    u32 useNativeRenderPlan,
    u32 commandCount,
    u32 staticPrefixCount,
    u32 staticKey,
    u32 dirtyRectCount,
    float bgR, float bgG, float bgB, float bgA,
    u32 blitToScreen
) {
    return Render::execute_frame(
        width, height,
        compositeMode,
        useNativeRenderPlan,
        commandCount,
        staticPrefixCount,
        staticKey,
        dirtyRectCount,
        bgR, bgG, bgB, bgA,
        blitToScreen
    );
}
WASM_EXPORT(wasm_render_timeline_frame) u32 wasm_render_timeline_frame(
    double nowMs,
    u32 width, u32 height,
    u32 frameTick,
    u32 sceneValid,
    float bgR, float bgG, float bgB, float bgA,
    u32 blitToScreen
) {
    return Render::render_timeline_frame(
        nowMs, width, height,
        frameTick, sceneValid,
        bgR, bgG, bgB, bgA,
        blitToScreen
    );
}
WASM_EXPORT(wasm_render_set_slot_texture) void wasm_render_set_slot_texture(u32 slot, u32 texId) {
    Render::set_slot_texture(slot, texId);
}
WASM_EXPORT(wasm_render_get_slot_textures_ptr) u32* wasm_render_get_slot_textures_ptr() {
    return Render::get_slot_textures_ptr();
}
WASM_EXPORT(wasm_render_set_resource_texture) void wasm_render_set_resource_texture(u32 resourceId, u32 texId) {
    Render::set_resource_texture(resourceId, texId);
}
WASM_EXPORT(wasm_render_calculate_resource_id) u32 wasm_render_calculate_resource_id(const char* src) {
    return hash31_str(src);
}
WASM_EXPORT(wasm_render_set_slot_glyph_cache) void wasm_render_set_slot_glyph_cache(u32 slot, u32 cacheSlot) {
    Render::set_slot_glyph_cache(slot, cacheSlot);
}
WASM_EXPORT(wasm_render_get_glyph_cache_count) u32 wasm_render_get_glyph_cache_count(u32 cacheSlot) {
    return Render::get_glyph_cache_count(cacheSlot);
}
WASM_EXPORT(wasm_render_reset_project_state) void wasm_render_reset_project_state() {
    Render::reset_project_state();
}
WASM_EXPORT(wasm_flush_gpu_transfer_queue) void wasm_flush_gpu_transfer_queue() {
    flush_gpu_transfer_queue();
}
WASM_EXPORT(wasm_get_transfer_queue_count) u32 wasm_get_transfer_queue_count() {
    return g_gpuQueue.count;
}
alignas(64) static MultiArray<u64, BATCH_EVAL_MAX_FRAMES> s_batchHashes;
WASM_EXPORT(wasm_render_get_batch_buffer_ptr) u64* wasm_render_get_batch_buffer_ptr() {
    return s_batchHashes.data();
}
WASM_EXPORT(wasm_render_batch_evaluate)
u32 wasm_render_batch_evaluate(double startMs, double endMs, double stepMs, u32 maxFrames) {
    if (stepMs <= 0.0 || startMs > endMs || maxFrames == 0u)
        return 0u;
    Position<>* pos = timeline_position_get_instance();
    if (!pos) return 0u;
    const u32 limit = (maxFrames < BATCH_EVAL_MAX_FRAMES) ? maxFrames : BATCH_EVAL_MAX_FRAMES;
    u32 frameCount = 0u;
    for (double t = startMs; t <= endMs && frameCount < limit; t += stepMs) {
        const TimePoint timeMs = static_cast<TimePoint>(t);
        StateInfo<>* st = timeline_position_advance(timeMs);
        s_batchHashes[frameCount++] = st ? st->key : 0ULL;
    }
    return frameCount;
}
