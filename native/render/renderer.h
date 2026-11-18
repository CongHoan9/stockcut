// ============================================================================
// [ĐÁNH GIÁ ĐIỂM NGHẼN HỆ THỐNG]:
// 1. Lớp trung gian JavaScript WebGL Bridge (Cross-Boundary Call Overhead):
//    - Gọi từng lệnh WebGL qua cầu nối JS-WASM tiêu tốn chi phí chuyển đổi ngữ cảnh
//      (context switch overhead) lên tới hàng chục nghìn lần mỗi giây khi có nhiều layer.
// 2. Chờ đồng bộ GPU và phân bổ đệm lặp lại (GPU Stalls & Texture Reallocation):
//    - Khởi tạo lại kết cấu hoặc đổi FBO kích thước liên tục gây GPU stall và xé hình.
//
// [HƯỚNG KHẮC PHỤC TRIỆT ĐỂ — KHÔNG VÁ]:
// 1. Phát lệnh Native OpenGL ES 3.0 trực tiếp từ C++ (Zero JS Middleware):
//    - Gọi trực tiếp các hàm GLES3 (glBindVertexArray, glDrawElements, glBufferSubData)
//      từ C++ WebAssembly, không trung gian qua JS.
// 2. Double-Buffering & Smart Texture Ping-Pong:
//    - Tái sử dụng vùng đệm FBO tĩnh (s_sceneFbo, s_staticFbo, s_mipTargets) căn chỉnh
//      theo kích thước canvas, triệt tiêu cấp phát lại trong vòng lặp render.
// ============================================================================
#pragma once
#include "types.h"
#include "transfer.h"
#include "effect.h"
#include "shared_state.h"
#include "data_structures.h"
#include "spatial/frame_dirty.h"
#include "manager.h"
#include "font_engine.h"
#include "cache/transposition_table.h"
#include "state.h"
namespace stockcut
{
    constexpr u32 MAX_GLYPH_CACHE_SLOTS = 64;
    constexpr u32 MAX_GLYPHS_PER_CACHED_RUN = 512;
    constexpr u32 MAX_LINES_PER_CACHED_RUN = 32;
    struct CachedGlyphQuad
    {
        float x;
        float y;
        float relX;
        float relY;
        float gw;
        float gh;
        float u0;
        float v0;
        float u1;
        float v1;
        u16 line;
        u16 atlasX;
        u16 atlasY;
        u16 width;
        u16 height;
        u16 kfIndex;
    };
    struct CachedGlyphRun
    {
        u32 glyphCount;
        u32 lineCount;
        u32 alignCode; // 0 = left, 1 = center, 2 = right
        float lineHeight;
        float baselineOffset;
        float maxLineWidth;
        float totalLinesH;
        float ascender;
        float resolvedBaselineOffset;
        float alignFactor;
        float invGlyphCount;
        float anchorX;
        float anchorY;
        float supShift;
        u8 decoFlags;
        u8 hasExplicitWidth;
        u8 hasExplicitHeight;
        u8 pad;
        u16 kfCount;
        u16 revealDurationMs;
        MultiArray<i32, MAX_TIMELINE_SLOTS> kfOffsets;
        MultiArray<float, MAX_LINES_PER_CACHED_RUN> lineWidths;
        MultiArray<CachedGlyphQuad, MAX_GLYPHS_PER_CACHED_RUN> glyphs;
    };
    enum FrameContractFlag : u32
    {
        FRAME_FLAG_NONE          = 0u,
        FRAME_FLAG_SEEKED        = 1u << 0,
        FRAME_FLAG_SCENE_INVALID = 1u << 1,
        FRAME_FLAG_TIME_TAINTED  = 1u << 2,
        FRAME_FLAG_HAS_POST_FX   = 1u << 3,
        FRAME_FLAG_BASS_ACTIVE   = 1u << 4,
        FRAME_FLAG_SPATIAL_DIRTY = 1u << 5,
        FRAME_FLAG_COMB_COUNT    = 1u << 6
    };

    consteval u32 canonicalize_contract_flags(u32 raw) noexcept
    {
        if (raw & FRAME_FLAG_BASS_ACTIVE)
            raw = (raw & ~FRAME_FLAG_BASS_ACTIVE) | FRAME_FLAG_TIME_TAINTED;
        if (raw & FRAME_FLAG_SCENE_INVALID)
            return FRAME_FLAG_SCENE_INVALID;
        if (raw & (FRAME_FLAG_SEEKED | FRAME_FLAG_HAS_POST_FX))
            return FRAME_FLAG_HAS_POST_FX | (raw & FRAME_FLAG_TIME_TAINTED);
        if ((raw & FRAME_FLAG_SPATIAL_DIRTY) == 0u)
            return FRAME_FLAG_NONE;
        return FRAME_FLAG_SPATIAL_DIRTY | (raw & FRAME_FLAG_TIME_TAINTED);
    }

    struct TelemetryBuffer;
    struct PassContext;
    template <typename Policy>
    struct PassExecutor;

    class Render
    {
    public:
        friend struct PassContext;
        template <typename Policy>
        friend struct PassExecutor;

        static bool init(const char *canvasSelector = "#native-gl-canvas") noexcept;
        static bool has_context() noexcept { return s_initialized; }
        static void begin_frame(i32 width, i32 height, float r = 0.0f, float g = 0.0f, float b = 0.0f, float a = 1.0f) noexcept;
        // Thực thi 1 frame hoàn chỉnh trực tiếp trên GPU từ C++ theo StateTransferPolicy:
        // - compositeMode == 1 -> StateTransferPolicy::ZeroTransfer (ReuseScene)
        // - compositeMode == 2 -> StateTransferPolicy::PartialTransfer (DirtyRedraw + glScissor + TransferQueue)
        // - compositeMode == 0 -> StateTransferPolicy::FullTransfer (FullRedraw / RenderPass DAG)
        template <typename Mask = Mask<1>>
        static u32 execute_frame(
            u32 width, u32 height,
            u32 compositeMode,
            u32 useNativeRenderPlan,
            u32 commandCount,
            u32 staticPrefixCount,
            u32 staticKey,
            u32 dirtyRectCount,
            float bgR, float bgG, float bgB, float bgA,
            u32 blitToScreen) noexcept;
        // Unified Zero-JS Hot-Path Master Render:
        // C++ tu dong dieu phoi 5 luong kien truc:
        // 1. Advance timeline position (stepTo)
        // 2. Lap lich Render Pass DAG (projectRuntime)
        // 3. Command Marshalling & Quad Packing (SIMD128)
        // 4. Dirty Rect Tracking & Mode Selection (SlotTracker)
        // 5. Hardware Blit thang vao Default Framebuffer 0 cua canvas hien thi
        template <typename Mask = Mask<1>>
        static u32 render_timeline_frame(
            double nowMs,
            u32 width, u32 height,
            u32 frameTick,
            u32 sceneValid,
            float bgR, float bgG, float bgB, float bgA,
            u32 blitToScreen) noexcept;
        // Hợp nhất toàn bộ chính sách thực thi phần cứng vào 1 Template duy nhất theo FrameContractFlag
        template <u32 ContractFlags = FRAME_FLAG_NONE>
        static u32 execute_policy(
            u32 width, u32 height, u32 commandCount,
            const QuadCommand *cmds,
            const timeline::runtime::Frame<> &plan,
            const SpatialFrameDirty<> &frameDirty,
            float bgR, float bgG, float bgB, float bgA,
            SharedRenderState *sharedRender,
            TelemetryBuffer *telem = nullptr,
            Key kParam = 0ULL) noexcept;
        static void blit_scene_to_screen(u32 width, u32 height) noexcept;
        static void render_selection_overlay(
            float stageW, float stageH,
            const void* segsPtr, u32 segCount, float dx, float dy,
            float lineThick) noexcept;
        static void sync_glyph_atlas() noexcept;
        static u32 alloc_texture() noexcept;
        static void bind_texture(u32 texId) noexcept;
        static void free_texture(u32 texId) noexcept;
        static u32 get_glyph_atlas_tex_id() noexcept { return s_glyphAtlasTex; }
        static u32 get_scene_fbo_id() noexcept { return s_sceneFbo; }
        static u32 get_scene_tex_id() noexcept { return s_sceneTex; }
        static GLuint get_quad_vao() noexcept { return s_quadVao; }
        static void store_glyph_cache(
            u32 cacheSlot,
            const float *glyphDataF32,
            u32 glyphCount,
            const float *lineWidthsF32,
            u32 lineCount,
            float lineHeight,
            u32 alignCode,
            float baselineOffset,
            const i32 *kfOffsets = nullptr,
            u16 kfCount = 0,
            u16 revealDurationMs = 0) noexcept;
        static float *get_glyph_upload_scratch() noexcept;
        static float *get_line_widths_scratch() noexcept;
        static void set_slot_texture(u32 slot, u32 texId) noexcept;
        static u32 get_slot_texture(u32 slot) noexcept;
        static void set_resource_texture(u32 resourceId, u32 texId) noexcept;
        static u32 get_resource_texture(u32 resourceId) noexcept;
        static void set_slot_glyph_cache(u32 slot, u32 cacheSlot) noexcept;
        static u32 get_glyph_cache_count(u32 cacheSlot) noexcept;
        static const CachedGlyphRun &get_glyph_cache_run(u32 cacheSlot) noexcept;
        static void reset_project_state() noexcept;
        static u32 get_slot_glyph_cache(u32 slot) noexcept;
        static void set_slot_text_source(u32 slot, const char *utf8, u32 utf8Len, const u16 *charKfIndex, u32 charCount, const char *fontFamily) noexcept;
        static void invalidate_all_text_caches() noexcept;
        static void invalidate_slot_text_cache(u32 slot) noexcept;
        static void ensure_slot_text_shaped(u32 slot, float sx) noexcept;
        static void set_monoid_batch_ops(const MathOp* ops) noexcept;
        static cache::TranspositionTable<cache::GpuSurface, 4, 4>& get_gpu_surface_tt() noexcept;
        static cache::ZobristPyramid<>& get_zobrist_pyramid() noexcept;
        static u32* get_slot_textures_ptr() noexcept { return s_slotTextures.data(); }
        static inline GLuint get_instanced_program() noexcept { return s_instancedProgram; }
    private:
        static bool s_initialized;
        alignas(64) static MultiArray<u32, MAX_TIMELINE_SLOTS> s_slotTextures;
        alignas(64) static MultiArray<u32, MAX_TIMELINE_SLOTS> s_slotGlyphCache;
        static GLuint s_instancedProgram;
        static GLuint s_copyProgram;
        static GLuint s_blurProgram;
        static GLuint s_brightnessProgram;
        static GLuint s_transformProgram;
        static GLuint s_bloomExtractProgram;
        static GLuint s_bloomDownProgram;
        static GLuint s_bloomUpProgram;
        static GLuint s_bloomCompositeProgram;
        static GLuint s_monoidProgram;
        static GLuint s_cachedFboForScissor;
        static GLuint s_quadVao;
        static GLuint s_quadVbo;
        static GLuint s_whiteTex;
        static GLuint s_glyphAtlasTex;
        static GLuint s_sceneFbo;
        static GLuint s_sceneTex;
        static u32 s_sceneW;
        static u32 s_sceneH;
    };
    } // namespace stockcut
