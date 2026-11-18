// ============================================================================
// native/render/shaders/halo_blur.wgsl
// WEBGPU COMPUTE SHADER: SEPARABLE FILTER CÓ VÙNG ĐỆM HALO (WORKGROUP SHARED MEMORY)
// ----------------------------------------------------------------------------
// [GPU-DRIVEN TRỤ CỘT 5 - EFFECT LẤY MẪU LÂN CẬN CÓ ĐỆM & BẢO TOÀN ĐỘ PHÂN GIẢI]:
// 1. Phù hợp tuyệt đối với GEMINI.md Rule 8: 100% Visual Fidelity, KHÔNG downscale
//    texture hay hạ độ phân giải để lấy tốc độ.
// 2. Sử dụng Workgroup Shared Memory với vùng đệm Halo bán kính R = 16 pixels.
// 3. Toàn bộ các pixel lân cận trong bán kính R được nạp 1 lần duy nhất vào LDS.
// 4. Tính toán Separable 1D Convolutions (Horizontal pass rồi Vertical pass)
//    hoàn toàn trong Shared Memory với băng thông tối đa của phần cứng.
// ============================================================================

struct BlurUniforms {
    stageW: f32,
    stageH: f32,
    radius: i32,
    direction: i32, // 0 = Horizontal, 1 = Vertical
};

@group(0) @binding(0) var<uniform> uniforms: BlurUniforms;
@group(0) @binding(1) var inTexture: texture_2d<f32>;
@group(0) @binding(2) var outTexture: texture_storage_2d<rgba8unorm, write>;

const TILE_DIM: u32 = 128u;
const RADIUS: i32 = 16;
const SHARED_SIZE: u32 = TILE_DIM + u32(RADIUS * 2);

var<workgroup> s_cache: array<vec4<f32>, SHARED_SIZE>;

// Trọng số Gaussian chuẩn mực 1D (R = 16)
fn get_gaussian_weight(offset: i32) -> f32 {
    let sigma: f32 = f32(uniforms.radius) * 0.5;
    let dist: f32 = f32(offset);
    return exp(-0.5 * (dist * dist) / (sigma * sigma));
}

@compute @workgroup_size(128, 1, 1)
fn main(
    @builtin(workgroup_id) workgroup_id: vec3<u32>,
    @builtin(local_invocation_id) local_id: vec3<u32>
) {
    let tid = local_id.x;
    let lineIdx = workgroup_id.y;
    let baseOffset = workgroup_id.x * TILE_DIM;

    // 1. Nạp vùng đệm trung tâm
    let centerCoord = i32(baseOffset + tid);
    if (uniforms.direction == 0) {
        // Horizontal: nạp pixel (x, y)
        let clampedX = clamp(centerCoord, 0, i32(uniforms.stageW) - 1);
        s_cache[tid + u32(RADIUS)] = textureLoad(inTexture, vec2<i32>(clampedX, i32(lineIdx)), 0);
    } else {
        // Vertical: nạp pixel (y, x)
        let clampedY = clamp(centerCoord, 0, i32(uniforms.stageH) - 1);
        s_cache[tid + u32(RADIUS)] = textureLoad(inTexture, vec2<i32>(i32(lineIdx), clampedY), 0);
    }

    // 2. Nạp vùng Halo trái/trên (Radius pixels)
    if (tid < u32(RADIUS)) {
        let leftCoord = i32(baseOffset) - RADIUS + i32(tid);
        if (uniforms.direction == 0) {
            let clampedX = clamp(leftCoord, 0, i32(uniforms.stageW) - 1);
            s_cache[tid] = textureLoad(inTexture, vec2<i32>(clampedX, i32(lineIdx)), 0);
        } else {
            let clampedY = clamp(leftCoord, 0, i32(uniforms.stageH) - 1);
            s_cache[tid] = textureLoad(inTexture, vec2<i32>(i32(lineIdx), clampedY), 0);
        }
    }

    // 3. Nạp vùng Halo phải/dưới (Radius pixels)
    if (tid >= TILE_DIM - u32(RADIUS)) {
        let rightCoord = i32(baseOffset + TILE_DIM) + (i32(tid) - (i32(TILE_DIM) - RADIUS));
        let destIdx = tid + u32(RADIUS * 2);
        if (uniforms.direction == 0) {
            let clampedX = clamp(rightCoord, 0, i32(uniforms.stageW) - 1);
            s_cache[destIdx] = textureLoad(inTexture, vec2<i32>(clampedX, i32(lineIdx)), 0);
        } else {
            let clampedY = clamp(rightCoord, 0, i32(uniforms.stageH) - 1);
            s_cache[destIdx] = textureLoad(inTexture, vec2<i32>(i32(lineIdx), clampedY), 0);
        }
    }

    workgroupBarrier();

    // 4. Tính toán tích chập Gaussian 1D Separable hoàn toàn trong Shared Memory
    var result = vec4<f32>(0.0);
    var weightSum = 0.0;
    let r = min(RADIUS, uniforms.radius);

    for (var k = -r; k <= r; k = k + 1) {
        let w = get_gaussian_weight(k);
        let sampleVal = s_cache[i32(tid) + RADIUS + k];
        result = result + sampleVal * w;
        weightSum = weightSum + w;
    }

    result = result / max(0.0001, weightSum);

    // 5. Ghi kết quả ra texture đích mà không làm giảm độ phân giải
    if (uniforms.direction == 0) {
        if (centerCoord < i32(uniforms.stageW) && i32(lineIdx) < i32(uniforms.stageH)) {
            textureStore(outTexture, vec2<i32>(centerCoord, i32(lineIdx)), result);
        }
    } else {
        if (i32(lineIdx) < i32(uniforms.stageW) && centerCoord < i32(uniforms.stageH)) {
            textureStore(outTexture, vec2<i32>(i32(lineIdx), centerCoord), result);
        }
    }
}
