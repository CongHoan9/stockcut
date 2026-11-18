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
// Bottleneck: timeline stepping checks the fixed slot set and retains a 256-entry
// state history. Optimize only after measuring real slot density and seek patterns;
// keep state and bitboards in-place to preserve the zero-allocation frame path.
#include "position.h"
#include "display_list.h"
namespace stockcut {
namespace {
constexpr u32 STATE_HISTORY_RING_SIZE = 256u;
static_assert((STATE_HISTORY_RING_SIZE & (STATE_HISTORY_RING_SIZE - 1u)) == 0u,
              "STATE_HISTORY_RING_SIZE must be a power of two");
alignas(64) Position<> g_timelinePosition;
alignas(64) MultiArray<StateInfo<>, STATE_HISTORY_RING_SIZE> g_stateHistory;
u32 g_historyIndex = 0;
}
alignas(64) MultiArray<u16, MAX_TIMELINE_SLOTS, MAX_BASS_HITS> g_bassHits{};
// Returns the single native timeline instance used by Wasm and shared-state adapters.
Position<>* position_get_instance() {
    return &g_timelinePosition;
}
Position<>* timeline_position_get_instance() {
    return position_get_instance();
}
// Clears timeline state and resets the history cursor without allocating memory.
void timeline_position_reset() {
    g_timelinePosition.clear();
    g_historyIndex = 0;
}
// Reserves the next slot in the fixed-size history ring for a timeline state.
StateInfo<>* timeline_position_alloc_state() {
    StateInfo<>* st = &g_stateHistory[g_historyIndex & (STATE_HISTORY_RING_SIZE - 1u)];
    ++g_historyIndex;
    return st;
}
// Advances native playback state, reusing the current state for an unchanged time/epoch.
StateInfo<>* timeline_position_advance(TimePoint timeMs) {
    if (g_timelinePosition.is_current(timeMs)) return g_timelinePosition.state();
    StateInfo<>* next = timeline_position_alloc_state();
    g_timelinePosition.step_to(timeMs, *next);
    return next;
}
// Returns the cached display-list size or generates commands for the requested state.
u32 timeline_position_display_list(StateInfo<>* state) {
    if (!state) return 0;
    if (state->dlReady && state->pad == display_list_generation() &&
        (state->activeBB & g_timelinePosition.continuous_tracks_bb()) == 0ULL)
        return display_list_get_count();
    return g_timelinePosition.generate_display_list(state);
}
} // namespace stockcut
