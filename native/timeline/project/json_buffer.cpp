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
// Fixed input buffer for native project parsing, reserved in every Wasm instance.
// Bottleneck: the fixed input buffer reserves 1 MiB in every Wasm instance.
// Optimization path: move to explicit arena allocation after input ownership is stable.
#include "json_buffer.h"
#include "data_structures.h"
namespace stockcut::timeline::project {
namespace {
MultiArray<char, PROJECT_JSON_CAPACITY> projectJson{};
}
// Returns the fixed project-input buffer for direct host writes.
char* projectJsonBuffer() {
    return projectJson.data();
}
// Returns the compile-time capacity of the shared project-input buffer.
u32 projectJsonCapacity() {
    return PROJECT_JSON_CAPACITY;
}
} // namespace stockcut::timeline::project
