// ============================================================================
// native/render/render_graph.cpp
// TRỤ CỘT 2: RENDER GRAPH (FRAMEGRAPH) & TRANSIENT MEMORY ALIASING
// ============================================================================
#include "render/graph.h"
#include "effect/magic.h"
#include <cmath>

namespace stockcut {

void RenderGraph::compile(u8 finalOutputTarget) noexcept {
    if (nodeCount == 0) return;

    // Tính toán topology signature để phát hiện vi sai đối xứng DAG: Delta = curr ^ last
    u64 currTopologyHash = static_cast<u64>(finalOutputTarget);
    currTopologyHash = (currTopologyHash * 31ULL) ^ static_cast<u64>(nodeCount);
    for (u32 i = 0; i < nodeCount; ++i) {
        const RenderPassNode& n = nodes[i];
        const u64 sig = (static_cast<u64>(n.runtimeKind) << 16) |
                        (static_cast<u64>(n.inputVirtualTarget) << 8) |
                        static_cast<u64>(n.outputVirtualTarget);
        currTopologyHash = (currTopologyHash * 1099511628211ULL) ^ sig;
    }

    const u64 deltaDiff = currTopologyHash ^ lastGraphTopologyHash;
    if (deltaDiff == 0ULL && nodeCount == lastNodeCount && finalOutputTarget == lastFinalOutputTarget) {
        // Physical Gatekeeper: DAG topology giống hệt frame trước
        // Bỏ qua DCE, Pass Contraction, và Memory Aliasing!
        for (u32 i = 0; i < nodeCount; ++i) {
            const PassAssignment& ca = cachedAssignments[i];
            nodes[i].inputPhysicalFbo = ca.inputFbo;
            nodes[i].outputPhysicalFbo = ca.outputFbo;
            nodes[i].isCulled = ca.isCulled;
            nodes[i].isFused = ca.isFused;
        }
        if (fusedPassCount > 0) {
            monoid_pass_contraction(finalOutputTarget);
        }
        return;
    }

    dead_code_elimination(finalOutputTarget);
    monoid_pass_contraction(finalOutputTarget);
    transient_memory_aliasing();

    for (u32 i = 0; i < nodeCount; ++i) {
        cachedAssignments[i] = {
            nodes[i].inputPhysicalFbo,
            nodes[i].outputPhysicalFbo,
            nodes[i].isCulled,
            nodes[i].isFused
        };
    }
    lastGraphTopologyHash = currTopologyHash;
    lastNodeCount = nodeCount;
    lastFinalOutputTarget = finalOutputTarget;
}

void RenderGraph::dead_code_elimination(u8 finalOutputTarget) noexcept {
    if (nodeCount == 0) return;
    Bitboard neededTargets(1ULL << finalOutputTarget);
    culledPassCount = 0;

    RenderPassNode* const pBegin = nodes.data();
    for (RenderPassNode* p = pBegin + nodeCount - 1; p >= pBegin; --p) {
        if (p->graphKind == RenderGraphPassKind::Clear || p->graphKind == RenderGraphPassKind::RasterLayers) {
            if ((p->writesMask & neededTargets).any()) {
                p->isCulled = 0;
                neededTargets |= p->readsMask;
            } else {
                p->isCulled = 1;
                ++culledPassCount;
            }
            continue;
        }

        if ((p->writesMask & neededTargets).any()) {
            p->isCulled = 0;
            neededTargets &= ~p->writesMask;
            neededTargets |= p->readsMask;
        } else {
            p->isCulled = 1;
            ++culledPassCount;
        }
    }
}

void RenderGraph::monoid_pass_contraction(u8 finalOutputTarget) noexcept {
    get_magic().contract_passes(
        nodes.data(), nodeCount, fusiblePassesMask,
        fusedPassCount, culledPassCount, finalOutputTarget
    );
}

void RenderGraph::transient_memory_aliasing() noexcept {
    for (Bitboard bb = referencedVirtualTargetsBB; bb;) {
        const u32 v = bb.pop_lsb();
        virtualLifetimes[v] = TargetLifetime{};
    }
    referencedVirtualTargetsBB = 0ULL;

    for (u32 i = 0; i < nodeCount; ++i) {
        const RenderPassNode& node = nodes[i];
        if (node.isCulled) continue;

        if (node.inputVirtualTarget < MAX_VIRTUAL_TARGETS) {
            auto& lt = virtualLifetimes[node.inputVirtualTarget];
            lt.isReferenced = 1;
            lt.lastReadPass = static_cast<u16>(i);
            lt.readCount++;
            referencedVirtualTargetsBB.set_bit(node.inputVirtualTarget);
        }
        if (node.outputVirtualTarget < MAX_VIRTUAL_TARGETS) {
            auto& lt = virtualLifetimes[node.outputVirtualTarget];
            if (!lt.isReferenced) {
                lt.firstWritePass = static_cast<u16>(i);
                lt.isReferenced = 1;
                referencedVirtualTargetsBB.set_bit(node.outputVirtualTarget);
            }
        }
    }

    virtualLifetimes[0].assignedFbo = 0;
    u32 maxFboUsed = 0;

    MultiArray<u16, MAX_PHYSICAL_FBOS> fboFreeAt{};
    fboFreeAt[0] = 0xFFFF; // FBO 0 luôn bảo toàn

    Bitboard activeTargets = referencedVirtualTargetsBB & ~1ULL;
    while (activeTargets) {
        const u32 v = activeTargets.pop_lsb();
        auto& lt = virtualLifetimes[v];

        u8 chosenFbo = 0;
        for (u8 f = 1; f < MAX_PHYSICAL_FBOS; ++f) {
            if (fboFreeAt[f] <= lt.firstWritePass) {
                chosenFbo = f;
                break;
            }
        }
        if (chosenFbo == 0) {
            if (maxFboUsed + 1 < MAX_PHYSICAL_FBOS) {
                chosenFbo = static_cast<u8>(++maxFboUsed);
            } else {
                chosenFbo = 1;
            }
        }
        fboFreeAt[chosenFbo] = lt.lastReadPass;
        lt.assignedFbo = chosenFbo;
    }
    physicalFboCount = maxFboUsed + 1;

    activeNodeCount = 0;
    for (u32 i = 0; i < nodeCount; ++i) {
        RenderPassNode& node = nodes[i];
        if (node.isCulled) continue;
        ++activeNodeCount;
        if (node.inputVirtualTarget < MAX_VIRTUAL_TARGETS) {
            node.inputPhysicalFbo = virtualLifetimes[node.inputVirtualTarget].assignedFbo;
        }
        if (node.outputVirtualTarget < MAX_VIRTUAL_TARGETS) {
            node.outputPhysicalFbo = virtualLifetimes[node.outputVirtualTarget].assignedFbo;
        }
        if (node.graphKind != RenderGraphPassKind::Clear &&
            node.graphKind != RenderGraphPassKind::RasterLayers &&
            node.inputPhysicalFbo == node.outputPhysicalFbo) {
            node.outputPhysicalFbo = (node.inputPhysicalFbo == 1) ? 2 : 1;
            if (node.outputPhysicalFbo >= physicalFboCount) {
                physicalFboCount = node.outputPhysicalFbo + 1;
            }
        }
    }
}

} // namespace stockcut
