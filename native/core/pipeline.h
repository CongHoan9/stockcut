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
// CoreEngine - Frame Pipeline Orchestrator
// ----------------------------------------------------------------------------
// [KỸ THUẬT STOCKFISH ÁP DỤNG]:
// 1. Multi-Stage Pipeline Cutoff (Circuit Breaker -> TT Probe -> Occlusion Culling)
//    - Kiểu bài toán: Quyết định vẽ lại hay bỏ qua toàn bộ khung hình trong O(1)
//    - Tham chiếu Stockfish: Stockfish/src/search.cpp (Search root node, TT probe, move cutoff)
// 2. Packed 32-byte PipelineResult
//    - Kiểu bài toán: Trả về kết quả đánh giá khung hình trong 1 khối 32 bytes
//    - Tham chiếu Stockfish: Stockfish/src/types.h
// Tra cứu thêm tại: native/phan_loai_nghiep_vu_stockfish.txt (Mục 2 & Mục 3)
// ============================================================================
#include "types.h"
namespace stockcut {
constexpr u32 PIPELINE_DECISION_DIRTY        = 0u;
constexpr u32 PIPELINE_DECISION_ZOBRIST_SKIP = 2u;
constexpr u32 PIPELINE_DECISION_TT_HIT       = 3u;

struct alignas(16) PipelineResult {
    u64 currentHash;
    u32 decision;
    u32 ttData;
    u32 ttTier;
    u32 culledCount;
    u64 activeMask;
};
static_assert(sizeof(PipelineResult) == 32, "PipelineResult must be 32 bytes");
static_assert(__builtin_offsetof(PipelineResult, activeMask) == 24,
              "PipelineResult activeMask ABI offset must remain 24 bytes");
PipelineResult* pipeline_get_result_ptr();

template <bool IsPlaying>
PipelineResult* pipeline_evaluate_T(
    u32 W, u32 H,
    u32 styleEpoch, u32 fontsVer, u32 styleVer, u32 mediaVer, u32 bgHash,
    u32 count, u64 lastFrameHash, u64 occludedMask,
    double nowMs, u32 frameIndex
);

template <bool IsPlaying>
PipelineResult* pipeline_evaluate_position_T(
    u32 W, u32 H,
    u32 styleEpoch, u32 fontsVer, u32 styleVer, u32 mediaVer, u32 bgHash,
    u64 lastFrameHash, u64 occludedMask,
    double nowMs, u32 frameIndex
);
} // namespace stockcut
