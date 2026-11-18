// ============================================================================
// CoreEngine - Natron Proxy RenderScale Implementation
// ============================================================================
#include "scale.h"
#include "math_utils.h"

namespace stockcut {

static RenderScale g_currentRenderScale(1.0f, 1.0f);

void render_scale_set(float sx, float sy) {
    g_currentRenderScale.x = clamp(sx, MIN_RENDER_SCALE, MAX_RENDER_SCALE);
    g_currentRenderScale.y = clamp(sy, MIN_RENDER_SCALE, MAX_RENDER_SCALE);
}

void render_scale_set_level(u32 level) {
    g_currentRenderScale = RenderScale::fromMipmapLevel(level);
}

RenderScale render_scale_get() {
    return g_currentRenderScale;
}

bool render_scale_is_identity() {
    return g_currentRenderScale.isIdentity();
}

} // namespace stockcut
