// ============================================================================
// CoreEngine - Natron Proxy RenderScale Subsystem
// ============================================================================
#pragma once

#include "types.h"
#include "dirty_rect.h"

namespace stockcut {

constexpr float MIN_RENDER_SCALE = 0.05f;
constexpr float MAX_RENDER_SCALE = 4.0f;

struct RenderScale {
    float x;
    float y;
    constexpr RenderScale() : x(1.0f), y(1.0f) {}
    constexpr RenderScale(float sx, float sy) : x(sx), y(sy) {}
    static constexpr RenderScale fromMipmapLevel(u32 level) {
        const float factor = 1.0f / static_cast<float>(1u << level);
        return RenderScale(factor, factor);
    }
    constexpr bool isIdentity() const {
        return (x >= 1.0f - IDENTITY_EPSILON && x <= 1.0f + IDENTITY_EPSILON) &&
               (y >= 1.0f - IDENTITY_EPSILON && y <= 1.0f + IDENTITY_EPSILON);
    }
    inline void canonicalToRender(float inX, float inY, float& outX, float& outY) const {
        outX = inX * x;
        outY = inY * y;
    }
    inline void renderToCanonical(float inX, float inY, float& outX, float& outY) const {
        outX = inX / x;
        outY = inY / y;
    }
    inline Rect<double> scaleRect(const Rect<double>& r) const {
        return Rect<double>{
            r.x0 * x,
            r.y0 * y,
            r.x1 * x,
            r.y1 * y
        };
    }
};

void render_scale_set(float sx, float sy);
void render_scale_set_level(u32 level);
RenderScale render_scale_get();
bool render_scale_is_identity();

} // namespace stockcut
