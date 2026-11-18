// ============================================================================
// [ĐÁNH GIÁ ĐIỂM NGHẼN HỆ THỐNG]:
// 1. Duyệt cây phân cấp đệ quy trong Hot Path 60 FPS:
//    - Duyệt qua lại các node timeline làm phát sinh cache miss và chi phí con trỏ.
// 2. Kế thừa template lặp lại ở mỗi khung hình:
//    - Hỏi quan hệ cha-con liên tục làm giảm hiệu suất xử lý khung hình.
//
// [HƯỚNG KHẮC PHỤC TRIỆT ĐỂ — KHÔNG VÁ]:
// 1. Kế thừa 1 lần duy nhất ở Cold Path & Bộ nhớ chia sẻ:
//    - Con trỏ trực tiếp vào bộ nhớ phẳng của cha, đọc O(1) không duyệt cây.
// 2. Bitboard Layer Masking & Ngăn xếp lịch sử StateInfo:
//    - Lọc trạng thái layer bằng bitmask 64-bit và lưu vết vi sai kiểu Stockfish.
// ============================================================================
#pragma once
// Bottleneck: each project item requires repeated JSON field scans.
// Optimization path: reuse tokenized spans and specialize per-media accumulation
// through byType dispatch rather than branching inside the hot accumulator.
#include "display_list.h"
namespace stockcut::timeline::project {
constexpr u32 PROJECT_DURATION_HAS_PERCENT_TIMES = 1u << 0;
struct DurationResult {
    double durationMs;
    u32 flags;
    u32 reserved;
};
// Computes project duration from a non-owning JSON span, returning -1 for invalid input.
DurationResult computeProjectDuration(const char* json, u32 length);
} // namespace stockcut::timeline::project
