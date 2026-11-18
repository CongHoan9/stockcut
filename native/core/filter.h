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
// CoreEngine - Shader Pipeline & CSS Filter/Blend Uniforms (Pha 4)
// ----------------------------------------------------------------------------
// [KỸ THUẬT STOCKFISH ÁP DỤNG]:
// 1. Packed 64-byte FilterUniforms Struct (Exactly 1 CPU Cacheline / GPU Uniform Block)
//    - Kiểu bài toán: Đóng gói toàn bộ tham số Blur, Shadow, Blend Mode và Matrix
//                     vào 1 struct 64 bytes nạp thẳng vào UBO (Uniform Buffer Object)
//    - Tham chiếu Stockfish: Stockfish/src/types.h, Stockfish/src/tt.h (alignas(64))
// 2. Precomputed Affine 4x5 Color Matrix via SIMD128
//    - Kiểu bài toán: Tích hợp contrast, brightness, saturate, hue-rotate, invert, sepia
//                     thành đúng 1 ma trận duy nhất trong C++, triệt tiêu tính toán nặng ở fragment shader
//    - Tham chiếu Stockfish: Stockfish/src/nnue/nnue_common.h (affine accumulator, matrix transforms)
// 3. Multi-Pass Dual Kawase Iteration Planning
//    - Kiểu bài toán: Lập lịch các pass downsample/upsample phân cấp tối ưu theo bán kính blur
//    - Tham chiếu Stockfish: Stockfish/src/movepick.cpp (step-by-step reduction planning)
// Tra cứu thêm tại: native/phan_loai_nghiep_vu_stockfish.txt (Mục 1, Mục 6 & Mục 9)
// ============================================================================
#include "types.h"
#include "math_utils.h"
namespace stockcut {
// Hằng số các chế độ hòa trộn CSS Blend Modes
constexpr u32 BLEND_NORMAL       = 0;
constexpr u32 BLEND_MULTIPLY     = 1;
constexpr u32 BLEND_SCREEN       = 2;
constexpr u32 BLEND_OVERLAY      = 3;
constexpr u32 BLEND_DARKEN       = 4;
constexpr u32 BLEND_LIGHTEN      = 5;
constexpr u32 BLEND_COLOR_DODGE  = 6;
constexpr u32 BLEND_COLOR_BURN   = 7;
constexpr u32 BLEND_HARD_LIGHT   = 8;
constexpr u32 BLEND_SOFT_LIGHT   = 9;
constexpr u32 BLEND_DIFFERENCE   = 10;
constexpr u32 BLEND_EXCLUSION    = 11;
// Bitflags đánh dấu các filter đang hoạt động
constexpr u32 FILTER_BIT_BLUR         = 1 << 0;
constexpr u32 FILTER_BIT_SHADOW       = 1 << 1;
constexpr u32 FILTER_BIT_BRIGHTNESS   = 1 << 2;
constexpr u32 FILTER_BIT_CONTRAST     = 1 << 3;
constexpr u32 FILTER_BIT_SATURATE     = 1 << 4;
constexpr u32 FILTER_BIT_HUE_ROTATE   = 1 << 5;
constexpr u32 FILTER_BIT_INVERT       = 1 << 6;
constexpr u32 FILTER_BIT_SEPIA        = 1 << 7;
constexpr u32 FILTER_BIT_BLEND_MODE   = 1 << 8;
// ----------------------------------------------------------------------------
constexpr u32 MAX_FILTER_SLOTS = 256;
constexpr u32 MAX_KAWASE_PASSES = 8;

// ----------------------------------------------------------------------------
// Struct FilterUniforms (64 bytes = đúng 1 L1 Cacheline)
// ----------------------------------------------------------------------------
struct alignas(64) FilterUniforms {
    float blurRadius;        // 0:  Bán kính blur (px)
    float shadowOffsetX;     // 4:  Độ dời bóng X (px)
    float shadowOffsetY;     // 8:  Độ dời bóng Y (px)
    float shadowBlur;        // 12: Độ nhòe bóng đổ (px)
    
    u32   shadowColor;       // 16: Màu bóng đổ RGBA (0xRRGGBBAA)
    u32   blendMode;         // 20: Chế độ hòa trộn CSS (0-11)
    float brightness;        // 24: Độ sáng (1.0 = chuẩn)
    float contrast;          // 28: Độ tương phản (1.0 = chuẩn)
    
    float saturate;          // 32: Độ bão hòa (1.0 = chuẩn)
    float hueRotateDeg;      // 36: Góc xoay màu sắc (độ: 0-360)
    float invert;            // 40: Nghịch đảo màu (0.0 - 1.0)
    float sepia;             // 44: Hiệu ứng ảnh cổ điển (0.0 - 1.0)
    
    float opacity;           // 48: Độ mờ đục tổng thể (0.0 - 1.0)
    float shadowSpread;      // 52: Độ lan tỏa bóng đổ (px)
    u32   activeFilterBits;  // 56: Bitflags các filter đang áp dụng
    u32   pad;               // 60: Đệm tròn đúng 64 bytes

    void reset() noexcept;
    void set_blur(float radius) noexcept;
    void set_shadow(float ox, float oy, float blur, float spread, u32 colorRgba) noexcept;
    void set_blend_mode(u32 mode) noexcept;
    void set_color_adjustments(float bright, float contrast, float sat, float hueDeg, float inv, float sep) noexcept;
    void compute_color_matrix_64b(ColorMatrix64B& outMat) const noexcept;
    void compute_color_matrix(float* out16Floats) const noexcept;
    [[nodiscard]] u64 compute_hash() const noexcept;
};
static_assert(sizeof(FilterUniforms) == 64, "FilterUniforms must be 64 bytes");
static_assert(alignof(FilterUniforms) == 64, "FilterUniforms must be 64-byte aligned");

// Cấu trúc mô tả từng pass trong Dual Kawase Blur (16 bytes)
struct alignas(16) KawasePassInfo {
    float offset;        // 4 bytes: Khoảng lấy mẫu (uv offset multiplier)
    u32   targetWidth;   // 4 bytes: Chiều rộng FBO mục tiêu
    u32   targetHeight;  // 4 bytes: Chiều cao FBO mục tiêu
    u32   isDownsample;  // 4 bytes: 1 = Downsample (4-tap bilinear), 0 = Upsample (8-tap tent)
};
static_assert(sizeof(KawasePassInfo) == 16, "KawasePassInfo must be 16 bytes");

// C++ API
FilterUniforms* filter_get_arena();
KawasePassInfo* filter_get_kawase_passes_buf();

// Stockfish Boundary Gateway: Kẹp biên toán học không rẽ nhánh qua Bitmask 2^N
[[nodiscard]] FilterUniforms& filter_slot(u32 idx) noexcept;

void filter_reset(u32 idx);
void filter_set_blur(u32 idx, float radius);
void filter_set_shadow(u32 idx, float ox, float oy, float blur, float spread, u32 colorRgba);
void filter_set_blend_mode(u32 idx, u32 blendMode);
void filter_set_color_adjustments(u32 idx, float bright, float contrast, float sat, float hueDeg, float inv, float sep);
void filter_compute_color_matrix(u32 idx, float* out16Floats);
void filter_compute_color_matrix_64b(u32 idx, ColorMatrix64B& outMat);
u32  filter_compute_kawase_passes(float blurRadius, u32 srcW, u32 srcH, KawasePassInfo* outPasses, u32 maxPasses);
u32  filter_compute_kawase_passes_static(float blurRadius, u32 srcW, u32 srcH);
u64  filter_hash(u32 idx);
} // namespace stockcut
