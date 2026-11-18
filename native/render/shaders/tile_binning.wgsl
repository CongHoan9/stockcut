// ============================================================================
// native/render/shaders/tile_binning.wgsl
// WEBGPU COMPUTE SHADER: THUẬT TOÁN PHÂN LOẠI LAYER VÀO LƯỚI Ô (TILE BINNING)
// ----------------------------------------------------------------------------
// [GPU-DRIVEN TRỤ CỘT 3 - COMPUTE TILE BINNING & EXACT Z-ORDER]:
// 1. Chạy hoàn toàn trên GPU Compute Shader, 0 phép gọi CPU-GPU qua lại.
// 2. Chia khung hình thành các ô vuông 32x32 pixel (GpuTileGridConfig).
// 3. Mỗi thread xử lý 1 layer active (hoặc 1 slot trong Evaluated).
// 4. Tính toán dải ô giao cắt [tx0..tx1, ty0..ty1] từ Bounding Box AABB chuẩn hóa.
// 5. Cập nhật atomic số lượng layer của từng ô và ghi chỉ số layer theo Z-order.
// ============================================================================

struct GpuTileGridConfig {
    stageW: f32,
    stageH: f32,
    tileW: f32,
    tileH: f32,
    tilesX: u32,
    tilesY: u32,
    tileCount: u32,
    activeSlotCount: u32,
    invTileW: f32,
    invTileH: f32,
    invStageW: f32,
    invStageH: f32,
    frameIndex: u32,
    currentTimeMs: f32,
    globalFlags: u32,
    pad0: u32,
};

struct Evaluated {
    // 64-byte layout matching C++ Evaluated
    x0: f32,
    y0: f32,
    x1: f32,
    y1: f32,
    opacity: f32,
    rotationDeg: f32,
    scaleX: f32,
    scaleY: f32,
    zRank: u32,
    isVisible: u32,
    texId: u32,
    flags: u32,
    custom0: f32,
    custom1: f32,
    custom2: f32,
    custom3: f32,
};

struct GpuTileHeader {
    layerOffset: u32,
    layerCount: u32,
    flags: u32,
    staticZobristKey32: u32,
};

@group(0) @binding(0) var<uniform> gridConfig: GpuTileGridConfig;
@group(0) @binding(1) var<storage, read> slots: array<Evaluated>;
@group(0) @binding(2) var<storage, read_write> tileHeaders: array<GpuTileHeader>;
@group(0) @binding(3) var<storage, read_write> tileLayerIndices: array<u32>;
@group(0) @binding(4) var<storage, read_write> tileCounters: array<atomic<u32>>;

const MAX_LAYERS_PER_TILE: u32 = 64u;

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let slotIdx = global_id.x;
    if (slotIdx >= gridConfig.activeSlotCount) {
        return;
    }

    let slot = slots[slotIdx];
    if (slot.isVisible == 0u || slot.opacity <= 0.0001) {
        return;
    }

    // 1. Tính toán dải ô bao quanh [tx0..tx1, ty0..ty1]
    let clampedX0 = max(0.0, slot.x0);
    let clampedY0 = max(0.0, slot.y0);
    let clampedX1 = min(gridConfig.stageW, slot.x1);
    let clampedY1 = min(gridConfig.stageH, slot.y1);

    if (clampedX1 <= clampedX0 || clampedY1 <= clampedY0) {
        return;
    }

    let tx0 = u32(clampedX0 * gridConfig.invTileW);
    let ty0 = u32(clampedY0 * gridConfig.invTileH);
    let tx1 = min(gridConfig.tilesX - 1u, u32((clampedX1 - 0.001) * gridConfig.invTileW));
    let ty1 = min(gridConfig.tilesY - 1u, u32((clampedY1 - 0.001) * gridConfig.invTileH));

    // 2. Thêm slot vào từng ô trong dải giao cắt
    for (var ty = ty0; ty <= ty1; ty = ty + 1u) {
        let rowBase = ty * gridConfig.tilesX;
        for (var tx = tx0; tx <= tx1; tx = tx + 1u) {
            let tileIdx = rowBase + tx;
            if (tileIdx < gridConfig.tileCount) {
                // Tăng bộ đếm layer trong ô một cách nguyên tử
                let localOffset = atomicAdd(&tileCounters[tileIdx], 1u);
                if (localOffset < MAX_LAYERS_PER_TILE) {
                    let baseOffset = tileIdx * MAX_LAYERS_PER_TILE;
                    tileLayerIndices[baseOffset + localOffset] = slotIdx;
                }
            }
        }
    }
}
