// ============================================================================
// [ĐÁNH GIÁ ĐIỂM NGHẼN HỆ THỐNG]:
// 1. Nghẽn trần khung hình do đồng bộ màn hình View (VSync 60Hz View Throttle):
//    - Đo FPS tại vòng lặp requestAnimationFrame của JavaScript khiến FPS bị khóa cứng ở 60 FPS
//      (hoặc tần số màn hình), che giấu hoàn toàn tốc độ xử lý thực sự của C++ Core Engine
//      (vốn đạt hàng trăm đến hàng nghìn FPS khi chỉ tốn 0.5 - 1.5ms per frame).
// 2. Hiện tượng phân tầng màu (Banding) và sai lệch sắc độ của Bloom/Glow:
//    - Thuật toán Bloom cũ chuyển đổi sRGB -> Linear 2 lần (Double-Linearization),
//      nén màu vào số nguyên 24-bit gây mất mát dải động cao (HDR Dynamic Range Crushing),
//      thiếu cosine direction similarity dẫn tới việc vùng sáng cần giữ lại bị khử nhầm.
// 3. Phân mảnh Draw Call và nghẽn băng thông GPU:
//    - Từng quad vẽ rời rạc gây GPU state thrashing.
//
// [HƯỚNG KHẮC PHỤC TRIỆT ĐỂ — KHÔNG VÁ]:
// 1. Tách biệt hoàn toàn View và Render Core Telemetry:
//    - C++ Render Engine tự đo đạc thời gian render thực tế (renderMs) thông qua
//      emscripten_get_now() độ phân giải microsecond ngay trong execute_frame().
//    - Tính toán Uncapped Throughput FPS = 1000.0 / avg(renderMs) đẩy vào TelemetryBuffer,
//      giúp Dashboard hiển thị chính xác năng lực thực sự của lõi C++ mà không phụ thuộc View.
// 2. Tái cấu trúc Bloom HDR 13-Tap Karis & Dual-Filter Mip Pyramid:
//    - Áp dụng mã hóa căn bậc hai ENC(x)/DEC(x) trên buffer trung gian để bảo toàn dải sáng 0..4.
//    - Khử răng cưa chập chờn bằng 13-tap Karis average + 13-tap Jimenez downsample.
//    - Ghép dồn Gaussian đa tầng tent 9-tap chuẩn hóa (1/16) với trọng số u_wLow / u_wHi.
//    - Áp dụng Jodie-Reinhard tonemapping mềm theo chuẩn SoDoTongQuan.txt, bảo toàn alpha mũ,
//      và IGN stochastic dither (±1 LSB) triệt tiêu hoàn toàn hiện tượng banding 8-bit.
// 3. Native OpenGL ES 3.0 Zero-JS Dispatch:
//    - Toàn bộ lệnh render pass và batch quad được đẩy thẳng qua VAO/VBO phần cứng.
// ============================================================================
#include "renderer.h"
#include "outline.h"
#include "core/string_parse.h"
#include "dirty_rect.h"
#include "math_utils.h"
#include "telemetry.h"
#include "runtime.h"
#include "audio/script.h"
#include "effect/engine.h"
#include "effect/algebra.h"
#include "effect/fusion_kernel.h"
#include "effect/gl_shader_util.h"
#include "manager.h"
#include "tracker.h"
#include "spatial/frame_dirty.h"
#include "position.h"
#include "font_map.h"
#include "render/state.h"
#include "render/ubo.h"
#include "render/pbo.h"
#include "render/instancing.h"
#include "cache/transposition_table.h"
#include <bit>
namespace stockcut
{
#if defined(STOCKCUT_HAS_GLES3)
    namespace render {
        GpuBackend<BackendKind::GLES3_Unified> g_gpuState{};
        UboRing<FrameUniformBlock>  g_frameUboRing{};
        UboRing<PassUniformBlock>   g_passUboRing{};
        UboRing<MonoidUniformBlock> g_monoidUboRing{};
        PboRing<DEFAULT_PBO_STREAM_CAPACITY> g_glyphPboRing{};
        InstancedBatch<MAX_INSTANCED_QUADS> g_gpuInstancedBatch{};
    }
    using render::g_gpuState;
    using render::g_frameUboRing;
    using render::g_passUboRing;
    using render::g_monoidUboRing;
    using render::g_glyphPboRing;
    using render::g_gpuInstancedBatch;
#endif
    bool Render::s_initialized = false;
    GLuint Render::s_instancedProgram = 0;
    GLuint Render::s_copyProgram = 0;
    GLuint Render::s_blurProgram = 0;
    GLuint Render::s_brightnessProgram = 0;
    GLuint Render::s_transformProgram = 0;
    GLuint Render::s_bloomExtractProgram = 0;
    GLuint Render::s_bloomDownProgram = 0;
    GLuint Render::s_bloomUpProgram = 0;
    GLuint Render::s_bloomCompositeProgram = 0;
    GLuint Render::s_monoidProgram = 0;
    GLuint Render::s_cachedFboForScissor = 0;
    GLuint Render::s_quadVao = 0;
    GLuint Render::s_quadVbo = 0;
    GLuint Render::s_whiteTex = 0;
    GLuint Render::s_glyphAtlasTex = 0;
    GLuint Render::s_sceneFbo = 0;
    GLuint Render::s_sceneTex = 0;
    u32 Render::s_sceneW = 0;
    u32 Render::s_sceneH = 0;
    alignas(64) MultiArray<u32, MAX_TIMELINE_SLOTS> Render::s_slotTextures{};
    alignas(64) MultiArray<u32, MAX_TIMELINE_SLOTS> Render::s_slotGlyphCache{};
    struct TextureEntry
    {
        u32 resourceId;
        u32 texId;
    };
    constexpr u32 MAX_RESOURCE_TEXTURES       = 256u;
    constexpr u32 FAST_RESOURCE_LOOKUP_CAP    = 256u;
    constexpr u32 CLUSTER_TT_BUCKET_COUNT     = 8u;
    constexpr usize SLOT_TEXT_UTF8_CAP        = 2048;
    constexpr usize SLOT_TEXT_CHAR_MAP_CAP    = 1024;
    constexpr usize SLOT_FONT_FAMILY_CAP      = 128;
    constexpr u32 GLYPH_UPLOAD_STRIDE_F32     = 8u;
    constexpr i32 GL_UNPACK_FLIP_Y_WEBGL_ENUM = 0x9240;
    constexpr u32 MAX_MONOID_UNIFORM_OPS      = 8u;
    constexpr u32 MONOID_UNIFORM_FLOATS       = MAX_MONOID_UNIFORM_OPS * 4u;
    constexpr u32 CMD_DISPATCH_TABLE_SIZE     = 6u;
    alignas(16) static MultiArray<TextureEntry, MAX_RESOURCE_TEXTURES> s_resourceTextures{};
    static u32 s_resourceTextureCount = 0;
    alignas(64) static MultiArray<u32, FAST_RESOURCE_LOOKUP_CAP> s_resourceTextureLookup{};
    namespace
    {
        struct PassRenderTarget
        {
            GLuint fbo = 0;
            GLuint tex = 0;
            u32 width = 0;
            u32 height = 0;
        };
        alignas(64) MultiArray<PassRenderTarget, timeline::runtime::RUNTIME_RENDER_TARGET_COUNT> s_passTargets{};
        // ----------------------------------------------------------------------------
        // TẦNG 4: STOCKFISH UNIVERSAL GPU SURFACE TRANSPOSITION TABLE & ZOBRIST PYRAMID
        // 4 Cụm x 4 Đường = 16 Bề Mặt Đồ Họa Tối Đa (Bounded VRAM <= 16 FBOs, 512B L1)
        // Dùng chung cho Static Z-Prefix & Untainted Frame theo cơ chế Aging LRU
        // ----------------------------------------------------------------------------
        alignas(64) static cache::TranspositionTable<cache::GpuSurface, 4, 4> s_gpuSurfaceTT;
        alignas(64) static cache::ZobristPyramid<> s_zobristPyramid;
        alignas(64) static cache::SpacetimeTileMMU<1024, 256, 4> s_tileMMU;
        alignas(64) static cache::TilePageTable64 s_frameTilePTE;
        static Key s_lastParamKey = 0;
        static u32 s_lastW = 0;
        static u32 s_lastH = 0;
        static double s_lastNowMs = -1.0;
        static u32 s_lastCmdCount = 0;
        static bool s_prevFrameHadPostFx = false;
        alignas(64) MultiArray<CachedGlyphRun, MAX_GLYPH_CACHE_SLOTS + 1> s_glyphCache;
        struct TextSource
        {
            MultiArray<char, SLOT_TEXT_UTF8_CAP> utf8;
            u16 utf8Len;
            MultiArray<u16, SLOT_TEXT_CHAR_MAP_CAP> charKfIndex;
            u16 charCount;
            MultiArray<char, SLOT_FONT_FAMILY_CAP> fontFamily;
            float lastShapedScaleX;
            u32 lastFontFileCount;
            bool dirty;
        };
        alignas(64) MultiArray<TextSource, MAX_TIMELINE_SLOTS> s_slotTextSources;
        using TextSourceMask = Mask<((MAX_TIMELINE_SLOTS + 63u) / 64u)>;
        using GlyphCacheMask = Mask<((MAX_GLYPH_CACHE_SLOTS + 63u) / 64u)>;
        TextSourceMask s_activeTextSourcesBB{};
        GlyphCacheMask s_usedGlyphCacheBB{};
        alignas(64) MultiArray<float, MAX_GLYPHS_PER_CACHED_RUN * GLYPH_UPLOAD_STRIDE_F32> s_glyphUploadScratch{};
        alignas(64) MultiArray<float, MAX_LINES_PER_CACHED_RUN> s_lineWidthsScratch{};
        alignas(64) MultiArray<u8, NATIVE_GLYPH_ATLAS_SIZE * NATIVE_GLYPH_ATLAS_SIZE> s_glyphAtlasRowScratch{};
        GLint s_locInstancedRes = -1;
        MultiArray<GLint, MAX_TEXTURE_SLOTS> s_locInstancedTextures{};
        GLint s_locCopyTex = -1;
        GLint s_locBlurTex = -1;
        GLint s_locBlurTexel = -1;
        GLint s_locBlurDir = -1;
        GLint s_locBlurTapCount = -1;
        GLint s_locBlurWeights = -1;
        GLint s_locBrightTex = -1;
        GLint s_locBrightVal = -1;
        GLint s_locTransTex = -1;
        GLint s_locTransRes = -1;
        GLint s_locTransShift = -1;
        GLint s_locTransScale = -1;
        GLint s_locTransRot = -1;
        GLint s_locTransTiled = -1;
        GLint s_locTransMirror = -1;
        GLint s_locBloomExtTex = -1;
        GLint s_locBloomExtTexel = -1;
        GLint s_locBloomExtThreshold = -1;
        GLint s_locBloomExtHasColor = -1;
        GLint s_locBloomExtTolerance = -1;
        GLint s_locBloomExtTargetColor = -1;
        GLint s_locBloomDownTex = -1;
        GLint s_locBloomDownTexel = -1;
        GLint s_locBloomUpTex = -1;
        GLint s_locBloomUpBase = -1;
        GLint s_locBloomUpTexel = -1;
        GLint s_locBloomUpWLow = -1;
        GLint s_locBloomUpWHi = -1;
        GLint s_locBloomCompTex = -1;
        GLint s_locBloomCompBase = -1;
        GLint s_locBloomCompTexel = -1;
        GLint s_locBloomCompGlowTexel = -1;
        GLint s_locBloomCompIntensity = -1;
        GLint s_locBloomCompFactor = -1;
        GLint s_locBloomCompAa = -1;
        GLint s_locBloomCompEnv = -1;
        GLint s_locMonoidTex = -1;
        GLint s_locMonoidRes = -1;
        GLint s_locMonoidCount = -1;
        GLint s_locMonoidBBox = -1;
        GLint s_locMonoidPremul = -1;
        GLint s_locMonoidTrans = -1;
        GLint s_locMonoidUseUber = -1;
        GLint s_locMonoidUberMat = -1;
        GLint s_locMonoidUberTrans = -1;
        GLint s_locMonoidUberGain = -1;
        GLint s_locMonoidUberEmit = -1;
        static const MathOp* s_monoidBatchOpsPtr = nullptr;
        MultiArray<GLuint, MAX_TEXTURE_SLOTS> s_boundTextures{};
        u32 s_boundTexCount = 0;
#if defined(STOCKCUT_HAS_GLES3)
        static const char *kBatchFS = R"(#version 300 es
precision highp float;
in vec2 v_uv;
in vec4 v_color;
in vec4 v_bgColor;
in float v_texId;
in float v_mode;
uniform sampler2D u_textures[8];
out vec4 outColor;
// Balanced Binary Tree Lookup: Tối đa 3 phép so sánh thay vì 7 phép tuần tự
vec4 sampleBatchTex(float id, vec2 uv) {
    if (id < 3.5) {
        if (id < 1.5) {
            return (id < 0.5) ? texture(u_textures[0], uv) : texture(u_textures[1], uv);
        } else {
            return (id < 2.5) ? texture(u_textures[2], uv) : texture(u_textures[3], uv);
        }
    } else {
        if (id < 5.5) {
            return (id < 4.5) ? texture(u_textures[4], uv) : texture(u_textures[5], uv);
        } else {
            return (id < 6.5) ? texture(u_textures[6], uv) : texture(u_textures[7], uv);
        }
    }
}
void main() {
    float opacity = v_color.a;
    if (v_mode < 0.5) {
        outColor = vec4(v_color.rgb * opacity, opacity);
        return;
    }
    vec4 col = sampleBatchTex(v_texId, v_uv);
    if (v_mode < 1.5 || v_mode > 3.5) {
        outColor = vec4(col.rgb * v_color.rgb, col.a) * opacity;
    } else if (v_mode > 2.5) {
        vec4 fgPremul = vec4(v_color.rgb * opacity, opacity);
        vec4 bgPremul = vec4(v_bgColor.rgb * v_bgColor.a, v_bgColor.a);
        outColor = mix(bgPremul, fgPremul, col.r);
    } else {
        vec2 p = v_uv - 0.5;
        float r = length(p);
        float holeRadius = v_color.r * 0.5;
        if ((v_color.g > 0.5 && r > 0.5) || (holeRadius > 0.0 && r < holeRadius)) discard;
        float edge = v_color.g > 0.5 ? 1.0 - smoothstep(0.495, 0.5, r) : 1.0;
        edge *= (holeRadius > 0.0) ? smoothstep(holeRadius, holeRadius + 0.005, r) : 1.0;
        outColor = col * (opacity * edge);
    }
}
)";
        static const char *kCopyFS = R"(#version 300 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D u_tex;
out vec4 outColor;
void main() {
    outColor = texture(u_tex, v_uv);
}
)";
        static const char *kBlurFS = R"(#version 300 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_texel;
uniform vec2 u_direction;
uniform int u_tapCount;
uniform float u_weights[33];
out vec4 outColor;
void main() {
    vec4 color = texture(u_tex, v_uv) * u_weights[0];
    for (int tap = 1; tap <= 32; ++tap) {
        if (tap > u_tapCount) break;
        vec2 offset = u_direction * u_texel * float(tap);
        color += texture(u_tex, v_uv + offset) * u_weights[tap];
        color += texture(u_tex, v_uv - offset) * u_weights[tap];
    }
    outColor = color;
}
)";
        static const char *kBrightnessFS = R"(#version 300 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform float u_brightness;
out vec4 outColor;
void main() {
    vec4 color = texture(u_tex, v_uv);
    outColor = vec4(clamp(color.rgb * u_brightness, 0.0, 1.0), color.a);
}
)";
        static const char *kTransformFS = R"(#version 300 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_res;
uniform vec2 u_shift;
uniform float u_scale;
uniform float u_rotation;
uniform float u_tiled;
uniform float u_mirror;
out vec4 outColor;
void main() {
    vec2 center = u_res * 0.5 + u_shift;
    vec2 p = gl_FragCoord.xy - center;
    float c = cos(u_rotation);
    float s = sin(u_rotation);
    vec2 rotated = vec2(p.x * c - p.y * s, p.x * s + p.y * c);
    vec2 uv = rotated / (u_res * max(u_scale, 0.05)) + 0.5;
    if (u_tiled > 0.5) {
        vec2 tile = floor(uv);
        vec2 localUv = uv - tile;
        if (u_mirror > 0.5) {
            if (mod(abs(tile.x), 2.0) >= 1.0) localUv.x = 1.0 - localUv.x;
            if (mod(abs(tile.y), 2.0) >= 1.0) localUv.y = 1.0 - localUv.y;
        }
        uv = localUv;
    } else {
        uv = clamp(uv, 0.0, 1.0);
    }
    outColor = texture(u_tex, uv);
}
)";
        static const char *kBloomExtractFS = R"(#version 300 es
precision highp float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_texel;
uniform float u_threshold;
uniform float u_hasColor;
uniform float u_tolerance;
uniform vec3 u_targetColor;
out vec4 outColor;
#define DEC(x) ((x) * (x) * 4.0)
#define ENC(x) sqrt(clamp((x), 0.0, 4.0) * 0.25)
vec3 sRGBToLinear(vec3 c) {
    vec3 col = max(c, vec3(0.0));
    return mix(col / 12.92, pow((col + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), col));
}
float luminance(vec3 c) {
    return dot(c, vec3(0.2126729, 0.7151522, 0.0721750));
}
float karis(vec3 c) {
    return 1.0 / (1.0 + luminance(c));
}
vec3 extractEmission(vec3 linearCol) {
    float lum = luminance(linearCol);
    if (u_hasColor > 0.5) {
        float tol = max(0.05, u_tolerance);
        float dist = distance(linearCol, u_targetColor);
        float distScore = 1.0 - smoothstep(tol * 0.15, tol, dist);
        vec3 nC = normalize(linearCol + 1e-4);
        vec3 nT = normalize(u_targetColor + 1e-4);
        float cosSim = dot(nC, nT);
        float dirScore = smoothstep(1.0 - tol * 0.7, 1.0, cosSim);
        // Saturation gating: chi cac pixel co do bao hoa mau moi phat sang theo mau chi dinh (tranh phat sang nen xam/trang trung tinh)
        float maxCh = max(linearCol.r, max(linearCol.g, linearCol.b));
        float minCh = min(linearCol.r, min(linearCol.g, linearCol.b));
        float sat = (maxCh - minCh) / max(maxCh, 1e-4);
        float match = max(distScore, dirScore * smoothstep(0.02, 0.15, lum) * smoothstep(0.05, 0.25, sat));
        return mix(linearCol, u_targetColor * max(lum * 1.5, 1.0), 0.5) * match;
    } else {
        float knee = max(0.05, u_threshold * 0.5);
        float rq = clamp(lum - u_threshold + knee, 0.0, 2.0 * knee);
        float soft = (rq * rq) / (4.0 * knee + 1e-4);
        float factor = max(soft, max(0.0, lum - u_threshold));
        return linearCol * (factor / max(lum, 1e-4));
    }
}
vec3 E(vec2 o) {
    return extractEmission(sRGBToLinear(texture(u_tex, v_uv + o * u_texel).rgb));
}
void main() {
    vec3 a = E(vec2(-2.0,  2.0));
    vec3 b = E(vec2( 0.0,  2.0));
    vec3 c = E(vec2( 2.0,  2.0));
    vec3 d = E(vec2(-2.0,  0.0));
    vec3 e = E(vec2( 0.0,  0.0));
    vec3 f = E(vec2( 2.0,  0.0));
    vec3 g = E(vec2(-2.0, -2.0));
    vec3 h = E(vec2( 0.0, -2.0));
    vec3 i = E(vec2( 2.0, -2.0));
    vec3 j = E(vec2(-1.0,  1.0));
    vec3 k = E(vec2( 1.0,  1.0));
    vec3 l = E(vec2(-1.0, -1.0));
    vec3 m = E(vec2( 1.0, -1.0));
    vec3 g0 = (j + k + l + m) * 0.25;
    vec3 g1 = (a + b + d + e) * 0.25;
    vec3 g2 = (b + c + e + f) * 0.25;
    vec3 g3 = (d + e + g + h) * 0.25;
    vec3 g4 = (e + f + h + i) * 0.25;
    float w0 = 0.5   * karis(g0);
    float w1 = 0.125 * karis(g1);
    float w2 = 0.125 * karis(g2);
    float w3 = 0.125 * karis(g3);
    float w4 = 0.125 * karis(g4);
    vec3 sum = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
    outColor = vec4(ENC(sum), 1.0);
}
)";
        static const char *kBloomDownFS = R"(#version 300 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_texel;
out vec4 outColor;
#define DEC(x) ((x) * (x) * 4.0)
#define ENC(x) sqrt(clamp((x), 0.0, 4.0) * 0.25)
vec3 tap(vec2 o) {
    return DEC(texture(u_tex, v_uv + o * u_texel).rgb);
}
void main() {
    vec3 a = tap(vec2(-2.0,  2.0));
    vec3 b = tap(vec2( 0.0,  2.0));
    vec3 c = tap(vec2( 2.0,  2.0));
    vec3 d = tap(vec2(-2.0,  0.0));
    vec3 e = tap(vec2( 0.0,  0.0));
    vec3 f = tap(vec2( 2.0,  0.0));
    vec3 g = tap(vec2(-2.0, -2.0));
    vec3 h = tap(vec2( 0.0, -2.0));
    vec3 i = tap(vec2( 2.0, -2.0));
    vec3 j = tap(vec2(-1.0,  1.0));
    vec3 k = tap(vec2( 1.0,  1.0));
    vec3 l = tap(vec2(-1.0, -1.0));
    vec3 m = tap(vec2( 1.0, -1.0));
    vec3 col = e * 0.125
             + (a + c + g + i) * 0.03125
             + (b + d + f + h) * 0.0625
             + (j + k + l + m) * 0.125;
    outColor = vec4(ENC(col), 1.0);
}
)";
        static const char *kBloomUpFS = R"(#version 300 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform sampler2D u_base;
uniform vec2 u_texel;
uniform float u_wLow;
uniform float u_wHi;
out vec4 outColor;
#define DEC(x) ((x) * (x) * 4.0)
#define ENC(x) sqrt(clamp((x), 0.0, 4.0) * 0.25)
vec3 lowTap(vec2 o) {
    return DEC(texture(u_tex, v_uv + o * u_texel).rgb);
}
void main() {
    vec3 s = lowTap(vec2(-1.0, -1.0)) + 2.0 * lowTap(vec2(0.0, -1.0)) + lowTap(vec2(1.0, -1.0))
           + 2.0 * lowTap(vec2(-1.0,  0.0)) + 4.0 * lowTap(vec2(0.0,  0.0)) + 2.0 * lowTap(vec2(1.0,  0.0))
           + lowTap(vec2(-1.0,  1.0)) + 2.0 * lowTap(vec2(0.0,  1.0)) + lowTap(vec2(1.0,  1.0));
    s *= (1.0 / 16.0);
    vec3 hi = DEC(texture(u_base, v_uv).rgb);
    outColor = vec4(ENC(hi * u_wHi + s * u_wLow), 1.0);
}
)";
        static const char *kBloomCompositeFS = R"(#version 300 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform sampler2D u_base;
uniform vec2 u_texel;
uniform vec2 u_glowTexel;
uniform float u_intensity;
uniform float u_factor;
uniform float u_aa;
uniform float u_env;
out vec4 outColor;
#define DEC(x) ((x) * (x) * 4.0)
vec3 sRGBToLinear(vec3 c) {
    vec3 col = max(c, vec3(0.0));
    return mix(col / 12.92, pow((col + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), col));
}
vec3 linearTosRGB(vec3 c) {
    vec3 col = max(c, vec3(0.0));
    return mix(col * 12.92, 1.055 * pow(col, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), col));
}
float luminance(vec3 c) {
    return dot(c, vec3(0.2126729, 0.7151522, 0.0721750));
}
float ignDither(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}
vec4 sampleRGSS4(sampler2D tex, vec2 uv, vec2 texel) {
    vec4 col = vec4(0.0);
    col += texture(tex, uv + vec2(-0.125, -0.375) * texel);
    col += texture(tex, uv + vec2( 0.375, -0.125) * texel);
    col += texture(tex, uv + vec2( 0.125,  0.375) * texel);
    col += texture(tex, uv + vec2(-0.375,  0.125) * texel);
    return col * 0.25;
}
vec3 glowTap(vec2 o) {
    return DEC(texture(u_tex, v_uv + o * u_glowTexel).rgb);
}
vec4 sampleBase(vec2 uv, float aaAmount) {
    vec4 center = texture(u_base, uv);
    if (aaAmount <= 0.001) return center;
    vec4 rgss = sampleRGSS4(u_base, uv, u_texel);
    return mix(center, rgss, aaAmount);
}
// Jodie Reinhard Tonemap tu SoDoTongQuan.txt (lam sang nhung mau can lam sang, bao toan do ruc mau)
vec3 jodieReinhardTonemap(vec3 c) {
    float l = luminance(c);
    vec3 tc = c / (c + 1.0);
    return mix(c / (l + 1.0), tc, tc);
}
void main() {
    vec4 baseTex = sampleBase(v_uv, u_aa * u_env);
    vec3 baseLinear = sRGBToLinear(baseTex.rgb);
    // Tent 9-tap tren mip half-res xoa artefact hinh thoi
    vec3 glow = glowTap(vec2(-1.0, -1.0)) + 2.0 * glowTap(vec2(0.0, -1.0)) + glowTap(vec2(1.0, -1.0))
              + 2.0 * glowTap(vec2(-1.0,  0.0)) + 4.0 * glowTap(vec2(0.0,  0.0)) + 2.0 * glowTap(vec2(1.0,  0.0))
              + glowTap(vec2(-1.0,  1.0)) + 2.0 * glowTap(vec2(0.0,  1.0)) + glowTap(vec2(1.0,  1.0));
    glow *= (1.0 / 16.0);
    // Cuong do chuan bloom.js: 3.5 * BASE_GAIN (0.6) = 2.1
    vec3 bloomGlow = glow * (u_intensity * u_factor * 2.1);
    // Ket hop linear va tonemap bao toan mau sac (Jodie Reinhard tu SoDoTongQuan.txt)
    vec3 finalLinear = baseLinear + bloomGlow;
    vec3 tonemapped = jodieReinhardTonemap(finalLinear);
    vec3 sRGBCol = linearTosRGB(tonemapped);
    vec3 finalCol = clamp(mix(baseTex.rgb, sRGBCol, u_env), 0.0, 1.0);
    // Bao toan alpha theo ham phan ra mu tu nhien chuan bloom.js
    float bloomAlpha = (1.0 - exp(-luminance(bloomGlow) * 2.5)) * u_env;
    float finalAlpha = clamp(baseTex.a + (1.0 - baseTex.a) * bloomAlpha, 0.0, 1.0);
    // Stochastic rounding dither (+-1 LSB) triet tieu hoan toan banding 8-bit
    float n1 = ignDither(gl_FragCoord.xy);
    float n2 = ignDither(gl_FragCoord.xy + vec2(5.588238));
    finalCol = floor(finalCol * 255.0 + 0.0001 + vec3(n1) * 0.9998) / 255.0;
    finalAlpha = floor(finalAlpha * 255.0 + 0.0001 + n2 * 0.9998) / 255.0;
    outColor = vec4(finalCol, finalAlpha);
}
)";
        static const char *kMonoidFS = R"(#version 300 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_res;
uniform vec4 u_opBBox[8];
uniform vec4 u_opPremul[8];
uniform vec4 u_opTrans[8];
uniform int u_monoidCount;
uniform mat2 u_uberMat;
uniform vec2 u_uberTrans;
uniform vec4 u_uberGain;
uniform vec4 u_uberEmit;
uniform int u_useUberShader;
out vec4 outColor;
void main() {
    if (u_useUberShader != 0) {
        vec2 p = v_uv - vec2(0.5);
        vec2 srcUV = clamp(u_uberMat * p + u_uberTrans + vec2(0.5), 0.0, 1.0);
        vec4 src = texture(u_tex, srcUV);
        outColor = u_uberGain * src + u_uberEmit;
        return;
    }
    vec2 p = v_uv * u_res;
    vec4 cur = texture(u_tex, v_uv);
    for (int i = 0; i < 8; ++i) {
        if (i >= u_monoidCount) break;
        vec4 bb = u_opBBox[i];
        vec2 inside = step(bb.xy, p) * step(p, bb.zw);
        float w = inside.x * inside.y;
        vec4 effP = u_opPremul[i] * w;
        vec4 effT = mix(vec4(1.0), u_opTrans[i], w);
        cur = effP + effT * cur;
    }
    outColor = cur;
}
)";
        static void ensure_fbo_target(GLuint &fbo, GLuint &tex, u32 &curW, u32 &curH, u32 w, u32 h, GLint filter) noexcept
        {
            if (fbo && tex && curW == w && curH == h)
                return;
            if (!tex)
                glGenTextures(1, &tex);
            if (!fbo)
                glGenFramebuffers(1, &fbo);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            curW = w;
            curH = h;
            g_gpuState.invalidate_fbo();
            g_gpuState.invalidate_texture<0>();
        }
        static void begin_batch_pass(u32 width, u32 height) noexcept
        {
            g_gpuState.use_program(Render::get_instanced_program());
            glUniform2f(s_locInstancedRes, static_cast<float>(width), static_cast<float>(height));
            g_gpuInstancedBatch.advance_ring();
            s_boundTexCount = 0;
        }
        static void flush_batch_pass(GLuint whiteTex) noexcept
        {
            const u32 count = g_gpuInstancedBatch.count();
            if (count == 0)
            {
                s_boundTexCount = 0;
                return;
            }
            const u32 texBindLimit = (s_boundTexCount > 0) ? s_boundTexCount : 1;
            for (u32 i = 0; i < texBindLimit; ++i)
            {
                const GLuint tex = (i < s_boundTexCount && s_boundTextures[i] != 0) ? s_boundTextures[i] : whiteTex;
                g_gpuState.bind_texture_dynamic(i, tex);
            }
            g_gpuState.bind_vao(g_gpuInstancedBatch.current_vao());
            g_gpuInstancedBatch.flush();
            s_boundTexCount = 0;
        }
        static void push_batch_quad(
            float x, float y, float w, float h,
            float pivotX, float pivotY, float rotRad,
            float r, float g, float b, float a,
            GLuint texId, float mode, float spinRad,
            float uvLeft, float uvTop, float uvRight, float uvBottom,
            GLuint whiteTex) noexcept
        {
            if (g_gpuInstancedBatch.count() >= render::MAX_INSTANCED_QUADS)
            {
                flush_batch_pass(whiteTex);
            }
            u32 texSlot = 0;
            if (mode > 0.5f && texId != 0)
            {
                i32 found = -1;
                for (u32 i = 0; i < s_boundTexCount; ++i)
                {
                    if (s_boundTextures[i] == texId)
                    {
                        found = static_cast<i32>(i);
                        break;
                    }
                }
                if (found == -1)
                {
                    if (s_boundTexCount >= MAX_TEXTURE_SLOTS)
                    {
                        flush_batch_pass(whiteTex);
                    }
                    found = static_cast<i32>(s_boundTexCount);
                    s_boundTextures[s_boundTexCount++] = texId;
                }
                texSlot = static_cast<u32>(found);
            }
            g_gpuInstancedBatch.push_quad(
                x, y, w, h,
                pivotX, pivotY, rotRad,
                r, g, b, a,
                static_cast<float>(texSlot), mode, spinRad,
                uvLeft, uvTop, uvRight, uvBottom);
        }
        static inline void emit_text_glyphs(
            const CachedGlyphRun &run,
            const QuadCommand &cmd,
            float boxX,
            float boxY,
            float boxW,
            float boxH,
            float alignBaseX,
            float baseTopY,
            float topMargin,
            float effPivotX,
            float effPivotY,
            GLuint glyphAtlasTex,
            GLuint whiteTex,
            u32 revealMode,
            bool wantDeco) noexcept
        {
            const u16 kfCount = run.kfCount;
            const i32 relMs = cmd.revealRelMs;
            const u32 relFlagsU32 = static_cast<u32>(relMs);
            const u16 revealDur = run.revealDurationMs;
            const u32 deco = run.decoFlags;
            const float supShift = run.supShift;
            const float ascender = run.ascender;
            const float baselineOffset = run.resolvedBaselineOffset;
            const float rotRad = cmd.rotRad;
            const float spinRad = cmd.spinRad;
            const float r = cmd.red;
            const float g = cmd.green;
            const float b = cmd.blue;
            const float a = cmd.alpha;

            const u32 bgPacked = std::bit_cast<u32>(spinRad);
            const bool hasBg = (bgPacked >> 24) != 0u;
            const ColorRGBA bgCol = hasBg ? ColorRGBA::from_packed(bgPacked) : ColorRGBA::transparent();
            const auto emitBgStrip = [&](float sx, float sy, float sw, float sh) noexcept {
                if (sw > 0.05f && sh > 0.05f) {
                    push_batch_quad(
                        sx, sy, sw, sh,
                        effPivotX, effPivotY, rotRad,
                        bgCol.r, bgCol.g, bgCol.b, bgCol.a,
                        0, static_cast<float>(QUAD_MODE_COLOR), 0.0f,
                        0.0f, 0.0f, 1.0f, 1.0f,
                        whiteTex);
                }
            };

            MultiArray<float, MAX_LINES_PER_CACHED_RUN> decoMinX;
            MultiArray<float, MAX_LINES_PER_CACHED_RUN> decoMaxX;
            MultiArray<float, MAX_LINES_PER_CACHED_RUN> decoAlpha;
            if (wantDeco)
            {
                for (u32 l = 0; l < run.lineCount; ++l)
                {
                    decoMinX[l] = RENDER_OFFSCREEN_BOUND;
                    decoMaxX[l] = -RENDER_OFFSCREEN_BOUND;
                    decoAlpha[l] = 0.0f;
                }
            }

            const float boxRight = boxX + boxW;
            const float boxBottom = boxY + boxH;
            u32 curLine = 0u;
            float lineBandY0 = topMargin;
            float lineBandY1 = std::fmin(boxBottom, topMargin + run.lineHeight);
            float curX = boxX;
            if (hasBg && lineBandY0 > boxY)
            {
                emitBgStrip(boxX, boxY, boxW, lineBandY0 - boxY);
            }

            const bool isKfReveal = (revealMode == 1u || revealMode == 2u);
            const bool isSweepReveal = (revealMode == 3u) && (relFlagsU32 < static_cast<u32>(PERMILLE_MAX));
            const float sweepProgress = isSweepReveal ? (static_cast<float>(relFlagsU32) * 0.001f) : 0.0f;
            for (u32 idx = 0; idx < run.glyphCount; ++idx)
            {
                const CachedGlyphQuad &q = run.glyphs[idx];
                float glyphAlpha = a;
                if (isKfReveal)
                {
                    const u16 k = (q.kfIndex < kfCount) ? q.kfIndex : static_cast<u16>(kfCount - 1u);
                    const i32 delta = relMs - run.kfOffsets[k];
                    if (delta < 0)
                        continue;
                    if (delta < static_cast<i32>(revealDur) && revealDur > 0)
                    {
                        glyphAlpha *= static_cast<float>(delta) / static_cast<float>(revealDur);
                    }
                }
                else if (isSweepReveal)
                {
                    const float glyphPos = static_cast<float>(idx) * run.invGlyphCount;
                    const float delta = sweepProgress - glyphPos;
                    if (delta < 0.0f)
                        continue;
                    if (delta < 0.08f)
                        glyphAlpha *= (delta * 12.5f);
                }
                if (glyphAlpha <= OPACITY_EPSILON)
                    continue;
                float gx = alignBaseX + q.relX;
                const float gy = baseTopY + q.relY;
                float gw = q.gw;
                const float gh = q.gh;
                float u0 = q.u0;
                if (wantDeco)
                {
                    const u32 line = q.line;
                    decoMinX[line] = std::fmin(decoMinX[line], gx);
                    decoMaxX[line] = std::fmax(decoMaxX[line], gx + gw);
                    decoAlpha[line] = std::fmax(decoAlpha[line], glyphAlpha);
                }
                if (hasBg)
                {
                    while (curLine < q.line && curLine < run.lineCount)
                    {
                        emitBgStrip(curX, lineBandY0, boxRight - curX, lineBandY1 - lineBandY0);
                        ++curLine;
                        lineBandY0 = lineBandY1;
                        lineBandY1 = (curLine + 1u < run.lineCount)
                            ? std::fmin(boxBottom, topMargin + static_cast<float>(curLine + 1u) * run.lineHeight)
                            : boxBottom;
                        curX = boxX;
                    }
                    if (gx < curX)
                    {
                        const float overlap = curX - gx;
                        if (overlap >= gw) continue;
                        u0 += (overlap / gw) * (q.u1 - q.u0);
                        gw -= overlap;
                        gx = curX;
                    }
                    else if (gx > curX)
                    {
                        emitBgStrip(curX, lineBandY0, gx - curX, lineBandY1 - lineBandY0);
                    }
                    const float topGap = gy - lineBandY0;
                    if (topGap > 0.0f)
                    {
                        emitBgStrip(gx, lineBandY0, gw, topGap);
                    }
                    const float botGap = lineBandY1 - (gy + gh);
                    if (botGap > 0.0f)
                    {
                        emitBgStrip(gx, gy + gh, gw, botGap);
                    }
                    curX = gx + gw;
                }
                push_batch_quad(
                    gx, gy, gw, gh,
                    effPivotX, effPivotY, rotRad,
                    r, g, b, glyphAlpha,
                    glyphAtlasTex, static_cast<float>(QUAD_MODE_TEXT), spinRad,
                    u0, q.v0, q.u1, q.v1,
                    whiteTex);
            }
            if (hasBg)
            {
                emitBgStrip(curX, lineBandY0, boxRight - curX, lineBandY1 - lineBandY0);
                if (lineBandY1 < boxBottom)
                {
                    emitBgStrip(boxX, lineBandY1, boxW, boxBottom - lineBandY1);
                }
            }
            if (wantDeco)
            {
                const float thick = std::fmax(1.0f, ascender * 0.075f);
                const float halfThick = thick * 0.5f;
                const float underOff = ascender * 0.14f - halfThick;
                const float strikeOff = -ascender * 0.32f - halfThick;
                const bool drawUnder = (deco & TextDecoration::Underline) != 0;
                const bool drawStrike = (deco & TextDecoration::Strikethrough) != 0;
                for (u32 l = 0; l < run.lineCount; ++l)
                {
                    const float lineW = decoMaxX[l] - decoMinX[l];
                    if (lineW <= 0.0f || decoAlpha[l] <= OPACITY_EPSILON)
                        continue;
                    const float baseY = topMargin + static_cast<float>(l) * run.lineHeight + baselineOffset + supShift;
                    if (drawUnder)
                    {
                        push_batch_quad(
                            decoMinX[l], baseY + underOff, lineW, thick,
                            effPivotX, effPivotY, rotRad,
                            r, g, b, decoAlpha[l],
                            0, static_cast<float>(QUAD_MODE_COLOR), spinRad,
                            0.0f, 0.0f, 1.0f, 1.0f,
                            whiteTex);
                    }
                    if (drawStrike)
                    {
                        push_batch_quad(
                            decoMinX[l], baseY + strikeOff, lineW, thick,
                            effPivotX, effPivotY, rotRad,
                            r, g, b, decoAlpha[l],
                            0, static_cast<float>(QUAD_MODE_COLOR), spinRad,
                            0.0f, 0.0f, 1.0f, 1.0f,
                            whiteTex);
                    }
                }
            }
        }

        // Stockfish-Style Compile-Time Quad Emitter Specialization (Rule 7):
        // Emits directly from 64-byte QuadCommand into InstancedBatch (0 RenderCommand marshalling).
        template <u32 RenderMode>
        struct BatchQuadEmitter
        {
            static inline void emit(
                const QuadCommand &cmd,
                GLuint glyphAtlasTex,
                GLuint whiteTex) noexcept
            {
                const float boundsX = cmd.quadX;
                const float boundsY = cmd.quadY;
                const float boundsW = cmd.quadW;
                const float boundsH = cmd.quadH;
                const float pivotX = cmd.pivotX;
                const float pivotY = cmd.pivotY;
                const float rotRad = cmd.rotRad;
                const float spinRad = cmd.spinRad;
                const float r = cmd.red;
                const float g = cmd.green;
                const float b = cmd.blue;
                const float a = cmd.alpha;
                if constexpr (RenderMode == QUAD_MODE_TEXT)
                {
                    if (cmd.resourceSlot > 0 && cmd.resourceSlot <= MAX_GLYPH_CACHE_SLOTS)
                    {
                        const CachedGlyphRun &run = s_glyphCache[cmd.resourceSlot];
                        if (run.glyphCount == 0 || run.lineCount == 0)
                            return;
                        const float maxLineWidth = run.maxLineWidth;
                        const float totalLinesH = run.totalLinesH;
                        const bool hasExplicitWidth = run.hasExplicitWidth != 0;
                        const bool hasExplicitHeight = run.hasExplicitHeight != 0;
                        const float anchorX = run.anchorX;
                        const float anchorY = run.anchorY;
                        float boxW = boundsW;
                        float boxH = boundsH;
                        float boxX = boundsX;
                        float boxY = boundsY;
                        float topMargin = boundsY;
                        if (!hasExplicitWidth)
                        {
                            const float newBoxW = (maxLineWidth > 0.0f) ? maxLineWidth : boundsW;
                            const float originX = boundsX + anchorX * boundsW;
                            boxX = originX - anchorX * newBoxW;
                            boxW = newBoxW;
                        }
                        if (!hasExplicitHeight)
                        {
                            boxH = (totalLinesH > 0.0f) ? totalLinesH : boundsH;
                            const float originY = boundsY + anchorY * boundsH;
                            boxY = originY - anchorY * boxH;
                            topMargin = boxY;
                        }
                        else
                        {
                            topMargin = boundsY + (boundsH - totalLinesH) * 0.5f;
                        }
                        const float effPivotX = boxX + boxW * 0.5f;
                        const float effPivotY = boxY + boxH * 0.5f;
                        const u32 revealMode = cmd.revealMode;
                        const i32 relMs = cmd.revealRelMs;
                        const u16 kfCount = run.kfCount;

                        if ((revealMode == 1 || revealMode == 2) && kfCount > 0 && relMs < run.kfOffsets[0])
                            return;

                        const bool wantDecoLines = (run.decoFlags & (TextDecoration::Underline | TextDecoration::Strikethrough)) != 0;
                        const float alignBaseX = boxX + boxW * run.alignFactor;
                        const float baseTopY = topMargin + run.supShift;

                        const u32 modeIdx = revealMode < 4u ? revealMode : 0u;
                        emit_text_glyphs(
                            run, cmd, boxX, boxY, boxW, boxH, alignBaseX, baseTopY, topMargin,
                            effPivotX, effPivotY, glyphAtlasTex, whiteTex, modeIdx, wantDecoLines);
                        return;
                    }
                }
                if constexpr (RenderMode == QUAD_MODE_TEXTURE || RenderMode == QUAD_MODE_MASKED_TEXTURE)
                {
                    if (cmd.textureId == 0)
                    {
                        return;
                    }
                }
                push_batch_quad(
                    boundsX, boundsY, boundsW, boundsH,
                    pivotX, pivotY, rotRad,
                    r, g, b, a,
                    cmd.textureId, static_cast<float>(cmd.renderMode), spinRad,
                    0.0f, 1.0f, 1.0f, 0.0f,
                    whiteTex);
            }
        };
        using EmitFn = void (*)(const QuadCommand&, GLuint, GLuint) noexcept;
        static constexpr MultiArray<EmitFn, 5> EMIT_TABLE = {{
            &BatchQuadEmitter<QUAD_MODE_COLOR>::emit,
            &BatchQuadEmitter<QUAD_MODE_TEXTURE>::emit,
            &BatchQuadEmitter<QUAD_MODE_MASKED_TEXTURE>::emit,
            &BatchQuadEmitter<QUAD_MODE_TEXT>::emit,
            &BatchQuadEmitter<QUAD_MODE_TEXT_REVEAL>::emit
        }};
        static inline void push_render_command(
            const QuadCommand &cmd,
            GLuint glyphAtlasTex,
            GLuint whiteTex) noexcept
        {
            if (__builtin_expect(cmd.visible == 0u || cmd.quadW <= 0.0f || cmd.quadH <= 0.0f, 0))
            {
                return;
            }
            const u32 mode = (cmd.renderMode < 5u) ? cmd.renderMode : 0u;
            EMIT_TABLE[mode](cmd, glyphAtlasTex, whiteTex);
        }
        static PassRenderTarget *bind_pass_target(u32 targetId, u32 width, u32 height, GLuint sceneFbo) noexcept
        {
            if (targetId == 0)
            {
                g_gpuState.bind_framebuffer(sceneFbo, width, height);
                return nullptr;
            }
            if (targetId >= timeline::runtime::RUNTIME_RENDER_TARGET_COUNT)
                return nullptr;
            PassRenderTarget &t = s_passTargets[targetId];
            ensure_fbo_target(t.fbo, t.tex, t.width, t.height, width, height, GL_LINEAR);
            g_gpuState.bind_framebuffer(t.fbo, width, height);
            // TRỤ CỘT 2: TILE SRAM LOADOP = DONTCARE (Bỏ qua nạp tile VRAM cũ vào tile memory)
            g_gpuState.discard_color_attachment(GL_FRAMEBUFFER);
            return &t;
        }
        static GLuint resolve_source_tex(u32 targetId, GLuint sceneTex) noexcept
        {
            if (targetId == 0)
                return sceneTex;
            if (targetId >= timeline::runtime::RUNTIME_RENDER_TARGET_COUNT)
                return sceneTex;
            return s_passTargets[targetId].tex;
        }
        static void resolve_target_size(u32 targetId, u32 fallbackW, u32 fallbackH, u32 &outW, u32 &outH) noexcept
        {
            if (targetId == 0 || targetId >= timeline::runtime::RUNTIME_RENDER_TARGET_COUNT)
            {
                outW = fallbackW;
                outH = fallbackH;
                return;
            }
            const PassRenderTarget &t = s_passTargets[targetId];
            outW = t.width > 0 ? t.width : fallbackW;
            outH = t.height > 0 ? t.height : fallbackH;
        }
#endif // defined(STOCKCUT_HAS_GLES3)
    } // namespace
    bool Render::init(const char *canvasSelector) noexcept
    {
#if defined(STOCKCUT_HAS_GLES3)
        if (s_initialized)
            return true;
        if (!g_gpuState.init_hardware_context(canvasSelector))
        {
            return false;
        }
        s_instancedProgram = effect::link_effect_program(render::kInstancedQuadVS, kBatchFS);
        s_copyProgram = effect::link_effect_program(effect::kQuadVS, kCopyFS);
        s_blurProgram = effect::link_effect_program(effect::kQuadVS, kBlurFS);
        s_brightnessProgram = effect::link_effect_program(effect::kQuadVS, kBrightnessFS);
        s_transformProgram = effect::link_effect_program(effect::kQuadVS, kTransformFS);
        s_bloomExtractProgram = effect::link_effect_program(effect::kQuadVS, kBloomExtractFS);
        s_bloomDownProgram = effect::link_effect_program(effect::kQuadVS, kBloomDownFS);
        s_bloomUpProgram = effect::link_effect_program(effect::kQuadVS, kBloomUpFS);
        s_bloomCompositeProgram = effect::link_effect_program(effect::kQuadVS, kBloomCompositeFS);
        s_monoidProgram = effect::link_effect_program(effect::kQuadVS, kMonoidFS);
        if (!s_instancedProgram || !s_copyProgram || !s_blurProgram || !s_brightnessProgram ||
            !s_transformProgram || !s_bloomExtractProgram || !s_bloomDownProgram ||
            !s_bloomUpProgram || !s_bloomCompositeProgram || !s_monoidProgram)
        {
            return false;
        }
        effect::fusion_kernel_init();
        s_locInstancedRes = glGetUniformLocation(s_instancedProgram, "u_res");
        MultiArray<char, 20> uname{};
        copy_cstr_bounded(uname.data(), 20, "u_textures[0]");
        for (u32 i = 0; i < MAX_TEXTURE_SLOTS; ++i)
        {
            uname[11] = static_cast<char>('0' + i);
            s_locInstancedTextures[i] = glGetUniformLocation(s_instancedProgram, uname.data());
        }
        s_locCopyTex = glGetUniformLocation(s_copyProgram, "u_tex");
        s_locBlurTex = glGetUniformLocation(s_blurProgram, "u_tex");
        s_locBlurTexel = glGetUniformLocation(s_blurProgram, "u_texel");
        s_locBlurDir = glGetUniformLocation(s_blurProgram, "u_direction");
        s_locBlurTapCount = glGetUniformLocation(s_blurProgram, "u_tapCount");
        s_locBlurWeights = glGetUniformLocation(s_blurProgram, "u_weights[0]");
        s_locBrightTex = glGetUniformLocation(s_brightnessProgram, "u_tex");
        s_locBrightVal = glGetUniformLocation(s_brightnessProgram, "u_brightness");
        s_locTransTex = glGetUniformLocation(s_transformProgram, "u_tex");
        s_locTransRes = glGetUniformLocation(s_transformProgram, "u_res");
        s_locTransShift = glGetUniformLocation(s_transformProgram, "u_shift");
        s_locTransScale = glGetUniformLocation(s_transformProgram, "u_scale");
        s_locTransRot = glGetUniformLocation(s_transformProgram, "u_rotation");
        s_locTransTiled = glGetUniformLocation(s_transformProgram, "u_tiled");
        s_locTransMirror = glGetUniformLocation(s_transformProgram, "u_mirror");
        s_locBloomExtTex = glGetUniformLocation(s_bloomExtractProgram, "u_tex");
        s_locBloomExtTexel = glGetUniformLocation(s_bloomExtractProgram, "u_texel");
        s_locBloomExtThreshold = glGetUniformLocation(s_bloomExtractProgram, "u_threshold");
        s_locBloomExtHasColor = glGetUniformLocation(s_bloomExtractProgram, "u_hasColor");
        s_locBloomExtTolerance = glGetUniformLocation(s_bloomExtractProgram, "u_tolerance");
        s_locBloomExtTargetColor = glGetUniformLocation(s_bloomExtractProgram, "u_targetColor");
        s_locBloomDownTex = glGetUniformLocation(s_bloomDownProgram, "u_tex");
        s_locBloomDownTexel = glGetUniformLocation(s_bloomDownProgram, "u_texel");
        s_locBloomUpTex = glGetUniformLocation(s_bloomUpProgram, "u_tex");
        s_locBloomUpBase = glGetUniformLocation(s_bloomUpProgram, "u_base");
        s_locBloomUpTexel = glGetUniformLocation(s_bloomUpProgram, "u_texel");
        s_locBloomUpWLow = glGetUniformLocation(s_bloomUpProgram, "u_wLow");
        s_locBloomUpWHi = glGetUniformLocation(s_bloomUpProgram, "u_wHi");
        s_locBloomCompTex = glGetUniformLocation(s_bloomCompositeProgram, "u_tex");
        s_locBloomCompBase = glGetUniformLocation(s_bloomCompositeProgram, "u_base");
        s_locBloomCompTexel = glGetUniformLocation(s_bloomCompositeProgram, "u_texel");
        s_locBloomCompGlowTexel = glGetUniformLocation(s_bloomCompositeProgram, "u_glowTexel");
        s_locBloomCompIntensity = glGetUniformLocation(s_bloomCompositeProgram, "u_intensity");
        s_locBloomCompFactor = glGetUniformLocation(s_bloomCompositeProgram, "u_factor");
        s_locBloomCompAa = glGetUniformLocation(s_bloomCompositeProgram, "u_aa");
        s_locBloomCompEnv = glGetUniformLocation(s_bloomCompositeProgram, "u_env");
        s_locMonoidTex = glGetUniformLocation(s_monoidProgram, "u_tex");
        s_locMonoidRes = glGetUniformLocation(s_monoidProgram, "u_res");
        s_locMonoidCount = glGetUniformLocation(s_monoidProgram, "u_monoidCount");
        s_locMonoidBBox = glGetUniformLocation(s_monoidProgram, "u_opBBox[0]");
        s_locMonoidPremul = glGetUniformLocation(s_monoidProgram, "u_opPremul[0]");
        s_locMonoidTrans = glGetUniformLocation(s_monoidProgram, "u_opTrans[0]");
        s_locMonoidUseUber = glGetUniformLocation(s_monoidProgram, "u_useUberShader");
        s_locMonoidUberMat = glGetUniformLocation(s_monoidProgram, "u_uberMat");
        s_locMonoidUberTrans = glGetUniformLocation(s_monoidProgram, "u_uberTrans");
        s_locMonoidUberGain = glGetUniformLocation(s_monoidProgram, "u_uberGain");
        s_locMonoidUberEmit = glGetUniformLocation(s_monoidProgram, "u_uberEmit");

        // Khởi tạo cố định sampler uniforms cho toàn bộ program shader 1 lần duy nhất:
        glUseProgram(s_instancedProgram);
        for (u32 i = 0; i < MAX_TEXTURE_SLOTS; ++i)
        {
            if (s_locInstancedTextures[i] >= 0)
            {
                glUniform1i(s_locInstancedTextures[i], static_cast<GLint>(i));
            }
        }
        glUseProgram(s_copyProgram);
        if (s_locCopyTex >= 0) glUniform1i(s_locCopyTex, 0);

        glUseProgram(s_blurProgram);
        if (s_locBlurTex >= 0) glUniform1i(s_locBlurTex, 0);

        glUseProgram(s_brightnessProgram);
        if (s_locBrightTex >= 0) glUniform1i(s_locBrightTex, 0);

        glUseProgram(s_transformProgram);
        if (s_locTransTex >= 0) glUniform1i(s_locTransTex, 0);

        glUseProgram(s_bloomExtractProgram);
        if (s_locBloomExtTex >= 0) glUniform1i(s_locBloomExtTex, 0);

        glUseProgram(s_bloomDownProgram);
        if (s_locBloomDownTex >= 0) glUniform1i(s_locBloomDownTex, 0);

        glUseProgram(s_bloomUpProgram);
        if (s_locBloomUpTex >= 0) glUniform1i(s_locBloomUpTex, 0);
        if (s_locBloomUpBase >= 0) glUniform1i(s_locBloomUpBase, 1);

        glUseProgram(s_bloomCompositeProgram);
        if (s_locBloomCompTex >= 0) glUniform1i(s_locBloomCompTex, 0);
        if (s_locBloomCompBase >= 0) glUniform1i(s_locBloomCompBase, 1);

        glUseProgram(s_monoidProgram);
        if (s_locMonoidTex >= 0) glUniform1i(s_locMonoidTex, 0);

        g_gpuState.reset();

        // TRỤ CỘT 1 & 2: KHỞI TẠO UBO RING-BUFFERS (std140) CHO FRAME N, N+1, N+2 PIPELINING
        render::g_frameUboRing.init(render::UBO_BINDING_FRAME);
        render::g_passUboRing.init(render::UBO_BINDING_PASS);
        render::g_monoidUboRing.init(render::UBO_BINDING_MONOID);

        // TRỤ CỘT 3: KHỞI TẠO PBO TRIPLE-BUFFERING CHO ASYNC DMA TEXTURE STREAMING
        render::g_glyphPboRing.init();

        // TRỤ CỘT 4: KHỞI TẠO HARDWARE INSTANCED QUAD RENDERING BATCH (64B Cacheline + Vertex Pulling)
        render::g_gpuInstancedBatch.init();
        gpu_driven::get_gpu_pipeline_manager().init();

        // Fullscreen Quad VAO & VBO for post-processing passes
        static constexpr MultiArray<float, 8> kQuadVerts = {{-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f}};
        glGenVertexArrays(1, &s_quadVao);
        glGenBuffers(1, &s_quadVbo);
        glBindVertexArray(s_quadVao);
        glBindBuffer(GL_ARRAY_BUFFER, s_quadVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVerts), kQuadVerts.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
        // 3. RenderLayer VBOs eliminated (Zero Duplication)
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        // 4. 1x1 White fallback texture
        static constexpr MultiArray<u8, 4> kWhitePixel = {{255u, 255u, 255u, 255u}};
        glGenTextures(1, &s_whiteTex);
        glBindTexture(GL_TEXTURE_2D, s_whiteTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, kWhitePixel.data());
        // 5. Native FreeType/HarfBuzz R8 Glyph Atlas Texture
        glGenTextures(1, &s_glyphAtlasTex);
        glBindTexture(GL_TEXTURE_2D, s_glyphAtlasTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_FLIP_Y_WEBGL_ENUM, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_R8,
            static_cast<GLsizei>(NATIVE_GLYPH_ATLAS_SIZE),
            static_cast<GLsizei>(NATIVE_GLYPH_ATLAS_SIZE),
            0, GL_RED, GL_UNSIGNED_BYTE,
            text_font_get_atlas());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        text_font_reset_atlas_dirty_rect();
        s_initialized = true;
        return true;
#else
        (void)canvasSelector;
        return false;
#endif
    }
    void Render::begin_frame(i32 width, i32 height, float r, float g, float b, float a) noexcept
    {
#if defined(STOCKCUT_HAS_GLES3)
        if (!s_initialized)
            return;
        glViewport(0, 0, width, height);
        glClearColor(r, g, b, a);
        glClear(GL_COLOR_BUFFER_BIT);
#else
        (void)width;
        (void)height;
        (void)r;
        (void)g;
        (void)b;
        (void)a;
#endif
    }
    void Render::sync_glyph_atlas() noexcept
    {
#if defined(STOCKCUT_HAS_GLES3)
        if (!s_initialized || !s_glyphAtlasTex)
        {
            return;
        }
        const u32 *dirty = text_font_get_atlas_dirty_rect();
        if (!dirty || dirty[0] == INVALID_U32)
        {
            return;
        }
        const u32 x0 = dirty[0];
        const u32 y0 = dirty[1];
        const u32 x1 = dirty[2];
        const u32 y1 = dirty[3];
        if (x1 <= x0 || y1 <= y0 || x1 > NATIVE_GLYPH_ATLAS_SIZE || y1 > NATIVE_GLYPH_ATLAS_SIZE)
        {
            text_font_reset_atlas_dirty_rect();
            return;
        }
        const u32 width = x1 - x0;
        const u32 height = y1 - y0;
        const u8 *atlas = text_font_get_atlas();
        // Copy từng dòng bằng std::memcpy (SIMD vectorized) thay vì lặp từng pixel
        u8 *dstPtr = s_glyphAtlasRowScratch;
        const u8 *srcPtr = atlas + (y0 * NATIVE_GLYPH_ATLAS_SIZE + x0);
        for (u32 y = 0; y < height; ++y)
        {
            std::memcpy(dstPtr, srcPtr, width);
            dstPtr += width;
            srcPtr += NATIVE_GLYPH_ATLAS_SIZE;
        }
        glBindTexture(GL_TEXTURE_2D, s_glyphAtlasTex);
        glPixelStorei(GL_UNPACK_FLIP_Y_WEBGL_ENUM, 0);
        glPixelStorei(0x9241 /* GL_UNPACK_PREMULTIPLY_ALPHA_WEBGL */, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        // TRỤ CỘT 3: PBO TRIPLE-BUFFERING CHO ASYNC DMA TEXTURE STREAMING (ZERO CPU STALL)
        render::g_glyphPboRing.advance_ring();
        render::g_glyphPboRing.stream_sub_image_2d(
            s_glyphAtlasTex,
            static_cast<GLint>(x0), static_cast<GLint>(y0),
            static_cast<GLsizei>(width), static_cast<GLsizei>(height),
            GL_RED, GL_UNSIGNED_BYTE,
            s_glyphAtlasRowScratch,
            width * height);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        text_font_reset_atlas_dirty_rect();
#endif
    }
    u32 Render::alloc_texture() noexcept
    {
#if defined(STOCKCUT_HAS_GLES3)
        if (!s_initialized)
            return 0;
        GLuint tex = 0;
        glGenTextures(1, &tex);
        if (!tex)
            return 0;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return tex;
#else
        return 0;
#endif
    }
    void Render::bind_texture(u32 texId) noexcept
    {
#if defined(STOCKCUT_HAS_GLES3)
        if (!s_initialized)
            return;
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texId));
#else
        (void)texId;
#endif
    }
    void Render::free_texture(u32 texId) noexcept
    {
#if defined(STOCKCUT_HAS_GLES3)
        if (!s_initialized || !texId)
            return;
        GLuint t = static_cast<GLuint>(texId);
        glDeleteTextures(1, &t);
#else
        (void)texId;
#endif
    }
    float *Render::get_glyph_upload_scratch() noexcept
    {
        return s_glyphUploadScratch;
    }
    float *Render::get_line_widths_scratch() noexcept
    {
        return s_lineWidthsScratch;
    }
    void Render::store_glyph_cache(
        u32 cacheSlot,
        const float *glyphDataF32,
        u32 glyphCount,
        const float *lineWidthsF32,
        u32 lineCount,
        float lineHeight,
        u32 alignCode,
        float baselineOffset,
        const i32 *kfOffsets,
        u16 kfCount,
        u16 revealDurationMs) noexcept
    {
        if (cacheSlot == 0 || cacheSlot > MAX_GLYPH_CACHE_SLOTS)
            return;
        s_usedGlyphCacheBB.set_bit(cacheSlot - 1u);
        CachedGlyphRun &run = s_glyphCache[cacheSlot];
        const u32 cappedGlyphs = glyphCount <= MAX_GLYPHS_PER_CACHED_RUN ? glyphCount : MAX_GLYPHS_PER_CACHED_RUN;
        const u32 cappedLines = lineCount <= MAX_LINES_PER_CACHED_RUN ? lineCount : MAX_LINES_PER_CACHED_RUN;
        run.glyphCount = cappedGlyphs;
        run.lineCount = cappedLines;
        run.alignCode = alignCode;
        run.lineHeight = lineHeight;
        run.baselineOffset = baselineOffset;
        run.kfCount = kfCount;
        run.revealDurationMs = revealDurationMs;
        if (kfOffsets && kfCount > 0)
        {
            const u32 copyCount = (kfCount < MAX_TIMELINE_SLOTS) ? kfCount : MAX_TIMELINE_SLOTS;
            for (u32 k = 0; k < copyCount; ++k)
                run.kfOffsets[k] = kfOffsets[k];
        }
        float maxW = 0.0f;
        for (u32 l = 0; l < cappedLines; ++l)
        {
            const float lw = lineWidthsF32 ? lineWidthsF32[l] : 0.0f;
            run.lineWidths[l] = lw;
            if (lw > maxW)
                maxW = lw;
        }
        run.maxLineWidth = maxW;
        run.totalLinesH = static_cast<float>(cappedLines) * lineHeight;
        run.ascender = (baselineOffset > 0.0f) ? baselineOffset : (lineHeight * 0.75f);
        const float descender = run.ascender * 0.125f;
        run.resolvedBaselineOffset = (lineHeight + run.ascender - descender) * 0.5f;
        run.alignFactor = static_cast<float>(alignCode <= 2 ? alignCode : 0) * 0.5f;
        run.invGlyphCount = 1.0f / static_cast<float>(cappedGlyphs > 1 ? cappedGlyphs : 1);
        run.anchorX = 0.5f;
        run.anchorY = 0.5f;
        run.supShift = 0.0f;
        run.decoFlags = 0u;
        run.hasExplicitWidth = 1u;
        run.hasExplicitHeight = 0u;
        run.pad = 0u;
        if (!glyphDataF32)
        {
            run.glyphCount = 0;
            return;
        }
        constexpr float invAtlas = 1.0f / static_cast<float>(NATIVE_GLYPH_ATLAS_SIZE);
        for (u32 i = 0; i < cappedGlyphs; ++i)
        {
            const float *src = glyphDataF32 + i * GLYPH_UPLOAD_STRIDE_F32;
            CachedGlyphQuad &dst = run.glyphs[i];
            dst.x = src[0];
            dst.y = src[1];
            dst.kfIndex = static_cast<u16>(src[2] >= 0.0f ? src[2] : 0.0f);
            const u16 rawLine = static_cast<u16>(src[3] >= 0.0f ? src[3] : 0.0f);
            const u16 lineIdx = (cappedLines > 0 && rawLine < cappedLines) ? rawLine : 0;
            dst.line = lineIdx;
            dst.atlasX = static_cast<u16>(src[4] >= 0.0f ? src[4] : 0.0f);
            dst.atlasY = static_cast<u16>(src[5] >= 0.0f ? src[5] : 0.0f);
            dst.width = static_cast<u16>(src[6] >= 0.0f ? src[6] : 0.0f);
            dst.height = static_cast<u16>(src[7] >= 0.0f ? src[7] : 0.0f);
            dst.relX = dst.x - run.lineWidths[lineIdx] * run.alignFactor;
            dst.relY = static_cast<float>(lineIdx) * lineHeight + run.resolvedBaselineOffset + dst.y;
            dst.gw = static_cast<float>(dst.width);
            dst.gh = static_cast<float>(dst.height);
            dst.u0 = static_cast<float>(dst.atlasX) * invAtlas;
            dst.v0 = static_cast<float>(dst.atlasY) * invAtlas;
            dst.u1 = static_cast<float>(dst.atlasX + dst.width) * invAtlas;
            dst.v1 = static_cast<float>(dst.atlasY + dst.height) * invAtlas;
        }
    }
    void Render::set_slot_texture(u32 slot, u32 texId) noexcept
    {
        s_slotTextures[slot & (MAX_TIMELINE_SLOTS - 1u)] = texId;
    }
    u32 Render::get_slot_texture(u32 slot) noexcept
    {
        return s_slotTextures[slot & (MAX_TIMELINE_SLOTS - 1u)];
    }
    void Render::set_resource_texture(u32 resourceId, u32 texId) noexcept
    {
        if (!resourceId)
            return;
        if (resourceId < FAST_RESOURCE_LOOKUP_CAP)
            s_resourceTextureLookup[resourceId] = texId;
        bool found = false;
        for (u32 i = 0; i < s_resourceTextureCount; ++i)
        {
            if (s_resourceTextures[i].resourceId == resourceId)
            {
                s_resourceTextures[i].texId = texId;
                found = true;
                break;
            }
        }
        if (!found && s_resourceTextureCount < MAX_RESOURCE_TEXTURES)
        {
            s_resourceTextures[s_resourceTextureCount++] = {resourceId, texId};
        }
        // Tu dong cap nhat s_slotTextures cho moi slot khop voi resourceId
        Position<> *pos = timeline_position_get_instance();
        if (pos)
        {
            Bitboard occ = pos->populated_slots();
            while (occ)
            {
                const u32 slot = occ.pop_lsb();
                if (pos->item(slot).resourceId == resourceId)
                {
                    s_slotTextures[slot] = texId;
                }
            }
        }
    }
    u32 Render::get_resource_texture(u32 resourceId) noexcept
    {
        if (!resourceId)
            return 0;
        if (resourceId < FAST_RESOURCE_LOOKUP_CAP)
            return s_resourceTextureLookup[resourceId];
        for (u32 i = 0; i < s_resourceTextureCount; ++i)
        {
            if (s_resourceTextures[i].resourceId == resourceId)
                return s_resourceTextures[i].texId;
        }
        return 0;
    }
    void Render::set_slot_glyph_cache(u32 slot, u32 cacheSlot) noexcept
    {
        const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
        s_slotGlyphCache[s] = cacheSlot;
        if (cacheSlot > 0 && cacheSlot <= MAX_GLYPH_CACHE_SLOTS)
        {
            CachedGlyphRun &run = s_glyphCache[cacheSlot];
            Position<> *pos = timeline_position_get_instance();
            if (pos)
            {
                const TimelineItem &it = pos->item(slot);
                const TextBlock &tb = pos->slot_block(slot);
                run.hasExplicitWidth = ((it.flags & ITEM_FLAG_EXPLICIT_WIDTH) != 0) ? 1u : 0u;
                run.hasExplicitHeight = ((it.flags & ITEM_FLAG_EXPLICIT_HEIGHT) != 0) ? 1u : 0u;
                run.anchorX = tb.get_value<float, text_reg::ANCHOR_X>();
                run.anchorY = tb.get_value<float, text_reg::ANCHOR_Y>();
                run.decoFlags = tb.get_value<u8, text_bit::DECORATION_UNDERLINE, 3>();
                run.supShift = -static_cast<float>((run.decoFlags >> 2) & 1u) * (run.ascender * 0.40f);
                if (run.glyphCount > 0 && run.lineCount > 0 && run.maxLineWidth > 0.0f && run.totalLinesH > 0.0f)
                {
                    const float sx = (s_slotTextSources[slot].lastShapedScaleX > OPACITY_EPSILON)
                                         ? s_slotTextSources[slot].lastShapedScaleX
                                         : 1.0f;
                    pos->update_slot_text_bounds(slot, run.maxLineWidth / sx, run.totalLinesH / sx);
                }
            }
        }
    }
    u32 Render::get_glyph_cache_count(u32 cacheSlot) noexcept
    {
        if (cacheSlot > 0 && cacheSlot <= MAX_GLYPH_CACHE_SLOTS)
        {
            return s_glyphCache[cacheSlot].glyphCount;
        }
        return 0;
    }
    const CachedGlyphRun &Render::get_glyph_cache_run(u32 cacheSlot) noexcept
    {
        return s_glyphCache[(cacheSlot > 0 && cacheSlot <= MAX_GLYPH_CACHE_SLOTS) ? cacheSlot : 0];
    }
    void Render::reset_project_state() noexcept
    {
        // Preserve s_resourceTextures so preloaded media resourceId -> texId bindings survive reloads.
        for (u32 i = 0; i < MAX_TIMELINE_SLOTS; ++i)
        {
            s_slotTextures[i] = 0;
            s_slotGlyphCache[i] = 0;
        }
        s_activeTextSourcesBB.for_each_active([](u32 i) noexcept
        {
            TextSource &src = s_slotTextSources[i];
            src.utf8Len = 0;
            src.charCount = 0;
            src.utf8[0] = '\0';
            src.fontFamily[0] = '\0';
            src.lastShapedScaleX = 0.0f;
            src.lastFontFileCount = 0u;
            src.dirty = false;
        });
        s_activeTextSourcesBB.clear();
        s_usedGlyphCacheBB.for_each_active([](u32 bit) noexcept
        {
            CachedGlyphRun &run = s_glyphCache[bit + 1u];
            run.glyphCount = 0;
            run.lineCount = 0;
            run.kfCount = 0;
        });
        s_usedGlyphCacheBB.clear();
        s_gpuSurfaceTT.invalidate_entries();
        s_zobristPyramid.clear();
        s_tileMMU.clear();
        s_lastParamKey = 0;
        s_lastW = 0;
        s_lastH = 0;
        s_lastNowMs = 0.0;
        s_lastCmdCount = 0;
        s_prevFrameHadPostFx = false;
        s_cachedFboForScissor = 0;
        g_gpuState.invalidate_all();
        g_slotTracker.reset();
        effect_engine_init();
        auto *script = get_active_music_script();
        if (script)
            script->reset();
    }
    u32 Render::get_slot_glyph_cache(u32 slot) noexcept
    {
        return (slot < MAX_TIMELINE_SLOTS) ? s_slotGlyphCache[slot] : 0;
    }
    namespace
    {
        template <TextTransformMode Mode>
        void shape_slot_text_lines(const TextSource &src, u32 lineCount, float pixelSize) noexcept
        {
            text_font_begin_glyph_batch(lineCount);
            char *utf8Buf = reinterpret_cast<char *>(text_font_get_utf8_buffer());
            u32 *kfMapBuf = text_font_get_keyframe_map_buffer();
            const u32 utf8Cap = text_font_get_utf8_capacity();
            const char *cursor = src.utf8.data();
            const char *const end = cursor + src.utf8Len;
            u32 charIdx = 0;
            for (u32 lineIdx = 0; lineIdx < lineCount; ++lineIdx)
            {
                u32 lineByteLen = 0;
                while (cursor < end)
                {
                    if (*cursor == '\n')
                    {
                        ++cursor;
                        ++charIdx;
                        break;
                    }
                    const u32 rawCp = utf8_decode_next(cursor, end);
                    const u32 codeUnits = (rawCp >= 0x10000u) ? 2u : 1u;
                    const u32 xformCp = transform_unicode_case<Mode>(rawCp);
                    if (lineByteLen + 4u <= utf8Cap)
                    {
                        const u32 written = utf8_encode_cp(xformCp, utf8Buf + lineByteLen);
                        const u32 mappedVal = (charIdx < SLOT_TEXT_CHAR_MAP_CAP) ? static_cast<u32>(src.charKfIndex[charIdx]) : charIdx;
                        for (u32 b = 0; b < written; ++b)
                        {
                            kfMapBuf[lineByteLen + b] = mappedVal;
                        }
                        lineByteLen += written;
                    }
                    charIdx += codeUnits;
                }
                text_font_set_keyframe_map_count(lineByteLen);
                text_font_shape(lineByteLen, pixelSize);
                text_font_append_shaped_line(lineIdx);
            }
        }
        using ShapeLinesFn = void (*)(const TextSource &, u32, float) noexcept;
        static constexpr MultiArray<ShapeLinesFn, TEXT_TRANSFORM_MODE_NB> kShapeLinesLut = {{
            &shape_slot_text_lines<TextTransformMode::None>,
            &shape_slot_text_lines<TextTransformMode::Uppercase>,
            &shape_slot_text_lines<TextTransformMode::Lowercase>,
            &shape_slot_text_lines<TextTransformMode::Capitalize>,
        }};
    } // namespace
    void Render::set_slot_text_source(
        u32 slot,
        const char *utf8,
        u32 utf8Len,
        const u16 *charKfIndex,
        u32 charCount,
        const char *fontFamily) noexcept
    {
        const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
        TextSource &src = s_slotTextSources[s];
        src.utf8Len = 0;
        src.charCount = 0;
        src.utf8[0] = '\0';
        src.fontFamily[0] = '\0';
        src.lastShapedScaleX = 0.0f;
        src.lastFontFileCount = 0u;
        src.dirty = false;
        if (!utf8 || utf8Len == 0)
        {
            s_activeTextSourcesBB.clear_bit(s);
            return;
        }
        s_activeTextSourcesBB.set_bit(s);
        const u32 copyLen = utf8Len < static_cast<u32>(SLOT_TEXT_UTF8_CAP - 1) ? utf8Len : static_cast<u32>(SLOT_TEXT_UTF8_CAP - 1);
        std::memcpy(src.utf8.data(), utf8, copyLen);
        src.utf8[copyLen] = '\0';
        src.utf8Len = static_cast<u16>(copyLen);
        const u32 copyChars = charCount < static_cast<u32>(SLOT_TEXT_CHAR_MAP_CAP) ? charCount : static_cast<u32>(SLOT_TEXT_CHAR_MAP_CAP);
        src.charCount = static_cast<u16>(copyChars);
        if (charKfIndex && copyChars > 0)
        {
            std::memcpy(src.charKfIndex.data(), charKfIndex, copyChars * sizeof(u16));
        }
        else
        {
            for (u32 i = 0; i < static_cast<u32>(SLOT_TEXT_CHAR_MAP_CAP); ++i)
                src.charKfIndex[i] = static_cast<u16>(i);
        }
        if (fontFamily && fontFamily[0])
        {
            copy_cstr_bounded(src.fontFamily.data(), SLOT_FONT_FAMILY_CAP, fontFamily);
        }
        src.dirty = true;
    }
    void Render::invalidate_all_text_caches() noexcept
    {
        s_activeTextSourcesBB.for_each_active([](u32 i) noexcept
        {
            s_slotTextSources[i].dirty = true;
        });
        s_lastParamKey = 0;
    }
    void Render::invalidate_slot_text_cache(u32 slot) noexcept
    {
        const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
        if (s_slotTextSources[s].utf8Len > 0)
        {
            s_slotTextSources[s].dirty = true;
        }
        s_lastParamKey = 0;
    }
    void Render::ensure_slot_text_shaped(u32 slot, float sx) noexcept
    {
        const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
        TextSource &src = s_slotTextSources[s];
        if (src.utf8Len == 0)
            return;
        const float safeSx = (sx > 0.01f && std::isfinite(sx)) ? sx : 1.0f;
        const u32 curFontFiles = font::FontMapRegistry::instance().loadedFileCount();
        if (!src.dirty &&
            s_slotGlyphCache[slot] != 0 &&
            std::abs(src.lastShapedScaleX - safeSx) < 0.005f &&
            src.lastFontFileCount == curFontFiles)
        {
            return;
        }
        Position<> *pos = timeline_position_get_instance();
        if (!pos)
            return;
        TextBlock &tb = pos->slot_block(slot);
        u16 fileId = static_cast<u16>(tb.get_value<u32, text_reg::RESOLVED_FONT_ID>());
        if (src.dirty || src.lastFontFileCount != curFontFiles || fileId == 0 || !font::FontMapRegistry::instance().lookupByFileId(fileId).buffer)
        {
            const char *fam = src.fontFamily[0] ? src.fontFamily.data() : "sans-serif";
            const u16 fw = static_cast<u16>(tb.get_value<u32, text_reg::FONT_WEIGHT>());
            const u16 resolvedId = (tb.get_value<bool, text_bit::IS_ITALIC, 1>())
                                       ? font::FontMapRegistry::instance().resolveFontFileId<true>(fam, fw)
                                       : font::FontMapRegistry::instance().resolveFontFileId<false>(fam, fw);
            if (resolvedId != 0)
            {
                fileId = resolvedId;
                if (fileId != static_cast<u16>(tb.get_value<u32, text_reg::RESOLVED_FONT_ID>()))
                {
                    tb.set_value<text_reg::RESOLVED_FONT_ID>(static_cast<u32>(fileId));
                    pos->base_slot_block(slot).set_value<text_reg::RESOLVED_FONT_ID>(static_cast<u32>(fileId));
                }
            }
        }
        if (fileId != 0 && !font::FontMapRegistry::instance().lookupByFileId(fileId).buffer)
        {
            const font::FontFileRecord* rec = font::FontMapRegistry::instance().getFileRecord(fileId);
            if (rec && rec->path[0])
            {
                font::FontMapRegistry::instance().loadFontFileDirect(rec->path.data());
            }
        }
        bool usedFallbackFont = false;
        if (fileId == 0 || !font::FontMapRegistry::instance().lookupByFileId(fileId).buffer)
        {
            for (u32 fi = 1; fi <= font::FontMapRegistry::instance().fileCount(); ++fi)
            {
                if (font::FontMapRegistry::instance().lookupByFileId(static_cast<u16>(fi)).buffer)
                {
                    fileId = static_cast<u16>(fi);
                    usedFallbackFont = true;
                    break;
                }
            }
        }
        if (fileId == 0 || !font::FontMapRegistry::instance().lookupByFileId(fileId).buffer)
        {
            return;
        }
        if (!text_font_load_by_file_id(fileId))
            return;
        u32 lineCount = 1;
        for (u32 i = 0; i < src.utf8Len; ++i)
        {
            if (src.utf8[i] == '\n' && lineCount < MAX_LINES_PER_CACHED_RUN)
                ++lineCount;
        }
        const float tbFontSize = tb.get_value<float, text_reg::FONT_SIZE>();
        const float tbLineHeight = tb.get_value<float, text_reg::LINE_HEIGHT>();
        const float pixelSize = std::max(1.0f, tbFontSize * safeSx);
        const float lineHeightPx = std::max(1.0f, tbFontSize * (tbLineHeight > 0.0f ? tbLineHeight : static_cast<float>(DEFAULT_LINE_HEIGHT)) * safeSx);
        kShapeLinesLut[tb.get_value<u8, text_bit::TEXT_TRANSFORM_LO, 2>()](src, lineCount, pixelSize);
        const float baselineOffset = text_font_get_baseline_offset();
        const u32 cacheSlot = (slot % MAX_GLYPH_CACHE_SLOTS) + 1u;
        const KeyframeTrack &tr = pos->get_track(slot);
        MultiArray<i32, MAX_TIMELINE_SLOTS> offsets;
        const u32 copyTrCount = (tr.count < MAX_TIMELINE_SLOTS) ? tr.count : MAX_TIMELINE_SLOTS;
        for (u32 k = 0; k < copyTrCount; ++k)
        {
            offsets[k] = pos->keyframe_offset_ms(slot, k);
        }
        store_glyph_cache(
            cacheSlot,
            text_font_get_render_glyph_data(),
            text_font_get_render_glyph_count(),
            text_font_get_render_line_widths(),
            lineCount,
            lineHeightPx,
            tb.get_value<u8, text_bit::TEXT_ALIGN_LO, 2>(),
            baselineOffset,
            offsets.data(),
            static_cast<u16>(tr.count),
            static_cast<double>(tb.get_value<float, text_reg::REVEAL_DURATION_MS>()));
        src.lastShapedScaleX = safeSx;
        src.lastFontFileCount = font::FontMapRegistry::instance().loadedFileCount();
        src.dirty = usedFallbackFont;
        set_slot_glyph_cache(slot, cacheSlot);
        sync_glyph_atlas();
    }
#if defined(STOCKCUT_HAS_GLES3)
    // ========================================================================
    // ZERO-IF HOMOGENEOUS MICRO-STREAM POLICIES & TEMPLATE EXECUTORS
    // [STOCKFISH TEMPLATE-CENTRIC]: Unified PassPolicy<PassKindId, Param, ContractFlags>
    // ========================================================================
    template <u32 PassKindId, typename Param = void, u32 ContractFlags = FRAME_FLAG_NONE>
    struct PassPolicy {};

    template <u32 ContractFlags = FRAME_FLAG_NONE>
    using ClearPassPolicy = PassPolicy<0, void, ContractFlags>;
    template <bool Scissored, u32 ContractFlags = FRAME_FLAG_NONE>
    using DirectDrawSpanPolicy = PassPolicy<1, std::bool_constant<Scissored>, ContractFlags>;
    using OffscreenDrawPolicy = PassPolicy<2>;
    template <bool PremultipliedBlend>
    using CopyPassPolicy = PassPolicy<3, std::bool_constant<PremultipliedBlend>>;
    template <bool IsVertical>
    using BlurPassPolicy = PassPolicy<4, std::bool_constant<IsVertical>>;
    using ApplyBrightnessPassPolicy = PassPolicy<5>;
    template <bool IsShake>
    using TransformPassPolicy = PassPolicy<6, std::bool_constant<IsShake>>;
    using BloomExtractPassPolicy = PassPolicy<7>;
    using BloomDownsamplePassPolicy = PassPolicy<8>;
    using BloomUpsamplePassPolicy = PassPolicy<9>;
    using BloomCompositePassPolicy = PassPolicy<10>;
    using ShaderEffectPassPolicy = PassPolicy<11>;
    using MonoidFusedPassPolicy = PassPolicy<12>;

    constexpr u32 MONOID_UNIFORM_FLOATS_CAP = MAX_MONOID_UNIFORM_OPS * 4u;

    struct alignas(64) PassContext
    {
        u32 width;
        u32 height;
        u32 commandCount;
        const timeline::runtime::Frame<> &plan;
        const SpatialFrameDirty<> &frameDirty;
        float bgR, bgG, bgB, bgA;
        const IndexBucketList<MAX_TIMELINE_SLOTS, MAX_DISPLAY_COMMANDS> *slotCmdBuckets;
        const QuadCommand *cmds;
    };

    inline PassRenderTarget *begin_fullscreen_pass(
        const timeline::runtime::RenderPass &pass,
        GLuint &outInTex) noexcept
    {
        PassRenderTarget *outTarget = bind_pass_target(pass.outputTarget, pass.width, pass.height, Render::get_scene_fbo_id());
        outInTex = resolve_source_tex(pass.inputTarget, Render::get_scene_tex_id());
        g_gpuState.set_scissor<false>();
        g_gpuState.set_blend<false>();
        g_gpuState.bind_vao(Render::get_quad_vao());

        // TRỤ CỘT 1: UBO PASS UNIFORM PACKING (Thay thế hàng chục lệnh glUniform* phân mảnh)
        render::PassUniformBlock passBlock{};
        passBlock.texelSize[0] = (pass.width > 0) ? (1.0f / static_cast<float>(pass.width)) : 0.0f;
        passBlock.texelSize[1] = (pass.height > 0) ? (1.0f / static_cast<float>(pass.height)) : 0.0f;
        passBlock.texelSize[2] = static_cast<float>(pass.width);
        passBlock.texelSize[3] = static_cast<float>(pass.height);

        passBlock.params0[0] = pass.radius;
        passBlock.params0[1] = pass.intensity;
        passBlock.params0[2] = pass.parameter0;
        passBlock.params0[3] = pass.parameter1;

        passBlock.params1[0] = pass.parameter2;
        passBlock.params1[1] = pass.parameter3;
        passBlock.params1[2] = static_cast<float>(pass.kernelIndex);
        passBlock.params1[3] = 0.0f;

        passBlock.targetColor[0] = pass.parameter1;
        passBlock.targetColor[1] = pass.parameter2;
        passBlock.targetColor[2] = pass.parameter3;
        passBlock.targetColor[3] = pass.parameter0;

        render::g_passUboRing.upload_current(passBlock);
        render::g_passUboRing.bind_current();

        return outTarget;
    }

    inline void finish_fullscreen_pass(PassRenderTarget *outTarget) noexcept
    {
        (void)outTarget;
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    template <typename Policy>
    struct PassExecutor;

    // 1. ClearPassPolicy<ContractFlags>: Dọn sạch nền hoặc scissor box (Tile Preservation)
    template <u32 ContractFlags>
    struct PassExecutor<ClearPassPolicy<ContractFlags>>
    {
        static inline void execute(const timeline::runtime::RenderPass &pass, const PassContext &ctx) noexcept
        {
            PassRenderTarget *outTarget = bind_pass_target(pass.outputTarget, pass.width, pass.height, Render::s_sceneFbo);
            (void)outTarget;
            constexpr bool kCanScissorClear =
                ((ContractFlags & FRAME_FLAG_SPATIAL_DIRTY) != 0u) &&
                ((ContractFlags & (FRAME_FLAG_SEEKED | FRAME_FLAG_HAS_POST_FX | FRAME_FLAG_SCENE_INVALID)) == 0u);
            if constexpr (kCanScissorClear)
            {
                if (pass.outputTarget == 0)
                {
                    if (Render::s_cachedFboForScissor != 0)
                    {
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, Render::s_cachedFboForScissor);
                        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, Render::s_sceneFbo);
                        glBlitFramebuffer(0, 0, ctx.width, ctx.height, 0, 0, ctx.width, ctx.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                        Render::s_cachedFboForScissor = 0;
                        g_gpuState.invalidate_fbo();
                    }
                    g_gpuState.set_scissor<true>();
                    glScissor(static_cast<GLint>(ctx.frameDirty.scissorX),
                              static_cast<GLint>(ctx.frameDirty.scissorY),
                              static_cast<GLsizei>(ctx.frameDirty.scissorW),
                              static_cast<GLsizei>(ctx.frameDirty.scissorH));
                    g_gpuState.set_blend<false>();
                    glClearColor(ctx.bgR, ctx.bgG, ctx.bgB, ctx.bgA);
                    glClear(GL_COLOR_BUFFER_BIT);
                    return;
                }
            }
            Render::s_cachedFboForScissor = 0;
            g_gpuState.set_scissor<false>();
            g_gpuState.set_blend<false>();
            glClearColor(ctx.bgR, ctx.bgG, ctx.bgB, ctx.bgA);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    };

    // 2. DirectDrawSpanPolicy<Scissored, ContractFlags>: Full Redraw (false) hoặc Tile Preservation (true)
    template <bool Scissored, u32 ContractFlags>
    struct PassExecutor<DirectDrawSpanPolicy<Scissored, ContractFlags>>
    {
        static inline void execute_span(Span<timeline::runtime::RenderPass> span, const PassContext &ctx) noexcept
        {
            g_gpuState.bind_framebuffer(Render::s_sceneFbo, ctx.width, ctx.height);
            if constexpr (Scissored)
            {
                g_gpuState.set_scissor<true>();
                glScissor(static_cast<GLint>(ctx.frameDirty.scissorX),
                          static_cast<GLint>(ctx.frameDirty.scissorY),
                          static_cast<GLsizei>(ctx.frameDirty.scissorW),
                          static_cast<GLsizei>(ctx.frameDirty.scissorH));
            }
            else
            {
                g_gpuState.set_scissor<false>();
            }
            g_gpuState.set_blend<true>();
            g_gpuState.set_blend_func_separate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            begin_batch_pass(ctx.width, ctx.height);

            if (__builtin_expect(ctx.slotCmdBuckets == nullptr, 1))
            {
                for (const QuadCommand *cmd = ctx.cmds; cmd->opCode != static_cast<u8>(CMD_SENTINEL_END); ++cmd)
                {
                    if constexpr (Scissored)
                    {
                        if (!ctx.frameDirty.is_slot_dirty(cmd->slot))
                            continue;
                    }
                    push_render_command(*cmd, Render::s_glyphAtlasTex, Render::s_whiteTex);
                }
            }
            else
            {
                if constexpr (!Scissored)
                {
                    if (span.begin() == (ctx.plan.passes.data() + 1) &&
                        ctx.cmds[0].opCode != static_cast<u8>(CMD_SENTINEL_END) &&
                        ctx.cmds[0].slot == ROOT_SLOT_SENTINEL)
                    {
                        push_render_command(ctx.cmds[0], Render::s_glyphAtlasTex, Render::s_whiteTex);
                    }
                }
                for (const timeline::runtime::RenderPass *p = span.begin(), *const end = span.end(); p != end; ++p)
                {
                    const u32 slot = p->slot & (MAX_TIMELINE_SLOTS - 1u);
                    if constexpr (Scissored)
                    {
                        if (!ctx.frameDirty.is_slot_dirty(slot))
                            continue;
                    }
                    ctx.slotCmdBuckets->for_each_in_slot(slot, [&](u32 cIdx) {
                        push_render_command(ctx.cmds[cIdx], Render::s_glyphAtlasTex, Render::s_whiteTex);
                    });
                }
            }

            flush_batch_pass(Render::s_whiteTex);
        }
    };

    // 3. OffscreenDrawPolicy: DrawLayer vẽ vào FBO phụ
    template <>
    struct PassExecutor<OffscreenDrawPolicy>
    {
        static inline void execute(const timeline::runtime::RenderPass &pass, const PassContext &ctx) noexcept
        {
            PassRenderTarget *outTarget = bind_pass_target(pass.outputTarget, pass.width, pass.height, Render::s_sceneFbo);
            (void)outTarget;
            g_gpuState.set_scissor<false>();
            g_gpuState.set_blend<false>();
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            g_gpuState.set_blend<true>();
            g_gpuState.set_blend_func_separate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            begin_batch_pass(pass.width, pass.height);
            if (pass.slot < MAX_TIMELINE_SLOTS && ctx.slotCmdBuckets)
            {
                ctx.slotCmdBuckets->for_each_in_slot(pass.slot, [&](u32 cIdx) {
                    push_render_command(ctx.cmds[cIdx], Render::s_glyphAtlasTex, Render::s_whiteTex);
                });
            }
            flush_batch_pass(Render::s_whiteTex);
        }
    };

    // 4. CopyPassPolicy<PremultipliedBlend>: Hợp nhất Downsample (false), CopyScene (false) & CompositeBlur/Glow (true)
    template <bool PremultipliedBlend>
    struct PassExecutor<CopyPassPolicy<PremultipliedBlend>>
    {
        static inline void execute(const timeline::runtime::RenderPass &pass, const PassContext &) noexcept
        {
            GLuint inTex = 0;
            PassRenderTarget *outTarget = begin_fullscreen_pass(pass, inTex);
            if constexpr (PremultipliedBlend)
            {
                g_gpuState.set_blend<true>();
                g_gpuState.set_blend_func_separate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            }
            g_gpuState.use_program(Render::s_copyProgram);
            g_gpuState.bind_texture<0>(inTex);
            finish_fullscreen_pass(outTarget);
        }
    };

    // 5. BlurPassPolicy<IsVertical>: Hợp nhất Gaussian Horizontal (false) & Vertical (true)
    template <bool IsVertical>
    struct PassExecutor<BlurPassPolicy<IsVertical>>
    {
        static inline void execute(const timeline::runtime::RenderPass &pass, const PassContext &ctx) noexcept
        {
            GLuint inTex = 0;
            PassRenderTarget *outTarget = begin_fullscreen_pass(pass, inTex);
            g_gpuState.use_program(Render::s_blurProgram);
            g_gpuState.bind_texture<0>(inTex);
            const u32 kIdx = pass.kernelIndex < timeline::runtime::RUNTIME_TIMELINE_CAPACITY ? pass.kernelIndex : 0u;
            const timeline::runtime::BlurKernel &k = ctx.plan.kernels[kIdx];
            const i32 taps = static_cast<i32>(k.tapCount <= timeline::runtime::RUNTIME_BLUR_TAP_CAPACITY ? k.tapCount : timeline::runtime::RUNTIME_BLUR_TAP_CAPACITY);
            glUniform2f(s_locBlurTexel, 1.0f / static_cast<float>(pass.width), 1.0f / static_cast<float>(pass.height));
            if constexpr (IsVertical)
                glUniform2f(s_locBlurDir, 0.0f, 1.0f);
            else
                glUniform2f(s_locBlurDir, 1.0f, 0.0f);
            glUniform1i(s_locBlurTapCount, taps);
            glUniform1fv(s_locBlurWeights, taps + 1, k.weights.data());
            finish_fullscreen_pass(outTarget);
        }
    };

    // 6. ApplyBrightnessPassPolicy: Độ sáng
    template <>
    struct PassExecutor<ApplyBrightnessPassPolicy>
    {
        static inline void execute(const timeline::runtime::RenderPass &pass, const PassContext &) noexcept
        {
            GLuint inTex = 0;
            PassRenderTarget *outTarget = begin_fullscreen_pass(pass, inTex);
            g_gpuState.use_program(Render::s_brightnessProgram);
            g_gpuState.bind_texture<0>(inTex);
            glUniform1f(s_locBrightVal, pass.intensity);
            finish_fullscreen_pass(outTarget);
        }
    };

    // 7. TransformPassPolicy<IsShake>: Hợp nhất ApplyZoom (false) & ApplyShake (true)
    template <bool IsShake>
    struct PassExecutor<TransformPassPolicy<IsShake>>
    {
        static inline void execute(const timeline::runtime::RenderPass &pass, const PassContext &) noexcept
        {
            GLuint inTex = 0;
            PassRenderTarget *outTarget = begin_fullscreen_pass(pass, inTex);
            g_gpuState.use_program(Render::s_transformProgram);
            g_gpuState.bind_texture<0>(inTex);
            const float fw = static_cast<float>(pass.width);
            const float fh = static_cast<float>(pass.height);
            glUniform2f(s_locTransRes, fw, fh);
            if constexpr (IsShake)
            {
                glUniform2f(s_locTransShift, pass.parameter0 * fw, -pass.parameter1 * fw);
                glUniform1f(s_locTransTiled, 0.0f);
                glUniform1f(s_locTransMirror, 0.0f);
            }
            else
            {
                glUniform2f(s_locTransShift, 0.0f, 0.0f);
                glUniform1f(s_locTransTiled, 1.0f);
                glUniform1f(s_locTransMirror, pass.kernelIndex != 0 ? 1.0f : 0.0f);
            }
            glUniform1f(s_locTransScale, pass.radius);
            glUniform1f(s_locTransRot, pass.intensity * DEG_TO_RAD_F32);
            finish_fullscreen_pass(outTarget);
        }
    };

    // 8. BloomExtractPassPolicy: Tách dải sáng
    template <>
    struct PassExecutor<BloomExtractPassPolicy>
    {
        static inline void execute(const timeline::runtime::RenderPass &pass, const PassContext &) noexcept
        {
            GLuint inTex = 0;
            PassRenderTarget *outTarget = begin_fullscreen_pass(pass, inTex);
            g_gpuState.use_program(Render::s_bloomExtractProgram);
            g_gpuState.bind_texture<0>(inTex);
            u32 sw = pass.width, sh = pass.height;
            resolve_target_size(pass.inputTarget, pass.width, pass.height, sw, sh);
            glUniform2f(s_locBloomExtTexel, 1.0f / static_cast<float>(sw), 1.0f / static_cast<float>(sh));
            glUniform1f(s_locBloomExtThreshold, pass.intensity);
            glUniform1f(s_locBloomExtHasColor, pass.radius);
            glUniform1f(s_locBloomExtTolerance, pass.parameter0);
            glUniform3f(s_locBloomExtTargetColor, pass.parameter1, pass.parameter2, pass.parameter3);
            finish_fullscreen_pass(outTarget);
        }
    };

    // 9. BloomDownsamplePassPolicy: Bloom Downsample pyramid
    template <>
    struct PassExecutor<BloomDownsamplePassPolicy>
    {
        static inline void execute(const timeline::runtime::RenderPass &pass, const PassContext &) noexcept
        {
            GLuint inTex = 0;
            PassRenderTarget *outTarget = begin_fullscreen_pass(pass, inTex);
            g_gpuState.use_program(Render::s_bloomDownProgram);
            g_gpuState.bind_texture<0>(inTex);
            u32 sw = pass.width, sh = pass.height;
            resolve_target_size(pass.inputTarget, pass.width, pass.height, sw, sh);
            glUniform2f(s_locBloomDownTexel, 1.0f / static_cast<float>(sw), 1.0f / static_cast<float>(sh));
            finish_fullscreen_pass(outTarget);
        }
    };

    // 10. BloomUpsamplePassPolicy: Bloom Upsample pyramid
    template <>
    struct PassExecutor<BloomUpsamplePassPolicy>
    {
        static inline void execute(const timeline::runtime::RenderPass &pass, const PassContext &) noexcept
        {
            GLuint inTex = 0;
            PassRenderTarget *outTarget = begin_fullscreen_pass(pass, inTex);
            const u32 baseId = static_cast<u32>(pass.parameter3);
            const GLuint baseTex = resolve_source_tex(baseId, Render::s_sceneTex);
            u32 lw = pass.width, lh = pass.height;
            resolve_target_size(pass.inputTarget, pass.width, pass.height, lw, lh);
            g_gpuState.use_program(Render::s_bloomUpProgram);
            g_gpuState.bind_texture<0>(inTex);
            g_gpuState.bind_texture<1>(baseTex);
            glUniform2f(s_locBloomUpTexel, 1.0f / static_cast<float>(lw), 1.0f / static_cast<float>(lh));
            glUniform1f(s_locBloomUpWLow, pass.parameter0);
            glUniform1f(s_locBloomUpWHi, pass.parameter1);
            finish_fullscreen_pass(outTarget);
        }
    };

    // 11. BloomCompositePassPolicy: Hòa trộn Bloom pyramid lên khung hình
    template <>
    struct PassExecutor<BloomCompositePassPolicy>
    {
        static inline void execute(const timeline::runtime::RenderPass &pass, const PassContext &) noexcept
        {
            GLuint inTex = 0;
            PassRenderTarget *outTarget = begin_fullscreen_pass(pass, inTex);
            const u32 baseId = static_cast<u32>(pass.parameter1);
            const GLuint baseTex = resolve_source_tex(baseId, Render::s_sceneTex);
            u32 bw = pass.width, bh = pass.height;
            resolve_target_size(baseId, pass.width, pass.height, bw, bh);
            u32 gw = pass.width, gh = pass.height;
            resolve_target_size(pass.inputTarget, pass.width, pass.height, gw, gh);
            g_gpuState.use_program(Render::s_bloomCompositeProgram);
            g_gpuState.bind_texture<0>(inTex);
            g_gpuState.bind_texture<1>(baseTex);
            glUniform2f(s_locBloomCompTexel, 1.0f / static_cast<float>(bw), 1.0f / static_cast<float>(bh));
            glUniform2f(s_locBloomCompGlowTexel, 1.0f / static_cast<float>(gw), 1.0f / static_cast<float>(gh));
            glUniform1f(s_locBloomCompIntensity, pass.intensity);
            glUniform1f(s_locBloomCompFactor, pass.radius);
            glUniform1f(s_locBloomCompAa, pass.parameter0);
            glUniform1f(s_locBloomCompEnv, pass.parameter2 > 0.0f ? pass.parameter2 : 1.0f);
            finish_fullscreen_pass(outTarget);
        }
    };

    // 12. ShaderEffectPassPolicy: Anonymous 3-Tier Fusion Kernel Execution (Zobrist TT + UBO Tape)
    template <>
    struct PassExecutor<ShaderEffectPassPolicy>
    {
        static inline void execute(const timeline::runtime::RenderPass &pass, const PassContext &) noexcept
        {
            GLuint inTex = 0;
            PassRenderTarget *outTarget = begin_fullscreen_pass(pass, inTex);
            const RainDropParams* rainTable = timeline::runtime::active_rain_params_ptr();
            const RainDropParams* slotRain = (rainTable && pass.slot < MAX_TIMELINE_SLOTS) ? &rainTable[pass.slot] : nullptr;
            effect::fusion_set_current_rain_params(slotRain);

            effect::EffectInvocation inv{};
            inv.algebraWord = effect_algebra::algebra_word_for_kind_index(pass.kernelIndex);
            inv.effectHash  = static_cast<u32>(pass.kernelIndex) * 2654435761u;
            inv.slot        = pass.slot;
            inv.timeSec     = pass.radius;
            inv.env         = pass.intensity;
            inv.p0          = pass.parameter0;
            inv.p1          = pass.parameter1;
            inv.p2          = pass.parameter2;
            inv.p3          = pass.parameter3;

            auto& planRcu = effect::global_fused_plan_rcu();
            planRcu.staging().compile_chain(&inv, 1u, pass.width, pass.height, effect_algebra::TileSummary64::full_active());
            planRcu.publish();
            const auto& plan = planRcu.read();
            if (plan.epochCount > 0u)
            {
                const auto& ep = plan.epochs[0];
                const bool useScissor = (ep.isBounded != 0u && ep.isGeomBarrier == 0u && ep.active_tile_count() <= 32u);
                if (useScissor)
                {
                    // Copy untouched background tiles via hardware ROP blit, then scissor shader to active tiles
                    const GLuint inFbo = (pass.inputTarget == 0u)
                        ? Render::s_sceneFbo
                        : ((pass.inputTarget < timeline::runtime::RUNTIME_RENDER_TARGET_COUNT)
                            ? s_passTargets[pass.inputTarget].fbo : Render::s_sceneFbo);
                    const GLuint dstFbo = outTarget ? outTarget->fbo : Render::s_sceneFbo;
                    if (inFbo != 0u && dstFbo != 0u && inFbo != dstFbo)
                    {
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, inFbo);
                        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
                        glBlitFramebuffer(0, 0, pass.width, pass.height, 0, 0, pass.width, pass.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                        glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
                        g_gpuState.invalidate_fbo();
                    }
                    const Rect<float> sc = effect_algebra::tile_mask_to_pixel_rect(
                        ep.tileMask64, static_cast<float>(pass.width), static_cast<float>(pass.height));
                    const GLint sx = static_cast<GLint>(sc.x0);
                    const GLint sy = static_cast<GLint>(sc.y0);
                    const GLsizei sw = static_cast<GLsizei>(max(1.0f, sc.x1 - sc.x0));
                    const GLsizei sh = static_cast<GLsizei>(max(1.0f, sc.y1 - sc.y0));
                    g_gpuState.set_scissor<true>();
                    glScissor(sx, sy, sw, sh);
                }
                effect::fusion_execute_epoch(plan, ep, inTex);
                finish_fullscreen_pass(outTarget);
                if (useScissor)
                {
                    g_gpuState.set_scissor<false>();
                }
            }
            else
            {
                finish_fullscreen_pass(outTarget);
            }
            g_gpuState.bind_texture<3>(0);
            g_gpuState.bind_texture<2>(0);
            g_gpuState.bind_texture<1>(0);
            g_gpuState.bind_texture<0>(0);
        }
    };

    void Render::set_monoid_batch_ops(const MathOp* ops) noexcept
    {
        s_monoidBatchOpsPtr = ops;
    }

    // 13. MonoidFusedPassPolicy: Dung hợp đa lớp Monoid trong 1 pass duy nhất
    template <>
    struct PassExecutor<MonoidFusedPassPolicy>
    {
        static inline void execute(const timeline::runtime::RenderPass &pass, const PassContext &) noexcept
        {
            GLuint inTex = 0;
            PassRenderTarget *outTarget = begin_fullscreen_pass(pass, inTex);
            g_gpuState.use_program(Render::s_monoidProgram);
            g_gpuState.bind_texture<0>(inTex);
            glUniform2f(s_locMonoidRes, static_cast<float>(pass.width), static_cast<float>(pass.height));
            const i32 count = static_cast<i32>(pass.kernelIndex);
            if (count == 0)
            {
                glUniform1i(s_locMonoidUseUber, 1);
                // OpenGL mat2 is column-major: col0 = (m00, m10), col1 = (m01, m11)
                const float mat2[4] = { pass.radius, pass.parameter0, pass.intensity, pass.parameter1 };
                glUniformMatrix2fv(s_locMonoidUberMat, 1, GL_FALSE, mat2);
                glUniform2f(s_locMonoidUberTrans, pass.parameter2, pass.parameter3);
                glUniform4f(s_locMonoidUberGain, 1.0f, 1.0f, 1.0f, 1.0f);
                glUniform4f(s_locMonoidUberEmit, 0.0f, 0.0f, 0.0f, 0.0f);
            }
            else
            {
                glUniform1i(s_locMonoidUseUber, 0);
                glUniform1i(s_locMonoidCount, count);
                if (count > 0 && s_monoidBatchOpsPtr)
            {
                const i32 uploadCount = (count < static_cast<i32>(MAX_MONOID_UNIFORM_OPS)) ? count : static_cast<i32>(MAX_MONOID_UNIFORM_OPS);
                alignas(16) static MultiArray<float, MONOID_UNIFORM_FLOATS_CAP> bboxes{};
                alignas(16) static MultiArray<float, MONOID_UNIFORM_FLOATS_CAP> premuls{};
                alignas(16) static MultiArray<float, MONOID_UNIFORM_FLOATS_CAP> transs{};
                float* __restrict bPtr = bboxes.data();
                float* __restrict pPtr = premuls.data();
                float* __restrict tPtr = transs.data();
                for (const MathOp* __restrict op = s_monoidBatchOpsPtr, * const __restrict opEnd = op + uploadCount;
                     op < opEnd; ++op, bPtr += 4, pPtr += 4, tPtr += 4)
                {
#if defined(__EMSCRIPTEN__)
                    wasm_v128_store(bPtr, op->vAABB);
                    wasm_v128_store(pPtr, op->vPremul);
                    wasm_v128_store(tPtr, op->vTrans);
#else
                    bPtr[0] = op->minX; bPtr[1] = op->minY; bPtr[2] = op->maxX; bPtr[3] = op->maxY;
                    pPtr[0] = op->premulR; pPtr[1] = op->premulG; pPtr[2] = op->premulB; pPtr[3] = op->alpha;
                    tPtr[0] = op->transR; tPtr[1] = op->transG; tPtr[2] = op->transB; tPtr[3] = op->oneMinusAlpha;
#endif
                }
                glUniform4fv(s_locMonoidBBox, uploadCount, bboxes.data());
                glUniform4fv(s_locMonoidPremul, uploadCount, premuls.data());
                glUniform4fv(s_locMonoidTrans, uploadCount, transs.data());
            }
            }
            finish_fullscreen_pass(outTarget);
        }
    };

    // Điều phối đơn lẻ bằng MultiArray LUT (0 switch/if rẽ nhánh trên enum RuntimePassKind)
    using SinglePassExecFn = void (*)(const timeline::runtime::RenderPass &, const PassContext &) noexcept;
    constexpr u32 SINGLE_PASS_KIND_COUNT = 18u;
    inline constexpr MultiArray<SinglePassExecFn, SINGLE_PASS_KIND_COUNT> kSinglePassExecLut = {{
        &PassExecutor<ClearPassPolicy<>>::execute,
        &PassExecutor<OffscreenDrawPolicy>::execute,
        &PassExecutor<CopyPassPolicy<false>>::execute,
        &PassExecutor<BlurPassPolicy<false>>::execute,
        &PassExecutor<BlurPassPolicy<true>>::execute,
        &PassExecutor<CopyPassPolicy<true>>::execute,
        &PassExecutor<CopyPassPolicy<true>>::execute,
        &PassExecutor<CopyPassPolicy<true>>::execute,
        &PassExecutor<ApplyBrightnessPassPolicy>::execute,
        &PassExecutor<CopyPassPolicy<false>>::execute,
        &PassExecutor<TransformPassPolicy<false>>::execute,
        &PassExecutor<TransformPassPolicy<true>>::execute,
        &PassExecutor<BloomExtractPassPolicy>::execute,
        &PassExecutor<BloomDownsamplePassPolicy>::execute,
        &PassExecutor<BloomUpsamplePassPolicy>::execute,
        &PassExecutor<BloomCompositePassPolicy>::execute,
        &PassExecutor<ShaderEffectPassPolicy>::execute,
        &PassExecutor<MonoidFusedPassPolicy>::execute,
    }};

    static inline void dispatch_single_pass(u32 safeKind, const timeline::runtime::RenderPass &pass, const PassContext &ctx) noexcept
    {
        kSinglePassExecLut[safeKind](pass, ctx);
    }
#endif // defined(STOCKCUT_HAS_GLES3)

    // ------------------------------------------------------------------------
    // HỢP ĐỒNG THỰC THI PHẦN CỨNG THỐNG NHẤT (UNIFIED TEMPLATE CONTRACT POLICY)
    // Phân nhánh 100% tại Compile-Time qua if constexpr trên FrameContractFlag
    // ------------------------------------------------------------------------
    template <u32 ContractFlags>
    u32 Render::execute_policy(
        u32 width, u32 height, u32 commandCount,
        const QuadCommand *cmds,
        const timeline::runtime::Frame<> &plan,
        const SpatialFrameDirty<> &frameDirty,
        float bgR, float bgG, float bgB, float bgA,
        SharedRenderState *sharedRender,
        TelemetryBuffer *telem,
        Key kParam) noexcept
    {
#if defined(STOCKCUT_HAS_GLES3)
        constexpr bool kSceneValid   = (ContractFlags & FRAME_FLAG_SCENE_INVALID) == 0u;
        constexpr bool kTimeTainted  = (ContractFlags & (FRAME_FLAG_TIME_TAINTED | FRAME_FLAG_BASS_ACTIVE)) != 0u;
        constexpr bool kForceFull    = ((ContractFlags & (FRAME_FLAG_SEEKED | FRAME_FLAG_HAS_POST_FX)) != 0u) || !kSceneValid;
        constexpr bool kSpatialDirty = (ContractFlags & FRAME_FLAG_SPATIAL_DIRTY) != 0u;
        constexpr bool kIsReuse      = !kForceFull && !kSpatialDirty;
        constexpr bool kIsScissored  = !kForceFull && kSpatialDirty;
        constexpr bool kCanFullTT    = kSceneValid && !kTimeTainted && !kIsReuse;
        constexpr bool kStoreFullTT  = kForceFull && kSceneValid && !kTimeTainted;
        constexpr bool kAllowPrefix  = kForceFull && kSceneValid && kTimeTainted;

        if constexpr (kCanFullTT)
        {
            if (kParam != 0ULL)
            {
                cache::Entry<cache::GpuSurface> *hit = s_gpuSurfaceTT.probe_if(kParam, [&](const cache::GpuSurface &s) {
                    return s.matches(width, height);
                });
                if (hit != nullptr)
                {
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, hit->payload.fbo);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_sceneFbo);
                    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    g_gpuState.invalidate_fbo();
                    s_cachedFboForScissor = hit->payload.fbo;
                    if (telem)
                    {
                        telem->ttHits++;
                        telem->renderMode = TELEMETRY_MODE_TT_HIT;
                    }
                    return commandCount;
                }
            }
        }

        if constexpr (kIsReuse)
        {
            if (sharedRender)
            {
                sharedRender->record_transfer<StateTransferPolicy::ZeroTransfer>(0u);
            }
            if (telem)
            {
                telem->renderMode = TELEMETRY_MODE_NONE;
                telem->noneCount++;
                if (kParam != 0ULL) telem->ttMisses++;
            }
            return commandCount;
        }
        else if constexpr (kIsScissored)
        {
            if (sharedRender)
            {
                const u32 activeCmds = (commandCount < MAX_DISPLAY_COMMANDS) ? commandCount : MAX_DISPLAY_COMMANDS;
                sharedRender->record_transfer<StateTransferPolicy::PartialTransfer>(
                    activeCmds * static_cast<u32>(sizeof(QuadCommand)));
            }
            if (telem)
            {
                telem->renderMode = TELEMETRY_MODE_DIRTY;
                telem->dirtyCount++;
                telem->dirtyN = frameDirty.dirty_tiles_bb().popcount();
                telem->dirtyAreaPx = static_cast<u32>(frameDirty.scissorW * frameDirty.scissorH);
                if (kParam != 0ULL) telem->ttMisses++;
            }

            g_gpuState.bind_framebuffer(s_sceneFbo, width, height);
            if (s_cachedFboForScissor != 0)
            {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, s_cachedFboForScissor);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_sceneFbo);
                glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                s_cachedFboForScissor = 0;
                g_gpuState.invalidate_fbo();
            }

            g_gpuState.set_scissor<true>();
            glScissor(static_cast<GLint>(frameDirty.scissorX),
                      static_cast<GLint>(frameDirty.scissorY),
                      static_cast<GLsizei>(frameDirty.scissorW),
                      static_cast<GLsizei>(frameDirty.scissorH));
            g_gpuState.set_blend<false>();
            glClearColor(bgR, bgG, bgB, bgA);
            glClear(GL_COLOR_BUFFER_BIT);

            const PassContext execCtx{
                width, height, commandCount,
                plan, frameDirty,
                bgR, bgG, bgB, bgA,
                nullptr, cmds
            };
            PassExecutor<DirectDrawSpanPolicy<true, ContractFlags>>::execute_span(
                Span(plan.passes.data(), plan.passCount), execCtx
            );
            g_gpuState.set_scissor<false>();
            return commandCount;
        }
        else
        {
            if (sharedRender)
            {
                const u32 activeCmds = (commandCount < MAX_DISPLAY_COMMANDS) ? commandCount : MAX_DISPLAY_COMMANDS;
                sharedRender->record_transfer<StateTransferPolicy::FullTransfer>(
                    activeCmds * static_cast<u32>(sizeof(QuadCommand)));
            }
            if (telem)
            {
                telem->renderMode = TELEMETRY_MODE_FULL;
                telem->fullCount++;
            }

            const bool hasOffscreenPasses = (plan.activeEffectCount > 0 || plan.hasPostProcessingEffect());
            const u32 staticPrefixCount = kAllowPrefix ? s_zobristPyramid.static_draw_prefix_count() : 0u;
            const bool needSlotBuckets = hasOffscreenPasses || (staticPrefixCount > 0u);

            alignas(64) static IndexBucketList<MAX_TIMELINE_SLOTS, MAX_DISPLAY_COMMANDS> slotCmdBuckets{};
            if (needSlotBuckets)
            {
                slotCmdBuckets.reset();
                const u32 safeCmdCount = (commandCount < MAX_DISPLAY_COMMANDS) ? commandCount : MAX_DISPLAY_COMMANDS;
                for (i32 cIdx = static_cast<i32>(safeCmdCount) - 1; cIdx >= 0; --cIdx)
                {
                    slotCmdBuckets.add<true>(cmds[cIdx].slot, static_cast<u32>(cIdx));
                }
            }

            const PassContext execCtx{
                width, height, commandCount,
                plan, frameDirty,
                bgR, bgG, bgB, bgA,
                needSlotBuckets ? &slotCmdBuckets : nullptr,
                cmds
            };

            const timeline::runtime::RenderPass *p = plan.passes.data();
            bool prefixHit = false;

            if constexpr (kAllowPrefix)
            {
                const Key staticPrefixKey = s_zobristPyramid.static_draw_prefix_key();
                if (staticPrefixCount > 0u && staticPrefixKey != 0ULL)
                {
                    cache::Entry<cache::GpuSurface> *hit = s_gpuSurfaceTT.probe_if(
                        staticPrefixKey,
                        [&](const cache::GpuSurface &s) { return s.matches(width, height); });
                    if (hit != nullptr)
                    {
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, hit->payload.fbo);
                        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_sceneFbo);
                        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                        glBindFramebuffer(GL_FRAMEBUFFER, s_sceneFbo);
                        g_gpuState.invalidate_fbo();
                        prefixHit = true;
                    }
                    else
                    {
                        PassExecutor<ClearPassPolicy<ContractFlags>>::execute(*p, execCtx);
                        PassExecutor<DirectDrawSpanPolicy<false, ContractFlags>>::execute_span(
                            Span(p + 1, staticPrefixCount), execCtx
                        );
                        cache::Entry<cache::GpuSurface> *slot = s_gpuSurfaceTT.allocate(staticPrefixKey);
                        if (slot != nullptr)
                        {
                            slot->payload.width = width;
                            slot->payload.height = height;
                            ensure_fbo_target(slot->payload.fbo, slot->payload.tex,
                                              slot->payload.width, slot->payload.height,
                                              width, height, GL_LINEAR);
                            glBindFramebuffer(GL_READ_FRAMEBUFFER, s_sceneFbo);
                            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, slot->payload.fbo);
                            glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                            glBindFramebuffer(GL_FRAMEBUFFER, s_sceneFbo);
                            g_gpuState.invalidate_fbo();
                            slot->flags = cache::TT_FLAG_VALID;
                            s_gpuSurfaceTT.advance_generation();
                        }
                    }
                    p += 1u + staticPrefixCount;
                }
            }

            while (p->kind != timeline::runtime::RuntimePassKind::Sentinel)
            {
                if (p->kind == timeline::runtime::RuntimePassKind::DrawLayer && p->outputTarget == 0)
                {
                    u32 spanCount = 1;
                    if (!hasOffscreenPasses)
                    {
                        while ((p + spanCount)->kind != timeline::runtime::RuntimePassKind::Sentinel)
                        {
                            ++spanCount;
                        }
                    }
                    else
                    {
                        while ((p + spanCount)->kind == timeline::runtime::RuntimePassKind::DrawLayer &&
                               (p + spanCount)->outputTarget == 0)
                        {
                            ++spanCount;
                        }
                    }
                    PassExecutor<DirectDrawSpanPolicy<false, ContractFlags>>::execute_span(
                        Span(p, spanCount), execCtx
                    );
                    p += spanCount;
                    continue;
                }

                // Anonymous Multi-Effect Epoch Coalescing: fuse consecutive ShaderEffect / Zoom / Shake / Brightness passes into minimal FBO draws
                auto is_fusible_post = [](timeline::runtime::RuntimePassKind k) noexcept -> bool {
                    return k == timeline::runtime::RuntimePassKind::ApplyShaderEffect ||
                           k == timeline::runtime::RuntimePassKind::ApplyZoom ||
                           k == timeline::runtime::RuntimePassKind::ApplyShake ||
                           k == timeline::runtime::RuntimePassKind::ApplyBrightness;
                };
                if (is_fusible_post(p->kind) && is_fusible_post((p + 1)->kind) &&
                    (p + 1)->inputTarget == p->outputTarget &&
                    (p + 1)->width == p->width && (p + 1)->height == p->height)
                {
                    alignas(64) static MultiArray<effect::EffectInvocation, effect::FUSION_MAX_INPUT_EFFECTS> chainBuf;
                    u32 chainLen = 0u;
                    const timeline::runtime::RenderPass *q = p;
                    const u8 firstInTarget = p->inputTarget;
                    u8 lastOutTarget = p->outputTarget;
                    while (q->kind != timeline::runtime::RuntimePassKind::Sentinel &&
                           is_fusible_post(q->kind) &&
                           chainLen < effect::FUSION_MAX_INPUT_EFFECTS &&
                           (chainLen == 0u || (q->inputTarget == lastOutTarget && q->width == p->width && q->height == p->height)))
                    {
                        effect::EffectInvocation& inv = chainBuf[chainLen++];
                        inv = effect::EffectInvocation{};
                        inv.slot = q->slot;
                        if (q->kind == timeline::runtime::RuntimePassKind::ApplyShaderEffect)
                        {
                            inv.algebraWord = effect_algebra::algebra_word_for_kind_index(q->kernelIndex);
                            inv.effectHash  = static_cast<u32>(q->kernelIndex) * 2654435761u;
                            inv.timeSec     = q->radius;
                            inv.env         = q->intensity;
                            inv.p0          = q->parameter0;
                            inv.p1          = q->parameter1;
                            inv.p2          = q->parameter2;
                            inv.p3          = q->parameter3;
                        }
                        else if (q->kind == timeline::runtime::RuntimePassKind::ApplyZoom)
                        {
                            inv.algebraWord = effect_algebra::kKernelAlgebraWords[effect_algebra::KERNEL_ORD_AFFINE_2D];
                            inv.effectHash  = effect_catalog::fnv1a_lower("zoom");
                            inv.p0          = q->radius;
                            inv.p1          = q->intensity;
                            inv.p2          = (q->kernelIndex != 0u) ? 1.0f : 0.0f;
                            inv.flags       = static_cast<u16>(1u | ((q->kernelIndex != 0u) ? 2u : 0u));
                        }
                        else if (q->kind == timeline::runtime::RuntimePassKind::ApplyShake)
                        {
                            inv.algebraWord = effect_algebra::kKernelAlgebraWords[effect_algebra::KERNEL_ORD_AFFINE_2D];
                            inv.effectHash  = effect_catalog::fnv1a_lower("shake");
                            inv.p0          = q->radius;
                            inv.p1          = q->intensity;
                            inv.extra0      = q->parameter0;
                            inv.extra1      = q->parameter1;
                            inv.flags       = 0u;
                        }
                        else // ApplyBrightness
                        {
                            inv.algebraWord = effect_algebra::kKernelAlgebraWords[effect_algebra::KERNEL_ORD_AFFINE_COL];
                            inv.effectHash  = effect_catalog::fnv1a_lower("brightness");
                            inv.p0          = q->intensity;
                        }
                        lastOutTarget = q->outputTarget;
                        ++q;
                    }

                    auto& planRcu = effect::global_fused_plan_rcu();
                    planRcu.staging().compile_chain(chainBuf.data(), chainLen, p->width, p->height, effect_algebra::TileSummary64::full_active());
                    planRcu.publish();
                    const auto& fplan = planRcu.read();
                    const RainDropParams* rainTable = timeline::runtime::active_rain_params_ptr();
                    u8 curTarget = firstInTarget;
                    for (u32 ei = 0u; ei < fplan.epochCount; ++ei)
                    {
                        const auto& ep = fplan.epochs[ei];
                        const bool isLast = (ei + 1u == fplan.epochCount);
                        u8 dstTarget = isLast ? lastOutTarget : static_cast<u8>(1u - curTarget);
                        if (dstTarget == curTarget) dstTarget = static_cast<u8>(1u - curTarget);

                        timeline::runtime::RenderPass synthPass{};
                        synthPass.inputTarget  = curTarget;
                        synthPass.outputTarget = dstTarget;
                        synthPass.width        = p->width;
                        synthPass.height       = p->height;
                        synthPass.slot         = ep.geomSourceSlot;

                        GLuint inTex = 0u;
                        PassRenderTarget *outTarget = begin_fullscreen_pass(synthPass, inTex);
                        if (ep.isGeomBarrier != 0u && rainTable && ep.geomSourceSlot < MAX_TIMELINE_SLOTS)
                        {
                            effect::fusion_set_current_rain_params(&rainTable[ep.geomSourceSlot]);
                        }
                        effect::fusion_execute_epoch(fplan, ep, inTex);
                        finish_fullscreen_pass(outTarget);
                        g_gpuState.bind_texture<3>(0);
                        g_gpuState.bind_texture<2>(0);
                        g_gpuState.bind_texture<1>(0);
                        g_gpuState.bind_texture<0>(0);
                        curTarget = dstTarget;
                    }
                    // If epoch count was 0 (all collapsed to identity) and targets differed, copy once
                    if (curTarget != lastOutTarget)
                    {
                        timeline::runtime::RenderPass copyPass{};
                        copyPass.kind = timeline::runtime::RuntimePassKind::CopyScene;
                        copyPass.inputTarget = curTarget;
                        copyPass.outputTarget = lastOutTarget;
                        copyPass.width = p->width;
                        copyPass.height = p->height;
                        PassExecutor<CopyPassPolicy<false>>::execute(copyPass, execCtx);
                    }
                    p = q;
                    continue;
                }

                const u8 kind = static_cast<u8>(p->kind);
                const u32 safeKind = (kind < SINGLE_PASS_KIND_COUNT) ? kind : 0u;
                dispatch_single_pass(safeKind, *p, execCtx);
                ++p;
            }

            if constexpr (kStoreFullTT)
            {
                if (kParam != 0ULL && s_sceneTex != 0 && !plan.hasLinearTransition())
                {
                    cache::Entry<cache::GpuSurface> *slot = s_gpuSurfaceTT.allocate(kParam);
                    if (slot != nullptr)
                    {
                        slot->payload.width = width;
                        slot->payload.height = height;
                        ensure_fbo_target(slot->payload.fbo, slot->payload.tex,
                                          slot->payload.width, slot->payload.height,
                                          width, height, GL_LINEAR);
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_sceneFbo);
                        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, slot->payload.fbo);
                        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                        glBindFramebuffer(GL_FRAMEBUFFER, 0);
                        g_gpuState.invalidate_fbo();
                        slot->flags = cache::TT_FLAG_VALID;
                        s_gpuSurfaceTT.advance_generation();
                    }
                }
            }

            if (telem && kParam != 0ULL)
            {
                if constexpr (kAllowPrefix)
                {
                    if (prefixHit) telem->ttHits++;
                    else telem->ttMisses++;
                }
                else
                {
                    telem->ttMisses++;
                }
            }
            return commandCount;
        }
#else
        (void)width; (void)height; (void)commandCount; (void)cmds;
        (void)plan; (void)frameDirty; (void)bgR; (void)bgG; (void)bgB; (void)bgA;
        (void)sharedRender; (void)telem; (void)kParam;
        return commandCount;
#endif
    }

    namespace
    {
        using FramePolicyExecFn = u32 (*)(
            u32, u32, u32,
            const QuadCommand *,
            const timeline::runtime::Frame<> &,
            const SpatialFrameDirty<> &,
            float, float, float, float,
            SharedRenderState *,
            TelemetryBuffer *,
            Key) noexcept;

        template <usize... Is>
        constexpr auto make_frame_contract_lut(std::index_sequence<Is...>) noexcept
        {
            return MultiArray<FramePolicyExecFn, sizeof...(Is)>{{
                &Render::execute_policy<canonicalize_contract_flags(static_cast<u32>(Is))>...
            }};
        }

        alignas(64) constexpr auto kFrameContractLut =
            make_frame_contract_lut(std::make_index_sequence<FRAME_FLAG_COMB_COUNT>{});
    } // namespace

    void Render::blit_scene_to_screen(u32 width, u32 height) noexcept
    {
#if defined(STOCKCUT_HAS_GLES3)
        g_gpuState.bind_framebuffer(0, width, height);
        g_gpuState.set_scissor<false>();
        g_gpuState.set_blend<false>();
        g_gpuState.use_program(s_copyProgram);
        g_gpuState.bind_vao(s_quadVao);
        g_gpuState.bind_texture<0>(s_sceneTex);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
#else
        (void)width; (void)height;
#endif
    }

    template <typename Mask>
    u32 Render::execute_frame(
        u32 width, u32 height,
        u32 compositeMode,
        u32 useNativeRenderPlan,
        u32 commandCount,
        u32 staticPrefixCount,
        u32 staticKey,
        u32 dirtyRectCount,
        float bgR, float bgG, float bgB, float bgA,
        u32 blitToScreen) noexcept
    {
#if defined(STOCKCUT_HAS_GLES3)
        if (!s_initialized || width == 0 || height == 0)
            return 0;

        const double tRenderStart = g_gpuState.now_ms();
        sync_glyph_atlas();
        ensure_fbo_target(s_sceneFbo, s_sceneTex, s_sceneW, s_sceneH, width, height, GL_LINEAR);
        const QuadCommand *cmds = engineManager().gpuQuadStream().data();
        SharedRenderState *sharedRender = engine_get_shared_render_state();
        const timeline::runtime::Frame<> &plan = engine_get_runtime_frame();
        const SpatialFrameDirty<> &frameDirty = engine_get_frame_spatial_dirty();

        render::g_frameUboRing.advance_ring();
        render::g_passUboRing.advance_ring();
        render::g_monoidUboRing.advance_ring();

        render::FrameUniformBlock frameData{};
        frameData.viewportRes[0] = static_cast<float>(width);
        frameData.viewportRes[1] = static_cast<float>(height);
        frameData.viewportRes[2] = (width > 0) ? (1.0f / static_cast<float>(width)) : 0.0f;
        frameData.viewportRes[3] = (height > 0) ? (1.0f / static_cast<float>(height)) : 0.0f;
        frameData.timeState[0] = static_cast<float>(tRenderStart);
        frameData.timeState[1] = 16.6667f;
        frameData.timeState[2] = static_cast<float>(staticKey);
        frameData.timeState[3] = (compositeMode == static_cast<u32>(CompositeFrameMode::ReuseScene)) ? 0.0f : 1.0f;
        frameData.bgColor[0] = bgR;
        frameData.bgColor[1] = bgG;
        frameData.bgColor[2] = bgB;
        frameData.bgColor[3] = bgA;

        render::g_frameUboRing.upload_current(frameData);
        render::g_frameUboRing.bind_current();

        const u32 contractFlags =
            (compositeMode == static_cast<u32>(CompositeFrameMode::ReuseScene))
                ? FRAME_FLAG_NONE
                : ((compositeMode == static_cast<u32>(CompositeFrameMode::DirtyRedraw) && frameDirty.hasScissor)
                       ? (FRAME_FLAG_SPATIAL_DIRTY | FRAME_FLAG_TIME_TAINTED)
                       : FRAME_FLAG_SCENE_INVALID);
        const u32 drawnCount = kFrameContractLut[contractFlags](
            width, height, commandCount, cmds,
            plan, frameDirty, bgR, bgG, bgB, bgA,
            sharedRender, nullptr, 0ULL);

        if (blitToScreen != 0)
        {
            blit_scene_to_screen(width, height);
        }

        constexpr double MIN_RENDER_TIME_MS = 0.0001;
        const double tRenderEnd = g_gpuState.now_ms();
        double renderMs = tRenderEnd - tRenderStart;
        if (renderMs <= 0.0) renderMs = MIN_RENDER_TIME_MS;
        telemetry_record_frame_time(renderMs);
        if (auto *telem = telemetry_get_ptr())
        {
            telem->cpuTimeMs = renderMs;
        }
        return drawnCount;
#else
        (void)width; (void)height; (void)compositeMode; (void)useNativeRenderPlan;
        (void)commandCount; (void)staticPrefixCount; (void)staticKey; (void)dirtyRectCount;
        (void)bgR; (void)bgG; (void)bgB; (void)bgA; (void)blitToScreen;
        return commandCount;
#endif
    }
    namespace
    {
        constexpr u32    CMD_DISPATCH_OP_COUNT = 12u;
        constexpr u32    MASK_FILTER_KIND_BITS = 0xFu;
        constexpr u32    MASK_FILTER_RADIUS_SHIFT = 4u;
        constexpr u32    MASK_FILTER_RADIUS_MASK  = 0xFFFu;
        constexpr float  BASS_QUANT_MAX_F32    = 15.0f;
        constexpr double MIN_SKIP_L0_TIME_MS   = 0.00002;
        constexpr double MIN_SKIP_L2_TIME_MS   = 0.00005;
        constexpr double MIN_HIT_TIME_MS       = 0.0001;

        inline void write_telemetry_frame_header(
            TelemetryBuffer *telem,
            double nowMs,
            u32 frameTick,
            Key currentHash,
            u32 activeLayersCount,
            u32 activeFxCount,
            u32 width,
            u32 height) noexcept
        {
            telem->nowMs = nowMs;
            telem->frameIndex = frameTick;
            telem->currentHash = currentHash;
            telem->activeLayersCount = activeLayersCount;
            telem->activeFxCount = activeFxCount;
            telem->baseW = width;
            telem->baseH = height;
        }
    } // namespace
    template <typename Mask>
    u32 Render::render_timeline_frame(
        double nowMs,
        u32 width, u32 height,
        u32 frameTick,
        u32 sceneValid,
        float bgR, float bgG, float bgB, float bgA,
        u32 blitToScreen) noexcept
    {
#if defined(STOCKCUT_HAS_GLES3)
        if (!s_initialized || width == 0 || height == 0)
            return 0;
        const double tFrameStart = g_gpuState.now_ms();
        Position<Mask> *pos = reinterpret_cast<Position<Mask>*>(timeline_position_get_instance());
        if (!pos)
            return 0;
        EngineSharedState *ss = engine_get_shared_state();
        const bool isSeeked = (ss && (ss->flags & (SHARED_FLAG_SEEKED | SHARED_FLAG_LOOPED))) ||
                              (s_lastNowMs >= 0.0 && (nowMs < s_lastNowMs || abs_val(nowMs - s_lastNowMs) > SEEK_DELTA_THRESHOLD_MS));
        TelemetryBuffer *telem = telemetry_get_ptr();
        QuadStream &quadStream = engineManager().gpuQuadStream();
        // Reset sạch số liệu đo đạc khi tua về đầu hoặc lặp timeline (GEMINI.md Rule 5)
        if (s_lastNowMs >= 0.0 && nowMs < s_lastNowMs)
        {
            if (telem)
            {
                telem->ttHits = 0;
                telem->ttMisses = 0;
                telem->zobristBreakers = 0;
                telem->noneCount = 0;
                telem->fullCount = 0;
                telem->dirtyCount = 0;
                telem->frameIndex = 0;
            }
            s_gpuSurfaceTT.invalidate_entries();
            s_zobristPyramid.clear();
            s_tileMMU.clear();
            s_lastParamKey = 0;
            s_lastW = 0;
            s_lastH = 0;
            s_lastCmdCount = 0;
            s_prevFrameHadPostFx = false;
            s_cachedFboForScissor = 0;
            engine_get_frame_spatial_dirty().clear_all();
            if (pos) pos->invalidate_history();
            quadStream.reset();
            timeline_position_display_list_invalidate();
        }
        if (isSeeked || sceneValid == 0)
        {
            s_gpuSurfaceTT.invalidate_entries();
            s_zobristPyramid.clear();
            s_lastParamKey = 0;
            s_lastCmdCount = 0;
            s_prevFrameHadPostFx = false;
            s_cachedFboForScissor = 0;
            engine_get_frame_spatial_dirty().clear_all();
            if (pos) pos->invalidate_history();
            quadStream.reset();
            timeline_position_display_list_invalidate();
            if (ss)
                ss->flags &= ~(SHARED_FLAG_SEEKED | SHARED_FLAG_LOOPED);
        }
        // 1. Advance timeline position (stepTo) - O(1) branchless query (5 - 15 nanoseconds)
        const TimePoint nextTimeMs = static_cast<TimePoint>(nowMs);
        StateInfo<Mask> *nextSt = reinterpret_cast<StateInfo<Mask>*>(timeline_position_advance(nextTimeMs));
        // ========================================================================
        // TẦNG 0: ZERO-COST ZOBRIST CIRCUIT BREAKER SKIP (STOCKFISH INVARIANT)
        // ========================================================================
        if (nowMs < s_lastNowMs)
        {
            engine_get_frame_spatial_dirty().clear_all();
        }
        const bool isStaticTopology = (sceneValid != 0) && (!isSeeked) &&
                                      (width == s_lastW) && (height == s_lastH) &&
                                      (s_lastParamKey != 0ULL) && (s_sceneTex != 0) &&
                                      (nowMs >= s_lastNowMs);
        const Mask activeIntrinsicVis = nextSt->activeBB & pos->template by_dynamic_axis_bb<DynamicAxis::IntrinsicVisual>();
        const u64 activeDynamicFxWord = nextSt->activeBB.word(0) & projectRuntime().dynamic_fx_slots_bb().summary();
        const bool hasActiveVisualMotion = (nextSt->dirtyBB != 0ULL) || activeIntrinsicVis.any() || (activeDynamicFxWord != 0ULL);
        const bool animatedPlayback = ss && ss->playbackState == PLAYBACK_PLAYING && hasActiveVisualMotion;
        if (isStaticTopology && !hasActiveVisualMotion)
        {
            const double tSkipEnd = g_gpuState.now_ms();
            double skipMs = tSkipEnd - tFrameStart;
            if (skipMs <= 0.0)
                skipMs = MIN_SKIP_L0_TIME_MS;
            telemetry_record_frame_time(skipMs);
            if (telem)
            {
                telem->cpuTimeMs = skipMs;
                write_telemetry_frame_header(
                    telem, nowMs, frameTick, s_lastParamKey,
                    static_cast<u32>(nextSt->activeBB.popcount()), 0u, width, height);
                telem->zobristBreakers++;
                telem->renderMode = TELEMETRY_MODE_NONE;
                telem->noneCount++;
            }
            s_lastNowMs = nowMs;
            return s_lastCmdCount;
        }
        // 2. Schedule Render Pass DAG in C++ (Đọc trực tiếp từ Position<> thống nhất — Zero 2nd Engine)
        timeline::runtime::Frame<> &plan = engine_get_runtime_frame();
        if constexpr (std::is_same_v<Mask, stockcut::Mask<1>>)
        {
            if (pos)
                projectRuntime().renderFromPosition(*pos, nowMs, width, height, plan);
            else
                projectRuntime().renderAt(nowMs, width, height, plan);
        }
        else
        {
            projectRuntime().renderAt(nowMs, width, height, plan);
        }
        // 3. Direct QuadCommand Packing from Position<Mask> (Zero DisplayList / Zero DrawCommand / Zero RenderCommand)
        auto textBinder = [&](u32 slot, float scaleX, u16 &outGlyphSlot, u32 &outAtlasTex) noexcept {
            Render::ensure_slot_text_shaped(slot, scaleX);
            const u32 gSlot = Render::get_slot_glyph_cache(slot);
            outGlyphSlot = static_cast<u16>((gSlot > 0 && gSlot <= MAX_GLYPH_CACHE_SLOTS)
                ? gSlot
                : ((slot % (MAX_TIMELINE_SLOTS - 1u)) + 1u));
            outAtlasTex = Render::get_glyph_atlas_tex_id();
        };
        auto texLookup = [&](u32 slot, u32 resId) noexcept -> u32 {
            u32 texId = Render::get_slot_texture(slot);
            if (!texId && resId != 0u)
            {
                texId = Render::get_resource_texture(resId);
            }
            return texId;
        };
        const u32 cmdCount = quadStream.build_from_position(*pos, nextSt, width, height, nowMs, textBinder, texLookup);
        if (ss)
            ss->commandCount = cmdCount;
        const QuadCommand *cmds = quadStream.data();
        // ------------------------------------------------------------------------
        // TẦNG 4: MERKLE ZOBRIST PYRAMID & TIME-TAINT PROPAGATION
        // ------------------------------------------------------------------------
        if (nextSt && pos)
        {
            s_zobristPyramid.build_leaf_hashes(*pos, *nextSt, activeDynamicFxWord);
        }
        else
        {
            s_zobristPyramid.clear();
        }
        constexpr u32 kTexOpMask =
            (1u << CMD_DRAW_IMAGE) | (1u << CMD_DRAW_VIDEO) | (1u << CMD_DRAW_PATH);
        Key dynamicTextureKey = 0ULL;
        for (const QuadCommand *rc = cmds; rc->opCode != static_cast<u8>(CMD_SENTINEL_END); ++rc)
        {
            if (!rc->visible)
                continue;
            const u32 opBit = 1u << rc->opCode;
            const u32 slotIdx = static_cast<u32>(rc->slot) & SLOT_TRACKER_MASK;
            if (opBit & kTexOpMask)
            {
                const bool ready = (rc->textureId != 0u);
                const Key resTok = ready
                    ? hzobrist_channel_token_T<HZobristChannel::Resource>(slotIdx, rc->textureId)
                    : 0ULL;
                dynamicTextureKey ^= resTok;
                s_zobristPyramid.bind_slot_resource(rc->slot, resTok, ready);
                sceneValid &= static_cast<u32>(ready);
            }
            else if (rc->opCode == CMD_DRAW_TEXT)
            {
                const u32 gCount = (rc->resourceSlot <= MAX_GLYPH_CACHE_SLOTS)
                    ? s_glyphCache[rc->resourceSlot].glyphCount
                    : 0u;
                const bool ready = (rc->resourceSlot != 0u) & (rc->textureId != 0u) & (gCount != 0u);
                const Key textTok = ready
                    ? hzobrist_channel_token_T<HZobristChannel::Resource>(
                          slotIdx, (static_cast<u64>(rc->textureId) << 32) | static_cast<u64>(gCount))
                    : 0ULL;
                dynamicTextureKey ^= textTok;
                s_zobristPyramid.bind_slot_resource(rc->slot, textTok, ready);
                sceneValid &= static_cast<u32>(ready);
            }
        }
        const u32 rootBgPacked = (pos ? pos->active_root_color_packed() : 0u) ^ (ss ? ss->activeBgColorRgba : 0u);
        const Key envKey = hzobrist_viewport_env(
            width, height,
            telem ? telem->styleEpoch : 0u,
            telem ? telem->fontsVer : 0u,
            0u,
            telem ? telem->mediaVer : 0u,
            rootBgPacked);
        s_zobristPyramid.build_pyramid(plan, envKey);

        Key kParam = s_zobristPyramid.root_hash() ^
                     dynamicTextureKey ^
                     plan.effectDynamicZobrist;
        const Mask activeVisBB = (nextSt && pos)
            ? (nextSt->activeBB & ~pos->template by_type_bb<MediaType::Audio>())
            : Mask{};
        const u32 visualLayersCount = static_cast<u32>(activeVisBB.popcount());
        const u32 fxCount = plan.activeEffectCount;
        const bool hasBassReactiveItem = (plan.hasBassReactiveItem != 0);
        const bool hasZoomTiltBass = ((plan.hasZoomTiltBass & 1u) != 0);
        if (hasBassReactiveItem)
        {
            const auto *script = get_active_music_script();
            if (script && script->totalSamples > 0)
            {
                const float lvl = script->sample_level(nowMs);
                const u32 qLvl = static_cast<u32>(clamp(lvl * BASS_QUANT_MAX_F32, 0.0f, BASS_QUANT_MAX_F32));
                u64 bassHash = static_cast<u64>(qLvl);
                if (hasZoomTiltBass)
                {
                    const u32 hit = script->sample_hit_index(nowMs);
                    const u32 hitBit = (hit & 1u);
                    bassHash = (bassHash << 1) | static_cast<u64>(hitBit);
                }
                kParam ^= hzobrist_mix64(bassHash * SPLITMIX_MUL1);
            }
        }
        if (!kParam)
            kParam = 1ULL;
        s_lastNowMs = nowMs;
        if (blitToScreen != 0)
        {
            // 1. ZOBRIST BREAKER SKIP: Trùng trạng thái thị giác khung trước -> 0 draw call, 0 blit, 0 spatial diff!
            if (sceneValid != 0 && !isSeeked && !hasBassReactiveItem && kParam == s_lastParamKey && width == s_lastW && height == s_lastH)
            {
                const double tSkipEnd = g_gpuState.now_ms();
                double skipMs = tSkipEnd - tFrameStart;
                if (skipMs <= 0.0)
                    skipMs = MIN_SKIP_L2_TIME_MS;
                telemetry_record_frame_time(skipMs);
                if (telem)
                {
                    telem->cpuTimeMs = skipMs;
                    write_telemetry_frame_header(telem, nowMs, frameTick, kParam, visualLayersCount, fxCount, width, height);
                    telem->zobristBreakers++;
                    telem->renderMode = TELEMETRY_MODE_NONE;
                    telem->noneCount++;
                }
                return s_lastCmdCount > 0 ? s_lastCmdCount : cmdCount;
            }
        }
        // ====================================================================
        // GIAI ĐOẠN 3: ĐÁNH GIÁ VI SAI KHÔNG GIAN & GPU-DRIVEN RANGE PACKING
        // ====================================================================
        const u64 dirtySlotsWord = nextSt ? nextSt->dirtyBB.word(0) : 0ULL;
        SpatialFrameDirty<> &frameDirty = engine_get_frame_spatial_dirty();
        frameDirty.clear();
        if (pos)
        {
            for (u32 m = 0; m < TRACK_MODE_NB; ++m) frameDirty.byModeBB[m] = Bitboard(pos->by_mode_bb_array()[m].word(0));
            for (u32 t = 0; t < MEDIA_TYPE_NB; ++t) frameDirty.byTypeBB[t] = Bitboard(pos->by_type_bb_array()[t].word(0));
            for (u32 n = 0; n < DYNAMIC_NATURE_NB; ++n) frameDirty.byNatureBB[n] = Bitboard(pos->by_nature_bb_array()[n].word(0));
            for (u32 a = 0; a < DYNAMIC_AXIS_NB; ++a) frameDirty.byDynamicAxisBB[a] = Bitboard(pos->by_dynamic_axis_bb_array()[a].word(0));
            for (u32 i = 0; i < cmdCount; ++i)
            {
                const QuadCommand &rc = cmds[i];
                if (rc.slot >= MAX_TIMELINE_SLOTS)
                    continue;
                const Rect<float> ab = rc.viewport_aabb();
                frameDirty.layerTilesBB[rc.slot] = frameDirty.layerTilesBB[rc.slot] |
                    Bitboard::FromAABBConservative(
                        ab.x0, ab.y0, ab.x1, ab.y1,
                        static_cast<double>(width), static_cast<double>(height));
            }
            gpu_driven::get_gpu_pipeline_manager().execute_gpu_pseudo_prep(
                *pos, dirtySlotsWord, static_cast<float>(nowMs));
        }
        frameDirty.evaluate_spatial_differential(
            static_cast<double>(width),
            static_cast<double>(height),
            dirtySlotsWord);

        // VideoOS Spacetime Tile CoW MMU: compute 64-tile local Zobrist keys & resolve CoW pages
        {
            alignas(64) MultiArray<u64, 64> tileZobrist64;
            tileZobrist64.fill(envKey ^ plan.effectDynamicZobrist);
            u64 visWord = activeVisBB.word(0);
            while (visWord)
            {
                const u32 slot = BitOps<u64>::pop_lsb(visWord);
                const Key leaf = s_zobristPyramid.leaf_hash(slot);
                u64 tiles = frameDirty.layerTilesBB[slot].value;
                while (tiles)
                {
                    const u32 t = BitOps<u64>::pop_lsb(tiles);
                    tileZobrist64[t] ^= leaf;
                }
            }
            s_tileMMU.resolve_frame_cow(tileZobrist64.data(), s_frameTilePTE);
        }

        // ====================================================================
        // GIAI ĐOẠN 4: CHUẨN BỊ UBO RING & ĐIỀU PHỐI BẢNG NHẢY HỢP ĐỒNG O(1)
        // ====================================================================
        ensure_fbo_target(s_sceneFbo, s_sceneTex, s_sceneW, s_sceneH, width, height, GL_LINEAR);

        const bool hasPostProcessingEffect = (plan.activeEffectCount > 0 || plan.hasPostProcessingEffect());
        const bool postFxJustEnded = s_prevFrameHadPostFx && !hasPostProcessingEffect;
        const u32 contractFlags =
            ((isSeeked || frameDirty.isFullRedraw || postFxJustEnded) ? FRAME_FLAG_SEEKED        : 0u) |
            ((sceneValid == 0 || s_sceneTex == 0)                     ? FRAME_FLAG_SCENE_INVALID : 0u) |
            ((animatedPlayback || s_zobristPyramid.is_root_tainted()) ? FRAME_FLAG_TIME_TAINTED  : 0u) |
            (hasPostProcessingEffect                                  ? FRAME_FLAG_HAS_POST_FX   : 0u) |
            (hasBassReactiveItem                                      ? FRAME_FLAG_BASS_ACTIVE   : 0u) |
            (!frameDirty.dirty_tiles_bb().empty()                     ? FRAME_FLAG_SPATIAL_DIRTY : 0u);
        s_prevFrameHadPostFx = hasPostProcessingEffect;

        constexpr u32 kNonReuseMask =
            FRAME_FLAG_SEEKED | FRAME_FLAG_SCENE_INVALID | FRAME_FLAG_HAS_POST_FX | FRAME_FLAG_SPATIAL_DIRTY;

        render::g_frameUboRing.advance_ring();
        render::g_passUboRing.advance_ring();
        render::g_monoidUboRing.advance_ring();

        render::FrameUniformBlock frameData{};
        frameData.viewportRes[0] = static_cast<float>(width);
        frameData.viewportRes[1] = static_cast<float>(height);
        frameData.viewportRes[2] = (width > 0) ? (1.0f / static_cast<float>(width)) : 0.0f;
        frameData.viewportRes[3] = (height > 0) ? (1.0f / static_cast<float>(height)) : 0.0f;
        frameData.timeState[0] = static_cast<float>(tFrameStart);
        frameData.timeState[1] = 16.6667f;
        frameData.timeState[2] = 0.0f;
        frameData.timeState[3] = ((contractFlags & kNonReuseMask) == 0u) ? 0.0f : 1.0f;
        frameData.bgColor[0] = bgR;
        frameData.bgColor[1] = bgG;
        frameData.bgColor[2] = bgB;
        frameData.bgColor[3] = bgA;

        render::g_frameUboRing.upload_current(frameData);
        render::g_frameUboRing.bind_current();

        SharedRenderState *sharedRender = engine_get_shared_render_state();
        const u32 drawn = kFrameContractLut[contractFlags](
            width, height, cmdCount, cmds,
            plan, frameDirty, bgR, bgG, bgB, bgA,
            sharedRender, telem, (blitToScreen != 0) ? kParam : 0ULL);

        // ====================================================================
        // GIAI ĐOẠN 5: XUẤT MÀN HÌNH VIEWPORT & BÁO CÁO TELEMETRY
        // ====================================================================
        if (blitToScreen != 0)
        {
            blit_scene_to_screen(width, height);
        }

        if (telem)
        {
            write_telemetry_frame_header(telem, nowMs, frameTick, kParam, visualLayersCount, fxCount, width, height);
            constexpr double MIN_RENDER_TIME_MS = 0.0001;
            const double tRenderEnd = g_gpuState.now_ms();
            double renderMs = tRenderEnd - tFrameStart;
            if (renderMs <= 0.0)
                renderMs = MIN_RENDER_TIME_MS;
            telemetry_record_frame_time(renderMs);
            telem->cpuTimeMs = renderMs;
        }

        s_lastParamKey = kParam;
        s_lastW = width;
        s_lastH = height;
        s_lastCmdCount = drawn;
        return drawn;
#else
        (void)nowMs;
        (void)width;
        (void)height;
        (void)frameTick;
        (void)sceneValid;
        (void)bgR;
        (void)bgG;
        (void)bgB;
        (void)bgA;
        (void)blitToScreen;
        return 0;
#endif
    }

    void Render::render_selection_overlay(
        float stageW, float stageH,
        const void* segsPtr, u32 segCount, float dx, float dy,
        float lineThick
    ) noexcept {
#if defined(STOCKCUT_HAS_GLES3)
        if (!s_initialized || stageW <= 0.0f || stageH <= 0.0f) return;

        const u32 w = static_cast<u32>(stageW);
        const u32 h = static_cast<u32>(stageH);
        g_gpuState.blit_to_screen(s_sceneFbo, w, h);

        if (!segsPtr || segCount == 0) return;

        g_gpuState.bind_framebuffer(0, w, h);
        g_gpuState.set_scissor<false>();
        g_gpuState.set_blend<true>();
        g_gpuState.set_blend_func_separate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        begin_batch_pass(w, h);

        // Stockfish Cool Green (#43ad6a -> RGB: 67, 173, 106)
        constexpr float sfGreenR = 0.262745f;
        constexpr float sfGreenG = 0.678431f;
        constexpr float sfGreenB = 0.415686f;
        const float effLineThick = (lineThick > 0.1f) ? lineThick : 2.0f;
        const float halfThick = effLineThick * 0.5f;

        const auto* segs = static_cast<const timeline::SelectionSegment*>(segsPtr);
        for (u32 i = 0; i < segCount; ++i) {
            const auto& s = segs[i];
            if (std::abs(s.y0 - s.y1) < 0.5f) {
                const float xMin = std::min(s.x0, s.x1) + dx;
                const float xMax = std::max(s.x0, s.x1) + dx;
                const float segW = xMax - xMin;
                const float y = s.y0 + dy - halfThick;
                if (segW > 0.1f) {
                    push_batch_quad(
                        xMin, y, segW, effLineThick,
                        xMin + segW * 0.5f, y + halfThick, 0.0f,
                        sfGreenR, sfGreenG, sfGreenB, 1.0f,
                        0, static_cast<float>(QUAD_MODE_COLOR), 0.0f,
                        0.0f, 0.0f, 1.0f, 1.0f, s_whiteTex);
                }
            } else if (std::abs(s.x0 - s.x1) < 0.5f) {
                const float yMin = std::min(s.y0, s.y1) + dy;
                const float yMax = std::max(s.y0, s.y1) + dy;
                const float segH = yMax - yMin;
                const float x = s.x0 + dx - halfThick;
                if (segH > 0.1f) {
                    push_batch_quad(
                        x, yMin, effLineThick, segH,
                        x + halfThick, yMin + segH * 0.5f, 0.0f,
                        sfGreenR, sfGreenG, sfGreenB, 1.0f,
                        0, static_cast<float>(QUAD_MODE_COLOR), 0.0f,
                        0.0f, 0.0f, 1.0f, 1.0f, s_whiteTex);
                }
            }
        }
        flush_batch_pass(s_whiteTex);
#else
        (void)stageW; (void)stageH; (void)segsPtr; (void)segCount;
        (void)dx; (void)dy; (void)lineThick;
#endif
    }

    cache::TranspositionTable<cache::GpuSurface, 4, 4>& Render::get_gpu_surface_tt() noexcept
    {
        return s_gpuSurfaceTT;
    }

    cache::ZobristPyramid<>& Render::get_zobrist_pyramid() noexcept
    {
        return s_zobristPyramid;
    }

    // Explicit template instantiations for Render
    template u32 Render::execute_frame<Mask<1>>(u32, u32, u32, u32, u32, u32, u32, u32, float, float, float, float, u32) noexcept;
    template u32 Render::execute_frame<Mask<2>>(u32, u32, u32, u32, u32, u32, u32, u32, float, float, float, float, u32) noexcept;
    template u32 Render::render_timeline_frame<Mask<1>>(double, u32, u32, u32, u32, float, float, float, float, u32) noexcept;
    template u32 Render::render_timeline_frame<Mask<2>>(double, u32, u32, u32, u32, float, float, float, float, u32) noexcept;
} // namespace stockcut
