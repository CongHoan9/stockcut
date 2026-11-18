// ============================================================================
// CoreEngine - VboBatcher (PixiJS v8 / Olive NLE Direct VBO Generator)
// ============================================================================
#include "batcher.h"
#include "math_utils.h"

namespace stockcut {

VboBatcher g_vboBatcher;

const u32* render_constants_data() {
    static constexpr MultiArray<u32, RC_COUNT> constants = {{
        MAX_BATCH_QUADS_CPP,
        FLOATS_PER_VERTEX_CPP,
        VERTICES_PER_QUAD_CPP,
        FLOATS_PER_QUAD_CPP,
        FLOATS_PER_QUAD_INPUT_CPP,
        MAX_TEXTURE_SLOTS,
        FLOAT_BYTES,
        VERTEX_POSITION_COMPONENTS,
        VERTEX_UV_COMPONENTS,
        VERTEX_COLOR_COMPONENTS,
        VERTEX_TEXTURE_ID_COMPONENTS,
        VERTEX_MODE_COMPONENTS,
        VERTEX_SPIN_COMPONENTS,
        VERTEX_POSITION_OFFSET,
        VERTEX_UV_OFFSET,
        VERTEX_COLOR_OFFSET,
        VERTEX_TEXTURE_ID_OFFSET,
        VERTEX_MODE_OFFSET,
        VERTEX_SPIN_OFFSET,
        MAX_DISPLAY_COMMANDS_RENDER,
        DRAW_COMMAND_WORDS,
        DRAW_COMMAND_FLOATS,
        RENDER_COMMAND_WORDS,
        RENDER_COMMAND_FLOATS,
        VBO_INPUT_X,
        VBO_INPUT_Y,
        VBO_INPUT_WIDTH,
        VBO_INPUT_HEIGHT,
        VBO_INPUT_PIVOT_X,
        VBO_INPUT_PIVOT_Y,
        VBO_INPUT_ROTATION_RAD,
        VBO_INPUT_RED,
        VBO_INPUT_GREEN,
        VBO_INPUT_BLUE,
        VBO_INPUT_ALPHA,
        VBO_INPUT_TEXTURE_SLOT,
        VBO_INPUT_MODE,
        VBO_INPUT_SPIN_RAD,
        VBO_INPUT_UV_LEFT,
        VBO_INPUT_UV_TOP,
        VBO_INPUT_UV_RIGHT,
        VBO_INPUT_UV_BOTTOM
    }};
    return constants.data();
}

VboBatcher::VboBatcher()
    : m_quadCount(0),
      m_cursor(m_buffer.data()),
      m_cachedRotation(0.0f),
      m_cachedCos(1.0f),
      m_cachedSin(0.0f),
      m_hasCachedRotation(false)
{
    reset();
}

void VboBatcher::reset() {
    m_cursor = m_buffer.data();
    m_quadCount = 0;
}

inline void write_vertex(
    float* dst, float x, float y, float u, float v,
    float r, float g, float b, float a,
    float texId, float mode, float spinRad
) {
    dst[VERTEX_POSITION_OFFSET] = x;
    dst[VERTEX_POSITION_OFFSET + 1] = y;
    dst[VERTEX_UV_OFFSET] = u;
    dst[VERTEX_UV_OFFSET + 1] = v;
    dst[VERTEX_COLOR_OFFSET] = r;
    dst[VERTEX_COLOR_OFFSET + 1] = g;
    dst[VERTEX_COLOR_OFFSET + 2] = b;
    dst[VERTEX_COLOR_OFFSET + 3] = a;
    dst[VERTEX_TEXTURE_ID_OFFSET] = texId;
    dst[VERTEX_MODE_OFFSET] = mode;
    dst[VERTEX_SPIN_OFFSET] = spinRad;
}

template <bool Rotated>
inline void emit_quad_vertices(
    float x, float y, float w, float h,
    float pivotX, float pivotY, float cosR, float sinR,
    float r, float g, float b, float a,
    float texId, float mode, float spinRad,
    float uvLeft, float uvTop, float uvRight, float uvBottom,
    float* dst
) {
    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y;
    float x2 = x, y2 = y + h;
    float x3 = x + w, y3 = y + h;
    if constexpr (Rotated) {
        const float dx0 = x0 - pivotX, dy0 = y0 - pivotY;
        const float dx1 = x1 - pivotX, dy1 = y1 - pivotY;
        const float dx2 = x2 - pivotX, dy2 = y2 - pivotY;
        const float dx3 = x3 - pivotX, dy3 = y3 - pivotY;
        x0 = pivotX + dx0 * cosR - dy0 * sinR;
        y0 = pivotY + dx0 * sinR + dy0 * cosR;
        x1 = pivotX + dx1 * cosR - dy1 * sinR;
        y1 = pivotY + dx1 * sinR + dy1 * cosR;
        x2 = pivotX + dx2 * cosR - dy2 * sinR;
        y2 = pivotY + dx2 * sinR + dy2 * cosR;
        x3 = pivotX + dx3 * cosR - dy3 * sinR;
        y3 = pivotY + dx3 * sinR + dy3 * cosR;
    }
    constexpr u32 stride = FLOATS_PER_VERTEX_CPP;
    write_vertex(dst, x0, y0, uvLeft, uvTop, r, g, b, a, texId, mode, spinRad);
    write_vertex(dst + stride, x1, y1, uvRight, uvTop, r, g, b, a, texId, mode, spinRad);
    write_vertex(dst + stride * 2, x2, y2, uvLeft, uvBottom, r, g, b, a, texId, mode, spinRad);
    write_vertex(dst + stride * 3, x2, y2, uvLeft, uvBottom, r, g, b, a, texId, mode, spinRad);
    write_vertex(dst + stride * 4, x1, y1, uvRight, uvTop, r, g, b, a, texId, mode, spinRad);
    write_vertex(dst + stride * 5, x3, y3, uvRight, uvBottom, r, g, b, a, texId, mode, spinRad);
}

namespace {

using EmitQuadFn = void (*)(
    float x, float y, float w, float h,
    float pivotX, float pivotY, float cosR, float sinR,
    float r, float g, float b, float a,
    float texId, float mode, float spinRad,
    float uvLeft, float uvTop, float uvRight, float uvBottom,
    float* dst
);

static constexpr MultiArray<EmitQuadFn, 2> kEmitQuadLut = {{
    &emit_quad_vertices<false>,
    &emit_quad_vertices<true>
}};

} // namespace

void VboBatcher::push_quad_fast(
    float x, float y, float w, float h,
    float pivotX, float pivotY, float rotRad,
    float r, float g, float b, float a,
    float texId, float mode, float spinRad,
    float uvLeft, float uvTop, float uvRight, float uvBottom
) noexcept {
    if (m_quadCount >= MAX_BATCH_QUADS_CPP) {
        return;
    }
    float* dst = m_cursor;
    m_cursor += FLOATS_PER_QUAD_CPP;
    ++m_quadCount;
    const bool isRot = (rotRad != 0.0f);
    if (isRot) {
        if (!m_hasCachedRotation || rotRad != m_cachedRotation) {
            m_cachedRotation = rotRad;
            m_cachedCos = fast_cos(rotRad);
            m_cachedSin = fast_sin(rotRad);
            m_hasCachedRotation = true;
        }
    }
    kEmitQuadLut[static_cast<usize>(isRot)](
        x, y, w, h, pivotX, pivotY, m_cachedCos, m_cachedSin,
        r, g, b, a, texId, mode, spinRad,
        uvLeft, uvTop, uvRight, uvBottom, dst);
}

bool VboBatcher::push_quad(
    float x, float y, float w, float h,
    float pivotX, float pivotY, float rotRad,
    float r, float g, float b, float a,
    float texId, float mode, float spinRad,
    float uvLeft, float uvTop, float uvRight, float uvBottom
) {
    if (m_quadCount >= MAX_BATCH_QUADS_CPP) {
        return false;
    }
    push_quad_fast(
        x, y, w, h, pivotX, pivotY, rotRad,
        r, g, b, a, texId, mode, spinRad,
        uvLeft, uvTop, uvRight, uvBottom);
    return true;
}

u32 VboBatcher::push_batch(const float* inputs, u32 count) {
    if (!inputs || count == 0)
        return 0;
    const u32 avail = MAX_BATCH_QUADS_CPP - m_quadCount;
    const u32 actualCount = count < avail ? count : avail;
    auto push_from_ptr = [this](const float* in) noexcept {
        push_quad_fast(
            in[VBO_INPUT_X], in[VBO_INPUT_Y], in[VBO_INPUT_WIDTH], in[VBO_INPUT_HEIGHT],
            in[VBO_INPUT_PIVOT_X], in[VBO_INPUT_PIVOT_Y], in[VBO_INPUT_ROTATION_RAD],
            in[VBO_INPUT_RED], in[VBO_INPUT_GREEN], in[VBO_INPUT_BLUE], in[VBO_INPUT_ALPHA],
            in[VBO_INPUT_TEXTURE_SLOT], in[VBO_INPUT_MODE], in[VBO_INPUT_SPIN_RAD],
            in[VBO_INPUT_UV_LEFT], in[VBO_INPUT_UV_TOP], in[VBO_INPUT_UV_RIGHT], in[VBO_INPUT_UV_BOTTOM]);
    };
    constexpr u32 kStride = FLOATS_PER_QUAD_INPUT_CPP;
    const float* p = inputs;
    const float* const end4 = inputs + (actualCount & ~3u) * kStride;
    const float* const end  = inputs + actualCount * kStride;
    for (; p < end4; p += kStride * 4) {
        push_from_ptr(p);
        push_from_ptr(p + kStride);
        push_from_ptr(p + kStride * 2);
        push_from_ptr(p + kStride * 3);
    }
    for (; p < end; p += kStride) {
        push_from_ptr(p);
    }
    return actualCount;
}

} // namespace stockcut

// ----------------------------------------------------------------------------
// C-ABI Exports cho WebAssembly
// ----------------------------------------------------------------------------
extern "C" {
WASM_EXPORT(wasm_vbo_reset) void wasm_vbo_reset() {
    stockcut::g_vboBatcher.reset();
}
WASM_EXPORT(wasm_vbo_push_batch) stockcut::u32 wasm_vbo_push_batch(
    stockcut::u32 count
) {
    return stockcut::g_vboBatcher.push_batch(
        stockcut::g_vboBatcher.input_data_mut(), count);
}
WASM_EXPORT(wasm_vbo_get_input_buffer_ptr) float* wasm_vbo_get_input_buffer_ptr() {
    return stockcut::g_vboBatcher.input_data_mut();
}
WASM_EXPORT(wasm_vbo_get_render_constants_ptr) const stockcut::u32*
wasm_vbo_get_render_constants_ptr() {
    return stockcut::render_constants_data();
}
WASM_EXPORT(wasm_vbo_get_render_constants_count) stockcut::u32
wasm_vbo_get_render_constants_count() {
    return stockcut::RC_COUNT;
}
WASM_EXPORT(wasm_vbo_get_buffer_ptr) float* wasm_vbo_get_buffer_ptr() {
    return stockcut::g_vboBatcher.vbo_data_mut();
}
WASM_EXPORT(wasm_vbo_get_quad_count) stockcut::u32 wasm_vbo_get_quad_count() {
    return stockcut::g_vboBatcher.quad_count();
}
}
