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
#include "duration.h"
namespace stockcut::timeline::project {
enum ProjectCompileStatus : u32 {
    PROJECT_COMPILE_OK = 0,
    PROJECT_COMPILE_INVALID_JSON = 1,
    PROJECT_COMPILE_MISSING_TIMELINE = 2,
    PROJECT_COMPILE_INVALID_TIMELINE = 3,
    PROJECT_COMPILE_SCENE_CAPACITY_EXCEEDED = 4,
    PROJECT_COMPILE_OUTPUT_CAPACITY_EXCEEDED = 5,
    PROJECT_COMPILE_TEMPLATE_CAPACITY_EXCEEDED = 6,
    PROJECT_COMPILE_FIELD_CAPACITY_EXCEEDED = 7,
    PROJECT_COMPILE_INVALID_TIMELINE_TIME = 8,
    PROJECT_COMPILE_INPUT_CAPACITY_EXCEEDED = 9
};
constexpr u32 PROJECT_COMPILE_MAX_SCENE_LEAVES = 256;
constexpr u32 PROJECT_COMPILE_MAX_TEMPLATES = 128;
constexpr u32 PROJECT_COMPILE_OUTPUT_CAPACITY = 4u * 1024u * 1024u;
struct CompileResult {
    DurationResult duration;
    u32 status;
    u32 topLevelItemCount;
    u32 sceneLeafCount;
    u32 templateCount;
};
struct CompileOptions {
    double stageWidth;
    double stageHeight;
};
struct CompiledSceneNode {
    u32 sourceStart;
    u32 sourceEnd;
    u32 topLevelIndex;
    u32 sequence;
    u16 depth;
    u16 reserved;
};
static_assert(sizeof(CompileResult) == 32);
static_assert(sizeof(CompiledSceneNode) == 20);
// Compiles project input metadata through the native project entry point.
CompileResult compileProject(
    const char* json,
    u32 length,
    const CompileOptions& options = {DEFAULT_STAGE_WIDTH, DEFAULT_STAGE_HEIGHT}
);
const CompiledSceneNode* compiledSceneNodes();
const char* compiledItemsJson();
u32 compiledItemsJsonLength();
void configureCompiledAudioTracks(double projectDurationMs);
struct IncrementalPatchReport {
    u64 changedSlotsBB   = 0ULL;
    u64 removedSlotsBB   = 0ULL;
    u64 textShapeDirtyBB = 0ULL;
    u32 skippedSlotCount = 0u;
    u32 updatedSlotCount = 0u;
    u8  fullRebuild      = 0u;
    u8  pad[3]           = {};
};

void resetCompiledMerkleTable() noexcept;
void configureCompiledTimelinePosition(double stageWidth, double stageHeight);
IncrementalPatchReport configureCompiledTimelinePositionIncremental(double stageWidth, double stageHeight);
} // namespace stockcut::timeline::project
