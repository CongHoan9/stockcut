// ============================================================================
// CoreEngine - Scene Graph Hierarchy & SIMD128 Geometry Implementation
// ============================================================================
#include "geometry.h"
#include "style.h"
#include "math_utils.h"

namespace stockcut {
alignas(64) static MultiArray<SceneNode, MAX_SCENE_NODES> g_sceneNodes;
alignas(64) static GeometryResult g_geometryResult;

SceneNode* geometry_get_nodes_ptr() {
    return g_sceneNodes.data();
}

GeometryResult* geometry_get_result_ptr() {
    return &g_geometryResult;
}

SceneNode& scene_node(u32 idx) noexcept {
    return g_sceneNodes[idx & (MAX_SCENE_NODES - 1u)];
}

void SceneNode::reset(u16 slot) noexcept {
    set_world_transform(Transform2D::identity());
    aabbMinX = 0.0f;
    aabbMinY = 0.0f;
    aabbMaxX = 0.0f;
    aabbMaxY = 0.0f;
    localX = 0.0f;
    localY = 0.0f;
    localW = 0.0f;
    localH = 0.0f;
    parentIndex = INVALID_U16;
    styleSlot = slot;
    clipNode = INVALID_U16;
    flags = NODE_FLAG_VISIBLE | NODE_FLAG_DIRTY;
}

void SceneNode::set_local(
    float x, float y, float w, float h,
    u16 parentIdx, u16 sSlot, u16 clip, u16 fl
) noexcept {
    localX = x;
    localY = y;
    localW = w;
    localH = h;
    parentIndex = parentIdx;
    styleSlot = sSlot;
    clipNode = clip;
    flags = fl | NODE_FLAG_DIRTY;
}

void geometry_reset_node(u32 idx) {
    scene_node(idx).reset(static_cast<u16>(idx));
}

void geometry_set_node_local(
    u32 idx,
    float x, float y, float w, float h,
    u16 parentIdx, u16 styleSlot, u16 clipNode, u16 flags
) {
    scene_node(idx).set_local(x, y, w, h, parentIdx, styleSlot, clipNode, flags);
}

void geometry_compute_aabb_simd(
    float a, float b, float c, float d, float tx, float ty,
    float lx, float ly, float lw, float lh,
    float* outAABB4Floats
) {
#if HAS_WASM_SIMD
    const v128_t vx = wasm_f32x4_make(lx, lx + lw, lx + lw, lx);
    const v128_t vy = wasm_f32x4_make(ly, ly, ly + lh, ly + lh);
    const v128_t va  = wasm_f32x4_splat(a);
    const v128_t vb  = wasm_f32x4_splat(b);
    const v128_t vc  = wasm_f32x4_splat(c);
    const v128_t vd  = wasm_f32x4_splat(d);
    const v128_t vtx = wasm_f32x4_splat(tx);
    const v128_t vty = wasm_f32x4_splat(ty);
    const v128_t px = wasm_f32x4_add(wasm_f32x4_add(wasm_f32x4_mul(va, vx), wasm_f32x4_mul(vc, vy)), vtx);
    const v128_t py = wasm_f32x4_add(wasm_f32x4_add(wasm_f32x4_mul(vb, vx), wasm_f32x4_mul(vd, vy)), vty);
    const v128_t px_rot2 = wasm_i32x4_shuffle(px, px, 2, 3, 0, 1);
    const v128_t px_min2 = wasm_f32x4_pmin(px, px_rot2);
    const v128_t px_max2 = wasm_f32x4_pmax(px, px_rot2);
    const v128_t px_rot1 = wasm_i32x4_shuffle(px_min2, px_min2, 1, 0, 3, 2);
    const v128_t px_min4 = wasm_f32x4_pmin(px_min2, px_rot1);
    const v128_t px_rot3 = wasm_i32x4_shuffle(px_max2, px_max2, 1, 0, 3, 2);
    const v128_t px_max4 = wasm_f32x4_pmax(px_max2, px_rot3);
    const v128_t py_rot2 = wasm_i32x4_shuffle(py, py, 2, 3, 0, 1);
    const v128_t py_min2 = wasm_f32x4_pmin(py, py_rot2);
    const v128_t py_max2 = wasm_f32x4_pmax(py, py_rot2);
    const v128_t py_rot1 = wasm_i32x4_shuffle(py_min2, py_min2, 1, 0, 3, 2);
    const v128_t py_min4 = wasm_f32x4_pmin(py_min2, py_rot1);
    const v128_t py_rot3 = wasm_i32x4_shuffle(py_max2, py_max2, 1, 0, 3, 2);
    const v128_t py_max4 = wasm_f32x4_pmax(py_max2, py_rot3);
    outAABB4Floats[0] = wasm_f32x4_extract_lane(px_min4, 0);
    outAABB4Floats[1] = wasm_f32x4_extract_lane(py_min4, 0);
    outAABB4Floats[2] = wasm_f32x4_extract_lane(px_max4, 0);
    outAABB4Floats[3] = wasm_f32x4_extract_lane(py_max4, 0);
#else
    const float x0 = a * lx + c * ly + tx;
    const float y0 = b * lx + d * ly + ty;
    const float x1 = a * (lx + lw) + c * ly + tx;
    const float y1 = b * (lx + lw) + d * ly + ty;
    const float x2 = a * (lx + lw) + c * (ly + lh) + tx;
    const float y2 = b * (lx + lw) + d * (ly + lh) + ty;
    const float x3 = a * lx + c * (ly + lh) + tx;
    const float y3 = b * lx + d * (ly + lh) + ty;
    outAABB4Floats[0] = stockcut::min(stockcut::min(x0, x1), stockcut::min(x2, x3));
    outAABB4Floats[1] = stockcut::min(stockcut::min(y0, y1), stockcut::min(y2, y3));
    outAABB4Floats[2] = stockcut::max(stockcut::max(x0, x1), stockcut::max(x2, x3));
    outAABB4Floats[3] = stockcut::max(stockcut::max(y0, y1), stockcut::max(y2, y3));
#endif
}

GeometryResult* geometry_evaluate_scene(u32 nodeCount, float viewportW, float viewportH) {
    const u32 count = (nodeCount > MAX_SCENE_NODES) ? MAX_SCENE_NODES : nodeCount;
    g_geometryResult.totalNodes = count;
    g_geometryResult.visibleNodes = 0u;
    g_geometryResult.culledNodes = 0u;
    g_geometryResult.clippedNodes = 0u;
    g_geometryResult.sceneMinX = AABB_INF_BOUND;
    g_geometryResult.sceneMinY = AABB_INF_BOUND;
    g_geometryResult.sceneMaxX = -AABB_INF_BOUND;
    g_geometryResult.sceneMaxY = -AABB_INF_BOUND;
    alignas(16) MultiArray<float, 6> localMat;
    alignas(16) MultiArray<float, 4> aabb;
    for (u32 i = 0; i < count; ++i) {
        SceneNode& node = g_sceneNodes[i];
        if (node.styleSlot < STYLE_ARENA_CAPACITY) {
            style_compute_affine_2d(node.styleSlot, localMat.data());
        } else {
            localMat[0] = 1.0f; localMat[1] = 0.0f;
            localMat[2] = 0.0f; localMat[3] = 1.0f;
            localMat[4] = node.localX; localMat[5] = node.localY;
        }
        const Transform2D localT{localMat[0], localMat[2], localMat[1], localMat[3], localMat[4], localMat[5], 0.0f, 0.0f};
        const u16 parent = node.parentIndex;
        node.set_world_transform((parent < i) ? localT.compose(g_sceneNodes[parent].world_transform()) : localT);
        geometry_compute_aabb_simd(
            node.worldA, node.worldB, node.worldC, node.worldD, node.worldTx, node.worldTy,
            node.localX, node.localY, node.localW, node.localH,
            aabb.data()
        );
        node.aabbMinX = aabb[0];
        node.aabbMinY = aabb[1];
        node.aabbMaxX = aabb[2];
        node.aabbMaxY = aabb[3];
        const u16 clip = node.clipNode;
        if (clip < i) {
            const SceneNode& c = g_sceneNodes[clip];
            if (node.aabbMinX < c.aabbMinX) node.aabbMinX = c.aabbMinX;
            if (node.aabbMinY < c.aabbMinY) node.aabbMinY = c.aabbMinY;
            if (node.aabbMaxX > c.aabbMaxX) node.aabbMaxX = c.aabbMaxX;
            if (node.aabbMaxY > c.aabbMaxY) node.aabbMaxY = c.aabbMaxY;
            node.flags |= NODE_FLAG_HAS_CLIP;
            if (node.aabbMinX >= node.aabbMaxX || node.aabbMinY >= node.aabbMaxY) {
                node.flags |= NODE_FLAG_CULLED;
                g_geometryResult.clippedNodes++;
            }
        }
        if (viewportW > 0.0f && viewportH > 0.0f) {
            if (node.aabbMaxX < 0.0f || node.aabbMinX > viewportW ||
                node.aabbMaxY < 0.0f || node.aabbMinY > viewportH) {
                node.flags |= NODE_FLAG_CULLED;
            }
        }
        if (node.flags & NODE_FLAG_CULLED) {
            g_geometryResult.culledNodes++;
        } else {
            g_geometryResult.visibleNodes++;
            if (node.aabbMinX < g_geometryResult.sceneMinX) g_geometryResult.sceneMinX = node.aabbMinX;
            if (node.aabbMinY < g_geometryResult.sceneMinY) g_geometryResult.sceneMinY = node.aabbMinY;
            if (node.aabbMaxX > g_geometryResult.sceneMaxX) g_geometryResult.sceneMaxX = node.aabbMaxX;
            if (node.aabbMaxY > g_geometryResult.sceneMaxY) g_geometryResult.sceneMaxY = node.aabbMaxY;
        }
        node.flags &= ~NODE_FLAG_DIRTY;
    }
    if (g_geometryResult.visibleNodes == 0u) {
        g_geometryResult.sceneMinX = 0.0f;
        g_geometryResult.sceneMinY = 0.0f;
        g_geometryResult.sceneMaxX = 0.0f;
        g_geometryResult.sceneMaxY = 0.0f;
    }
    return &g_geometryResult;
}
} // namespace stockcut
