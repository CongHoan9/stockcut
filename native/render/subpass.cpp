// ============================================================================
// native/render/subpass.cpp
// TRỤ CỘT 3: TILE-BASED CLUSTERED EXECUTION & BITBOARD DISPATCH
// TRỤ CỘT 4: COMPUTE SHADER SUBPASSES & ON-CHIP TILE MEMORY
// ============================================================================
#include "render/subpass.h"

namespace stockcut {

void TileSubpassChain::compile_from_render_graph(
    const RenderGraph& graph,
    float viewportW,
    float viewportH
) noexcept {
    reset();
    const RenderPassNode* const pBegin = graph.nodes.data();
    const RenderPassNode* const pEnd = pBegin + graph.nodeCount;

    u32 passIndex = 0;
    for (const RenderPassNode* p = pBegin; p < pEnd; ++p, ++passIndex) {
        if (p->isCulled) continue;

        if (p->graphKind == RenderGraphPassKind::Clear || p->graphKind == RenderGraphPassKind::RasterLayers) {
            continue;
        }

        Bitboard activeTiles = ~0ULL;
        const bool isIntermediate = (p->outputVirtualTarget != 0);
        const bool canUseFbFetch = (subpassCount > 0);

        add_subpass(
            passIndex,
            activeTiles,
            isIntermediate,
            canUseFbFetch,
            viewportW,
            viewportH
        );
    }
}

} // namespace stockcut
