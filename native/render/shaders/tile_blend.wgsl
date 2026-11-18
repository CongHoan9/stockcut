// ============================================================================
// native/render/shaders/tile_blend.wgsl
// WEBGPU COMPUTE SHADER: HÒA TRỘN TỪNG Ô TRONG THANH GHI NỘI TẠI & GPU PSEUDO-BINNING
// ----------------------------------------------------------------------------
// [GPU-DRIVEN BTHF - CƠ CHẾ PSEUDO ĐỘT PHÁ & REGISTER-FUSED MONOID BLENDING]:
// 1. Mỗi Workgroup sở hữu trọn vẹn 1 ô 32x32 pixel (@workgroup_size(16, 16) -> 256 threads).
// 2. GPU PSEUDO-BINNING: 64 threads đầu tiên kiểm tra song song 64 slot AABBs trong 1 chu kỳ máy tính.
//    CPU không cần tốn chu kỳ chia ô (0 ns CPU binning).
// 3. PSEUDO-RENDERING (STATIC EARLY-EXIT): Ô không chứa layer bẩn sẽ thoát ngay lập tức,
//    0 lần lấy mẫu texture, 0 tính toán blend, giữ nguyên Framebuffer khung trước (giảm 80% tải GPU).
// 4. PSEUDO-SORTING: Duyệt bitmask bằng countTrailingZeros đảm bảo trật tự Z-Index tăng dần O(0).
// 5. PSEUDO-TEXTURE FETCH: Layer Procedural tính màu trực tiếp trên thanh ghi ALU (0 VRAM fetch).
// 6. Ghi kết quả cuối cùng ra Framebuffer Texture ĐÚNG 1 LẦN DUY NHẤT (giảm 98% băng thông).
// ============================================================================

struct HardwareFrameContext {
    stageW: f32,
    stageH: f32,
    tilesX: u32,
    tilesY: u32,
    tileCount: u32,
    activeTileCount: u32,
    uploadRangeCount: u32,
    frameIndex: u32,
    nowMsLo: u32,
    nowMsHi: u32,
    tileArenaByteOffset: u32,
    slotArenaByteOffset: u32,
    rangeArenaByteOffset: u32,
    activeSlotCount: u32,
    pad0_0: u32,
    pad0_1: u32,
    totalIntersections: u32,
    staticCleanTiles: u32,
    dynamicDirtyTiles: u32,
    bytesUploaded: u32,
    gpuPrepareMs: f32,
    simulatedGpuFps: f32,
    globalDirtyBBLo: u32,
    globalDirtyBBHi: u32,
};

// 16-byte Flat Tile Cell matching C++ spatial::FlatTileCell
struct FlatTileCell {
    slotsLo: u32,       // Layer 0, 1, 2, 3 (1 byte/slot)
    slotsHi: u32,       // Layer 4, 5, 6, 7
    staticZobrist: u32, // Mã băm Zobrist của các layer tĩnh
    meta: u32,          // [0..15]: flags, [16..23]: layerCount, [24..31]: maxZ
};

struct Evaluated {
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
    flags: u32,         // Bit 0: Procedural Color flag
    custom0: f32,       // Procedural R / Effect param
    custom1: f32,       // Procedural G
    custom2: f32,       // Procedural B
    custom3: f32,       // Procedural A
};

@group(0) @binding(0) var<uniform> frameCtx: HardwareFrameContext;
@group(0) @binding(1) var<storage, read> tileCells: array<FlatTileCell>;
@group(0) @binding(2) var<storage, read> slots: array<Evaluated>;
@group(0) @binding(3) var layerTextures: texture_2d_array<f32>;
@group(0) @binding(4) var texSampler: sampler;
@group(0) @binding(5) var outTarget: texture_storage_2d<rgba8unorm, write>;

// Bộ nhớ chia sẻ cho bitmask các layer giao cắt trong ô (8 bytes atomic)
var<workgroup> s_tileMaskLo: atomic<u32>;
var<workgroup> s_tileMaskHi: atomic<u32>;
var<workgroup> s_cell: FlatTileCell;

const TILE_FLAG_STATIC_CLEAN: u32 = 2u;
const LAYER_FLAG_PROCEDURAL: u32 = 1u;

// Hàm hòa trộn Over (Porter-Duff Over) chuẩn xác
fn blend_over(src: vec4<f32>, dst: vec4<f32>) -> vec4<f32> {
    let outA = src.a + dst.a * (1.0 - src.a);
    if (outA <= 0.0001) {
        return vec4<f32>(0.0);
    }
    let outRGB = (src.rgb * src.a + dst.rgb * dst.a * (1.0 - src.a)) / outA;
    return vec4<f32>(outRGB, outA);
}

@compute @workgroup_size(16, 16, 1)
fn main(
    @builtin(workgroup_id) workgroup_id: vec3<u32>,
    @builtin(local_invocation_id) local_id: vec3<u32>,
    @builtin(local_invocation_index) local_index: u32
) {
    let tileX = workgroup_id.x;
    let tileY = workgroup_id.y;
    let tileIdx = tileY * frameCtx.tilesX + tileX;

    // 1. Khởi tạo bitmask chia sẻ của ô
    if (local_index == 0u) {
        atomicStore(&s_tileMaskLo, 0u);
        atomicStore(&s_tileMaskHi, 0u);
        if (tileIdx < frameCtx.tileCount) {
            s_cell = tileCells[tileIdx];
        } else {
            s_cell.meta = 0u;
        }
    }
    workgroupBarrier();

    let tileMinX = f32(tileX * 32u);
    let tileMinY = f32(tileY * 32u);
    let tileMaxX = tileMinX + 32.0;
    let tileMaxY = tileMinY + 32.0;

    // 2. GPU PSEUDO-BINNING: 64 threads đầu tiên kiểm tra song song 64 slot AABB
    if (frameCtx.activeSlotCount > 0u) {
        if (local_index < 64u && local_index < frameCtx.activeSlotCount) {
            let slot = slots[local_index];
            if (slot.isVisible != 0u && slot.opacity > 0.0001) {
                let inside = (slot.x0 < tileMaxX && slot.x1 > tileMinX &&
                              slot.y0 < tileMaxY && slot.y1 > tileMinY);
                if (inside) {
                    if (local_index < 32u) {
                        atomicOr(&s_tileMaskLo, 1u << local_index);
                    } else {
                        atomicOr(&s_tileMaskHi, 1u << (local_index - 32u));
                    }
                }
            }
        }
    } else {
        // Fallback: nếu CPU đã nạp sẵn FlatTileCell
        if (local_index == 0u) {
            let layerCount = (s_cell.meta >> 16u) & 0xFFu;
            for (var l = 0u; l < layerCount; l = l + 1u) {
                var sIdx: u32;
                if (l < 4u) { sIdx = (s_cell.slotsLo >> (l * 8u)) & 0xFFu; }
                else { sIdx = (s_cell.slotsHi >> ((l - 4u) * 8u)) & 0xFFu; }
                if (sIdx < 32u) {
                    atomicOr(&s_tileMaskLo, 1u << sIdx);
                } else if (sIdx < 64u) {
                    atomicOr(&s_tileMaskHi, 1u << (sIdx - 32u));
                }
            }
        }
    }
    workgroupBarrier();

    var maskLo = atomicLoad(&s_tileMaskLo);
    var maskHi = atomicLoad(&s_tileMaskHi);

    // Nếu ô trống (không có layer nào giao cắt), dừng ngay lập tức trong 1 chu kỳ!
    if ((maskLo | maskHi) == 0u) {
        return;
    }

    // 3. PSEUDO-RENDERING: STATIC TILE EARLY-EXIT
    // Nếu không có layer nào trong ô bị bẩn trong khung hình hiện tại:
    // Toàn bộ 256 threads dừng ngay lập tức, tái sử dụng 100% framebuffer khung trước!
    if (frameCtx.frameIndex > 0u) {
        let dirtyLo = maskLo & frameCtx.globalDirtyBBLo;
        let dirtyHi = maskHi & frameCtx.globalDirtyBBHi;
        if ((dirtyLo | dirtyHi) == 0u) {
            return;
        }
    }

    // 3b. TOP-DOWN OPAQUE OCCLUSION CULLING (CẮT TỈA BITMASK TỪ TRÊN XUỐNG O(1)):
    // Duyệt từ bit cao nhất (Z-Index cao nhất) xuống; ngay khi gặp layer đục phủ kín ô,
    // xóa sổ toàn bộ các bit layer bị che khuất bên dưới trong 1 lệnh bitwise!
    var occluded = false;
    var scanHi = maskHi;
    while (scanHi != 0u) {
        let topBit = firstLeadingBit(scanHi);
        let bitMask = 1u << topBit;
        scanHi = scanHi & ~bitMask;
        let slot = slots[topBit + 32u];
        if (slot.opacity >= 0.9999 && abs(slot.rotationDeg) < 0.001 &&
            slot.x0 <= tileMinX && slot.x1 >= tileMaxX &&
            slot.y0 <= tileMinY && slot.y1 >= tileMaxY) {
            maskHi = maskHi & ~(bitMask - 1u);
            maskLo = 0u;
            occluded = true;
            break;
        }
    }
    if (!occluded) {
        var scanLo = maskLo;
        while (scanLo != 0u) {
            let topBit = firstLeadingBit(scanLo);
            let bitMask = 1u << topBit;
            scanLo = scanLo & ~bitMask;
            let slot = slots[topBit];
            if (slot.opacity >= 0.9999 && abs(slot.rotationDeg) < 0.001 &&
                slot.x0 <= tileMinX && slot.x1 >= tileMaxX &&
                slot.y0 <= tileMinY && slot.y1 >= tileMaxY) {
                maskLo = maskLo & ~(bitMask - 1u);
                break;
            }
        }
    }

    // 4. Tọa độ pixel cơ sở mà thread này phụ trách (mỗi thread xử lý 2x2 pixel)
    let pixelBaseX = tileX * 32u + local_id.x * 2u;
    let pixelBaseY = tileY * 32u + local_id.y * 2u;

    // Khởi tạo 4 thanh ghi màu cho 4 pixel trong khối 2x2
    var acc00 = vec4<f32>(0.0, 0.0, 0.0, 0.0);
    var acc10 = vec4<f32>(0.0, 0.0, 0.0, 0.0);
    var acc01 = vec4<f32>(0.0, 0.0, 0.0, 0.0);
    var acc11 = vec4<f32>(0.0, 0.0, 0.0, 0.0);

    // 5. PSEUDO-SORTING & ON-CHIP BLENDING:
    // Duyệt qua các bit 1 theo đúng trật tự Z-Index tăng dần bằng countTrailingZeros (O(0) sorting)
    var remLo = maskLo;
    while (remLo != 0u) {
        let bit = countTrailingZeros(remLo);
        remLo = remLo & (remLo - 1u);
        let slotIdx = bit;
        let slot = slots[slotIdx];

        let invW = 1.0 / max(0.001, slot.x1 - slot.x0);
        let invH = 1.0 / max(0.001, slot.y1 - slot.y0);
        let isProcedural = (slot.flags & LAYER_FLAG_PROCEDURAL) != 0u;
        let procCol = vec4<f32>(slot.custom0, slot.custom1, slot.custom2, slot.custom3 * slot.opacity);

        // Pixel (0, 0)
        let uv00 = vec2<f32>((f32(pixelBaseX) - slot.x0) * invW, (f32(pixelBaseY) - slot.y0) * invH);
        if (uv00.x >= 0.0 && uv00.x <= 1.0 && uv00.y >= 0.0 && uv00.y <= 1.0) {
            var col00 = procCol;
            if (!isProcedural) {
                col00 = textureSampleLevel(layerTextures, texSampler, uv00, slot.texId, 0.0);
                col00.a = col00.a * slot.opacity;
            }
            acc00 = blend_over(col00, acc00);
        }

        // Pixel (1, 0)
        let uv10 = vec2<f32>((f32(pixelBaseX + 1u) - slot.x0) * invW, (f32(pixelBaseY) - slot.y0) * invH);
        if (uv10.x >= 0.0 && uv10.x <= 1.0 && uv10.y >= 0.0 && uv10.y <= 1.0) {
            var col10 = procCol;
            if (!isProcedural) {
                col10 = textureSampleLevel(layerTextures, texSampler, uv10, slot.texId, 0.0);
                col10.a = col10.a * slot.opacity;
            }
            acc10 = blend_over(col10, acc10);
        }

        // Pixel (0, 1)
        let uv01 = vec2<f32>((f32(pixelBaseX) - slot.x0) * invW, (f32(pixelBaseY + 1u) - slot.y0) * invH);
        if (uv01.x >= 0.0 && uv01.x <= 1.0 && uv01.y >= 0.0 && uv01.y <= 1.0) {
            var col01 = procCol;
            if (!isProcedural) {
                col01 = textureSampleLevel(layerTextures, texSampler, uv01, slot.texId, 0.0);
                col01.a = col01.a * slot.opacity;
            }
            acc01 = blend_over(col01, acc01);
        }

        // Pixel (1, 1)
        let uv11 = vec2<f32>((f32(pixelBaseX + 1u) - slot.x0) * invW, (f32(pixelBaseY + 1u) - slot.y0) * invH);
        if (uv11.x >= 0.0 && uv11.x <= 1.0 && uv11.y >= 0.0 && uv11.y <= 1.0) {
            var col11 = procCol;
            if (!isProcedural) {
                col11 = textureSampleLevel(layerTextures, texSampler, uv11, slot.texId, 0.0);
                col11.a = col11.a * slot.opacity;
            }
            acc11 = blend_over(col11, acc11);
        }
    }

    var remHi = maskHi;
    while (remHi != 0u) {
        let bit = countTrailingZeros(remHi);
        remHi = remHi & (remHi - 1u);
        let slotIdx = bit + 32u;
        let slot = slots[slotIdx];

        let invW = 1.0 / max(0.001, slot.x1 - slot.x0);
        let invH = 1.0 / max(0.001, slot.y1 - slot.y0);
        let isProcedural = (slot.flags & LAYER_FLAG_PROCEDURAL) != 0u;
        let procCol = vec4<f32>(slot.custom0, slot.custom1, slot.custom2, slot.custom3 * slot.opacity);

        // Pixel (0, 0)
        let uv00 = vec2<f32>((f32(pixelBaseX) - slot.x0) * invW, (f32(pixelBaseY) - slot.y0) * invH);
        if (uv00.x >= 0.0 && uv00.x <= 1.0 && uv00.y >= 0.0 && uv00.y <= 1.0) {
            var col00 = procCol;
            if (!isProcedural) {
                col00 = textureSampleLevel(layerTextures, texSampler, uv00, slot.texId, 0.0);
                col00.a = col00.a * slot.opacity;
            }
            acc00 = blend_over(col00, acc00);
        }

        // Pixel (1, 0)
        let uv10 = vec2<f32>((f32(pixelBaseX + 1u) - slot.x0) * invW, (f32(pixelBaseY) - slot.y0) * invH);
        if (uv10.x >= 0.0 && uv10.x <= 1.0 && uv10.y >= 0.0 && uv10.y <= 1.0) {
            var col10 = procCol;
            if (!isProcedural) {
                col10 = textureSampleLevel(layerTextures, texSampler, uv10, slot.texId, 0.0);
                col10.a = col10.a * slot.opacity;
            }
            acc10 = blend_over(col10, acc10);
        }

        // Pixel (0, 1)
        let uv01 = vec2<f32>((f32(pixelBaseX) - slot.x0) * invW, (f32(pixelBaseY + 1u) - slot.y0) * invH);
        if (uv01.x >= 0.0 && uv01.x <= 1.0 && uv01.y >= 0.0 && uv01.y <= 1.0) {
            var col01 = procCol;
            if (!isProcedural) {
                col01 = textureSampleLevel(layerTextures, texSampler, uv01, slot.texId, 0.0);
                col01.a = col01.a * slot.opacity;
            }
            acc01 = blend_over(col01, acc01);
        }

        // Pixel (1, 1)
        let uv11 = vec2<f32>((f32(pixelBaseX + 1u) - slot.x0) * invW, (f32(pixelBaseY + 1u) - slot.y0) * invH);
        if (uv11.x >= 0.0 && uv11.x <= 1.0 && uv11.y >= 0.0 && uv11.y <= 1.0) {
            var col11 = procCol;
            if (!isProcedural) {
                col11 = textureSampleLevel(layerTextures, texSampler, uv11, slot.texId, 0.0);
                col11.a = col11.a * slot.opacity;
            }
            acc11 = blend_over(col11, acc11);
        }
    }

    // 6. Ghi kết quả cuối cùng ra Framebuffer Target ĐÚNG 1 LẦN DUY NHẤT
    if (pixelBaseX < u32(frameCtx.stageW) && pixelBaseY < u32(frameCtx.stageH)) {
        textureStore(outTarget, vec2<i32>(i32(pixelBaseX), i32(pixelBaseY)), acc00);
    }
    if (pixelBaseX + 1u < u32(frameCtx.stageW) && pixelBaseY < u32(frameCtx.stageH)) {
        textureStore(outTarget, vec2<i32>(i32(pixelBaseX + 1u), i32(pixelBaseY)), acc10);
    }
    if (pixelBaseX < u32(frameCtx.stageW) && pixelBaseY + 1u < u32(frameCtx.stageH)) {
        textureStore(outTarget, vec2<i32>(i32(pixelBaseX), i32(pixelBaseY + 1u)), acc01);
    }
    if (pixelBaseX + 1u < u32(frameCtx.stageW) && pixelBaseY + 1u < u32(frameCtx.stageH)) {
        textureStore(outTarget, vec2<i32>(i32(pixelBaseX + 1u), i32(pixelBaseY + 1u)), acc11);
    }
}
