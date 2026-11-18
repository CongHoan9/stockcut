// ============================================================================
// NodeVideo Unified _RaindropPro ("rain drop") Engine
// Supports all 5 composable sub-layers + shared global _strength/_blur_type/_blur:
//   - RAIN_LAYER_STATIC1 : _static1 small static droplets
//   - RAIN_LAYER_STATIC2 : _static2 medium static droplets
//   - RAIN_LAYER_DYNAMIC : _dynamic gravity-meandering drops
//   - RAIN_LAYER_TRAIL   : _trail (ViewRaindrop baked trail ribbons + head drops + Stained_BumpDistort)
//   - RAIN_LAYER_TRAIL2  : _trail2 (8x8 RainStreaksTextureSheet + layer_style_glass bevel)
// ============================================================================
#pragma once
#include "types.h"

#if defined(__EMSCRIPTEN__)
#include <GLES3/gl3.h>

namespace stockcut {
namespace effect {

bool raindrop_suite_fx_init() noexcept;
void raindrop_suite_set_current_params(const RainDropParams* params) noexcept;
void raindrop_suite_fx_render(GLuint inTex, u32 width, u32 height,
                              float time, float env) noexcept;

} // namespace effect
} // namespace stockcut
#endif
