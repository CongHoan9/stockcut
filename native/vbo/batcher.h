// ============================================================================
// CoreEngine - VboBatcher (PixiJS v8 / Olive NLE Direct VBO Generator)
// ============================================================================
#pragma once
#include "types.h"
#include "data_structures.h"
#include "render_constants.h"

namespace stockcut {

class VboBatcher {
public:
    VboBatcher();
    void reset();

    bool push_quad(
        float x, float y, float w, float h,
        float pivotX, float pivotY, float rotRad,
        float r, float g, float b, float a,
        float texId, float mode, float spinRad,
        float uvLeft, float uvTop, float uvRight, float uvBottom
    );
    void push_quad_fast(
        float x, float y, float w, float h,
        float pivotX, float pivotY, float rotRad,
        float r, float g, float b, float a,
        float texId, float mode, float spinRad,
        float uvLeft, float uvTop, float uvRight, float uvBottom
    ) noexcept;
    u32 push_batch(const float* inputs, u32 count);

    u32 quad_count() const noexcept { return m_quadCount; }
    const float* vbo_data() const noexcept { return m_buffer.data(); }
    float* vbo_data_mut() noexcept { return m_buffer.data(); }
    float* input_data_mut() noexcept { return m_inputBuffer.data(); }

private:
    alignas(16) MultiArray<float, TOTAL_VBO_FLOATS> m_buffer;
    alignas(16) MultiArray<float, TOTAL_VBO_INPUT_FLOATS> m_inputBuffer;
    u32 m_quadCount = 0;
    float* m_cursor = nullptr;
    float m_cachedRotation = 0.0f;
    float m_cachedCos = 1.0f;
    float m_cachedSin = 0.0f;
    bool m_hasCachedRotation = false;
};

extern VboBatcher g_vboBatcher;

} // namespace stockcut
