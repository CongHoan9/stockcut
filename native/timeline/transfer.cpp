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
#include "transfer.h"
#include "renderer.h"
namespace stockcut {
namespace {
inline void upload_gpu_transfer_cmd(const TransferCmd& c, GLuint& boundVbo) noexcept {
    if (c.vboId == 0 || c.size == 0u || !c.dataPtr) return;
    if (c.vboId != boundVbo) {
        boundVbo = c.vboId;
        glBindBuffer(GL_ARRAY_BUFFER, boundVbo);
    }
    glBufferSubData(GL_ARRAY_BUFFER, c.offset, c.size, c.dataPtr);
}
} // namespace

// Cấp phát tĩnh vùng nhớ thực sự tại đây (Zero-Allocation)
TransferQueue g_gpuQueue;
void flush_gpu_transfer_queue() noexcept {
    const u32 totalCmds = g_gpuQueue.count;
    if (!Render::has_context() || totalCmds == 0u) {
        g_gpuQueue.clear();
        return;
    }
    GLuint boundVbo = 0;
    for (const TransferCmd* p = g_gpuQueue.commands.data(), *const end = p + totalCmds; p < end; ++p) {
        upload_gpu_transfer_cmd(*p, boundVbo);
    }
    g_gpuQueue.clear();
}
} // namespace stockcut
