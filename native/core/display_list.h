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
// CoreEngine - Display List & Command Queue Subsystem (Pha 5)
// ----------------------------------------------------------------------------
// [KỸ THUẬT STOCKFISH ÁP DỤNG]:
// 1. Packed 32-Byte DisplayCommand Struct (Exactly 1/2 L1 Cacheline)
//    - Kiểu bài toán: Cấu trúc lệnh phẳng nạp thẳng vào GPU/Canvas không cần unpack
//    - Tham chiếu Stockfish: Stockfish/src/types.h (Move, ExtMove struct 16/32 bytes)
// 2. Static Command Buffer Generation (MoveList Model)
//    - Kiểu bài toán: Phát sinh chuỗi lệnh vẽ vào mảng tĩnh phẳng, 0 heap allocation
//    - Tham chiếu Stockfish: Stockfish/src/movegen.h, Stockfish/src/movegen.cpp
// 3. Staged Priority Traversal & Spatial Bitboard Occlusion Pruning
//    - Kiểu bài toán: Cắt tỉa lệnh bị che khuất bằng bitboard 8x8 (Stockfish Beta Cutoff)
//    - Tham chiếu Stockfish: Stockfish/src/movepick.cpp, Stockfish/src/bitboard.h
// 4. Boundary Sanitization & Zero-Check Hotpath Dispatchers
//    - Kiểu bài toán: Giả định 100% dữ liệu đã sạch qua Boundary Gateway, triệt tiêu mọi phép kiểm tra null/biên trong Hotpath
//    - Tham chiếu Stockfish: Stockfish/src/evaluate.cpp (evaluate_pieces<PieceType>()), Stockfish/src/bitboard.h (attacks_bb<PieceType>())
// Tra cứu thêm tại: native/phan_loai_nghiep_vu_stockfish.txt (Mục 1, Mục 4, Mục 10 & Mục 11)
// ============================================================================
#include "types.h"
#include "data_structures.h"
namespace stockcut {
// MEDIA_TYPE_NB defined in types.h
// MediaTier and MEDIA_TIER_NB defined in types.h
// Định kiểu Media packed (Tương ứng với kiểu Piece = make_piece(Color, PieceType) của Stockfish)
using PackedMediaTag = u8;
inline constexpr PackedMediaTag make_media(MediaTier tier, MediaType type) {
    return static_cast<PackedMediaTag>((static_cast<u8>(tier) << 4) | (static_cast<u8>(type) & 0x0F));
}
inline constexpr MediaType type_of(PackedMediaTag m) {
    return static_cast<MediaType>(m & 0x0F);
}
inline constexpr MediaTier tier_of(PackedMediaTag m) {
    return static_cast<MediaTier>((m >> 4) & 0x07);
}
// Hằng số mã thao tác Display List OpCodes
constexpr u16 CMD_NOP          = 0;
constexpr u16 CMD_CLEAR_RECT   = 1;
constexpr u16 CMD_DRAW_RECT    = 2;  // Vẽ background / box / border / bo góc
constexpr u16 CMD_DRAW_TEXT    = 3;  // Vẽ cụm text layout
constexpr u16 CMD_DRAW_IMAGE   = 4;  // Vẽ ảnh bitmap / media frame
constexpr u16 CMD_DRAW_VIDEO   = 5;  // Vẽ khung hình video
constexpr u16 CMD_PUSH_CLIP    = 6;  // Kẹp khung scissor clip
constexpr u16 CMD_POP_CLIP     = 7;  // Khôi phục khung clip
constexpr u16 CMD_PUSH_FILTER  = 8;  // Kích hoạt CSS Filter / Blend Mode slot
constexpr u16 CMD_POP_FILTER   = 9;  // Khôi phục Filter / Blend Mode
constexpr u16 CMD_DRAW_PATH    = 10; // Vẽ vector 2D SVG path Bézier
constexpr u16 CMD_SENTINEL_END = 0xFFu; // Kẻ gác cổng chặn đuôi luồng lệnh
constexpr u32 MAX_DISPLAY_COMMANDS = 256u;
constexpr u32 DL_CACHE_INVALID = INVALID_U32; // cache_save trả về khi list quá dài để lưu
// ----------------------------------------------------------------------------
// Cấu trúc DisplayCommand (32 bytes = đúng 1/2 L1 Cacheline)
// 2 lệnh đóng gói vừa khít 1 Cacheline 64 bytes
// ----------------------------------------------------------------------------
struct alignas(32) DisplayCommand {
    u16   opCode;       // 0..1: CMD_*
    u16   flags;        // 2..3: Cờ hiển thị (OPAQUE, DIRTY, CULLED, CLIPPED)
    u32   resourceId;   // 4..7: Hash / UID tài nguyên hoặc Node index
    float x0;           // 8..11: Tọa độ AABB minX thế giới
    float y0;           // 12..15: Tọa độ AABB minY thế giới
    float x1;           // 16..19: Tọa độ AABB maxX thế giới
    float y1;           // 20..23: Tọa độ AABB maxY thế giới
    float opacity;      // 24..27: Tổng độ mờ (0.0 .. 1.0)
    u16   styleSlot;    // 28..29: Slot PackedStyle (0..255)
    u16   filterSlot;   // 30..31: Slot FilterUniforms (0..255)
};
static_assert(sizeof(DisplayCommand) == 32, "DisplayCommand must be exactly 32 bytes");

// ----------------------------------------------------------------------------
// DisplayList: Cấu trúc dữ liệu tự chủ quản lý chuỗi lệnh vẽ phẳng
// Lấy cảm hứng từ MoveList của Stockfish: đóng gói LinearStream liên tục,
// 0 cấp phát động, tịnh tiến con trỏ không rẽ nhánh, theo dõi generation.
// ----------------------------------------------------------------------------
class alignas(64) DisplayList {
    LinearStream<DisplayCommand, MAX_DISPLAY_COMMANDS> stream_; // KHONG DUNG {}
    u32 generation_ = 1;
public:
    static DisplayList& instance() noexcept;

    inline void bump_generation_by(u32 delta) noexcept {
        generation_ += delta;
        generation_ |= static_cast<u32>(generation_ == 0u);
    }

    inline void reset() noexcept {
        stream_.reset();
        bump_generation_by(1u);
    }

    [[nodiscard]] inline u32 count() const noexcept {
        return static_cast<u32>(stream_.size());
    }

    [[nodiscard]] inline u32 generation() const noexcept {
        return generation_;
    }

    inline void set_count(u32 count) noexcept {
        stream_.set_size(count);
        bump_generation_by(1u);
    }

    [[nodiscard]] inline DisplayCommand* data() noexcept {
        return stream_.data();
    }

    [[nodiscard]] inline const DisplayCommand* data() const noexcept {
        return stream_.data();
    }

    [[nodiscard]] inline u32* u32_data() noexcept {
        return reinterpret_cast<u32*>(stream_.data());
    }

    [[nodiscard]] inline const u32* u32_data() const noexcept {
        return reinterpret_cast<const u32*>(stream_.data());
    }

    inline bool push_next(const DisplayCommand& cmd) noexcept {
        const bool ok = stream_.push_next(cmd);
        bump_generation_by(static_cast<u32>(ok));
        return ok;
    }

    inline void push_next_fast(const DisplayCommand& cmd) noexcept {
        stream_.push_next_fast(cmd);
        bump_generation_by(1u);
    }

    inline DisplayCommand* next_slot() noexcept {
        DisplayCommand* slot = stream_.next_slot();
        bump_generation_by(1u);
        return slot;
    }

    template <bool Cond>
    inline bool push_if(const DisplayCommand& cmd) noexcept {
        if constexpr (Cond) {
            return push_next(cmd);
        } else {
            return false;
        }
    }

    inline bool push_if(bool cond, const DisplayCommand& cmd) noexcept {
        const bool pushed = stream_.push_next_if(cond, cmd);
        bump_generation_by(static_cast<u32>(pushed));
        return pushed;
    }

    inline DisplayCommand* get_command(u32 idx) noexcept {
        return (idx < stream_.size()) ? &stream_[idx] : nullptr;
    }

    inline const DisplayCommand* get_command(u32 idx) const noexcept {
        return (idx < stream_.size()) ? &stream_[idx] : nullptr;
    }

    inline void terminate_sentinel() noexcept {
        stream_.terminate_sentinel(CMD_SENTINEL_END);
    }

    u32 cull(u64 occludedMask, double W, double H) noexcept;
};

// C++ API (Zero-overhead inline wrappers delegating to DisplayList::instance())
u32*  display_list_get_ptr();
u32   display_list_get_count();
u32   display_list_generation();
void  display_list_set_count(u32 count);
void  display_list_reset();
void  display_list_terminate_sentinel();
// Phát lệnh đơn lẻ vào danh sách
bool  display_list_push(
    u16 opCode, u16 flags, u32 resourceId,
    float x0, float y0, float x1, float y1,
    float opacity, u16 styleSlot, u16 filterSlot
);
bool  display_list_push_if(
    bool cond,
    u16 opCode, u16 flags, u32 resourceId,
    float x0, float y0, float x1, float y1,
    float opacity, u16 styleSlot, u16 filterSlot
);
template <bool Cond>
inline bool display_list_push_if(
    u16 opCode, u16 flags, u32 resourceId,
    float x0, float y0, float x1, float y1,
    float opacity, u16 styleSlot, u16 filterSlot
) {
    if constexpr (Cond) {
        return display_list_push(opCode, flags, resourceId, x0, y0, x1, y1, opacity, styleSlot, filterSlot);
    } else {
        return false;
    }
}
// Lấy con trỏ lệnh tại vị trí idx
DisplayCommand* display_list_get_command(u32 idx);
// Cắt tỉa các lệnh bị che khuất hoàn toàn bởi Bitboard 8x8
u32   display_list_cull(u64 occludedMask, double W, double H);
// Tự động duyệt cây SceneNode và sinh toàn bộ Display List trong C++ Wasm (Zero-DOM)
u32   display_list_generate_from_scene(u32 nodeCount, float viewportW, float viewportH);
// Transposition Table DisplayList Command Buffer Cache Arena (Stockfish intermediate caching)
u32   display_list_cache_save(u64 key, u8 tier);
bool  display_list_cache_restore(u32 slot, u64 expectedKey);
void  display_list_cache_clear();
// Returns static bytes reserved by the display-list cache.
usize display_list_cache_memory_bytes();
// ----------------------------------------------------------------------------
// ZERO-CHECK HOTPATH TEMPLATE DISPATCHERS
// Tham chiếu Stockfish: evaluate_pieces<PieceType>() trong Stockfish/src/evaluate.cpp
// Giả định 100% dữ liệu đã sạch qua Boundary Gateway, không kiểm tra null, không rẽ nhánh
// ----------------------------------------------------------------------------
template<MediaType M>
struct MediaExecutor {
    static inline void execute(const DisplayCommand& /*cmd*/) {}
};
template<>
struct MediaExecutor<MediaType::Image> {
    static inline void execute(const DisplayCommand& /*cmd*/) {
        // Direct execution for sanitized image command
    }
};
template<>
struct MediaExecutor<MediaType::Video> {
    static inline void execute(const DisplayCommand& /*cmd*/) {
        // Direct execution for sanitized video command
    }
};
template<u16 OpCode>
struct CommandDispatcher {
    static inline void dispatch(const DisplayCommand& /*cmd*/) {}
};
template<>
struct CommandDispatcher<CMD_DRAW_IMAGE> {
    static inline void dispatch(const DisplayCommand& cmd) {
        MediaExecutor<MediaType::Image>::execute(cmd);
    }
};
template<>
struct CommandDispatcher<CMD_DRAW_VIDEO> {
    static inline void dispatch(const DisplayCommand& cmd) {
        MediaExecutor<MediaType::Video>::execute(cmd);
    }
};
} // namespace stockcut
