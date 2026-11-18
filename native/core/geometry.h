// ============================================================================
// [ĐÁNH GIÁ ĐIỂM NGHẼN HỆ THỐNG]:
// 1. Phân mảnh dữ liệu bộ nhớ và truy cập L1 Cache rời rạc:
//    - Cấu trúc đối tượng rời rạc và việc kiểm tra phòng thủ lặp lại trong Hot Path 60 FPS
//      làm tăng tỉ lệ trượt L1 Cache và gây lãng phí chu kỳ vi xử lý.
// 2. Chi phí cấp phát heap động (Heap Thrashing):
//    - Cấp phát heap trong vòng lặp chính làm phát sinh áp lực dọn rác và phân mảnh RAM.
//
// [HƯỚNG KHẮC PHỤC TRIỆT ĐỂ — KHÔNG VÁ]:
// 1. Kiến trúc mảng phẳng DOD (Data-Oriented Design) & Căn lề Cacheline:
//    - Căn lề dữ liệu 64-byte (alignas(64)), nạp dữ liệu liên tục theo đường truyền SIMD.
// 2. Straight-line Execution & Zero-Allocation:
//    - Loại bỏ kiểm tra hợp lệ phòng thủ trong Hot Path; 0 malloc/free ở tần số 60 FPS.
// ============================================================================
#pragma once
// ============================================================================
// CoreEngine - Scene Graph Hierarchy & SIMD128 Geometry Engine (Pha 3)
// ----------------------------------------------------------------------------
// [KỸ THUẬT STOCKFISH ÁP DỤNG]:
// 1. Zero-Pointer Flat Array Traversal (Topological Sort 0 -> N-1)
//    - Kiểu bài toán: Khử hoàn toàn con trỏ cây cảnh quan, lặp phẳng mảng SceneNode 64B
//    - Tham chiếu Stockfish: Stockfish/src/search.cpp (Stack ss[MAX_PLY] linear array)
// 2. SIMD128 Vectorized 4-Corner Affine AABB Projection
//    - Kiểu bài toán: Chiếu 4 góc hình chữ nhật và tìm min/max bằng wasm_f32x4_pmin/pmax
//    - Tham chiếu Stockfish: Stockfish/src/nnue/nnue_common.h (SIMD vectorization)
// 3. Viewport Frustum & Scissor Culling
//    - Kiểu bài toán: Cắt tỉa node ngoài màn hình và kẹp khung clip box tức thì
//    - Tham chiếu Stockfish: Stockfish/src/movepick.cpp (Pruning & bounding)
// Tra cứu thêm tại: native/phan_loai_nghiep_vu_stockfish.txt (Mục 5 & Mục 6)
// ============================================================================
#include "types.h"
#include "data_structures.h"
#include "../effect/monoid_operator.h"
namespace stockcut {
// Cờ trạng thái cho SceneNode
constexpr u16 NODE_FLAG_VISIBLE   = 1 << 0; // Node hiển thị
constexpr u16 NODE_FLAG_DIRTY     = 1 << 1; // Transform cần tính lại
constexpr u16 NODE_FLAG_CULLED    = 1 << 2; // Bị cắt tỉa (nằm ngoài viewport hoặc clip)
constexpr u16 NODE_FLAG_HAS_CLIP  = 1 << 3; // Có parent clipping box
constexpr u16 NODE_FLAG_OPAQUE    = 1 << 4; // Node đặc hoàn toàn (dùng cho occlusion)
// ----------------------------------------------------------------------------
// Cấu trúc SceneNode (64 bytes = đúng 1 L1 Cacheline)
// Biểu diễn một phần tử trong cây render phân cấp phẳng không đụng con trỏ
// ----------------------------------------------------------------------------
struct alignas(64) SceneNode {
    // 1. Ma trận thế giới 2D Affine World Transform [a, b, c, d, tx, ty] - 24 bytes
    float worldA;      // 0
    float worldB;      // 4
    float worldC;      // 8
    float worldD;      // 12
    float worldTx;     // 16
    float worldTy;     // 20
    
    // 2. Tọa độ bao quanh AABB thế giới [minX, minY, maxX, maxY] - 16 bytes
    float aabbMinX;    // 24
    float aabbMinY;    // 28
    float aabbMaxX;    // 32
    float aabbMaxY;    // 36
    
    // 3. Khung bao cục bộ Local Box [x, y, w, h] - 16 bytes
    float localX;      // 40
    float localY;      // 44
    float localW;      // 48
    float localH;      // 52
    
    // 4. Chỉ số phân cấp và cờ điều khiển - 8 bytes
    u16   parentIndex; // 56: Chỉ số node cha (0xFFFF = Root)
    u16   styleSlot;   // 58: Chỉ số slot PackedStyle (0-255)
    u16   clipNode;    // 60: Node cha làm clip box (0xFFFF = None)
    u16   flags;       // 62: Bitflags điều khiển

    void reset(u16 slot) noexcept;
    void set_local(float x, float y, float w, float h, u16 parentIdx, u16 sSlot, u16 clip, u16 fl) noexcept;

    [[nodiscard]] [[gnu::always_inline]] inline Transform2D world_transform() const noexcept {
        return Transform2D{worldA, worldC, worldB, worldD, worldTx, worldTy, 0.0f, 0.0f};
    }
    [[gnu::always_inline]] inline void set_world_transform(const Transform2D& t) noexcept {
        worldA  = t.m00;
        worldC  = t.m01;
        worldB  = t.m10;
        worldD  = t.m11;
        worldTx = t.tx;
        worldTy = t.ty;
    }
};
static_assert(sizeof(SceneNode) == 64, "SceneNode must be exactly 64 bytes (1 Cacheline)");
// ----------------------------------------------------------------------------
// Kết quả thống kê tính toán Geometry & Culling (64 bytes = đúng 1 Cacheline)
// ----------------------------------------------------------------------------
struct alignas(64) GeometryResult {
    u32   totalNodes;      // Tổng số node trong scene
    u32   visibleNodes;    // Số node hiển thị thực tế
    u32   culledNodes;     // Số node bị cắt tỉa ngoài màn hình
    u32   clippedNodes;    // Số node bị scissor/clip box cắt
    float sceneMinX;       // Bounding box toàn cảnh
    float sceneMinY;
    float sceneMaxX;
    float sceneMaxY;
    MultiArray<u32, 8> reserved; // Đệm tròn 64 bytes
};
static_assert(sizeof(GeometryResult) == 64, "GeometryResult must be exactly 64 bytes");
constexpr u32 MAX_SCENE_NODES = 256;
// C++ API
SceneNode*      geometry_get_nodes_ptr();
GeometryResult* geometry_get_result_ptr();

// Stockfish Boundary Gateway: Kẹp biên toán học không rẽ nhánh qua Bitmask 2^N
[[nodiscard]] SceneNode& scene_node(u32 idx) noexcept;

void geometry_reset_node(u32 idx);
void geometry_set_node_local(u32 idx, float x, float y, float w, float h, u16 parentIdx, u16 styleSlot, u16 clipNode, u16 flags);
// Hàm chính: Tính toán toàn bộ cây phân cấp ma trận và AABB bằng SIMD128
GeometryResult* geometry_evaluate_scene(u32 nodeCount, float viewportW, float viewportH);
// Phép chiếu 4 đỉnh box qua ma trận Affine và tính AABB cực nhanh bằng SIMD128
void geometry_compute_aabb_simd(
    float a, float b, float c, float d, float tx, float ty,
    float lx, float ly, float lw, float lh,
    float* outAABB4Floats
);
} // namespace stockcut
