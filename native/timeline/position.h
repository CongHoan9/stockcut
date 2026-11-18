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
// ============================================================================
// CoreEngine - Nanosecond Timeline Engine & TrackMode Subsystem
// ----------------------------------------------------------------------------
// [CẤU TRÚC ĐỘT PHÁ SÁNG TẠO - BẢN QUYỀN HỆ THỐNG]:
// 1. TrackMode Bitboard Partitioning (Tương tự MoveList<T> nhưng siêu việt hóa):
//    - Tự động phân loại 64 timeline slots vào 4 tập hợp Bitboard bất biến:
//      + byModeBB[Static]: 0% math, 0% search, O(1) direct blit từ Rect<float>
//      + byModeBB[Harmonic]: Nghiệm giải tích đóng f(t) = w*t trong đúng 1 chu kỳ ALU
//      + byModeBB[Kinematic]: Vectorized SIMD piecewise linear/cubic interpolation
//      + byModeBB[Procedural]: Dẫn xuất thời gian thực (Audio reactive, dynamic reveal)
// 2. Canonical Stage Space (CSS) & Rect<float> (Căn chỉnh 16 bytes = 128-bit SIMD):
//    - Hệ trục tọa độ không gian bất biến chuẩn hóa: x0, y0, x1, y1
//    - Triệt tiêu hoàn toàn sự xáo trộn DOM và sai lệch tâm xoay
// 3. Geometry is expressed in canonical stage coordinates for every visual.
// ============================================================================
#include "types.h"
#include "render_constants.h"
#include "dirty_rect.h"
#include "timeline/bitboard.h"
#include "timeline/dirty_fields.h"
#include "display_list.h"
#include "math_utils.h"
#include "audio/script.h"
#include "timeline/effect.h"
#include "effect/monoid_operator.h"
#include "cache/zobrist.h"
#include "core/branchless_sort.h"
#include "path/path_types.h"
#include "property/color.h"
#include "property/property_types.h"
#include "property/block.h"
#include "property/media.h"
#include "property/text.h"
#include "property/bass.h"
#include <cstring>
#include <cstddef>
namespace stockcut
{
   // ----------------------------------------------------------------------------
   // Cấu trúc NativeKeyframe (32 bytes - 1/2 L1 Cacheline)
   // ----------------------------------------------------------------------------
   struct alignas(16) NativeKeyframe
   {
       i32 offsetMs;    // 0..3: mili-giây tính từ startMs của item
       float opacity;   // 4..7: độ mờ đục (0.0 .. 1.0)
       float x;         // 8..11: tọa độ X
       float y;         // 12..15: tọa độ Y
       float w;         // 16..19: chiều rộng (0: dùng baseW của item)
       float h;         // 20..23: chiều cao (0: dùng baseH của item)
       float rotateDeg; // 24..27: góc quay (độ)
       float scale;     // 28..31: tỷ lệ phóng đại (1.0 = gốc)
   };
   static_assert(sizeof(NativeKeyframe) == 32, "NativeKeyframe must be exactly 32 bytes");

   struct alignas(16) StyleColorKeyframe
   {
       ColorRGBA fillColor   = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f); // 0..15:  Fill / Text / Background color
       ColorRGBA strokeColor = ColorRGBA(0.0f, 0.0f, 0.0f, 1.0f); // 16..31: Stroke / Border color
       float     strokeWidth = 0.0f;                              // 32..35: Stroke width (px)
       float     blurRadius  = 0.0f;                              // 36..39: Gaussian blur radius (px)
       u32       colorFlags  = 0;                                 // 40..43: Bit 0: hasFill, Bit 1: hasStroke
       u32       pad         = 0;                                 // 44..47: Explicit 16-byte alignment pad
   };
   static_assert(sizeof(StyleColorKeyframe) == 48, "StyleColorKeyframe must be exactly 48 bytes");

   constexpr u8 KF_TRACK_FLAG_FROM_LEGACY = 1u << 0;

   // Unified 32-byte KeyframeTrack header (1/2 L1 Cacheline).
   // Replaces both the legacy 2,408-byte static KeyframeTrack and duplicate TypedKfSlotHeader.
   // Actual keyframe SoA slices [NativeKeyframe 32B hot stream + optional StyleColorGroup 48B]
   // live inside Position::keyframeArena (AlignedPtr<u8[]>).
   struct alignas(32) KeyframeTrack
   {
       u64 keyframedProps = 0ULL;         // 0..7:   Bitmask 64-bit các thuộc tính biến đổi
       u32 spatialOffset = INVALID_U32;   // 8..11:  NativeKeyframe[0..n+1] trong keyframeArena
       u32 invSpanOffset = INVALID_U32;   // 12..15: double[0..n+1] (1.0 / spanMs) trong keyframeArena
       u32 styleColorOffset = INVALID_U32;// 16..19: StyleColorGroup[0..n+1] khi activeColorKfMask != 0
       u32 contentMask = 0;               // 20..23: Bitmask keyframe có trường "content"
       u16 count = 0;                     // 24..25: Số keyframe thực tế
       u8 activeColorKfMask = 0;          // 26:     bit 0: fill, 1: strokeCol, 2: blur, 3: strokeW
       u8 easing = 0;                     // 27:     0: linear, 1: ease-in, 2: ease-out, 3: ease-in-out
       u8 hasSpatial = 0;                 // 28:     1 nếu có biến thiên không gian / color / fx
       u8 flags = 0;                      // 29:     bit 0: KF_TRACK_FLAG_FROM_LEGACY
       mutable u8 lastTypedIdx = 0;       // 30:     Sequential cursor hint cho spatial/style keyframe
       mutable u8 lastContentIdx = 0;     // 31:     Sequential cursor hint cho content keyframe

       [[nodiscard]] inline bool has_arena_frames() const noexcept
       {
           return spatialOffset != INVALID_U32 && count > 0;
       }
   };
   static_assert(sizeof(KeyframeTrack) == 32, "KeyframeTrack must be exactly 32 bytes");
   // ----------------------------------------------------------------------------
   // Cấu trúc TimelineItem (Căn chỉnh 64 bytes - Đúng 1 L1 Cacheline)
   // ----------------------------------------------------------------------------
   struct alignas(64) TimelineItem
   {
       u32 uid;           // 0..3: UID duy nhất của item
       MediaType type;    // 4..5: Loại media (MediaType u16)
       u8 tier;           // 6: TIER_STATIC / TIER_REVEAL / TIER_DYNAMIC
       u8 flags;          // 7: Cờ trạng thái (DIRTY, OPAQUE, SPINNING)
       TimePoint startMs; // 8..15: Mốc bắt đầu (ms)
       TimePoint endMs;   // 16..23: Mốc kết thúc (ms)
       float baseW;       // 24..27: Chiều rộng cơ sở
       float baseH;       // 28..31: Chiều cao cơ sở
       float spinSpeed;   // 32..35: Vận tốc góc xoay (độ/giây)
       float maskHole;    // 36..39: Radial image-mask hole ratio (0..1).
       float volume;      // 40..43: Mức âm lượng
       i32 zIndex;        // 44..47: Thứ tự hiển thị z
       u32 resourceId;    // 48..51: Mã băm tài nguyên media / texture handle
       float baseX;       // 52..55: Tọa độ gốc X
       float baseY;       // 56..59: Tọa độ gốc Y
       float rotateDeg;   // 60..63: Góc xoay cơ sở (độ) thay cho pad
   };
   static_assert(sizeof(TimelineItem) == 64, "TimelineItem must be exactly 64 bytes");
   struct ItemPresentation
   {
       u32 renderTier;
       u32 flags;
       float maskHole;
       float rotateDeg;
       float spinSpeed;
       float volume;
       u32 revealMode;
       double revealDurationMs;
       double revealStaggerMs;
   };
   static_assert(sizeof(ItemPresentation) == 48);
   // ----------------------------------------------------------------------------
   // [PHÂN TẦNG KIẾN TRÚC KINH ĐIỂN]:
   // TÁCH BIỆT BIẾN KHAI BÁO (AUTHORING STATE) VÀ BIẾN ĐỌC RUNTIME (RUNTIME EVALUATED STATE)
   // - Authoring State (TimelineItem, KeyframeTrack, TextBlock, BassBeatParams):
   //   Lưu trữ ý đồ khai báo (Cold Path), chỉnh sửa qua JSON/UI, không truy vấn đệ quy trong hot path.
   // - Runtime Evaluated State (Evaluated):
   //   Được tính toán tại mili-giây T (Hot Path 60-120 FPS), phẳng, căn lề 64 bytes (1 L1 Cacheline),
   //   toàn bộ 64 slot = 4 KB vừa trọn L1 Data Cache.
   // ----------------------------------------------------------------------------
   constexpr u32 EVALUATED_CUSTOM_PARAMS_NB = 5u;
   struct alignas(64) Evaluated
   {
       Rect<float> aabb;    // 0..15: Tọa độ bounding box [x0, y0, x1, y1] tại mili-giây T (TransformAABB)
       float rotateDeg;       // 16..19: Góc xoay thực tế (độ) (TransformRot)
       float opacity;         // 20..23: Độ mờ [0.0 .. 1.0] (Opacity)
       u32 resourceId;        // 24..27: Mã màu RGBA hoặc texture handle tại mili-giây T (Resource)
       u32 filter;            // 28..31: Packed mask / reveal filter / filter slot (TextLayout)
       u16 opCode;            // 32..33: Mã lệnh (CMD_DRAW_RECT, CMD_DRAW_IMAGE, CMD_DRAW_TEXT, etc.)
       u16 flags;             // 34..35: Flags / rotate packed / progress permille
       u16 zRank;             // 36..37: Thứ tự hiển thị z đã chuẩn hóa
       u16 slot;              // 38..39: Slot index gốc (0..63)
       u32 isVisible;         // 40..43: 1 nếu active và opacity > OPACITY_EPSILON, 0 nếu ẩn (Visibility)
       MultiArray<float, EVALUATED_CUSTOM_PARAMS_NB> customParams; // 44..63: Tham số mở rộng (Uniforms)

       inline void clear() noexcept {
           std::memset(this, 0, sizeof(*this));
       }

       // Chuyển đổi O(1) sang toán tử Monoid MathOp (64 bytes)
       inline MathOp to_math_op() const noexcept {
           const float alpha = (isVisible != 0) ? opacity : 0.0f;
           float r = 1.0f, g = 1.0f, b = 1.0f;
           if (opCode == CMD_DRAW_RECT && (resourceId >> 24) != 0) {
               r = static_cast<float>((resourceId >> 16) & INVALID_U8) * INV_255_F32;
               g = static_cast<float>((resourceId >> 8) & INVALID_U8) * INV_255_F32;
               b = static_cast<float>(resourceId & INVALID_U8) * INV_255_F32;
           }
           const float rotRad = rotateDeg * DEG_TO_RAD_F32;
           return MathOp::make_layer(
               aabb.x0, aabb.y0, aabb.x1, aabb.y1,
               r, g, b, alpha,
               1.0f, 1.0f, 1.0f,
               1.0f, rotRad, 0.0f, 0.0f
           );
       }
   };
   static_assert(sizeof(Evaluated) == 64, "Evaluated must be exactly 64 bytes");
   static_assert(offsetof(Evaluated, aabb) == 0, "aabb must sit at offset 0");
   static_assert(offsetof(Evaluated, rotateDeg) == 16, "rotateDeg must sit at offset 16");
   static_assert(offsetof(Evaluated, opacity) == 20, "opacity must sit at offset 20");
   static_assert(offsetof(Evaluated, resourceId) == 24, "resourceId must sit at offset 24");
   static_assert(offsetof(Evaluated, filter) == 28, "filter must sit at offset 28");
   static_assert(offsetof(Evaluated, opCode) == 32, "opCode must sit at offset 32");
   static_assert(offsetof(Evaluated, flags) == 34, "flags must sit at offset 34");
   static_assert(offsetof(Evaluated, zRank) == 36, "zRank must sit at offset 36");
   static_assert(offsetof(Evaluated, slot) == 38, "slot must sit at offset 38");
   static_assert(offsetof(Evaluated, isVisible) == 40, "isVisible must sit at offset 40");
   static_assert(offsetof(Evaluated, customParams) == 44, "customParams must sit at offset 44");
   // ----------------------------------------------------------------------------
   // [TEMPORAL BITBOARD CHUNKING]:
   // Chia trục thời gian timeline thành các chunk vừa vặn 64-bit đơn vị (256ms/chunk)
   // Truy xuất O(1) tức thì khi tua/scrub và trong vòng lặp 60-120 FPS!
   // ----------------------------------------------------------------------------
   constexpr u32 TIME_CHUNK_SHIFT     = 8u;    // 256 ms per chunk
   constexpr u32 MAX_TEMPORAL_CHUNKS  = 2048u; // 2048 * 256 ms = 524,288 ms (~8.7 phút)
   constexpr u32 TEMPORAL_SUBCHUNK_NB = 4u;    // 4 sub-intervals per 256ms chunk
   // ----------------------------------------------------------------------------
   // [HARDWARE-ALIGNED BRANCHLESS TEMPORAL CHUNK (64 BYTES - ĐÚNG 1 L1 CACHELINE)]
   // ----------------------------------------------------------------------------
   struct alignas(32) TemporalTransition {
       TimePoint t1;        // 0..7: Mốc chuyển tiếp 1 (ms)
       TimePoint t2;        // 8..15: Mốc chuyển tiếp 2 (ms)
       TimePoint t3;        // 16..23: Mốc chuyển tiếp 3 (ms)
       u16 transitionCount; // 24..25: Số mốc hợp lệ (0..3)
       u16 overflow;        // 26..27: 0 nếu <= 3 transitions, 1 nếu chunk có quá 3 mốc (fallback)
       MultiArray<u8, TEMPORAL_SUBCHUNK_NB> activeBgSlot; // 28..31: Pre-baked O(1) active Background slot
   };
   static_assert(sizeof(TemporalTransition) == 32, "TemporalTransition must be exactly 32 bytes");

   template <typename Mask = Mask<1>>
   using TemporalChunk = stockcut::Chunk<TemporalTransition, Mask, TEMPORAL_SUBCHUNK_NB>;
   static_assert(sizeof(TemporalChunk<Mask<1>>) == 64, "TemporalChunk must be exactly 64 bytes (1 L1 Cacheline)");

   template <typename Mask = Mask<1>>
   using TemporalChunkZobrist = stockcut::Chunk<MultiArray<Key, TEMPORAL_SUBCHUNK_NB>, Mask, TEMPORAL_SUBCHUNK_NB>;
   static_assert(sizeof(TemporalChunkZobrist<Mask<1>>) == 64, "TemporalChunkZobrist must be exactly 64 bytes (1 L1 Cacheline)");

   template <typename Mask = Mask<1>>
   using TemporalChunkOcclusion = stockcut::Chunk<MultiArray<Bitboard, TEMPORAL_SUBCHUNK_NB>, Mask, TEMPORAL_SUBCHUNK_NB>;
   static_assert(sizeof(TemporalChunkOcclusion<Mask<1>>) == 64, "TemporalChunkOcclusion must be 64 bytes (1 L1 Cacheline)");
   
   template <typename Mask = Mask<1>>
   struct alignas(64) ChunkTable
   {
       using Chunk = TemporalChunk<Mask>;
       using ChunkZobrist = TemporalChunkZobrist<Mask>;
       using ChunkOcclusion = TemporalChunkOcclusion<Mask>;

       Sentinel<Chunk, MAX_TEMPORAL_CHUNKS, 0, 1>          chunks;
       Sentinel<ChunkZobrist, MAX_TEMPORAL_CHUNKS, 0, 1>   chunkZobrist;
       Sentinel<ChunkOcclusion, MAX_TEMPORAL_CHUNKS, 0, 1> chunkOcclusion;
       u32 validChunkCount = 0;
       bool dirty = true;
   };
   // ============================================================================
    // COMPILE-TIME MEDIA CONSTANTS & TRAITS (CANH GIOI VO DANH TUYET DOI)
    // 0 Runtime Nominal Trait Tables, 0 Nominal Queries, Folded by Inliner
    // ============================================================================
    constexpr u16 media_opcode(MediaType t) noexcept {
        switch (t) {
            case MediaType::Background: return CMD_DRAW_RECT;
            case MediaType::Video:      return CMD_DRAW_VIDEO;
            case MediaType::Image:      return CMD_DRAW_IMAGE;
            case MediaType::Path:       return CMD_DRAW_PATH;
            case MediaType::Lyric:
            case MediaType::Text:       return CMD_DRAW_TEXT;
            default:                    return CMD_NOP;
        }
    }

   template <MediaType Type>
   struct MediaTrackTraits
   {
       static constexpr MediaType kType = Type;
       static constexpr u32 kTypeIdx = static_cast<u32>(Type);
       static constexpr bool kIsVisual = (Type != MediaType::None && Type != MediaType::Audio);
       static constexpr bool kIsText = (Type == MediaType::Text || Type == MediaType::Lyric);
       static constexpr bool kIsBackground = (Type == MediaType::Background);
       static constexpr bool kIsImage = (Type == MediaType::Image);
       static constexpr bool kIsVideo = (Type == MediaType::Video);
       static constexpr bool kIsPath = (Type == MediaType::Path);
       static constexpr u16 kOpCode = media_opcode(Type);
   };
   template <MediaType Type>
   constexpr u16 media_opcode_T()
   {
       return MediaTrackTraits<Type>::kOpCode;
   }
   inline u16 image_mask_filter(const TimelineItem &item)
   {
       constexpr u16 MASK_SHAPE_BITS = static_cast<u16>(ITEM_FLAG_MASK_HOLE | ITEM_FLAG_MASK_CIRCLE);
       const u16 shape = item.flags & MASK_SHAPE_BITS;
       if (shape == 0)
       {
           return 0;
       }
       constexpr float PERMILLE_MAX_F32 = static_cast<float>(PERMILLE_MAX);
       const u32 hole = static_cast<u32>(stockcut::min(PERMILLE_MAX_F32, stockcut::max(0.0f, item.maskHole * PERMILLE_MAX_F32)) + 0.5f);
       return static_cast<u16>((hole << 4) | shape);
   }
   // ----------------------------------------------------------------------------
   // Cấu trúc StateInfo (Căn chỉnh 64 bytes - 2 L1 Cachelines = 128 bytes)
   // ----------------------------------------------------------------------------
       template <typename Mask = Mask<1>>
    struct alignas(64) StateInfo
    {
        // Cacheline 1 (0..63):
        Key key;                  // 0..7: Khóa băm Zobrist của toàn bộ frame hiện tại
        Mask activeBB;            // 8..15: Bitmask các layer đang active (start <= t < end)
        Mask dirtyBB;             // 16..23: Bitmask các layer có biến thiên (animation / lerp)
        Bitboard occludedBB;      // 24..31: Spatial 8x8 Bitboard các vùng bị che khuất (luôn 64 bit)
        Mask occludedSlotsBB;     // 32..39: Bitmask các slot bị che phủ hoàn toàn (Precomputed O(1))
        TimePoint timeMs;         // 40..47: Mốc mili-giây chính xác của frame này
        u32 commandCount;         // 48..51: Số lượng DisplayCommand 32-byte đã sinh ra
        u16 generation;           // 52..53: Vòng đời frame
        u16 chunkTag;             // 54..55: (chunkIdx << 2) | subIdx — Stockfish Invariant Carry-Over Tag
        u32 dlReady;              // 56..59: 1 nếu DisplayList của state này đã được sinh
        u32 pad;                  // 60..63: display-list arena generation for dlReady

        // Cacheline 2 (64..127):
        StateInfo *previous;     // 64..71: Con trỏ lùi về frame trước (Stockfish backward pointer)
#if defined(__wasm32__) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4)
        u32 _ptrPad = 0;          // 68..71: Đồng bộ ABI 64-bit cho Wasm32 (previous pointer là 32-bit)
#endif
        Mask dynamicVisualBB;     // 72..79: Active visual slots có biến thiên liên tục
        Mask opaqueSlotsBB;       // 80..87: Active slots có độ đục 100%
        Key visualExactKey;       // 88..95: Khóa Zobrist chính xác tuyệt đối cấp IEEE-754 cho DisplayList & CompositeCommand TT
        Mask pseudoCulledBB;      // 96..103: Bitmask các slot bị loại bỏ sớm (O(1) speculative pseudo-cull)
        Mask dirtyTransformBB;    // 104..111: Bitmask các layer có Transform bẩn
        Mask dirtyOpacityBB;      // 112..119: Bitmask các layer có Opacity bẩn
        Mask dirtyContentBB;      // 120..127: Bitmask các layer có Content / Uniforms bẩn
    };
    static_assert(sizeof(StateInfo<Mask<1>>) == 128, "StateInfo must be exactly 128 bytes");
         // Normalizes a finite angle into the interval [0, 360) without library calls.
   constexpr float FULL_CIRCLE_DEG_F32 = 360.0f;
   constexpr float INV_360_F32         = 1.0f / FULL_CIRCLE_DEG_F32;
   constexpr double MS_PER_SECOND_F64  = 1000.0;
   constexpr float INV_MS_PER_SEC_F32  = 0.001f;
   inline float fast_fmod360(float angle)
   {
       const float q = angle * INV_360_F32;
       const i32 qi = static_cast<i32>(q);
       const float rem = angle - static_cast<float>(qi) * FULL_CIRCLE_DEG_F32;
       return (rem < 0.0f) ? rem + FULL_CIRCLE_DEG_F32 : rem;
   }
   using EasingFn = float (*)(float) noexcept;
   inline constexpr MultiArray<EasingFn, EASING_CURVE_NB> kEasingTable{{
       &eval_quad_easing<EasingCurve::Linear, float>,
       &eval_quad_easing<EasingCurve::EaseIn, float>,
       &eval_quad_easing<EasingCurve::EaseOut, float>,
       &eval_quad_easing<EasingCurve::EaseInOut, float>,
   }};
   // ----------------------------------------------------------------------------
   // Lớp Position (Vận hành thế trận kịch bản ở tốc độ Nano-giây)
   // Áp dụng triết lý C++20 <T> Template Specialization chuẩn Stockfish
   // ----------------------------------------------------------------------------
    // ----------------------------------------------------------------------------
    // Cấu trúc StyleSegment & StyleEnvironment (Global Ambient Style Blackboard)
    // Dùng trực tiếp TextBlock (Block<24, 1>) làm nguồn dữ liệu thật duy nhất
    // ----------------------------------------------------------------------------
    struct alignas(64) StyleSegment {
        TimePoint startMs = 0;
        TimePoint endMs = 0;
        Key segmentZobristKey = 0ULL;
        u16 activeTypesMask = 0;
        u8 pad0[6]{};
        MultiArray<TextBlock, MEDIA_TYPE_NB> targets;
    };

    struct alignas(64) StyleEnvironment {
        TimePoint currentStartMs = -1;
        TimePoint currentEndMs = -1;
        u32 activeSegmentIndex = INVALID_U32;
        u16 activeTypesMask = 0;
        u8 pad0[2]{0, 0};
        Key styleZobristToken = 0ULL;
        MultiArray<TextBlock, MEDIA_TYPE_NB> targets;

        inline void clear() noexcept {
            currentStartMs = -1;
            currentEndMs = -1;
            activeSegmentIndex = INVALID_U32;
            activeTypesMask = 0;
            pad0[0] = 0; pad0[1] = 0;
            styleZobristToken = 0ULL;
            for (u32 i = 0; i < MEDIA_TYPE_NB; ++i) {
                targets[i].clear();
                targets[i].set_value<text_reg::OPACITY>(1.0f);
                targets[i].set_value<text_reg::LINE_HEIGHT>(1.25f);
                targets[i].set_value<text_reg::FONT_WEIGHT>(400u);
            }
        }
    };
    static_assert(alignof(StyleEnvironment) == 64, "StyleEnvironment must be 64-byte aligned");

   template <typename Mask = Mask<1>>
   class Position
   {
   public:
       static constexpr usize CAPACITY    = Mask::BITS;
       static constexpr usize BLOCK_COUNT = Mask::BLOCK_COUNT;
        using Chunk = typename ChunkTable<Mask>::Chunk;
        using ChunkZobrist = typename ChunkTable<Mask>::ChunkZobrist;
        using ChunkOcclusion = typename ChunkTable<Mask>::ChunkOcclusion;

        struct alignas(64) SlotStaticLayout {
            Rect<float> aabb{0.0f, 0.0f, 0.0f, 0.0f}; // 0..15
            Key staticZobristKey = 0ULL;              // 16..23
            Key dynamicZobristKey = 0ULL;             // 24..31 (Fused dynamic Zobrist delta key)
            Bitboard tileFootprint = 0ULL;            // 32..39
            Bitboard innerFootprint = 0ULL;           // 40..47
            float originX = 0.0f;                     // 48..51
            float originY = 0.0f;                     // 52..55
            u16 maskFilter = 0u;                      // 56..57
            u16 rotateFlags = 0u;                     // 58..59
            u8 varyingProps = 0u;                     // 60: 7-bit varying property mask
            TrackMode mode = TrackMode::Static;       // 61: Preclassified runtime motion mode
            u8 topLevelIndex = 0u;                    // 62: Top-level item index
            u8 hasCustomOrigin : 1 = 0u;              // 63 (bit 0)
            u8 isFullyOccluded : 1 = 0u;              // 63 (bit 1)
            u8 isPartiallyOccluded : 1 = 0u;          // 63 (bit 2)
            u8 isGroupChild : 1 = 0u;                 // 63 (bit 3)
            u8 padFlags : 4 = 0u;                     // 63 (bits 4..7)
        };
        static_assert(sizeof(SlotStaticLayout) == 64, "SlotStaticLayout must be exactly 64 bytes");
        static_assert(alignof(SlotStaticLayout) == 64, "SlotStaticLayout must be 64-byte aligned");

        // --------------------------------------------------------------------
        // Unified Typed Keyframe Arena (Stockfish memory.h AlignedPtr + Sentinels)
        // Lưu trực tiếp mảng comp::Keyframe<MediaT> căn lề 64B, 0 byte lãng phí
        // cho slot tĩnh hoặc không mang keyframe.
        // --------------------------------------------------------------------
        struct TypedKeyframeArena {
            static constexpr usize INITIAL_SLAB_BYTES = 32768u;
            AlignedPtr<u8[]> buffer;
            usize capacity = 0;
            usize cursor = 0;

            inline void reset_cursor() noexcept {
                cursor = 0;
            }

            [[nodiscard]] inline u8* alloc_aligned(usize bytes, usize alignment, u32& outOffset) noexcept {
                const usize alignMask = (alignment > 1u) ? (alignment - 1u) : 0u;
                const usize alignedPos = (cursor + alignMask) & ~alignMask;
                const usize needed = alignedPos + bytes;
                if (needed > capacity) {
                    usize newCap = (capacity == 0) ? INITIAL_SLAB_BYTES : capacity;
                    while (newCap < needed) newCap *= 2u;
                    AlignedPtr<u8[]> next = make_unique_aligned<u8[]>(newCap);
                    if (!next) return nullptr;
                    if (buffer && cursor > 0) {
                        std::memcpy(next.get(), buffer.get(), cursor);
                    }
                    buffer = std::move(next);
                    capacity = newCap;
                }
                outOffset = static_cast<u32>(alignedPos);
                cursor = needed;
                return buffer.get() + alignedPos;
            }
        };

        static constexpr usize MAX_STYLE_SEGMENTS = 32;
        Sentinel<StyleSegment, MAX_STYLE_SEGMENTS, 0, 1> styleSegments;
        u32 styleSegmentCount = 0;
        TimePoint styleValidFromMs = TIME_MIN_SENTINEL_I64;
        TimePoint styleValidUntilMs = TIME_MAX_SENTINEL_I64;
        StyleEnvironment activeStyleEnv;

        inline void clear_style_segments() noexcept {
            styleSegmentCount = 0;
            styleSegments[0].startMs = TIME_MAX_SENTINEL_I64;
            styleSegments[0].endMs = TIME_MAX_SENTINEL_I64;
            styleValidFromMs = TIME_MIN_SENTINEL_I64;
            styleValidUntilMs = TIME_MAX_SENTINEL_I64;
            activeStyleEnv.clear();
        }

        inline void add_style_segment(
            TimePoint startMs,
            TimePoint endMs,
            const TextBlock* inTargets,
            u16 typesMask,
            Key zobrist
        ) noexcept {
            if (styleSegmentCount >= MAX_STYLE_SEGMENTS) return;
            StyleSegment& seg = styleSegments[styleSegmentCount++];
            seg.startMs = startMs;
            seg.endMs = endMs;
            seg.segmentZobristKey = zobrist;
            seg.activeTypesMask = typesMask;
            for (u32 i = 0; i < MEDIA_TYPE_NB; ++i) {
                if (inTargets) seg.targets[i] = inTargets[i];
                else seg.targets[i].clear();
            }
            styleSegments[styleSegmentCount].startMs = TIME_MAX_SENTINEL_I64;
            styleSegments[styleSegmentCount].endMs = TIME_MAX_SENTINEL_I64;
            styleValidFromMs = TIME_MAX_SENTINEL_I64;
            styleValidUntilMs = -1;
        }

        static constexpr MultiArray<MediaType, MEDIA_TYPE_NB> kStyleAliasType = {{
            MediaType::Background, // Root falls back to Background style
            MediaType::None,       // Background
            MediaType::None,       // Video
            MediaType::None,       // Image
            MediaType::None,       // Path
            MediaType::Text,       // Lyric falls back to Text style
            MediaType::Lyric,      // Text falls back to Lyric style
            MediaType::None        // Audio
        }};

        [[nodiscard]] inline const TextBlock* resolve_active_style_preset(MediaType type) const noexcept {
            if (activeStyleEnv.activeSegmentIndex == INVALID_U32) return nullptr;
            const u32 idx = static_cast<u32>(type);
            if (idx >= MEDIA_TYPE_NB) return nullptr;
            if (activeStyleEnv.targets[idx].has_any_prop())
                return &activeStyleEnv.targets[idx];
            const u32 fbIdx = static_cast<u32>(kStyleAliasType[idx]);
            if (fbIdx != 0u && fbIdx < MEDIA_TYPE_NB && activeStyleEnv.targets[fbIdx].has_any_prop())
                return &activeStyleEnv.targets[fbIdx];
            return nullptr;
        }

        inline void sync_active_slot_blocks() noexcept {
            if (const TextBlock* rootPreset = resolve_active_style_preset(MediaType::Root)) {
                this->activeRootBlock.merge_style(this->baseRootBlock, *rootPreset);
            } else {
                this->activeRootBlock = this->baseRootBlock;
            }

            for (Mask occ = populated_slots(); occ;) {
                const u32 s = occ.pop_lsb();
                if (const TextBlock* preset = resolve_active_style_preset(this->items[s].type)) {
                    this->activeSlotBlocks[s].merge_style(this->baseSlotBlocks[s], *preset);
                } else {
                    this->activeSlotBlocks[s] = this->baseSlotBlocks[s];
                }
            }
        }

        inline bool update_style_environment(TimePoint nowMs) noexcept {
            if (__builtin_expect(static_cast<u64>(nowMs - styleValidFromMs) < static_cast<u64>(styleValidUntilMs - styleValidFromMs), 1)) {
                return false;
            }
            if (__builtin_expect(styleSegmentCount == 0, 0)) {
                styleValidFromMs = TIME_MIN_SENTINEL_I64;
                styleValidUntilMs = TIME_MAX_SENTINEL_I64;
                return false;
            }

            u32 foundIdx = INVALID_U32;
            TimePoint nextStartGapMs = TIME_MAX_SENTINEL_I64;
            for (u32 i = 0; i < styleSegmentCount; ++i) {
                const StyleSegment& s = styleSegments[i];
                if (nowMs >= s.startMs && nowMs < s.endMs) {
                    foundIdx = i;
                    break;
                }
                if (s.startMs > nowMs && s.startMs < nextStartGapMs) {
                    nextStartGapMs = s.startMs;
                }
            }
            if (foundIdx != INVALID_U32) {
                styleValidFromMs = styleSegments[foundIdx].startMs;
                styleValidUntilMs = styleSegments[foundIdx].endMs;
            } else {
                styleValidFromMs = nowMs;
                styleValidUntilMs = nextStartGapMs;
            }
            if (foundIdx != activeStyleEnv.activeSegmentIndex) {
                activeStyleEnv.activeSegmentIndex = foundIdx;
                if (foundIdx != INVALID_U32) {
                    const StyleSegment& seg = styleSegments[foundIdx];
                    activeStyleEnv.currentStartMs = seg.startMs;
                    activeStyleEnv.currentEndMs = seg.endMs;
                    activeStyleEnv.styleZobristToken = seg.segmentZobristKey;
                    activeStyleEnv.activeTypesMask = seg.activeTypesMask;
                    activeStyleEnv.targets = seg.targets;
                } else {
                    activeStyleEnv.clear();
                }
                sync_active_slot_blocks();
                return true;
            }
            return false;
        }

        inline const StyleEnvironment& active_style_env() const noexcept {
            return activeStyleEnv;
        }

        inline TextBlock& base_slot_block(u32 slot) noexcept {
            assert(slot < CAPACITY);
            return this->baseSlotBlocks[slot];
        }
        inline const TextBlock& base_slot_block(u32 slot) const noexcept {
            assert(slot < CAPACITY);
            return this->baseSlotBlocks[slot];
        }
        inline TextBlock& slot_block(u32 slot) noexcept {
            assert(slot < CAPACITY);
            return this->activeSlotBlocks[slot];
        }
        inline const TextBlock& slot_block(u32 slot) const noexcept {
            assert(slot < CAPACITY);
            return this->activeSlotBlocks[slot];
        }

        template <MediaType Type>
        [[nodiscard]] [[gnu::always_inline]] inline Media<Type>& slot_media(u32 slot) noexcept {
            assert(slot < CAPACITY);
            return reinterpret_cast<Media<Type>&>(this->activeSlotBlocks[slot]);
        }
        template <MediaType Type>
        [[nodiscard]] [[gnu::always_inline]] inline const Media<Type>& slot_media(u32 slot) const noexcept {
            assert(slot < CAPACITY);
            return reinterpret_cast<const Media<Type>&>(this->activeSlotBlocks[slot]);
        }
        template <MediaType Type>
        [[nodiscard]] [[gnu::always_inline]] inline Media<Type>& base_slot_media(u32 slot) noexcept {
            assert(slot < CAPACITY);
            return reinterpret_cast<Media<Type>&>(this->baseSlotBlocks[slot]);
        }
        template <MediaType Type>
        [[nodiscard]] [[gnu::always_inline]] inline const Media<Type>& base_slot_media(u32 slot) const noexcept {
            assert(slot < CAPACITY);
            return reinterpret_cast<const Media<Type>&>(this->baseSlotBlocks[slot]);
        }
        inline const TextBlock* slot_blocks_arena() const noexcept {
            return this->activeSlotBlocks.data();
        }
        inline TextBlock& base_root_block() noexcept {
            return this->baseRootBlock;
        }
        inline const TextBlock& base_root_block() const noexcept {
            return this->baseRootBlock;
        }
        inline TextBlock& root_block() noexcept {
            return this->activeRootBlock;
        }
        [[nodiscard]] inline Mask eval_visible_bb() const noexcept {
            return this->evalVisibleBB;
        }
        [[nodiscard]] inline Mask custom_origin_slots() const noexcept {
            return this->customOriginBB;
        }
        [[nodiscard]] inline Bitboard static_tile_footprint(u32 slot) const noexcept {
            assert(slot < CAPACITY);
            return this->staticLayout[slot].tileFootprint;
        }
        [[nodiscard]] inline Bitboard slot_visible_tiles(u32 slot) const noexcept {
            assert(slot < CAPACITY);
            return this->slotVisibleTiles[slot];
        }
        [[nodiscard]] inline const Rect<float>& slot_scissor_box(u32 slot) const noexcept {
            assert(slot < CAPACITY);
            return this->slotScissorBoxes[slot];
        }
        [[nodiscard]] inline bool is_slot_fully_occluded(u32 slot) const noexcept {
            assert(slot < CAPACITY);
            return this->staticLayout[slot].isFullyOccluded != 0u;
        }
        [[nodiscard]] inline bool is_slot_partially_occluded(u32 slot) const noexcept {
            assert(slot < CAPACITY);
            return this->staticLayout[slot].isPartiallyOccluded != 0u;
        }
        [[nodiscard]] inline Mask static_occluded_slots_bb() const noexcept {
            return this->staticOccludedSlotsBB;
        }
        inline const TextBlock& root_block() const noexcept {
            return this->activeRootBlock;
        }
        inline u32 active_root_color_packed() const noexcept {
            return this->activeRootBlock.template get_value<u32, 4>(16);
        }
        inline ColorRGBA active_root_color() const noexcept {
            return ColorRGBA::from_packed(this->activeRootBlock.template get_value<u32, 4>(16));
        }

        // ====================================================================
        // WARM PATH: ATOMIC IN-PLACE MUTATION API (< 100ns, Zero JSON)
        // ====================================================================
        // WARM PATH: ATOMIC IN-PLACE MUTATION API (< 100ns, Zero JSON)
        // ====================================================================
        inline void mark_slot_dirty(u32 slot, u16 fieldBits = DirtyField::All) noexcept {
            assert(slot < CAPACITY);
            warmDirtySlotsBB.set_bit(slot);
            diffAccumulator.mark_field(slot, fieldBits);
            if (st) {
                st->dirtyBB.set_bit(slot);
                if (fieldBits & DirtyField::Transform) st->dirtyTransformBB.set_bit(slot);
                if (fieldBits & DirtyField::Opacity)   st->dirtyOpacityBB.set_bit(slot);
                if (fieldBits & (DirtyField::Resource | DirtyField::TextLayout | DirtyField::Uniforms | DirtyField::Visibility))
                    st->dirtyContentBB.set_bit(slot);
                st->dlReady = 0;
            }
            ++epoch;
        }

        inline void mutate_slot_reg_f32(u32 slot, u32 regIdx, float val) noexcept {
            assert(slot < CAPACITY);
            baseSlotBlocks[slot].set_value(regIdx, val);
            activeSlotBlocks[slot].set_value(regIdx, val);

            TimelineItem& it = items[slot];
            u16 dirtyField = DirtyField::TransformAABB;
            switch (regIdx) {
                case 0: it.baseX = val; break;
                case 1: it.baseY = val; break;
                case 2: it.baseW = val; break;
                case 3: it.baseH = val; break;
                case 4: it.rotateDeg = val; dirtyField = DirtyField::TransformRot; break;
                case 6: dirtyField = DirtyField::Opacity; break;
                default: dirtyField = DirtyField::All; break;
            }
            if (regIdx <= 3) {
                staticLayout[slot].aabb = {it.baseX, it.baseY, it.baseX + it.baseW, it.baseY + it.baseH};
                evaluatedSlots[slot].aabb = staticLayout[slot].aabb;
            }
            if (regIdx == 4) {
                evaluatedSlots[slot].rotateDeg = val;
                staticLayout[slot].rotateFlags = static_cast<u16>(fast_fmod360(val));
            } else if (regIdx == 6) {
                evaluatedSlots[slot].opacity = val;
            }
            staticLayout[slot].staticZobristKey = compute_slot_static_zobrist_key(slot);
            recompute_slot_static_occlusion(slot);
            mark_slot_dirty(slot, dirtyField);
        }

        inline void mutate_slot_reg_u32(u32 slot, u32 regIdx, u32 val) noexcept {
            assert(slot < CAPACITY);
            baseSlotBlocks[slot].set_value(regIdx, val);
            activeSlotBlocks[slot].set_value(regIdx, val);
            if (regIdx == 16) {
                evaluatedSlots[slot].resourceId = val;
            }
            const u16 dirtyField = (regIdx == 16 || regIdx == 17) ? DirtyField::Resource : DirtyField::All;
            mark_slot_dirty(slot, dirtyField);
        }

        inline void mutate_slot_bit(u32 slot, u32 bitIdx, u32 val) noexcept {
            assert(slot < CAPACITY);
            baseSlotBlocks[slot].set_value(bitIdx, val != 0);
            activeSlotBlocks[slot].set_value(bitIdx, val != 0);
            if (bitIdx == 0) {
                if (val == 0) items[slot].flags |= 1u;
                else items[slot].flags &= ~1u;
            }
            mark_slot_dirty(slot, DirtyField::All);
        }

        [[nodiscard]] inline float get_slot_reg_f32(u32 slot, u32 regIdx) const noexcept {
            assert(slot < CAPACITY);
            return activeSlotBlocks[slot].template get_value<float, 4>(regIdx);
        }

        [[nodiscard]] inline u32 get_slot_reg_u32(u32 slot, u32 regIdx) const noexcept {
            assert(slot < CAPACITY);
            return activeSlotBlocks[slot].template get_value<u32, 4>(regIdx);
        }

        [[nodiscard]] inline bool get_slot_bit(u32 slot, u32 bitIdx) const noexcept {
            assert(slot < CAPACITY);
            return activeSlotBlocks[slot].template get_value<bool, 1>(bitIdx);
        }

        [[nodiscard]] inline bool has_warm_dirty(u32 slot) const noexcept {
            assert(slot < CAPACITY);
            return warmDirtySlotsBB.test_bit(slot);
        }

        [[nodiscard]] inline Mask warm_dirty_mask() const noexcept {
            return warmDirtySlotsBB;
        }

       // Initializes fixed arrays and bitboards for a fresh, allocation-free timeline instance.
       Position()
           : st(nullptr), currentTimeMs(0), projectDurationMs(0),
             stageW(DEFAULT_STAGE_WIDTH_F32), stageH(DEFAULT_STAGE_HEIGHT_F32)
       {
           clear();
       }
        // Clears all configured items, cached geometry, and type/tier/mode indexes in-place.
       void clear()
       {
            clear_style_segments();
            keyframeArena.reset_cursor();
           topLevelCount = 0;
           TextBlock defaultSlotBlock{};
           defaultSlotBlock.clear();
           defaultSlotBlock.set_value(0, true);
           defaultSlotBlock.set_value(0, RectF{stageW * 0.5f, stageH * 0.5f, 0.0f, 0.0f});
           defaultSlotBlock.set_value(5, 1.0f);
           defaultSlotBlock.set_value(6, 1.0f);
           defaultSlotBlock.set_value(7, 0.5f);
           defaultSlotBlock.set_value(8, 0.5f);
           defaultSlotBlock.set_value(9, 32.0f);
           defaultSlotBlock.set_value(10, 1.25f);
           defaultSlotBlock.set_value(16, RGBA_OPAQUE_WHITE);
           defaultSlotBlock.set_value(17, RGBA_OPAQUE_BLACK);
           defaultSlotBlock.set_value(20, 400u);
           defaultSlotBlock.set_value<text_bit::LINE_COUNT_BASE, 8>(1u);
           for (u32 i = 0; i < CAPACITY; ++i)
           {
               items[i].uid = 0;
               items[i].type = MediaType::None;
               items[i].tier = 0;
               items[i].baseX = 0.0f;
               items[i].baseY = 0.0f;
               items[i].baseW = 0.0f;
               items[i].baseH = 0.0f;
               staticLayout[i] = SlotStaticLayout{};
               staticLayout[i].topLevelIndex = static_cast<u8>(i);
               tracks[i] = KeyframeTrack{};
               path::PathStringArena::instance().clear_slot(static_cast<u32>(i));
               bassParams[i].clear();
               baseSlotBlocks[i] = defaultSlotBlock;
               activeSlotBlocks[i] = defaultSlotBlock;
               evaluatedSlots[i].clear();
               zShadowBB[i] = 0ULL;
               slotVisibleTiles[i] = 0ULL;
               slotScissorBoxes[i] = Rect<float>{};
               kfPlateauEndMs[i] = 0;
               zRankToSlot_[i] = static_cast<u8>(i);
               slotToZRank_[i] = static_cast<u8>(i);
           }
           zRankCount_ = 0;
           baseBgInsertRank_ = 0;
           overflowValidFromMs = TIME_MAX_SENTINEL_I64;
           overflowValidUntilMs = -1;
           overflowChunkTag = INVALID_U16;
           overflowActiveBgSlot = static_cast<u8>(CAPACITY);
           overflowBaseZobrist = 0ULL;
           overflowKfActiveBB.clear();
           overflowStaticOccludedBB.clear();
           overflowStaticOpaqueTiles = 0ULL;
           overflowActiveBB.clear();
           staticFullscreenBlockersBB = 0ULL;
           staticOccludedSlotsBB = 0ULL;
           lastQueriedChunkStaticOccludedBB = 0ULL;
           lastQueriedChunkStaticOpaqueTiles = 0ULL;
           lastQueriedChunkActiveBgSlot = static_cast<u8>(CAPACITY);
           baseRootBlock.clear();
           baseRootBlock.set_value(0, true);
           baseRootBlock.set_value(0, RectF{0.0f, 0.0f, stageW, stageH});
           baseRootBlock.set_value(5, 1.0f);
           baseRootBlock.set_value(6, 1.0f);
           baseRootBlock.set_value(7, 0.5f);
           baseRootBlock.set_value(8, 0.5f);
           baseRootBlock.set_value(9, 32.0f);
           baseRootBlock.set_value(10, 1.25f);
           baseRootBlock.set_value(16, RGBA_OPAQUE_BLACK);
           baseRootBlock.set_value(17, RGBA_OPAQUE_BLACK);
           baseRootBlock.set_value(20, 400u);
           activeRootBlock = baseRootBlock;
           byTypeBB.clear();
           presentTypesMask = 0;
           byTierBB.clear();
           byModeBB.clear();
           byNatureBB.clear();
           byDynamicAxisBB.clear();
           visualSlotsBB.clear();
           textSlotsBB.clear();
           audioSlotsBB.clear();
           dirtyGenSlotsBB.clear();
           continuousDynBB.clear();
           runningDynamicZobrist = 0ULL;
           lastQueriedChunkBaseZobrist = 0ULL;
           lastQueriedChunkKfActiveBB = 0ULL;
           prevChunkKfActiveBB = 0ULL;
           rampingSlotsBB = 0ULL;
           kfPlateauBB = 0ULL;
           minKfPlateauEndMs = TIME_MAX_SENTINEL_I64;
           evalVisibleBB = 0ULL;
           staticOffscreenBB.clear();
           customOriginBB = 0ULL;
           zBucketCount = 0;
           st = nullptr;
           currentTimeMs = -1;
           projectDurationMs = 0;
           temporalTable.dirty = true;
           temporalTable.validChunkCount = 0;
           styleValidFromMs = TIME_MIN_SENTINEL_I64;
           styleValidUntilMs = TIME_MAX_SENTINEL_I64;
           warmDirtySlotsBB.clear();
            diffAccumulator.clear();
           ++epoch;
           ++topologyEpoch;
           rebuild_dynamic_caches();
       }
        // Updates canonical stage size and recomputes geometry for the already-configured slots.
       inline void set_stage_dimensions(float w, float h)
       {
           stageW = (w > 0.0f) ? w : DEFAULT_STAGE_WIDTH_F32;
           stageH = (h > 0.0f) ? h : DEFAULT_STAGE_HEIGHT_F32;
           for (Mask occ = populated_slots(); occ;)
           {
               recompute_track_mode(occ.pop_lsb());
           }
           rebuild_z_shadows();
           touch();
       }
       // Returns the canonical stage width used by geometry and display-list generation.
       inline float stage_w() const { return stageW; }
       // Returns the canonical stage height used by geometry and display-list generation.
       inline float stage_h() const { return stageH; }
        // Stores one normalized item and updates its fixed bitboard memberships.
       inline void put_item(u32 slot, const TimelineItem &item)
       {
           if (byTypeBB[ALL_ITEMS].test_bit(slot))
               remove_item(slot);
           evalVisibleBB.clear_bit(slot);
           kfPlateauBB.clear_bit(slot);
           items[slot] = item;
           staticLayout[slot].maskFilter = (item.type == MediaType::Image) ? image_mask_filter(item) : 0u;
           staticLayout[slot].rotateFlags = static_cast<u16>(fast_fmod360(item.rotateDeg));
           if (items[slot].tier >= MEDIA_TIER_NB)
           {
               items[slot].tier = 0; // không bao giờ rơi khỏi bitboard
           }
           const u32 typeIdx = static_cast<u32>(item.type);
           byTypeBB[ALL_ITEMS].set_bit(slot);
           if (typeIdx < MEDIA_TYPE_NB)
           {
               byTypeBB[typeIdx].set_bit(slot);
               presentTypesMask |= static_cast<u16>(1u << typeIdx);
           }
           if (item.type != MediaType::Audio && item.type != MediaType::None)
           {
               visualSlotsBB.set_bit(slot);
               dirtyGenSlotsBB.set_bit(slot);
           }
           if (item.type == MediaType::Text || item.type == MediaType::Lyric)
           {
               textSlotsBB.set_bit(slot);
           }
           if (item.type == MediaType::Audio)
           {
               audioSlotsBB.set_bit(slot);
           }
           if (item.type == MediaType::Video || item.type == MediaType::Audio)
           {
               continuousDynBB.set_bit(slot);
           }
           const u32 tierIdx = static_cast<u32>(items[slot].tier);
           if (tierIdx < MEDIA_TIER_NB)
           {
               byTierBB[tierIdx].set_bit(slot);
           }
           recompute_track_mode(slot);
           insert_z_order_slot(slot);
           rebuild_z_shadows();
           recompute_slot_static_occlusion(slot);
           touch();
       }
        // Removes one slot and clears its media, tier, mode, text, and bass state.
       inline void remove_item(u32 slot)
       {
           const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
        remove_z_order_slot(slot);
           visualSlotsBB.clear_bit(slot);
           dirtyGenSlotsBB.clear_bit(slot);
           textSlotsBB.clear_bit(slot);
           audioSlotsBB.clear_bit(slot);
           continuousDynBB.clear_bit(slot);
           const u32 typeIdx = static_cast<u32>(items[slot].type);
           if (typeIdx < MEDIA_TYPE_NB)
           {
               byTypeBB[typeIdx].clear_bit(slot);
               if (byTypeBB[typeIdx].empty())
               {
                   presentTypesMask &= static_cast<u16>(~(1u << typeIdx));
               }
           }
           byTypeBB[ALL_ITEMS].clear_bit(slot);
           const u32 tierIdx = static_cast<u32>(items[slot].tier);
           if (tierIdx < MEDIA_TIER_NB)
           {
               byTierBB[tierIdx].clear_bit(slot);
           }
           for (u32 m = 0; m < TRACK_MODE_NB; ++m)
           {
               byModeBB[m].clear_bit(slot);
           }
           for (u32 n = 0; n < DYNAMIC_NATURE_NB; ++n)
           {
               byNatureBB[n].clear_bit(slot);
           }
           for (u32 a = 0; a < DYNAMIC_AXIS_NB; ++a)
           {
               byDynamicAxisBB[a].clear_bit(slot);
           }
           items[slot].type = MediaType::None;
           items[slot].uid = 0;
           evalVisibleBB.clear_bit(slot);
           customOriginBB.clear_bit(slot);
           kfPlateauBB.clear_bit(slot);
           staticLayout[slot] = SlotStaticLayout{};
           staticLayout[slot].topLevelIndex = static_cast<u8>(s);
           tracks[slot] = KeyframeTrack{};
           baseSlotBlocks[slot].clear(); activeSlotBlocks[slot].clear();
           path::PathStringArena::instance().clear_slot(slot);
           bassParams[slot].clear();
           rebuild_z_shadows();
           touch();
       }
        // Configures bass analysis cadence, palette size, and activation for one slot.
       inline void set_bass_params(u32 slot, float hop, u32 colorCount, u32 isBassActive)
       {
           const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
        bassParams[slot].hop = (hop > 0.0f) ? hop : DEFAULT_BASS_HOP_F32;
           bassParams[slot].colorCount = (colorCount <= MAX_BASS_COLORS) ? colorCount : MAX_BASS_COLORS;
           bassParams[slot].isBassActive = isBassActive;
           recompute_track_mode(slot);
           touch();
       }
        // Writes one background/text palette pair into the slot's fixed color arrays.
       inline void set_bass_color(u32 slot, u32 colorIdx, u32 bgRgba, u32 textRgba)
       {
           const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
        const u32 c = colorIdx & (MAX_BASS_COLORS - 1u);
        bassParams[slot].colorsBg[colorIdx] = bgRgba;
           bassParams[slot].colorsText[colorIdx] = textRgba;
           touch();
       }
        // Stores hit-index data in the preallocated shared native hit arena.
       inline void set_bass_hit_data(u32 slot, const u16 *hits, u32 count)
       {
           if (!hits) return;
        const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
        const u32 n = (count <= MAX_BASS_HITS) ? count : MAX_BASS_HITS;
           bassParams[slot].hitCount = n;
           u16 *dst = g_bassHits[slot].data();
           for (const u16 *src = hits, *const srcEnd = hits + n; src < srcEnd; ++src)
           {
               *dst++ = *src;
           }
           touch();
       }
        // Reports whether the requested fixed slot is configured for bass reactivity.
       inline bool is_bass_active(u32 slot) const noexcept
       {
           return bassParams[slot & (MAX_TIMELINE_SLOTS - 1u)].isBassActive != 0;
       }
        // Maps timeline time to a palette index using the shared hit array and configured hop.
       inline u32 resolve_bass_hit_index(u32 slot, TimePoint nowMs) const
       {
           const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
        const BassBeatParams &bp = bassParams[slot];
           if (!bp.isBassActive || bp.colorCount == 0)
               return 0;
           const auto* script = get_active_music_script();
           if (script && script->totalSamples > 0)
           {
               const u32 curHit = script->sample_hit_index(static_cast<double>(nowMs));
               const TimePoint startMs = items[slot].startMs;
               const u32 startHit = (startMs > 0) ? script->sample_hit_index(static_cast<double>(startMs)) : 0;
               const u32 relHit = (curHit >= startHit) ? (curHit - startHit) : 0;
               return relHit % bp.colorCount;
           }
           if (bp.hop <= 0.0f)
               return 0;
           const float hop = bp.hop;
           const u32 n = bp.hitCount;
           if (n == 0)
               return 0;
           const u32 curPos = (nowMs > 0) ? static_cast<u32>((static_cast<double>(nowMs) / MS_PER_SECOND_F64) / static_cast<double>(hop)) : 0;
           const TimePoint startMs = items[slot].startMs;
           const u32 startPos = (startMs > 0) ? static_cast<u32>((static_cast<double>(startMs) / MS_PER_SECOND_F64) / static_cast<double>(hop)) : 0;
           const u32 curHit = (curPos < n) ? g_bassHits[slot][curPos] : g_bassHits[slot][n - 1];
           const u32 startHit = (startPos < n) ? g_bassHits[slot][startPos] : g_bassHits[slot][n - 1];
           const u32 relHit = (curHit >= startHit) ? (curHit - startHit) : 0;
           return relHit % bp.colorCount;
       }
        // Resolves a slot's current bass-reactive background color or its base resource color.
       inline u32 resolve_bass_bg_color(u32 slot, TimePoint nowMs) const
        {
            const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
            const BassBeatParams &bp = bassParams[slot];
            if (!bp.isBassActive || bp.colorCount == 0)
            {
                const auto &st = activeStyleEnv.targets[static_cast<u32>(MediaType::Background)];
                if ((items[slot].flags & 128u) == 0 && st.template get_value<bool, 0, 1>())
                    return st.template get_value<u32, text_reg::COLOR>();
                return items[slot].resourceId;
            }
            const u32 idx = resolve_bass_hit_index(slot, nowMs);
            return bp.colorsBg[idx];
        }
        inline u32 resolve_bass_text_color(u32 slot, TimePoint nowMs) const
       {
           const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
        const BassBeatParams &bp = bassParams[slot];
           if (!bp.isBassActive || bp.colorCount == 0)
               return activeSlotBlocks[slot].template get_value<u32, text_reg::COLOR>();
           const u32 idx = resolve_bass_hit_index(slot, nowMs);
           return bp.colorsText[idx];
       }
        // Background active trên cùng tại một tập activeBB (dùng chung cho Cold-Path & Hot-Path Fallback)
       inline u8 eval_active_bg_from_bb(Mask activeBB) const noexcept
       {
           Mask bb = activeBB & byTypeBB[static_cast<u32>(MediaType::Background)];
           u32 best = static_cast<u32>(CAPACITY);
           i64 bestKey = -1;
           bb.for_each_active([this, &best, &bestKey](u32 s) {
               const TimelineItem &it = items[s];
               const i64 key = ((static_cast<i64>(it.zIndex) + Z_INDEX_SORT_BIAS) << 6) | static_cast<i64>(s);
               const i64 take = static_cast<i64>(key > bestKey);
               const i64 mask = -take;
               best = static_cast<u32>((static_cast<i64>(s) & mask) | (static_cast<i64>(best) & ~mask));
               bestKey = (key & mask) | (bestKey & ~mask);
           });
           return static_cast<u8>(best);
       }
        // Background active trên cùng tại nowMs (zIndex lớn nhất; hòa -> slot lớn hơn).
       inline u32 active_background_slot(TimePoint nowMs) const
       {
           if (lastQueriedChunkTag != INVALID_U16 && nowMs == currentTimeMs)
               return static_cast<u32>(lastQueriedChunkActiveBgSlot);
           if (st && nowMs == currentTimeMs)
           {
               const Mask bgActive = st->activeBB & byTypeBB[static_cast<u32>(MediaType::Background)];
               return static_cast<u32>(eval_active_bg_from_bb(bgActive));
           }
           Mask bgCandidates = byTypeBB[static_cast<u32>(MediaType::Background)];
           Mask bgActive{};
           bgCandidates.for_each_active([this, nowMs, &bgActive](u32 s) {
               const TimelineItem &it = items[s];
               const bool inRange = (nowMs >= it.startMs) & (nowMs < it.endMs);
               bgActive.set_bit(s, inRange);
           });
           return static_cast<u32>(eval_active_bg_from_bb(bgActive));
       }
        // Màu fill runtime theo kiến trúc bộ nhớ vật lý: Đọc O(1) từ L1 cache không rẽ nhánh
        [[nodiscard]] inline ColorRGBA resolve_slot_color(u32 slot, TimePoint nowMs) const noexcept
        {
            const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
            const BassBeatParams &bp = bassParams[s];
            if (__builtin_expect(bp.isBassActive && bp.colorCount, 0))
            {
                const u32 packed = (items[s].type == MediaType::Background)
                    ? bp.colorsBg[resolve_bass_hit_index(s, nowMs)]
                    : bp.colorsText[resolve_bass_hit_index(s, nowMs)];
                return ColorRGBA::from_packed(packed);
            }
            return ColorRGBA::from_packed(activeSlotBlocks[s].template get_value<u32, 4>(16));
        }

        // Màu chữ runtime: Đọc O(1) từ L1 cache, triệt tiêu 100% rẽ nhánh và quét background tìm cha
        inline u32 resolve_text_color(u32 slot, TimePoint nowMs) const noexcept
        {
            const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
            const BassBeatParams &bp = bassParams[s];
            if (__builtin_expect(bp.isBassActive && bp.colorCount, 0))
                return bp.colorsText[resolve_bass_hit_index(s, nowMs)];

            const MediaType type = items[s].type;
            const u32 typeIdx = static_cast<u32>(type);
            if (typeIdx < MEDIA_TYPE_NB)
            {
                const auto &st = activeStyleEnv.targets[typeIdx];
                if (st.has_prop(text_reg::COLOR)) return st.template get_value<u32, text_reg::COLOR>();
            }
            if (type == MediaType::Lyric)
            {
                const auto &stText = activeStyleEnv.targets[static_cast<u32>(MediaType::Text)];
                if (stText.has_prop(text_reg::COLOR)) return stText.template get_value<u32, text_reg::COLOR>();
            }
            else if (type == MediaType::Text)
            {
                const auto &stLyric = activeStyleEnv.targets[static_cast<u32>(MediaType::Lyric)];
                if (stLyric.has_prop(text_reg::COLOR)) return stLyric.template get_value<u32, text_reg::COLOR>();
            }
            const u32 tc = activeSlotBlocks[s].template get_value<u32, text_reg::COLOR>();
            return (tc != 0u) ? tc : RGBA_OPAQUE_WHITE;
        }
        [[nodiscard]] inline u32 resolve_text_bg_color(u32 slot) const noexcept
        {
            const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
            const MediaType type = items[s].type;
            const u32 typeIdx = static_cast<u32>(type);
            if (typeIdx < MEDIA_TYPE_NB)
            {
                const auto &st = activeStyleEnv.targets[typeIdx];
                if (st.has_prop(text_reg::BG_COLOR)) return st.template get_value<u32, text_reg::BG_COLOR>();
            }
            return activeSlotBlocks[s].template get_value<u32, text_reg::BG_COLOR>();
        }
        [[nodiscard]] inline u32 resolve_path_fill_color(u32 slot, TimePoint nowMs) const noexcept
        {
            (void)nowMs;
            const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
            const auto &st = activeStyleEnv.targets[static_cast<u32>(MediaType::Path)];
            if (__builtin_expect(st.has_prop(text_reg::COLOR), 1)) return st.template get_value<u32, text_reg::COLOR>();
            if (!activeSlotBlocks[s].template get_value<bool>(path_bit::HAS_FILL)) return 0u;
            return activeSlotBlocks[s].template get_value<u32, 4>(path_reg::EFFECTIVE_FILL_COL);
        }
        [[nodiscard]] inline u32 resolve_path_stroke_color(u32 slot, TimePoint nowMs) const noexcept
        {
            (void)nowMs;
            const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
            const auto &st = activeStyleEnv.targets[static_cast<u32>(MediaType::Path)];
            if (st.has_prop(text_reg::STROKE_COLOR)) return st.template get_value<u32, text_reg::STROKE_COLOR>();
            if (!activeSlotBlocks[s].template get_value<bool>(path_bit::HAS_STROKE)) return 0u;
            return activeSlotBlocks[s].template get_value<u32, 4>(path_reg::EFFECTIVE_STROKE_C);
        }
        [[nodiscard]] inline float resolve_path_stroke_width(u32 slot, TimePoint nowMs) const noexcept
        {
            (void)nowMs;
            const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
            const auto &st = activeStyleEnv.targets[static_cast<u32>(MediaType::Path)];
            if (st.has_prop(text_reg::STROKE_WIDTH)) return st.template get_value<float, text_reg::STROKE_WIDTH>();
            if (!activeSlotBlocks[s].template get_value<bool>(path_bit::HAS_STROKE)) return 0.0f;
            return activeSlotBlocks[s].template get_value<float, 4>(path_reg::EFFECTIVE_STROKE_W);
        }
        inline const BassBeatParams &get_bass_params(u32 slot) const
       {
           return bassParams[slot & (MAX_TIMELINE_SLOTS - 1u)];
       }
        // Stores normalized text layout/reveal parameters and refreshes slot specialization.
       inline void set_slot_text_block(u32 slot, const TextBlock &b)
       {
           const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
           baseSlotBlocks[s] = b;
           activeSlotBlocks[s] = b;
           recompute_track_mode(slot);
           touch();
       }
        // Returns immutable native text parameters for the requested slot.
       inline const KeyframeTrack &get_track(u32 slot) const
       {
           return tracks[slot & (MAX_TIMELINE_SLOTS - 1u)];
       }
       inline const TextBlock &slot_text_block(u32 slot) const noexcept
       {
           return activeSlotBlocks[slot & (MAX_TIMELINE_SLOTS - 1u)];
       }
        inline void set_text_reveal_timing(
           u32 slot, double durationMs, double staggerMs
       )
       {
           const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
           baseSlotBlocks[s].template set_value<text_reg::REVEAL_DURATION_MS>(static_cast<float>(durationMs));
           baseSlotBlocks[s].template set_value<text_reg::REVEAL_STAGGER_MS>(static_cast<float>(staggerMs));
           activeSlotBlocks[s].template set_value<text_reg::REVEAL_DURATION_MS>(static_cast<float>(durationMs));
           activeSlotBlocks[s].template set_value<text_reg::REVEAL_STAGGER_MS>(static_cast<float>(staggerMs));
           touch();
       }
        inline double get_text_reveal_duration_ms(u32 slot) const
       {
           return static_cast<double>(activeSlotBlocks[slot & (MAX_TIMELINE_SLOTS - 1u)].template get_value<float, text_reg::REVEAL_DURATION_MS>());
       }
        inline double get_text_reveal_stagger_ms(u32 slot) const
       {
           return static_cast<double>(activeSlotBlocks[slot & (MAX_TIMELINE_SLOTS - 1u)].template get_value<float, text_reg::REVEAL_STAGGER_MS>());
       }
        // Exposes the slot blocks arena for zero-copy host reads.
       inline const TextBlock *text_params_arena() const noexcept
       {
           return activeSlotBlocks.data();
       }

        // Writes one normalized keyframe directly into the unified KeyframeArena SoA Hot Stream (C-ABI / legacy shim).
       inline void set_keyframe(u32 slot, u32 kfIdx, i32 offsetMs, float opacity, float x, float y, float w, float h, float rotateDeg, float scale)
       {
           const u32 s = slot & (CAPACITY - 1u);
           const u32 k = (kfIdx < MAX_KEYFRAMES_PER_SLOT) ? kfIdx : (MAX_KEYFRAMES_PER_SLOT - 1u);
           KeyframeTrack &hdr = tracks[s];

           if (hdr.spatialOffset == INVALID_U32 ||
               (hdr.flags & KF_TRACK_FLAG_FROM_LEGACY) == 0u ||
               !keyframeArena.buffer)
           {
               const u32 prevCount = hdr.count;
               const u32 prevContentMask = hdr.contentMask;
               const u8 prevEasing = hdr.easing;
               const u8 prevHasSpatial = hdr.hasSpatial;
               const u64 prevKfProps = hdr.keyframedProps;
               constexpr usize maxSentinelSlots = MAX_KEYFRAMES_PER_SLOT + 2u;
               u32 kfOff = INVALID_U32;
               u8 *kfMem = keyframeArena.alloc_aligned(maxSentinelSlots * sizeof(NativeKeyframe), alignof(NativeKeyframe), kfOff);
               u32 invOff = INVALID_U32;
               u8 *invMem = keyframeArena.alloc_aligned(maxSentinelSlots * sizeof(double), alignof(double), invOff);
               if (!kfMem || !invMem)
                   return;
               std::memset(kfMem, 0, maxSentinelSlots * sizeof(NativeKeyframe));
               std::memset(invMem, 0, maxSentinelSlots * sizeof(double));
               hdr = KeyframeTrack{};
               hdr.spatialOffset = kfOff;
               hdr.invSpanOffset = invOff;
               hdr.styleColorOffset = INVALID_U32;
               hdr.count = prevCount;
               hdr.contentMask = prevContentMask;
               hdr.easing = prevEasing;
               hdr.hasSpatial = prevHasSpatial;
               hdr.keyframedProps = prevKfProps;
               hdr.flags = KF_TRACK_FLAG_FROM_LEGACY;
           }

           NativeKeyframe *arr = reinterpret_cast<NativeKeyframe *>(keyframeArena.buffer.get() + hdr.spatialOffset);
           arr[k + 1u] = NativeKeyframe{offsetMs, opacity, x, y, w, h, rotateDeg, scale};
           if (k + 1u > hdr.count)
               hdr.count = static_cast<u16>(k + 1u);
           bake_track_sentinels_impl(s, true);
           touch();
       }

       // Lưu trữ trực tiếp dòng Keyframe SoA vào KeyframeArena (Zero intermediate comp::Keyframe copy):
       // - Hot Spatial Stream (32B/kf): NativeKeyframe[0..n+1] + double invSpan[0..n+1]
       // - Conditional StyleColor Stream (48B/kf): CHỈ cấp phát khi cMask != 0!
       inline void set_staged_keyframes(
           u32 slot,
           const NativeKeyframe* inSpatial,
           const StyleColorKeyframe* inStyleColor,
           u32 count,
           u32 easing,
           const NativeKeyframe& baseSpatial,
           const StyleColorKeyframe& baseStyleColor,
           u8 cMask,
           u64 kfPropsWord0 = 0ULL
       ) noexcept
       {
           const u32 s = slot & (CAPACITY - 1u);
           KeyframeTrack& hdr = tracks[s];
           const u32 prevContentMask = hdr.contentMask;
           const u8 prevHasSpatial = hdr.hasSpatial;
           hdr = KeyframeTrack{};
           hdr.easing = static_cast<u8>(easing & (EASING_CURVE_NB - 1u));
           hdr.contentMask = prevContentMask;
           hdr.hasSpatial = prevHasSpatial;
           if (!inSpatial || count == 0) {
               return;
           }
           const u32 n = (count <= MAX_KEYFRAMES_PER_SLOT) ? count : MAX_KEYFRAMES_PER_SLOT;
           hdr.count = static_cast<u16>(n);

           const usize kfCountWithSentinels = static_cast<usize>(n) + 2u;
           u32 kfOff = INVALID_U32;
           u8* kfMem = keyframeArena.alloc_aligned(kfCountWithSentinels * sizeof(NativeKeyframe), alignof(NativeKeyframe), kfOff);
           u32 invOff = INVALID_U32;
           u8* invMem = keyframeArena.alloc_aligned(kfCountWithSentinels * sizeof(double), alignof(double), invOff);
           if (!kfMem || !invMem) {
               hdr = KeyframeTrack{};
               return;
           }
           NativeKeyframe* sentinelArr = reinterpret_cast<NativeKeyframe*>(kfMem);
           double* invArr = reinterpret_cast<double*>(invMem);

           for (u32 i = 0; i < n; ++i) {
               sentinelArr[i + 1u] = inSpatial[i];
           }
           sentinelArr[0] = (n > 0) ? sentinelArr[1] : baseSpatial;
           sentinelArr[0].offsetMs = KF_GATEKEEPER_MIN_I32;
           sentinelArr[n + 1u] = (n > 0) ? sentinelArr[n] : baseSpatial;
           sentinelArr[n + 1u].offsetMs = KF_GATEKEEPER_MAX_I32;

           invArr[0] = 0.0;
           invArr[n + 1u] = 0.0;
           for (u32 k = 0; k < n; ++k) {
               if (k + 1u < n) {
                   const i32 span = sentinelArr[k + 2u].offsetMs - sentinelArr[k + 1u].offsetMs;
                   invArr[k + 1u] = (span > 0) ? (1.0 / static_cast<double>(span)) : 0.0;
               } else {
                   invArr[k + 1u] = 0.0;
               }
           }

           hdr.spatialOffset = kfOff;
           hdr.invSpanOffset = invOff;
           hdr.activeColorKfMask = cMask;
           if (cMask != 0 && inStyleColor) {
               u32 scOff = INVALID_U32;
               u8* scMem = keyframeArena.alloc_aligned(
                   kfCountWithSentinels * sizeof(StyleColorKeyframe), alignof(StyleColorKeyframe), scOff);
               if (scMem) {
                   StyleColorKeyframe* scArr = reinterpret_cast<StyleColorKeyframe*>(scMem);
                   for (u32 i = 0; i < n; ++i) {
                       scArr[i + 1u] = inStyleColor[i];
                   }
                   scArr[0] = (n > 0) ? scArr[1] : baseStyleColor;
                   scArr[n + 1u] = (n > 0) ? scArr[n] : baseStyleColor;
                   hdr.styleColorOffset = scOff;
               }
           }

           hdr.keyframedProps = kfPropsWord0;
           hdr.lastTypedIdx = 0;
           hdr.lastContentIdx = 0;
           if (hdr.activeColorKfMask != 0) {
               hdr.hasSpatial = 1u;
           }
           recompute_track_mode(s);
           touch();
       }

       [[nodiscard]] inline const KeyframeTrack& typed_kf_header(u32 slot) const noexcept {
           return tracks[slot & (CAPACITY - 1u)];
       }

       [[nodiscard]] inline i32 keyframe_offset_ms(u32 slot, u32 kfIdx) const noexcept
       {
           const u32 s = slot & (CAPACITY - 1u);
           const KeyframeTrack &tr = tracks[s];
           if (tr.spatialOffset == INVALID_U32 || kfIdx >= tr.count || !keyframeArena.buffer)
               return 0;
           const NativeKeyframe *arr = reinterpret_cast<const NativeKeyframe *>(keyframeArena.buffer.get() + tr.spatialOffset);
           return arr[kfIdx + 1u].offsetMs;
       }

       [[nodiscard]] inline NativeKeyframe get_keyframe_view(u32 slot, u32 kfIdx) const noexcept
       {
           const u32 s = slot & (CAPACITY - 1u);
           const KeyframeTrack &tr = tracks[s];
           if (tr.spatialOffset == INVALID_U32 || kfIdx >= tr.count || !keyframeArena.buffer)
               return NativeKeyframe{};
           const NativeKeyframe *arr = reinterpret_cast<const NativeKeyframe *>(keyframeArena.buffer.get() + tr.spatialOffset);
           return arr[kfIdx + 1u];
       }

       // Duy nhất 1 hàm nội suy Hot Path 32-byte/frame SoA cho MỌI MediaType (0 memset stack, 0 template 9x bloat)
       inline bool evaluate_unified_kinematic(
           u32 slot,
           i32 relMs,
           Rect<float>& outBox,
           float& outOpacity,
           float& outRotate,
           u32& outFillPacked,
           float& outBlur,
           bool& outRamping,
           TimePoint* outPlateauRelEndMs = nullptr
       ) const noexcept
       {
           const u32 s = slot & (CAPACITY - 1u);
           const KeyframeTrack& hdr = tracks[s];
           if (__builtin_expect(hdr.spatialOffset == INVALID_U32 || hdr.count == 0 || !keyframeArena.buffer, 0)) {
               return false;
           }
           const NativeKeyframe* base = reinterpret_cast<const NativeKeyframe*>(keyframeArena.buffer.get() + hdr.spatialOffset);
           const double* invSpans = reinterpret_cast<const double*>(keyframeArena.buffer.get() + hdr.invSpanOffset);
           const NativeKeyframe* kf = Sentinel<NativeKeyframe, 0>::step_raw_moment(
               base, hdr.lastTypedIdx, relMs, [](const NativeKeyframe& k) noexcept { return k.offsetMs; });
           const u32 rawIdx = hdr.lastTypedIdx;

           const double invSpan = invSpans[rawIdx];
           if (invSpan == 0.0) {
               outOpacity = kf->opacity;
               outRotate = kf->rotateDeg;
               outBox = {kf->x, kf->y, kf->x + kf->w, kf->y + kf->h};
               if (__builtin_expect(hdr.activeColorKfMask != 0 && hdr.styleColorOffset != INVALID_U32, 0)) {
                   const StyleColorKeyframe* sc =
                       reinterpret_cast<const StyleColorKeyframe*>(keyframeArena.buffer.get() + hdr.styleColorOffset) + rawIdx;
                   if (hdr.activeColorKfMask & 1u) outFillPacked = sc->fillColor.to_packed();
                   if (hdr.activeColorKfMask & 4u) outBlur = sc->blurRadius;
               }
               outRamping = false;
               if (outPlateauRelEndMs) *outPlateauRelEndMs = static_cast<TimePoint>((kf + 1)->offsetMs);
               return true;
           }
           outRamping = true;
           const NativeKeyframe& k1 = *(kf + 1);
           const float linear = static_cast<float>(static_cast<double>(relMs - kf->offsetMs) * invSpan);
           const u32 curve = static_cast<u32>(hdr.easing & (EASING_CURVE_NB - 1u));
           const u32 quadMode = curve + static_cast<u32>((curve == 3u) & (linear >= 0.5f));
           const u64 hornerKey = (static_cast<u64>(epoch) << 32) |
                                 (static_cast<u64>(rawIdx) << 4) |
                                 static_cast<u64>(quadMode);
           auto& hm = slotHorner[s];
           if (__builtin_expect(slotHornerKey[s] != hornerKey, 0)) {
               hm.trackCount = 0u;
               hm.bake_quad_mode(quadMode, static_cast<u16>(s), 0u, kf->x, k1.x);
               hm.bake_quad_mode(quadMode, static_cast<u16>(s), 1u, kf->y, k1.y);
               hm.bake_quad_mode(quadMode, static_cast<u16>(s), 2u, kf->w, k1.w);
               hm.bake_quad_mode(quadMode, static_cast<u16>(s), 3u, kf->h, k1.h);
               hm.bake_quad_mode(quadMode, static_cast<u16>(s), 4u, kf->opacity, k1.opacity);
               hm.bake_quad_mode(quadMode, static_cast<u16>(s), 5u, kf->rotateDeg, k1.rotateDeg);
               hm.bake_quad_mode(quadMode, static_cast<u16>(s), 6u, 0.0f, 1.0f); // exact eased t in lane 6
               hm.bake_quad_mode(quadMode, static_cast<u16>(s), 7u, 0.0f, 0.0f);
               slotHornerKey[s] = hornerKey;
           }
           alignas(16) MultiArray<float, 8> evVals;
           hm.template eval_tau_fixed<2u>(linear, evVals.data());
           outBox = {evVals[0], evVals[1], evVals[0] + evVals[2], evVals[1] + evVals[3]};
           outOpacity = evVals[4];
           outRotate = evVals[5];
           if (__builtin_expect(hdr.activeColorKfMask != 0 && hdr.styleColorOffset != INVALID_U32, 0)) {
               const float t = evVals[6];
               const StyleColorKeyframe* scArr =
                   reinterpret_cast<const StyleColorKeyframe*>(keyframeArena.buffer.get() + hdr.styleColorOffset);
               const StyleColorKeyframe& s0 = scArr[rawIdx];
               const StyleColorKeyframe& s1 = scArr[rawIdx + 1u];
               if (hdr.activeColorKfMask & 1u) outFillPacked = s0.fillColor.lerp(s1.fillColor, t).to_packed();
               if (hdr.activeColorKfMask & 4u) outBlur = s0.blurRadius + t * (s1.blurRadius - s0.blurRadius);
           }
           return true;
       }
       inline void set_slot_custom_origin(u32 slot, float ox, float oy) noexcept
       {
           const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
           staticLayout[s].originX = ox;
           staticLayout[s].originY = oy;
           staticLayout[s].hasCustomOrigin = 1u;
           customOriginBB.set_bit(s);
           touch();
       }
       inline bool get_slot_custom_origin(u32 slot, float &ox, float &oy) const noexcept
       {
           const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
           if (!staticLayout[s].hasCustomOrigin) return false;
           ox = staticLayout[s].originX;
           oy = staticLayout[s].originY;
           return true;
       }
       inline void bake_track_sentinels_impl(u32 slot, bool rebakeLegacy) noexcept
       {
           const u32 s = slot & (CAPACITY - 1u);
           KeyframeTrack &tr = tracks[s];
           const u32 n = (tr.count <= MAX_KEYFRAMES_PER_SLOT) ? tr.count : MAX_KEYFRAMES_PER_SLOT;
           if (n == 0 || tr.spatialOffset == INVALID_U32 || !keyframeArena.buffer)
           {
               tr.lastTypedIdx = 0;
               tr.lastContentIdx = 0;
               return;
           }
           if (tr.lastTypedIdx > n) tr.lastTypedIdx = 0;
           if (tr.lastContentIdx > n) tr.lastContentIdx = 0;
           if (!rebakeLegacy)
               return;

           const TimelineItem &it = items[s];
           NativeKeyframe *arr = reinterpret_cast<NativeKeyframe *>(keyframeArena.buffer.get() + tr.spatialOffset);
           double *invArr = reinterpret_cast<double *>(keyframeArena.buffer.get() + tr.invSpanOffset);

           if (tr.flags & KF_TRACK_FLAG_FROM_LEGACY)
           {
               for (u32 k = 0; k < n; ++k)
               {
                   NativeKeyframe &f = arr[k + 1u];
                   f.w = (f.w > 0.0f || f.scale == 0.0f) ? f.w : it.baseW;
                   f.h = (f.h > 0.0f || f.scale == 0.0f) ? f.h : it.baseH;
               }
           }
           arr[0] = arr[1];
           arr[0].offsetMs = KF_GATEKEEPER_MIN_I32;
           arr[n + 1u] = arr[n];
           arr[n + 1u].offsetMs = KF_GATEKEEPER_MAX_I32;

           invArr[0] = 0.0;
           invArr[n + 1u] = 0.0;
           for (u32 k = 0; k < n; ++k)
           {
               if (k + 1u < n)
               {
                   const i32 span = arr[k + 2u].offsetMs - arr[k + 1u].offsetMs;
                   invArr[k + 1u] = (span > 0) ? (1.0 / static_cast<double>(span)) : 0.0;
               }
               else
               {
                   invArr[k + 1u] = 0.0;
               }
           }
       }
       inline void bake_track_sentinels(u32 slot) noexcept
       {
           const u32 s = slot & (CAPACITY - 1u);
           bake_track_sentinels_impl(s, (tracks[s].flags & KF_TRACK_FLAG_FROM_LEGACY) != 0u);
       }
        // Sets keyframe count/easing/content metadata and recomputes the slot's track mode.
       inline void set_track_info(u32 slot, u32 count, u32 easing, u32 contentMask = 0, u32 hasSpatial = 1, u64 kfProps = 0ULL)
       {
           const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
           KeyframeTrack &tr = tracks[s];
           tr.count = static_cast<u16>((count <= MAX_KEYFRAMES_PER_SLOT) ? count : MAX_KEYFRAMES_PER_SLOT);
           tr.easing = static_cast<u8>(easing & (EASING_CURVE_NB - 1u));
           tr.contentMask = contentMask;
           tr.hasSpatial = static_cast<u8>((hasSpatial != 0u || tr.activeColorKfMask != 0u) ? 1u : 0u);
           if (kfProps != 0ULL)
           {
               tr.keyframedProps = kfProps;
           }
           recompute_track_mode(s);
           bake_track_sentinels(s);
           staticLayout[s].staticZobristKey = compute_slot_static_zobrist_key(s);
           recompute_slot_static_occlusion(s);
           touch();
       }
        // ------------------------------------------------------------------------
       // C++20 <T> Template Specialization cho Tính Toán Hình Học Từng MediaType
       // ------------------------------------------------------------------------
       // Updates a text or lyric slot's geometry with real shaped bounds from font metrics.
       inline void update_slot_text_bounds(u32 slot, float realW, float realH)
       {
           if (realW <= 0.0f || realH <= 0.0f)
               return;
           const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
           TimelineItem &it = items[s];
           if (it.type != MediaType::Text && it.type != MediaType::Lyric)
               return;
            const float anchorX = activeSlotBlocks[s].template get_value<float, text_reg::ANCHOR_X>();
            const float anchorY = activeSlotBlocks[s].template get_value<float, text_reg::ANCHOR_Y>();
           // Reconstruct the authoring origin point:
           const float originX = it.baseX + anchorX * it.baseW;
           const float originY = it.baseY + anchorY * it.baseH;
           const bool hasExplicitWidth = (it.flags & ITEM_FLAG_EXPLICIT_WIDTH) != 0;
           const bool hasExplicitHeight = (it.flags & ITEM_FLAG_EXPLICIT_HEIGHT) != 0;
           const float effW = hasExplicitWidth ? it.baseW : realW;
           const float effH = hasExplicitHeight ? it.baseH : realH;
           // Re-anchor to the real shaped bounds:
           const float newX0 = originX - anchorX * effW;
           const float newY0 = originY - anchorY * effH;
           constexpr float TEXT_BOUNDS_EPSILON_F32 = 0.05f;
           if (abs_val(effW - it.baseW) < TEXT_BOUNDS_EPSILON_F32 &&
               abs_val(effH - it.baseH) < TEXT_BOUNDS_EPSILON_F32 &&
               abs_val(newX0 - it.baseX) < TEXT_BOUNDS_EPSILON_F32 &&
               abs_val(newY0 - it.baseY) < TEXT_BOUNDS_EPSILON_F32)
           {
               return;
           }
           const float oldBaseW = it.baseW;
           const float oldBaseH = it.baseH;
           it.baseX = newX0;
           it.baseY = newY0;
           it.baseW = effW;
           it.baseH = effH;
           staticLayout[s].aabb = {newX0, newY0, newX0 + effW, newY0 + effH};
           evaluatedSlots[s].aabb = staticLayout[s].aabb;
           KeyframeTrack &tr = tracks[s];
           if (tr.has_arena_frames() && keyframeArena.buffer)
           {
               const u32 n = (tr.count <= MAX_KEYFRAMES_PER_SLOT) ? tr.count : MAX_KEYFRAMES_PER_SLOT;
               NativeKeyframe *arr = reinterpret_cast<NativeKeyframe *>(keyframeArena.buffer.get() + tr.spatialOffset);
               for (u32 k = 0; k < n; ++k)
               {
                   NativeKeyframe &f = arr[k + 1u];
                   if (f.w <= 0.0f || abs_val(f.w - oldBaseW) < TEXT_BOUNDS_EPSILON_F32)
                       f.w = effW;
                   if (f.h <= 0.0f || abs_val(f.h - oldBaseH) < TEXT_BOUNDS_EPSILON_F32)
                       f.h = effH;
               }
               bake_track_sentinels_impl(s, true);
           }
           staticLayout[s].staticZobristKey = compute_slot_static_zobrist_key(s);
           recompute_slot_static_occlusion(s);
           touch();
       }
        // Initializes canonical geometry and selects the compile-time media track path.
       template <MediaType Type>
       inline void init_slot_geometry_T(u32 slot)
       {
           const TimelineItem &it = items[slot];
           if constexpr (Type == MediaType::None)
           {
               staticLayout[slot].mode = TrackMode::Static;
               return;
           }
           else
           {
               staticLayout[slot].aabb = {it.baseX, it.baseY, it.baseX + it.baseW, it.baseY + it.baseH};
           }
            const KeyframeTrack &tr = tracks[slot];
            const bool hasTypedDynamics = (tr.count > 1 && tr.activeColorKfMask != 0);
            if ((tr.count > 1 && tr.hasSpatial != 0) || hasTypedDynamics)
            {
                staticLayout[slot].mode = TrackMode::Kinematic;
                byModeBB[static_cast<u32>(TrackMode::Kinematic)].set_bit(slot);
            }
           else if (it.spinSpeed != 0.0f)
           {
               staticLayout[slot].mode = TrackMode::Harmonic;
               byModeBB[static_cast<u32>(TrackMode::Harmonic)].set_bit(slot);
           }
           else if constexpr (Type == MediaType::Text || Type == MediaType::Lyric)
           {
               if (activeSlotBlocks[slot].template get_value<float, text_reg::REVEAL_DURATION_MS>() > 0.0f || activeSlotBlocks[slot].template get_value<u8, text_bit::REVEAL_MODE_0, 4>() > 0 || tr.contentMask != 0 || bassParams[slot].isBassActive != 0)
               {
                   staticLayout[slot].mode = TrackMode::Procedural;
                   byModeBB[static_cast<u32>(TrackMode::Procedural)].set_bit(slot);
               }
               else
               {
                   staticLayout[slot].mode = TrackMode::Static;
                   byModeBB[static_cast<u32>(TrackMode::Static)].set_bit(slot);
               }
           }
           else if (bassParams[slot].isBassActive != 0)
           {
               staticLayout[slot].mode = TrackMode::Procedural;
               byModeBB[static_cast<u32>(TrackMode::Procedural)].set_bit(slot);
           }
           else
           {
               staticLayout[slot].mode = TrackMode::Static;
               byModeBB[static_cast<u32>(TrackMode::Static)].set_bit(slot);
           }
       }
        // Adapts one MediaType template specialization to the runtime geometry function table.
       template <MediaType Type>
       static void dispatch_init_geometry(Position &pos, u32 slot)
       {
           pos.init_slot_geometry_T<Type>(slot);
       }
        // --------------------------------------------------------------------
        // DUAL-AXIS DYNAMIC CLASSIFICATION & HOMOMORPHIC ZOBRIST CHANNELS
        // - Axis 1 (IntrinsicDynamic): Video, Audio, Effect, Spin, Bass
        // - Axis 2 (KeyframeDynamic) : Các thuộc tính thực sự biến đổi theo Keyframe/Reveal
        // --------------------------------------------------------------------
        static constexpr u64 VARY_GEOM_BIT    = (1ULL << 0);
        static constexpr u64 VARY_ROTATE_BIT  = (1ULL << 1);
        static constexpr u64 VARY_OPACITY_BIT = (1ULL << 2);
        static constexpr u64 VARY_COLOR_BIT   = (1ULL << 3);
        static constexpr u64 VARY_REVEAL_BIT  = (1ULL << 4);
        static constexpr u64 VARY_MEDIA_BIT   = (1ULL << 5);
        static constexpr u64 VARY_VOLUME_BIT  = (1ULL << 6);
        static constexpr float VARY_POS_EPSILON     = 0.01f;
        static constexpr float VARY_SCALE_EPSILON   = 0.001f;
        static constexpr float VARY_OPACITY_EPSILON = 0.0005f;
        static constexpr TimePoint MEDIA_FRAME_QUANT_MS = 16;
       inline u64 compute_slot_varying_props_mask(u32 slot) const noexcept
        {
            slot &= (MAX_TIMELINE_SLOTS - 1u);
            const TimelineItem &it = items[slot];
            if (it.type == MediaType::None) return 0ULL;
           u64 vMask = 0ULL;
            const KeyframeTrack &tr = tracks[slot];
           // 1. Phân tích sự biến đổi thực tế giữa các Keyframe (Dynamic Keyframe)
            if (tr.count > 1 && tr.has_arena_frames())
            {
                const NativeKeyframe f0 = get_keyframe_view(slot, 0);
                const float w0 = (f0.w > 0.0f || f0.scale == 0.0f) ? f0.w : it.baseW;
                const float h0 = (f0.h > 0.0f || f0.scale == 0.0f) ? f0.h : it.baseH;
                for (u32 k = 1; k < tr.count; ++k)
                {
                    const NativeKeyframe fi = get_keyframe_view(slot, k);
                    const float wi = (fi.w > 0.0f || fi.scale == 0.0f) ? fi.w : it.baseW;
                    const float hi = (fi.h > 0.0f || fi.scale == 0.0f) ? fi.h : it.baseH;
                    if (abs_val(fi.x - f0.x) > VARY_POS_EPSILON || abs_val(fi.y - f0.y) > VARY_POS_EPSILON ||
                        abs_val(wi - w0) > VARY_POS_EPSILON || abs_val(hi - h0) > VARY_POS_EPSILON ||
                        abs_val(fi.scale - f0.scale) > VARY_SCALE_EPSILON)
                    {
                        vMask |= VARY_GEOM_BIT;
                    }
                    if (abs_val(fi.rotateDeg - f0.rotateDeg) > VARY_POS_EPSILON)
                    {
                        vMask |= VARY_ROTATE_BIT;
                    }
                    if (abs_val(fi.opacity - f0.opacity) > VARY_OPACITY_EPSILON)
                    {
                        if (it.type == MediaType::Audio)
                            vMask |= VARY_VOLUME_BIT;
                        else
                            vMask |= VARY_OPACITY_BIT;
                    }
                }
            }
           // 2. Phân tích Reveal / Content steps của Lyric & Text (Dynamic Keyframe)
            if (it.type == MediaType::Text || it.type == MediaType::Lyric)
            {
                if (tr.contentMask != 0 || activeSlotBlocks[slot].template get_value<float, text_reg::REVEAL_DURATION_MS>() > 0.0f || activeSlotBlocks[slot].template get_value<u8, text_bit::REVEAL_MODE_0, 4>() > 0)
                {
                    vMask |= VARY_REVEAL_BIT;
                }
                if (bassParams[slot].isBassActive != 0)
                {
                    vMask |= VARY_COLOR_BIT;
                }
            }
            // 3. Phân tích Bản chất Động học Nội tại (Intrinsic Dynamic: Spin, Bass, Video, Effect)
            if (it.spinSpeed != 0.0f)
            {
                vMask |= VARY_ROTATE_BIT;
            }
            if (bassParams[slot].isBassActive != 0 || tracks[slot].activeColorKfMask != 0)
            {
                vMask |= VARY_COLOR_BIT;
            }
            if (it.type == MediaType::Video || it.type == MediaType::Audio)
            {
                vMask |= VARY_MEDIA_BIT;
            }
           return vMask;
        }
       // Tự động phân loại TrackMode, Dual-Axis Dynamic Bitboards & Khóa Tĩnh Zobrist
        inline void recompute_track_mode(u32 slot)
        {
            slot &= (CAPACITY - 1u);
            for (u32 m = 0; m < TRACK_MODE_NB; ++m)
            {
                byModeBB[m].clear_bit(slot);
            }
            for (u32 n = 0; n < DYNAMIC_NATURE_NB; ++n)
            {
                byNatureBB[n].clear_bit(slot);
            }
            for (u32 a = 0; a < DYNAMIC_AXIS_NB; ++a)
            {
                byDynamicAxisBB[a].clear_bit(slot);
            }
           using GeomInitFn = void (*)(Position &, u32);
            static constexpr MultiArray<GeomInitFn, MEDIA_TYPE_NB> kGeomTable{{
                &Position::dispatch_init_geometry<MediaType::None>,
                &Position::dispatch_init_geometry<MediaType::Background>,
                &Position::dispatch_init_geometry<MediaType::Video>,
                &Position::dispatch_init_geometry<MediaType::Image>,
                &Position::dispatch_init_geometry<MediaType::Path>,
                &Position::dispatch_init_geometry<MediaType::Lyric>,
                &Position::dispatch_init_geometry<MediaType::Text>,
                &Position::dispatch_init_geometry<MediaType::Audio>
            }};
            const TimelineItem &it = items[slot];
            const u32 t = static_cast<u32>(it.type);
            kGeomTable[t](*this, slot);
           if (it.type == MediaType::None)
            {
                staticLayout[slot].varyingProps = 0u;
                staticLayout[slot].staticZobristKey = 0ULL;
                staticLayout[slot].dynamicZobristKey = 0ULL;
                evaluatedSlots[slot].isVisible = 0;
                return;
            }
           // Trục 1: Bản chất nội tại (IntrinsicDynamic: Video, Audio, Effect, Spin, Bass)
            const bool isIntrinsicDynamic =
                (it.type == MediaType::Video || it.type == MediaType::Audio) ||
                (it.spinSpeed != 0.0f) ||
                (bassParams[slot].isBassActive != 0);
            if (isIntrinsicDynamic)
            {
                byNatureBB[static_cast<u32>(DynamicNature::IntrinsicDynamic)].set_bit(slot);
                byDynamicAxisBB[static_cast<u32>(DynamicAxis::Intrinsic)].set_bit(slot);
                if (it.type != MediaType::Audio && it.type != MediaType::None)
                {
                    byDynamicAxisBB[static_cast<u32>(DynamicAxis::IntrinsicVisual)].set_bit(slot);
                }
            }
            else
            {
                byNatureBB[static_cast<u32>(DynamicNature::IntrinsicStatic)].set_bit(slot);
            }
            // Trục 2: Động học do Keyframe / Reveal tác động (KeyframeDynamic)
            const u64 vMask = compute_slot_varying_props_mask(slot);
            staticLayout[slot].varyingProps = static_cast<u8>(vMask);
            const u64 kfVaryMask = vMask & (VARY_GEOM_BIT | VARY_ROTATE_BIT | VARY_OPACITY_BIT | VARY_COLOR_BIT | VARY_REVEAL_BIT | VARY_VOLUME_BIT);
            const bool isText = (it.type == MediaType::Text || it.type == MediaType::Lyric);
            const bool hasKfOrReveal = ((tracks[slot].count > 1) && kfVaryMask != 0ULL) ||
                                       (isText && (vMask & VARY_REVEAL_BIT) != 0ULL);
            if (hasKfOrReveal)
            {
                byDynamicAxisBB[static_cast<u32>(DynamicAxis::Keyframe)].set_bit(slot);
                if (it.type != MediaType::Audio && it.type != MediaType::None)
                {
                    byDynamicAxisBB[static_cast<u32>(DynamicAxis::KeyframeVisual)].set_bit(slot);
                }
            }
           bake_track_sentinels(slot);
           staticLayout[slot].staticZobristKey = compute_slot_static_zobrist_key(slot);
           recompute_slot_static_occlusion(slot);
        }
       // --------------------------------------------------------------------
        // HOMOMORPHIC CHUNK-DELTA ZOBRIST PRE-BAKING & INCREMENTAL DELTA O(1)
        // --------------------------------------------------------------------
        inline Key compute_slot_static_zobrist_key(u32 slot) const noexcept
        {
            slot &= (MAX_TIMELINE_SLOTS - 1u);
            const TimelineItem &it = items[slot];
            if (it.type == MediaType::None) return 0ULL;
            const auto &sl = staticLayout[slot];
            const u8 vMask = sl.varyingProps;
            Key k = hzobrist_identity(slot, static_cast<u16>(it.type), it.zIndex, it.tier, it.flags);
            if ((vMask & VARY_GEOM_BIT) == 0u)
            {
                const Rect<float> &box = sl.aabb;
                k ^= hzobrist_geom(slot, box.x0, box.y0, box.x1, box.y1);
            }
            if ((vMask & VARY_ROTATE_BIT) == 0u)
            {
                k ^= hzobrist_rot(slot, it.rotateDeg);
            }
            if ((vMask & VARY_COLOR_BIT) == 0u)
            {
                const u32 extraCol = (it.type == MediaType::Text || it.type == MediaType::Lyric)
                                         ? activeSlotBlocks[slot].template get_value<u32, text_reg::COLOR>()
                                         : 0u;
                k ^= hzobrist_resource(slot, it.resourceId, extraCol);
            }
            if ((vMask & VARY_REVEAL_BIT) == 0u)
            {
                const u32 filterVal = sl.maskFilter;
                k ^= hzobrist_reveal(slot, static_cast<u32>(PERMILLE_MAX), filterVal);
            }
            return k;
        }
       // Register-Direct Dynamic Zobrist Key Evaluator (0 intermediate structs, 1 L1 cacheline)
        [[nodiscard]] inline Key compute_slot_dynamic_key(u32 slot, TimePoint nowMs) const noexcept
        {
            slot &= (MAX_TIMELINE_SLOTS - 1u);
            const u8 vMask = staticLayout[slot].varyingProps;
            if (vMask == 0u || !visualSlotsBB.test_bit(slot)) return 0ULL;
            const TimelineItem &it = items[slot];
            if (it.type == MediaType::None) return 0ULL;
            const Evaluated &ev = evaluatedSlots[slot];
            const u64 relFrame = (vMask & VARY_MEDIA_BIT)
                                     ? (static_cast<u64>(nowMs > it.startMs ? (nowMs - it.startMs) : 0) >> 4u)
                                     : 0ULL;
            if (vMask == VARY_MEDIA_BIT)
            {
                const u64 lo = (relFrame << 32) | static_cast<u64>(ev.resourceId);
                const u64 hi = (relFrame & INVALID_U16) << 48;
                return hzobrist_bijective128_to_64(slot, lo, hi);
            }
            u64 lo = 0ULL;
            if (vMask & VARY_GEOM_BIT)
            {
                lo = hzobrist_pack_geom64(ev.aabb.x0, ev.aabb.y0, ev.aabb.x1, ev.aabb.y1);
            }
            else
            {
                const u32 dynCol = (vMask & VARY_COLOR_BIT)
                                       ? (((it.type == MediaType::Text || it.type == MediaType::Lyric) && (tracks[slot].activeColorKfMask & 1u) == 0u)
                                              ? resolve_text_color(slot, nowMs)
                                              : ev.resourceId)
                                       : ev.resourceId;
                lo = (relFrame << 32) | static_cast<u64>(dynCol);
            }
            const u64 qRot = (vMask & VARY_ROTATE_BIT)
                                 ? static_cast<u64>(hzobrist_pack_rot16(ev.rotateDeg))
                                 : 0ULL;
            const u64 qOpVis = (vMask & (VARY_OPACITY_BIT | VARY_VOLUME_BIT))
                                   ? (static_cast<u64>(hzobrist_pack_opacity16(ev.opacity)) |
                                      (static_cast<u64>(ev.isVisible & 1u) << 15))
                                   : 0ULL;
            const u64 qRevFlags = (vMask & VARY_REVEAL_BIT)
                                      ? static_cast<u64>(ev.flags)
                                      : 0ULL;
            u16 aux16 = (vMask & VARY_REVEAL_BIT) ? static_cast<u16>(ev.filter) : 0u;
            if (vMask & VARY_COLOR_BIT)
            {
                const u32 col = ((it.type == MediaType::Text || it.type == MediaType::Lyric) && (tracks[slot].activeColorKfMask & 1u) == 0u)
                                    ? resolve_text_color(slot, nowMs)
                                    : ev.resourceId;
                aux16 ^= static_cast<u16>((col ^ (col >> 16)) & INVALID_U16);
            }
            if (vMask & VARY_MEDIA_BIT)
            {
                aux16 ^= static_cast<u16>(relFrame & INVALID_U16);
            }
            const u64 hi = qRot | (qOpVis << 16) | (qRevFlags << 32) | (static_cast<u64>(aux16) << 48);
            return hzobrist_bijective128_to_64(slot, lo, hi);
        }
        // Merkle Pyramid Tier 0: Evaluated visual leaf hash for slot s (Single L1 Cacheline Read)
        [[nodiscard]] inline Key slot_leaf_hash(u32 slot, const StateInfo<Mask> &stateRecord) const noexcept
        {
            const u32 s = slot & (CAPACITY - 1u);
            if (!stateRecord.activeBB.test_bit(s) || stateRecord.pseudoCulledBB.test_bit(s) || !visualSlotsBB.test_bit(s))
            {
                return 0ULL;
            }
            const auto &sl = staticLayout[s];
            return sl.staticZobristKey ^ ((sl.varyingProps != 0u) ? sl.dynamicZobristKey : 0ULL);
        }
        [[nodiscard]] inline Key slot_leaf_hash(u32 slot) const noexcept
        {
            const u32 s = slot & (CAPACITY - 1u);
            if (!visualSlotsBB.test_bit(s))
            {
                return 0ULL;
            }
            const auto &sl = staticLayout[s];
            return sl.staticZobristKey ^ ((sl.varyingProps != 0u) ? sl.dynamicZobristKey : 0ULL);
        }
        // Pha E: Direct Visual Bounds Resolution from Evaluated (0 DisplayCommand reads)
        inline void resolve_slot_visual_bounds(
            u32 slot,
            float stageW,
            float stageH,
            float scaleX,
            float scaleY,
            Rect<float>& outBounds,
            bool& outIsContinuous,
            u32& outExtraKey
        ) const noexcept
        {
            const u32 s = slot & (CAPACITY - 1u);
            const Evaluated& ev = evaluatedSlots[s];
            const TimelineItem& it = items[s];
            float x0 = ev.aabb.x0 * scaleX;
            float y0 = ev.aabb.y0 * scaleY;
            float x1 = ev.aabb.x1 * scaleX;
            float y1 = ev.aabb.y1 * scaleY;

            const bool isText = textSlotsBB.test_bit(s);
            if (isText)
            {
                constexpr float MIN_DIRTY_PAD_PX = 128.0f;
                constexpr float PAD_FONT_MUL_F32 = 1.5f;
                constexpr float MAX_TEXT_W_PX    = 4096.0f;
                constexpr float MAX_TEXT_H_PX    = 2048.0f;
                const float effFsVal = activeSlotBlocks[s].template get_value<float, text_reg::FONT_SIZE>();
                const float effFs = (effFsVal > 0.0f) ? effFsVal : static_cast<float>(DEFAULT_FONT_SIZE);
                const float lineMulVal = activeSlotBlocks[s].template get_value<float, text_reg::LINE_HEIGHT>();
                const float lineMul = (lineMulVal > 0.0f) ? lineMulVal : static_cast<float>(DEFAULT_LINE_HEIGHT);
                const float lines = static_cast<float>(max<u16>(1u, activeSlotBlocks[s].template get_value<u16, text_bit::LINE_COUNT_BASE, 8>()));
                const float scaledFs = effFs * scaleY;
                const float basePad = max(MIN_DIRTY_PAD_PX, scaledFs * PAD_FONT_MUL_F32);
                const float bw = x1 - x0;
                const float bh = y1 - y0;
                const float effH = max(bh, lines * scaledFs * lineMul);
                const float extraY = effH - bh;
                const float padX = min(basePad, max(0.0f, (MAX_TEXT_W_PX - bw) * 0.5f));
                const float padY = min(basePad, max(0.0f, (MAX_TEXT_H_PX - effH) * 0.5f)) + extraY * 0.5f;
                x0 = max(0.0f, x0 - padX);
                y0 = max(0.0f, y0 - padY);
                x1 = min(stageW, x1 + padX);
                y1 = min(stageH, y1 + padY);
            }

            const bool hasRot = isText
                ? (ev.filter == 0u && ev.flags != 0u)
                : (ev.rotateDeg != 0.0f || ev.flags != 0u);
            if (hasRot)
            {
                const float cx = (x0 + x1) * 0.5f;
                const float cy = (y0 + y1) * 0.5f;
                const float hw = (x1 - x0) * 0.5f;
                const float hh = (y1 - y0) * 0.5f;
                const float rad = sqrt_f32(hw * hw + hh * hh);
                x0 = cx - rad;
                y0 = cy - rad;
                x1 = cx + rad;
                y1 = cy + rad;
            }

            outBounds.x0 = max(0.0f, x0);
            outBounds.y0 = max(0.0f, y0);
            outBounds.x1 = min(stageW, x1);
            outBounds.y1 = min(stageH, y1);
            outExtraKey = isText
                ? ((static_cast<u32>(ev.filter & INVALID_U16) << 16) | (static_cast<u32>(ev.flags) & INVALID_U16))
                : (ev.resourceId ^ (static_cast<u32>(ev.filter & INVALID_U16) << 16));
            outIsContinuous = (it.type == MediaType::Video) ||
                              (isText && ev.filter > 0u && static_cast<i32>(ev.flags) < PERMILLE_MAX);
        }
        // Pha E: Single-Pass O(1) Commit Digest (replaces triple SlotSnapshot rebuild in NNUECutProjectBrain)
        inline void compute_slot_commit_digest(u32 slot, u64& outFullKey, u32& outSubCheck) const noexcept
        {
            const u32 s = slot & (CAPACITY - 1u);
            const TimelineItem& it = items[s];
            if (it.type == MediaType::None)
            {
                outFullKey = 0ULL;
                outSubCheck = 0u;
                return;
            }
            const auto mix8 = [](u64 v) noexcept -> u32 {
                return static_cast<u32>((v ^ (v >> 17) ^ (v >> 37)) & 0xFFu);
            };
            const u64 tHash = (static_cast<u64>(it.startMs) * SPLITMIX_MUL1) ^
                              (static_cast<u64>(it.endMs) * SPLITMIX_MUL2);
            const KeyframeTrack& tr = tracks[s];
            const u64 kHash = (static_cast<u64>(tr.count) << 40) ^
                              (static_cast<u64>(tr.easing) << 32) ^
                              static_cast<u64>(tr.contentMask) ^
                              static_cast<u64>(tr.keyframedProps) ^
                              static_cast<u64>(tr.activeColorKfMask);
            const BassBeatParams& bp = bassParams[s];
            const u64 sHash = staticLayout[s].staticZobristKey ^
                              (static_cast<u64>(activeSlotBlocks[s].template get_value<u32, text_reg::COLOR>()) << 32) ^
                              static_cast<u64>(activeSlotBlocks[s].template get_value<u32, text_reg::RESOLVED_FONT_ID>()) ^
                              static_cast<u64>(bp.isBassActive) << 16 ^
                              static_cast<u64>(bp.colorCount);
            const u64 gHash = (static_cast<u64>(__builtin_bit_cast(u32, it.baseX)) << 32) ^
                              static_cast<u64>(__builtin_bit_cast(u32, it.baseY)) ^
                              (static_cast<u64>(static_cast<u32>(it.zIndex)) << 16) ^
                              static_cast<u64>(__builtin_bit_cast(u32, it.rotateDeg));
            const u64 full = (staticLayout[s].staticZobristKey ^ tHash ^ kHash ^ sHash ^ gHash);
            outFullKey = full ? full : 1ULL;
            outSubCheck = mix8(tHash) |
                          (mix8(kHash) << 8) |
                          (mix8(gHash) << 16) |
                          (mix8(sHash) << 24);
        }
       // Kiểm tra xem trong khoảng thời gian [winStartMs, winEndMs], slot có thực sự đang biến đổi
        inline bool is_slot_keyframe_varying_in_window(u32 slot, TimePoint winStartMs, TimePoint winEndMs) const noexcept
        {
            slot &= (MAX_TIMELINE_SLOTS - 1u);
            const TimelineItem &it = items[slot];
            if (it.type == MediaType::None) return false;
           const i64 relStart = winStartMs - it.startMs;
            const i64 relEnd   = winEndMs - it.startMs;
            const KeyframeTrack &tr = tracks[slot];
           if (it.type == MediaType::Text || it.type == MediaType::Lyric)
            {
                const u16 rDur = static_cast<u16>(activeSlotBlocks[slot].template get_value<float, text_reg::REVEAL_DURATION_MS>());
                const u8 rMode = activeSlotBlocks[slot].template get_value<u8, text_bit::REVEAL_MODE_0, 4>();
                const i64 revDur = (rDur > 0) ? static_cast<i64>(rDur) : 0LL;
                if (tr.contentMask != 0 && tr.count > 0 && tr.has_arena_frames())
                {
                    bool varying = false;
                    stockcut::Mask<1> cMask(tr.contentMask & ((1ULL << tr.count) - 1ULL));
                    cMask.for_each_active([&](u32 i) {
                        const i64 kfStart = static_cast<i64>(keyframe_offset_ms(slot, i));
                        const i64 kfEnd   = kfStart + (revDur > 0 ? revDur : 1LL);
                        varying |= (relEnd >= kfStart && relStart <= kfEnd);
                    });
                    if (varying) return true;
                }
                else if (revDur > 0)
                {
                    if (relStart <= revDur && relEnd >= 0) return true;
                }
                else if (rMode > 0)
                {
                    return true;
                }
            }
           if (tr.count > 1 && tr.has_arena_frames())
            {
                for (u32 i = 0; i + 1u < tr.count; ++i)
                {
                    const NativeKeyframe k0 = get_keyframe_view(slot, i);
                    const NativeKeyframe k1 = get_keyframe_view(slot, i + 1u);
                    const i64 s0 = static_cast<i64>(k0.offsetMs);
                    const i64 s1 = static_cast<i64>(k1.offsetMs);
                    if (relEnd < s0 || relStart > s1) continue;
                    if (tr.activeColorKfMask != 0u) return true;
                    const float w0 = (k0.w > 0.0f || k0.scale == 0.0f) ? k0.w : it.baseW;
                    const float w1 = (k1.w > 0.0f || k1.scale == 0.0f) ? k1.w : it.baseW;
                    const float h0 = (k0.h > 0.0f || k0.scale == 0.0f) ? k0.h : it.baseH;
                    const float h1 = (k1.h > 0.0f || k1.scale == 0.0f) ? k1.h : it.baseH;
                    if (k1.x != k0.x ||
                        k1.y != k0.y ||
                        w1 != w0 ||
                        h1 != h0 ||
                        k1.opacity != k0.opacity ||
                        k1.rotateDeg != k0.rotateDeg ||
                        k1.scale != k0.scale)
                    {
                        return true;
                    }
                }
            }
            return false;
        }
        // O(1) Sequential Cursor + Two-Sided Gatekeeper trực tiếp trên KeyframeArena 32B hot slice
        inline u16 walk_arena_kf_cursor(const KeyframeTrack &tr, u8 &cursorIdx, i32 relMs) const noexcept
        {
            if (tr.spatialOffset == INVALID_U32 || tr.count == 0 || !keyframeArena.buffer)
                return 0u;
            const NativeKeyframe *base = reinterpret_cast<const NativeKeyframe *>(keyframeArena.buffer.get() + tr.spatialOffset);
            Sentinel<NativeKeyframe, 0>::step_raw_moment(
                base, cursorIdx, relMs, [](const NativeKeyframe &k) noexcept { return k.offsetMs; });
            return cursorIdx;
        }
        inline u64 opacity_state_key(u32 slot, TimePoint nowMs) const
       {
           constexpr float OPACITY_HASH_QUANT = 1024.0f;
           const u32 s = slot & (CAPACITY - 1u);
           const TimelineItem &it = items[s];
           const KeyframeTrack &tr = tracks[s];
           float op = baseSlotBlocks[s].template get_value<float, 4>(6);
           if (tr.has_arena_frames())
           {
               const i32 relMs = static_cast<i32>(nowMs - it.startMs);
               const u16 idx = walk_arena_kf_cursor(tr, tr.lastTypedIdx, relMs);
               const NativeKeyframe *base = reinterpret_cast<const NativeKeyframe *>(keyframeArena.buffer.get() + tr.spatialOffset);
               const double *invSpans = reinterpret_cast<const double *>(keyframeArena.buffer.get() + tr.invSpanOffset);
               const NativeKeyframe &k0 = base[idx];
               const double invSpan = invSpans[idx];
               if (invSpan == 0.0)
               {
                   op = k0.opacity;
               }
               else
               {
                   const NativeKeyframe &k1 = base[idx + 1u];
                   const float t = static_cast<float>(static_cast<double>(relMs - k0.offsetMs) * invSpan);
                   op = k0.opacity + (k1.opacity - k0.opacity) * t;
               }
           }
           const u64 qOp = static_cast<u64>(static_cast<i64>(op * OPACITY_HASH_QUANT));
           const u64 h = HASH_GOLDEN_64 ^ (qOp * SPLITMIX_MUL1);
           return h ^ (h >> 31);
       }
        inline void set_property_masks(u32 slot, u64 defMask, u64 kfMask)
       {
           (void)defMask;
           slot &= (MAX_TIMELINE_SLOTS - 1u);
           tracks[slot].keyframedProps = kfMask;
           recompute_track_mode(slot);
           touch();
       }
        inline u64 defined_props(u32 slot) const noexcept
       {
           return baseSlotBlocks[slot & (MAX_TIMELINE_SLOTS - 1u)].active.words[0];
       }
        inline u64 keyframed_props(u32 slot) const noexcept
       {
           return tracks[slot & (MAX_TIMELINE_SLOTS - 1u)].keyframedProps;
       }
        inline Key static_zobrist_key(u32 slot) const noexcept
       {
           return staticLayout[slot & (MAX_TIMELINE_SLOTS - 1u)].staticZobristKey;
       }
        inline bool is_property_defined(u32 slot, prop::PropertyId prop) const noexcept
       {
           return baseSlotBlocks[slot & (MAX_TIMELINE_SLOTS - 1u)].active.test_bit(prop);
       }
        inline bool is_property_keyframed(u32 slot, prop::PropertyId prop) const noexcept
       {
           return (prop < 64u) && ((tracks[slot & (MAX_TIMELINE_SLOTS - 1u)].keyframedProps & (1ULL << prop)) != 0ULL);
       }
        // Stores the total project duration used by timeline consumers.
       inline void set_duration(TimePoint durMs)
       {
           projectDurationMs = durMs;
           temporalTable.dirty = true;
           touch();
       }
       // Returns the configured project duration in milliseconds.
       inline TimePoint duration() const { return projectDurationMs; }
       // Returns the timestamp associated with the current state.
       inline TimePoint time() const { return currentTimeMs; }
       // Returns the current history-linked state record.
       inline StateInfo<Mask> *state() const { return st; }
       // Tests whether any non-empty media type has a configured slot.
       inline bool has_items() const
       {
           return populated_slots().any();
       }
       inline Mask populated_slots() const noexcept {
           return byTypeBB[ALL_ITEMS];
       }
       inline bool empty(u32 slot) const noexcept {
           return !byTypeBB[ALL_ITEMS].test_bit(slot);
       }
       // Returns the active-slot mask or zero before the first timeline step.
       inline Mask active_items() const { return st ? st->activeBB : Mask{}; }
       // Returns the number of populated, sorted z-level bitboards.
       inline u32 z_bucket_count() const { return zBucketCount; }
       // Returns one sorted z-level value.
       inline i32 z_bucket_level(u32 index) const
       {
           return index < zBucketCount ? zBucketLevels[index] : 0;
       }
       // Returns the slots sharing one z level as a bitboard.
       inline Mask z_bucket(u32 index) const
       {
           return index < zBucketCount ? zBuckets[index] : Mask{};
       }
       // Returns active audio/video slots used by browser media synchronization.
       inline Mask active_media_items() const
       {
           return st ? (st->activeBB & (byTypeBB[static_cast<u32>(MediaType::Audio)] | byTypeBB[static_cast<u32>(MediaType::Video)])) : Mask{};
       }
       // Returns active audio slots.
       inline Mask active_audio_items() const
       {
           return st ? (st->activeBB & by_type_bb<MediaType::Audio>()) : Mask{};
       }
        // Bitboard of intrinsic dynamic tracks (Video, Audio, Effect, Spin, Bass).
        inline Mask intrinsic_dynamic_bb() const noexcept
        {
            return byDynamicAxisBB[static_cast<u32>(DynamicAxis::Intrinsic)];
        }
       // Bitboard of visual-only intrinsic dynamic tracks (excludes Audio).
        inline Mask intrinsic_visual_dynamic_bb() const noexcept
        {
            return byDynamicAxisBB[static_cast<u32>(DynamicAxis::IntrinsicVisual)];
        }
       // Bitboard of keyframe-driven tracks (both intrinsic-static and intrinsic-dynamic items).
        inline Mask keyframe_dynamic_bb() const noexcept
        {
            return byDynamicAxisBB[static_cast<u32>(DynamicAxis::Keyframe)];
        }
       // Bitboard of visual-only keyframe-driven tracks (excludes Audio volume keyframes).
        inline Mask keyframe_visual_dynamic_bb() const noexcept
        {
            return byDynamicAxisBB[static_cast<u32>(DynamicAxis::KeyframeVisual)];
        }
               // Bitboard of all visual tracks that can exhibit variation across time.
        inline Mask continuous_tracks_bb() const noexcept
        {
            return byDynamicAxisBB[static_cast<u32>(DynamicAxis::IntrinsicVisual)] |
                   byDynamicAxisBB[static_cast<u32>(DynamicAxis::KeyframeVisual)];
        }
        // Bitboard of all active visual slots with strictly lower Z-order than slot.
        inline Mask z_shadow_bb(u32 slot) const noexcept
        {
            return zShadowBB[slot & (CAPACITY - 1u)];
        }
        // Bitboard of all slots that are static fullscreen opaque blockers.
        inline Mask static_fullscreen_blockers_bb() const noexcept
        {
            return staticFullscreenBlockersBB;
        }
        // Bitboard of 8x8 spatial tiles covered by static slot.
        inline Bitboard static_tile_footprint_bb(u32 slot) const noexcept
        {
            return staticLayout[slot & (MAX_TIMELINE_SLOTS - 1u)].tileFootprint;
        }
        [[nodiscard]] inline u32 z_rank_count() const noexcept { return zRankCount_; }
        [[nodiscard]] inline u32 z_rank_to_slot(u32 rank) const noexcept { return zRankToSlot_[rank & (CAPACITY - 1u)]; }
        [[nodiscard]] inline u32 slot_to_z_rank(u32 slot) const noexcept { return slotToZRank_[slot & (CAPACITY - 1u)]; }
        [[nodiscard]] inline u32 base_bg_insert_rank() const noexcept { return baseBgInsertRank_; }
        inline void rebuild_z_shadows() noexcept
        {
            Mask accum{};
            u32 rank = 0u;
            u32 bgRank = 0u;
            bool bgRankFound = false;
            constexpr i32 BASE_BG_Z = -1;
            for (u32 level = 0; level < zBucketCount; ++level)
            {
                if (!bgRankFound && zBucketLevels[level] >= BASE_BG_Z)
                {
                    bgRank = rank;
                    bgRankFound = true;
                }
                zBuckets[level].for_each_active([this, accum, &rank](u32 slot) {
                    zShadowBB[slot] = accum;
                    zRankToSlot_[rank] = static_cast<u8>(slot);
                    slotToZRank_[slot] = static_cast<u8>(rank);
                    ++rank;
                });
                accum |= zBuckets[level];
            }
            if (!bgRankFound) bgRank = rank;
            zRankCount_ = static_cast<u8>(rank);
            baseBgInsertRank_ = static_cast<u8>(bgRank);

            // ================================================================
            // KHÁM PHÁ CHE KHUẤT KHÔNG GIAN ĐA TẦNG (SPATIAL OCCLUSION & CLIPPING)
            // Duyệt từ Z cao nhất xuống Z thấp nhất:
            // Layer trên báo vùng che, layer dưới tự cắt bỏ vùng bị che.
            // ================================================================
            staticOccludedSlotsBB = 0ULL;
            if (zBucketCount > 0)
            {
                for (i32 level = static_cast<i32>(zBucketCount) - 1; level >= 0; --level)
                {
                    zBuckets[level].for_each_active([this, level](u32 slot) {
                        auto &sl = staticLayout[slot];
                        const Bitboard myCoverage = sl.tileFootprint;
                        const TimePoint myStart = items[slot].startMs;
                        const TimePoint myEnd = items[slot].endMs;

                        Bitboard accumUpperTiles = 0ULL;
                        for (u32 uLevel = static_cast<u32>(level + 1); uLevel < zBucketCount; ++uLevel)
                        {
                            zBuckets[uLevel].for_each_active([this, myStart, myEnd, &accumUpperTiles](u32 uSlot) {
                                const TimelineItem &uItem = items[uSlot];
                                if (uItem.startMs <= myStart && uItem.endMs >= myEnd)
                                {
                                    accumUpperTiles |= staticLayout[uSlot].innerFootprint;
                                }
                            });
                        }

                        const Bitboard myVisible = myCoverage & ~accumUpperTiles;
                        slotVisibleTiles[slot] = myVisible;
                        if (myCoverage != 0ULL && myVisible == 0ULL)
                        {
                            sl.isFullyOccluded = 1;
                            sl.isPartiallyOccluded = 0;
                            staticOccludedSlotsBB.set_bit(slot);
                        }
                        else if (myVisible != myCoverage && myVisible != 0ULL)
                        {
                            sl.isFullyOccluded = 0;
                            sl.isPartiallyOccluded = 1;
                            double scX = 0.0, scY = 0.0, scW = 0.0, scH = 0.0;
                            if (myVisible.to_scissor_box(stageW, stageH, scX, scY, scW, scH))
                            {
                                slotScissorBoxes[slot] = Rect<float>{
                                    static_cast<float>(scX),
                                    static_cast<float>(scY),
                                    static_cast<float>(scX + scW),
                                    static_cast<float>(scY + scH)
                                };
                            }
                        }
                        else
                        {
                            sl.isFullyOccluded = 0;
                            sl.isPartiallyOccluded = 0;
                        }
                    });
                }
            }
        }
        inline void recompute_slot_static_occlusion(u32 slot) noexcept
        {
            slot &= (CAPACITY - 1u);
            const TimelineItem &it = items[slot];
            staticFullscreenBlockersBB.clear_bit(slot);
            auto &sl = staticLayout[slot];
            sl.tileFootprint = 0ULL;
            sl.innerFootprint = 0ULL;
            if (it.type == MediaType::None) return;
            const bool isVisual = (it.type == MediaType::Background) ||
                                  (it.type == MediaType::Image) ||
                                  (it.type == MediaType::Video) ||
                                  (it.type == MediaType::Lyric) ||
                                  (it.type == MediaType::Text) ||
                                  (it.type == MediaType::Path);
            if (!isVisual) return;
            if (it.type == MediaType::Background)
            {
                staticFullscreenBlockersBB.set_bit(slot);
                sl.tileFootprint = ~0ULL;
                sl.innerFootprint = ~0ULL;
                return;
            }
            const bool noKeyframeVariation = (sl.varyingProps == 0ULL);
            const bool noRotation = (it.rotateDeg == 0.0f);
            const bool noHole = (it.maskHole == 0.0f);
            const bool isStaticFullscreen = noKeyframeVariation && noRotation && noHole &&
                                            (it.baseX <= 0.0f) && (it.baseY <= 0.0f) &&
                                            (it.baseX + it.baseW >= stageW) && (it.baseY + it.baseH >= stageH);
            const Rect<float>& aabb = sl.aabb;
            if (isStaticFullscreen)
            {
                staticFullscreenBlockersBB.set_bit(slot);
                sl.tileFootprint = ~0ULL;
                sl.innerFootprint = ~0ULL;
            } else {
                sl.tileFootprint = Bitboard::FromAABBT<Bitboard::SpatialCoverage::ExactAABB>(
                    aabb.x0, aabb.y0, aabb.x1, aabb.y1, stageW, stageH
                );
                sl.innerFootprint = Bitboard::FromAABBT<Bitboard::SpatialCoverage::ConservativeInner>(
                    aabb.x0, aabb.y0, aabb.x1, aabb.y1, stageW, stageH
                );
            }
        }
        inline void eval_chunk_sub_occlusion(Mask activeBB, Mask &outOccludedBB, Bitboard &outOpaqueTiles) const noexcept
        {
            outOccludedBB = activeBB & staticOccludedSlotsBB;
            outOpaqueTiles = 0ULL;
            const Mask activeBlockers = activeBB & staticFullscreenBlockersBB;
            if (activeBlockers)
            {
                outOpaqueTiles = ~0ULL;
                activeBlockers.for_each_active([this, &outOccludedBB, activeBB](u32 slot) {
                    outOccludedBB |= (activeBB & zShadowBB[slot]);
                });
            }
        }
        // Rebuilds the 64-bit unit temporal chunk bitboard index for O(1) time slicing.
       inline void rebuild_temporal_index()
       {
           TimePoint maxEnd = projectDurationMs;
           for (Mask slots = populated_slots(); slots;)
           {
               const u32 s = slots.pop_lsb();
               if (items[s].endMs > maxEnd)
               {
                   maxEnd = items[s].endMs;
               }
           }
           constexpr TimePoint CHUNK_SPAN_MS = (1LL << TIME_CHUNK_SHIFT);
           u32 maxChunks = static_cast<u32>((maxEnd + (CHUNK_SPAN_MS - 1LL)) >> TIME_CHUNK_SHIFT);
           if (maxChunks == 0) maxChunks = 1;
           if (maxChunks > MAX_TEMPORAL_CHUNKS) maxChunks = MAX_TEMPORAL_CHUNKS;
           std::memset(temporalTable.chunks.data(), 0, maxChunks * sizeof(Chunk));
           std::memset(temporalTable.chunkZobrist.data(), 0, maxChunks * sizeof(ChunkZobrist));
           std::memset(temporalTable.chunkOcclusion.data(), 0, maxChunks * sizeof(ChunkOcclusion));
           const Mask visualMask = cachedVisualMask;
           const Mask kfCandidates = cachedKfDynAll;
           auto eval_chunk_sub_zobrist = [this, visualMask, kfCandidates](
               Mask activeBB, TimePoint subStart, TimePoint subEnd,
               Key &outBaseZobrist, Mask &outKfActiveBB) {
               Key baseSum = 0ULL;
               (activeBB & visualMask).for_each_active([this, &baseSum](u32 s) {
                   baseSum ^= staticLayout[s].staticZobristKey;
               });
               outBaseZobrist = baseSum;
               Mask kfActive{};
               (activeBB & kfCandidates).for_each_active([this, subStart, subEnd, &kfActive](u32 s) {
                   if (is_slot_keyframe_varying_in_window(s, subStart, subEnd))
                   {
                       kfActive.set_bit(s);
                   }
               });
               outKfActiveBB = kfActive;
           };
           auto push_unique_transition = [](ValueList<TimePoint, MAX_TIMELINE_SLOTS * 2> &list,
                                            TimePoint t, TimePoint lo, TimePoint hi) noexcept
           {
               if (t <= lo || t >= hi) return;
               for (usize i = 0; i < list.size(); ++i)
               {
                   if (list[i] == t) return;
               }
               list.push_back(t);
           };
           constexpr u32 MAX_INLINE_TRANSITIONS = TEMPORAL_SUBCHUNK_NB - 1u;
           for (u32 c = 0; c < maxChunks; ++c)
           {
               auto &chunk = temporalTable.chunks[c];
               const TimePoint chunkStart = static_cast<TimePoint>(c) << TIME_CHUNK_SHIFT;
               const TimePoint chunkEnd = chunkStart + CHUNK_SPAN_MS;
               ValueList<TimePoint, MAX_TIMELINE_SLOTS * 2> trans;
               for (Mask slots = populated_slots(); slots;)
               {
                   const u32 slot = slots.pop_lsb();
                   const TimelineItem &it = items[slot];
                   if (it.endMs <= it.startMs) continue;
                   push_unique_transition(trans, it.startMs, chunkStart, chunkEnd);
                   push_unique_transition(trans, it.endMs, chunkStart, chunkEnd);
               }
               DynamicSortingNetwork<TimePoint, SortOrder::Ascending>::sort(trans.data(), trans.size());
               const u32 transCount = static_cast<u32>(trans.size());
                if (transCount > MAX_INLINE_TRANSITIONS)
                {
                    chunk.payload.overflow = 1;
                    chunk.payload.transitionCount = static_cast<u16>(transCount);
                }
                else
                {
                    chunk.payload.overflow = 0;
                    chunk.payload.transitionCount = static_cast<u16>(transCount);
                    chunk.payload.t1 = (transCount >= 1u) ? trans[0] : TIME_MAX_SENTINEL_I64;
                    chunk.payload.t2 = (transCount >= 2u) ? trans[1] : TIME_MAX_SENTINEL_I64;
                    chunk.payload.t3 = (transCount >= 3u) ? trans[2] : TIME_MAX_SENTINEL_I64;
                    MultiArray<TimePoint, TEMPORAL_SUBCHUNK_NB + 1> subBounds{{
                        chunkStart,
                        (transCount >= 1u) ? chunk.payload.t1 : chunkEnd,
                        (transCount >= 2u) ? chunk.payload.t2 : chunkEnd,
                        (transCount >= 3u) ? chunk.payload.t3 : chunkEnd,
                        chunkEnd,
                    }};
                    auto &cz = temporalTable.chunkZobrist[c];
                    auto &co = temporalTable.chunkOcclusion[c];
                    for (u32 sub = 0; sub < TEMPORAL_SUBCHUNK_NB; ++sub)
                    {
                        if (sub == 0 || sub <= transCount)
                        {
                            chunk.bb[sub] = query_active_fallback(subBounds[sub]);
                            chunk.payload.activeBgSlot[sub] = eval_active_bg_from_bb(chunk.bb[sub]);
                            eval_chunk_sub_zobrist(chunk.bb[sub], subBounds[sub], subBounds[sub + 1],
                                                   cz.payload[sub], cz.mask[sub]);
                            eval_chunk_sub_occlusion(chunk.bb[sub], co.mask[sub], co.payload[sub]);
                        }
                        else
                        {
                            chunk.bb[sub] = chunk.bb[sub - 1];
                            chunk.payload.activeBgSlot[sub] = chunk.payload.activeBgSlot[sub - 1];
                            cz.payload[sub] = cz.payload[sub - 1];
                            cz.mask[sub] = cz.mask[sub - 1];
                            co.mask[sub] = co.mask[sub - 1];
                            co.payload[sub] = co.payload[sub - 1];
                        }
                    }
                }
            }
           temporalTable.validChunkCount = maxChunks;
           {
               auto &sentChunk = temporalTable.chunks[maxChunks];
               sentChunk.payload.t1 = TIME_MAX_SENTINEL_I64;
               sentChunk.payload.t2 = TIME_MAX_SENTINEL_I64;
               sentChunk.payload.t3 = TIME_MAX_SENTINEL_I64;
               sentChunk.payload.transitionCount = 0u;
               sentChunk.payload.overflow = 0u;
               sentChunk.payload.activeBgSlot.fill(static_cast<u8>(CAPACITY));
               sentChunk.bb.fill(Mask{});
               temporalTable.chunkZobrist[maxChunks].payload.fill(0ULL);
               temporalTable.chunkZobrist[maxChunks].mask.fill(Mask{});
               temporalTable.chunkOcclusion[maxChunks].mask.fill(Mask{});
               temporalTable.chunkOcclusion[maxChunks].payload.fill(Bitboard(0ULL));
           }
           temporalTable.dirty = false;
       }
        // Fast branchless fallback scanner iterating only valid slots via pop_lsb
        [[nodiscard]] inline Mask query_active_fallback(TimePoint timeMs) const noexcept {
            Mask active{};
            populated_slots().for_each_active([this, timeMs, &active](u32 slot) {
                const TimelineItem& it = items[slot];
                const bool inRange = (it.startMs <= timeMs) & (timeMs < it.endMs);
                active.set_bit(slot, inRange);
            });
            return active;
        }

        // Resolves the 64-bit active item bitboard in O(1) time via Sentinel Chunk clamping (0 right-bound fallback).
       inline Mask query_active_bb(TimePoint timeMs)
       {
           lastQueriedChunkTag = INVALID_U16;
           lastQueriedChunkActiveBgSlot = static_cast<u8>(CAPACITY);
           if (__builtin_expect(temporalTable.dirty, 0))
           {
               rebuild_temporal_index();
           }
           if (__builtin_expect(timeMs < 0, 0))
           {
               return query_active_fallback(timeMs);
           }
           const u32 rawC = static_cast<u32>(timeMs) >> TIME_CHUNK_SHIFT;
           const u32 c = (rawC < temporalTable.validChunkCount) ? rawC : temporalTable.validChunkCount;
           const auto &chunk = temporalTable.chunks[c];
           if (__builtin_expect(chunk.payload.overflow != 0, 0))
           {
               if (__builtin_expect(timeMs >= overflowValidFromMs && timeMs < overflowValidUntilMs, 1))
               {
                   lastQueriedChunkTag = overflowChunkTag;
                   lastQueriedChunkActiveBgSlot = overflowActiveBgSlot;
                   lastQueriedChunkBaseZobrist = overflowBaseZobrist;
                   lastQueriedChunkKfActiveBB = overflowKfActiveBB;
                   lastQueriedChunkStaticOccludedBB = overflowStaticOccludedBB;
                   lastQueriedChunkStaticOpaqueTiles = overflowStaticOpaqueTiles;
                   return overflowActiveBB;
               }
               constexpr TimePoint CHUNK_SPAN_MS = (1LL << TIME_CHUNK_SHIFT);
               const TimePoint chunkStart = static_cast<TimePoint>(c) << TIME_CHUNK_SHIFT;
               const TimePoint chunkEnd = chunkStart + CHUNK_SPAN_MS;
               TimePoint subStart = chunkStart;
               TimePoint subEnd = chunkEnd;
               Mask active{};
               populated_slots().for_each_active([this, timeMs, &subStart, &subEnd, &active](u32 slot) {
                   const TimelineItem &it = items[slot];
                   if (it.endMs <= it.startMs) return;
                   if (it.startMs <= timeMs && timeMs < it.endMs)
                   {
                       active.set_bit(slot);
                       if (it.startMs > subStart) subStart = it.startMs;
                       if (it.endMs < subEnd) subEnd = it.endMs;
                   }
                   else if (it.endMs <= timeMs)
                   {
                       if (it.endMs > subStart) subStart = it.endMs;
                   }
                   else if (it.startMs > timeMs)
                   {
                       if (it.startMs < subEnd) subEnd = it.startMs;
                   }
               });
               overflowValidFromMs = subStart;
               overflowValidUntilMs = subEnd;
               overflowActiveBB = active;
               overflowChunkTag = static_cast<u16>(0x8000u | ((c & 0x7Eu) << 8) | (static_cast<u32>(subStart - chunkStart) & 0xFFu));
               overflowActiveBgSlot = eval_active_bg_from_bb(active);
               Key baseSum = 0ULL;
               (active & cachedVisualMask).for_each_active([this, &baseSum](u32 s) {
                   baseSum ^= staticLayout[s].staticZobristKey;
               });
               overflowBaseZobrist = baseSum;
               Mask kfActive{};
               (active & cachedKfDynAll).for_each_active([this, subStart, subEnd, &kfActive](u32 s) {
                   if (is_slot_keyframe_varying_in_window(s, subStart, subEnd))
                   {
                       kfActive.set_bit(s);
                   }
               });
               overflowKfActiveBB = kfActive;
               eval_chunk_sub_occlusion(active, overflowStaticOccludedBB, overflowStaticOpaqueTiles);
               lastQueriedChunkTag = overflowChunkTag;
               lastQueriedChunkActiveBgSlot = overflowActiveBgSlot;
               lastQueriedChunkBaseZobrist = overflowBaseZobrist;
               lastQueriedChunkKfActiveBB = overflowKfActiveBB;
               lastQueriedChunkStaticOccludedBB = overflowStaticOccludedBB;
               lastQueriedChunkStaticOpaqueTiles = overflowStaticOpaqueTiles;
               return active;
           }
           const u32 nextC = c + 1;
           if (__builtin_expect(nextC <= temporalTable.validChunkCount, 1))
           {
               __builtin_prefetch(&temporalTable.chunks[nextC], 0, 1);
               __builtin_prefetch(&temporalTable.chunkZobrist[nextC], 0, 1);
           }
            // Zero-loop, branchless interval index calculation via hardware flags / SIMD
           const u32 idx = static_cast<u32>(timeMs >= chunk.payload.t1) +
                           static_cast<u32>(timeMs >= chunk.payload.t2) +
                           static_cast<u32>(timeMs >= chunk.payload.t3);
           constexpr u32 CHUNK_TAG_INDEX_MASK = 0x3FFFu;
           lastQueriedChunkTag = static_cast<u16>(((c & CHUNK_TAG_INDEX_MASK) << 2) | (idx & (TEMPORAL_SUBCHUNK_NB - 1u)));
           lastQueriedChunkActiveBgSlot = chunk.payload.activeBgSlot[idx];
           const auto &cz = temporalTable.chunkZobrist[c];
           lastQueriedChunkBaseZobrist = cz.payload[idx];
           lastQueriedChunkKfActiveBB  = cz.mask[idx];
           const auto &co = temporalTable.chunkOcclusion[c];
           lastQueriedChunkStaticOccludedBB = co.mask[idx];
           lastQueriedChunkStaticOpaqueTiles = co.payload[idx];
           return chunk.bb[idx];
       }
       // Shared visual exact Zobrist key builder & dynamic XOR accumulator (Zero Duplication)
       inline void finalize_state_zobrist_keys(StateInfo<Mask> &newSt, Key composedKey, Mask currActive, TimePoint nextTimeMs) const noexcept
       {
           const Key k = composedKey ^ activeStyleEnv.styleZobristToken;
           newSt.key = (k != 0ULL) ? k : 1ULL;
           const bool hasActiveRamp = (currActive & (rampingSlotsBB | byDynamicAxisBB[static_cast<u32>(DynamicAxis::IntrinsicVisual)])).any();
           newSt.visualExactKey = newSt.key ^ (hasActiveRamp ? (static_cast<u64>(nextTimeMs) * HASH_GOLDEN_64 ^ ZOBRIST_MUL_A) : 0ULL);
       }
       inline Key accumulate_dynamic_zobrist_xor(Mask varSlots, TimePoint nextTimeMs) noexcept
       {
           Key dynXor = 0ULL;
           varSlots.for_each_active([this, nextTimeMs, &dynXor](u32 slot) {
               const Key delta = compute_slot_dynamic_key(slot, nextTimeMs);
               staticLayout[slot].dynamicZobristKey = delta;
               dynXor ^= delta;
           });
           return dynXor;
       }
       // Advances state in the caller-provided record and computes its half-open active interval mask,
        // Dual-Axis incremental dirtyBB, Keyframe Plateau moment skip, and O(1) Homomorphic Chunk-Delta Zobrist key.
         inline void step_to(TimePoint nextTimeMs, StateInfo<Mask> &newSt)
         {
             const StateInfo<Mask> *prevSt = st;
             const bool validHistory = (prevSt != nullptr && stepEpoch == epoch);
             const bool styleChanged = update_style_environment(nextTimeMs);
             newSt.timeMs = nextTimeMs;
             newSt.previous = st;
             newSt.activeBB = query_active_bb(nextTimeMs);
             newSt.chunkTag = lastQueriedChunkTag;
             newSt.dirtyBB = 0ULL;
             newSt.occludedBB = lastQueriedChunkStaticOpaqueTiles;
             newSt.occludedSlotsBB = lastQueriedChunkStaticOccludedBB;
             newSt.dynamicVisualBB = newSt.activeBB & continuous_tracks_bb();
             newSt.opaqueSlotsBB = newSt.activeBB & staticFullscreenBlockersBB;
             newSt.commandCount = 0;
             newSt.dlReady = 0;
             newSt.pad = 0;
             newSt.generation = prevSt ? (prevSt->generation + 1) : 0;
             const Mask currActive = newSt.activeBB;
             newSt.pseudoCulledBB = currActive & (newSt.occludedSlotsBB | staticOffscreenBB);
             const Mask visualMask = cachedVisualMask;
             const bool sameChunkTag = validHistory &&
                                       (newSt.chunkTag != INVALID_U16) &&
                                       (newSt.chunkTag == prevSt->chunkTag);
             const bool sameActiveSetForward = validHistory &&
                                               (newSt.chunkTag != INVALID_U16) &&
                                               (prevSt->chunkTag != INVALID_U16) &&
                                               (nextTimeMs >= prevSt->timeMs) &&
                                               (currActive == prevSt->activeBB);
             const bool sameSubChunk = sameChunkTag || sameActiveSetForward;
             const Mask activeChunkDyn  = (newSt.chunkTag != INVALID_U16)
                                              ? (cachedIntrinsicDynAll | lastQueriedChunkKfActiveBB |
                                                 (sameChunkTag ? Mask{0ULL} : (prevChunkKfActiveBB | rampingSlotsBB | kfPlateauBB)))
                                              : cachedAllDyn;
             prevChunkKfActiveBB = lastQueriedChunkKfActiveBB;
             if (sameSubChunk)
             {
                 const TimePoint prevTimeMs = prevSt->timeMs;
                 Mask candidateDirty = currActive & activeChunkDyn;
                 const Mask reenteredBB = prevSt->pseudoCulledBB & ~newSt.pseudoCulledBB & currActive;
                 candidateDirty |= reenteredBB;
                 if (__builtin_expect(kfPlateauBB.any(), 0))
                 {
                     if (__builtin_expect(nextTimeMs >= prevTimeMs, 1))
                     {
                         if (__builtin_expect(nextTimeMs < minKfPlateauEndMs, 1))
                         {
                             candidateDirty &= ~(kfPlateauBB & ~cachedIntrinsicDynAll);
                         }
                         else
                         {
                             Mask steadyKf = candidateDirty & kfPlateauBB & ~cachedIntrinsicDynAll;
                             if (__builtin_expect(steadyKf.any(), 0))
                             {
                                 TimePoint newMinEnd = TIME_MAX_SENTINEL_I64;
                                 steadyKf.for_each_active([this, nextTimeMs, &candidateDirty, &newMinEnd](u32 s) {
                                     const TimePoint pEnd = kfPlateauEndMs[s];
                                     if (nextTimeMs < pEnd) {
                                         candidateDirty.clear_bit(s);
                                         if (pEnd < newMinEnd) newMinEnd = pEnd;
                                     } else {
                                         kfPlateauBB.clear_bit(s);
                                     }
                                 });
                                 minKfPlateauEndMs = newMinEnd;
                             }
                         }
                     }
                     else
                     {
                         kfPlateauBB.clear();
                         minKfPlateauEndMs = TIME_MAX_SENTINEL_I64;
                     }
                 }
                 Mask styleDirtyMask{0ULL};
                 if (__builtin_expect(styleChanged, 0))
                 {
                     const Mask styleAffectedBB = byTypeBB[static_cast<u32>(MediaType::Text)] |
                                                  byTypeBB[static_cast<u32>(MediaType::Lyric)] |
                                                  byTypeBB[static_cast<u32>(MediaType::Background)] |
                                                  byTypeBB[static_cast<u32>(MediaType::Path)];
                     styleDirtyMask = currActive & styleAffectedBB;
                 }
                 const Mask warmSlots = warmDirtySlotsBB;
                 newSt.dirtyBB = candidateDirty | styleDirtyMask | warmSlots;
                 warmDirtySlotsBB.clear();
                 currentTimeMs = nextTimeMs;
                 stepEpoch = epoch;
                 st = &newSt;
                 if (!newSt.dirtyBB.any())
                 {
                     newSt.dirtyTransformBB.clear();
                     newSt.dirtyOpacityBB.clear();
                     newSt.dirtyContentBB.clear();
                     finalize_state_zobrist_keys(newSt, lastQueriedChunkBaseZobrist ^ runningDynamicZobrist, currActive, nextTimeMs);
                     return;
                 }
                 evaluate_runtime_slots(newSt, styleChanged || warmSlots.any());
                 Mask dynVisScan = newSt.dirtyBB & visualMask;
                 const bool cleanStyle = !styleChanged;
                 const bool noWarm = !warmSlots.any();
                 dynVisScan.for_each_active([this, nextTimeMs, prevTimeMs, &newSt, cleanStyle, noWarm, styleDirtyMask, warmSlots](u32 slot) {
                     auto &sl = staticLayout[slot];
                     if (sl.varyingProps == VARY_MEDIA_BIT && noWarm)
                     {
                         const TimePoint startMs = items[slot].startMs;
                         const u64 curBucket = static_cast<u64>(nextTimeMs > startMs ? (nextTimeMs - startMs) : 0) >> 4u;
                         const u64 prevBucket = static_cast<u64>(prevTimeMs > startMs ? (prevTimeMs - startMs) : 0) >> 4u;
                         if (curBucket == prevBucket)
                         {
                             if (cleanStyle || !styleDirtyMask.test_bit(slot))
                             {
                                 newSt.dirtyBB.clear_bit(slot);
                             }
                             return;
                         }
                     }
                     const Key newDelta = compute_slot_dynamic_key(slot, nextTimeMs);
                     if (newDelta != sl.dynamicZobristKey)
                     {
                         runningDynamicZobrist ^= (sl.dynamicZobristKey ^ newDelta);
                         sl.dynamicZobristKey = newDelta;
                     }
                     else if ((cleanStyle || !styleDirtyMask.test_bit(slot)) && (noWarm || !warmSlots.test_bit(slot)))
                     {
                         newSt.dirtyBB.clear_bit(slot);
                     }
                 });
                 if (__builtin_expect(!noWarm, 0)) {
                     Key baseSum = 0ULL;
                     (currActive & visualMask).for_each_active([this, &baseSum](u32 s) {
                         baseSum ^= staticLayout[s].staticZobristKey;
                     });
                     lastQueriedChunkBaseZobrist = baseSum;
                 }
                 newSt.dirtyTransformBB = newSt.dirtyBB & (diffAccumulator.transformSlots | cachedTransformDynBB);
                 newSt.dirtyOpacityBB   = newSt.dirtyBB & (diffAccumulator.opacitySlots   | cachedOpacityDynBB);
                 newSt.dirtyContentBB   = newSt.dirtyBB & (diffAccumulator.contentSlots   | cachedContentDynBB);
                 finalize_state_zobrist_keys(newSt, lastQueriedChunkBaseZobrist ^ runningDynamicZobrist, currActive, nextTimeMs);
                 return;
             }
             kfPlateauBB.clear();
             minKfPlateauEndMs = TIME_MAX_SENTINEL_I64;
             // [CHUNK BOUNDARY / ENTER-EXIT / SEEK PATH]:
             Mask enteredBB = currActive;
             Mask exitedBB{0ULL};
             Mask reenteredBB{0ULL};
             bool bgEdge = false;
             const Mask warmSlots = warmDirtySlotsBB;
             warmDirtySlotsBB.clear();
             if (validHistory)
             {
                 const Mask prevActive = prevSt->activeBB;
                 enteredBB = currActive & ~prevActive;
                 exitedBB  = prevActive & ~currActive;
                 reenteredBB = prevSt->pseudoCulledBB & ~newSt.pseudoCulledBB & currActive;
                 bgEdge = ((enteredBB | exitedBB) & byTypeBB[static_cast<u32>(MediaType::Background)]).any();
                 Mask mutatedBB = currActive & cachedAllDyn;
                 if (__builtin_expect(bgEdge | styleChanged, 0))
                 {
                     const Mask styleAffectedBB = byTypeBB[static_cast<u32>(MediaType::Text)] |
                                                  byTypeBB[static_cast<u32>(MediaType::Lyric)] |
                                                  byTypeBB[static_cast<u32>(MediaType::Background)] |
                                                  byTypeBB[static_cast<u32>(MediaType::Path)];
                     mutatedBB |= (currActive & styleAffectedBB);
                 }
                 newSt.dirtyBB = enteredBB | exitedBB | mutatedBB | reenteredBB | warmSlots;
             }
             else
             {
                 newSt.dirtyBB = currActive | warmSlots;
             }
             currentTimeMs = nextTimeMs;
             stepEpoch = epoch;
             st = &newSt;
             const Mask allVaryingVisual = currActive & cachedAllVaryingVisual;
             rampingSlotsBB &= activeChunkDyn;
             evaluate_runtime_slots(newSt, !validHistory || styleChanged || bgEdge || warmSlots.any());
             Key baseZobrist = lastQueriedChunkBaseZobrist;
             if (newSt.chunkTag == INVALID_U16)
             {
                 baseZobrist = 0ULL;
                 (currActive & visualMask).for_each_active([this, &baseZobrist](u32 s) {
                     baseZobrist ^= staticLayout[s].staticZobristKey;
                 });
                 lastQueriedChunkBaseZobrist = baseZobrist;
             }
             const Key dynXor = accumulate_dynamic_zobrist_xor(allVaryingVisual, nextTimeMs);
             runningDynamicZobrist = dynXor;
             const Mask enteredOrReentered = enteredBB | reenteredBB;
             newSt.dirtyTransformBB = (newSt.dirtyBB & (diffAccumulator.transformSlots | cachedTransformDynBB)) | enteredOrReentered;
             newSt.dirtyOpacityBB   = (newSt.dirtyBB & (diffAccumulator.opacitySlots   | cachedOpacityDynBB))   | enteredOrReentered;
             newSt.dirtyContentBB   = (newSt.dirtyBB & (diffAccumulator.contentSlots   | cachedContentDynBB))   | enteredOrReentered | exitedBB;
             finalize_state_zobrist_keys(newSt, baseZobrist ^ dynXor, currActive, nextTimeMs);
         }
        // Restores the previous state if its history-ring generation has not been overwritten.
       inline bool undo_step()
       {
           if (!st || !st->previous)
               return false;
           // Ring 256 ô bị ghi đè sau khi quay vòng: previous không còn đúng đời thì từ chối lùi
           if (st->previous->generation + 1 != st->generation)
               return false;
           st = st->previous;
           currentTimeMs = st->timeMs;
           if (st) evaluate_runtime_slots(*st, true);
           return true;
       }
        // true nếu state hiện hành đã đúng thời điểm t và dữ liệu chưa đổi kể từ lần step_to
       // Checks timestamp and configuration epoch for idempotent state reuse.
       inline bool is_current(TimePoint t) const
       {
           return st != nullptr && currentTimeMs == t && stepEpoch == epoch;
       }
       // Xóa sạch lịch sử bước nhảy khi seek hoặc loop để buộc tính toán vi sai lại từ đầu
       inline void invalidate_history() noexcept
       {
           st = nullptr;
           currentTimeMs = -1;
           stepEpoch = INVALID_U32;
           lastQueriedChunkTag = INVALID_U16;
           lastQueriedChunkBaseZobrist = 0ULL;
           lastQueriedChunkKfActiveBB.clear();
           prevChunkKfActiveBB.clear();
           runningDynamicZobrist = 0ULL;
           overflowValidFromMs = TIME_MAX_SENTINEL_I64;
           overflowValidUntilMs = -1;
           kfPlateauBB.clear();
           minKfPlateauEndMs = TIME_MAX_SENTINEL_I64;
           evalVisibleBB.clear();
           warmDirtySlotsBB.clear();
       }
       inline bool is_live_dl_current(u32 cmdCount) const noexcept
       {
           return st != nullptr && stepEpoch == epoch && st->dlReady != 0 &&
                  st->pad == display_list_generation() && st->commandCount == cmdCount;
       }
        // Invalidates current-state reuse after a timeline configuration update.
       inline void rebuild_dynamic_caches() noexcept
       {
           visualSlotsBB   = ~byTypeBB[static_cast<u32>(MediaType::Audio)] & byTypeBB[ALL_ITEMS];
           textSlotsBB     = byTypeBB[static_cast<u32>(MediaType::Text)] | byTypeBB[static_cast<u32>(MediaType::Lyric)];
           audioSlotsBB    = byTypeBB[static_cast<u32>(MediaType::Audio)];
           dirtyGenSlotsBB = visualSlotsBB;
           continuousDynBB = byTypeBB[static_cast<u32>(MediaType::Video)] | byTypeBB[static_cast<u32>(MediaType::Audio)];
           cachedVisualMask = visualSlotsBB;
           cachedIntrinsicDynAll = byDynamicAxisBB[static_cast<u32>(DynamicAxis::Intrinsic)];
           cachedKfDynAll        = byDynamicAxisBB[static_cast<u32>(DynamicAxis::Keyframe)];
           cachedAllDyn          = cachedIntrinsicDynAll | cachedKfDynAll;
           cachedAllVaryingVisual = cachedVisualMask &
               (byDynamicAxisBB[static_cast<u32>(DynamicAxis::IntrinsicVisual)] |
                byDynamicAxisBB[static_cast<u32>(DynamicAxis::KeyframeVisual)]);
           cachedTransformDynBB = byModeBB[static_cast<u32>(TrackMode::Harmonic)] |
                                  byModeBB[static_cast<u32>(TrackMode::Kinematic)];
           cachedOpacityDynBB   = byModeBB[static_cast<u32>(TrackMode::Kinematic)];
           cachedContentDynBB   = textSlotsBB |
                                  byModeBB[static_cast<u32>(TrackMode::Procedural)] |
                                  byTypeBB[static_cast<u32>(MediaType::Video)] |
                                  byTypeBB[static_cast<u32>(MediaType::Background)];
           recompute_static_offscreen_bb();
       }

       inline void recompute_static_offscreen_bb() noexcept
       {
           staticOffscreenBB.clear();
           const Mask staticOrHarmonic = (byModeBB[static_cast<u32>(TrackMode::Static)] |
                                          byModeBB[static_cast<u32>(TrackMode::Harmonic)]) &
                                         cachedVisualMask &
                                         ~byTypeBB[static_cast<u32>(MediaType::Background)];
           if (!staticOrHarmonic)
               return;

           constexpr float PADDING_HALO = 32.0f;
           staticOrHarmonic.for_each_active([this, PADDING_HALO](u32 s) {
               const auto &sl = staticLayout[s];
               const TimelineItem &it = items[s];
               const Rect<float> &b = sl.aabb;
               const float w = b.x1 - b.x0;
               const float h = b.y1 - b.y0;

               const bool rotates = (sl.mode == TrackMode::Harmonic) ||
                                    (it.rotateDeg != 0.0f) ||
                                    (sl.rotateFlags != 0) ||
                                    (it.spinSpeed != 0.0f);

               if (!rotates)
               {
                   if ((b.x1 + PADDING_HALO <= 0.0f) || (b.x0 - PADDING_HALO >= stageW) ||
                       (b.y1 + PADDING_HALO <= 0.0f) || (b.y0 - PADDING_HALO >= stageH))
                   {
                       staticOffscreenBB.set_bit(s);
                   }
               }
               else
               {
                   const float px = sl.hasCustomOrigin ? sl.originX : (b.x0 + 0.5f * w);
                   const float py = sl.hasCustomOrigin ? sl.originY : (b.y0 + 0.5f * h);
                   const float maxDx = stockcut::max(std::abs(b.x0 - px), std::abs(b.x1 - px));
                   const float maxDy = stockcut::max(std::abs(b.y0 - py), std::abs(b.y1 - py));
                   const float radius = sqrt_f32(maxDx * maxDx + maxDy * maxDy) + PADDING_HALO;

                   if ((px + radius <= 0.0f) || (px - radius >= stageW) ||
                       (py + radius <= 0.0f) || (py - radius >= stageH))
                   {
                       staticOffscreenBB.set_bit(s);
                   }
               }
           });
       }
       inline Mask static_offscreen_bb() const noexcept { return staticOffscreenBB; }
       inline void touch()
       {
           temporalTable.dirty = true;
           overflowValidFromMs = TIME_MAX_SENTINEL_I64;
           overflowValidUntilMs = -1;
           ++epoch;
           ++topologyEpoch;
           rebuild_dynamic_caches();
       }
       // Returns the configuration epoch guarding cached frame state.
       inline u32 configuration_epoch() const { return epoch; }
       inline u32 topology_epoch() const noexcept { return topologyEpoch; }
        // Counts activated keyframe.content entries for deterministic native text reveal state.
        inline u16 resolve_active_kf_content_steps(u32 slot, TimePoint nowMs) const noexcept
       {
           const u32 s = slot & (CAPACITY - 1u);
           const KeyframeTrack &tr = tracks[s];
           if (tr.contentMask == 0 || !tr.has_arena_frames())
               return 0;
           const i32 relMs = static_cast<i32>(nowMs - items[s].startMs);
           const u16 storageIdx = walk_arena_kf_cursor(tr, tr.lastContentIdx, relMs);
           const u32 activeMask = static_cast<u32>((1ULL << storageIdx) - 1ULL) & tr.contentMask;
           return static_cast<u16>(BitOps<u32>::popcount(activeMask));
       }
        // Computes text reveal progress and packs content-step/mode data for CMD_DRAW_TEXT.
       inline void evaluate_text_state(u32 slot, TimePoint nowMs, u16 &outFlags, u16 &outFilterSlot) const
       {
           constexpr u16 PERMILLE_MAX_U16 = static_cast<u16>(PERMILLE_MAX);
           const TimelineItem &it = items[slot];
           const u8 revMode = activeSlotBlocks[slot].template get_value<u8, text_bit::REVEAL_MODE_0, 4>();
           const u16 revDur = static_cast<u16>(activeSlotBlocks[slot].template get_value<float, text_reg::REVEAL_DURATION_MS>());
           const u16 kfSteps = resolve_active_kf_content_steps(slot, nowMs);
            outFilterSlot = static_cast<u16>((kfSteps << 8) | (revMode & INVALID_U8));
           if (revMode == 0 && revDur == 0)
           {
               outFlags = PERMILLE_MAX_U16;
               return;
           }
           const i64 elapsedMs = nowMs - it.startMs;
           if (elapsedMs <= 0)
           {
               outFlags = 0;
           }
           else if (revDur > 0)
           {
               outFlags = (elapsedMs < revDur)
                   ? static_cast<u16>((elapsedMs * PERMILLE_MAX) / revDur)
                   : PERMILLE_MAX_U16;
           }
           else
           {
               const i64 dur = (it.endMs > it.startMs) ? (it.endMs - it.startMs) : 1;
               outFlags = (elapsedMs < dur) ? static_cast<u16>((elapsedMs * PERMILLE_MAX) / dur) : PERMILLE_MAX_U16;
           }
       }
        // Interpolates a fixed keyframe track directly from KeyframeArena (Cold/Aux & fallback accessor).
       inline void interpolate_kinematic(u32 slot, TimePoint nowMs, Rect<float> &outBox, float &outOpacity, float &outRotate) const
       {
           const u32 s = slot & (CAPACITY - 1u);
           u32 unusedFill = 0u;
           float unusedBlur = 0.0f;
           bool isRamping = false;
           const i32 relMs = static_cast<i32>(nowMs - items[s].startMs);
           if (!evaluate_unified_kinematic(s, relMs, outBox, outOpacity, outRotate, unusedFill, unusedBlur, isRamping))
           {
               rampingSlotsBB.clear_bit(s);
               outBox = staticLayout[s].aabb;
               outOpacity = baseSlotBlocks[s].template get_value<float, 4>(6);
               outRotate = items[s].rotateDeg;
               return;
           }
           if (isRamping) rampingSlotsBB.set_bit(s);
           else rampingSlotsBB.clear_bit(s);
       }

       template <MediaType Type = MediaType::None>
       inline void interpolate_kinematic_T(u32 slot, TimePoint nowMs, Rect<float> &outBox, float &outOpacity, float &outRotate) const
       {
           interpolate_kinematic(slot, nowMs, outBox, outOpacity, outRotate);
       }

       // Returns the preclassified runtime motion mode for one slot.
        inline TrackMode mode_of(u32 slot) const noexcept { return staticLayout[slot].mode; }
        inline Bitboard static_inner_footprint_bb(u32 slot) const noexcept { return staticLayout[slot & (MAX_TIMELINE_SLOTS - 1u)].innerFootprint; }
       template <TrackMode Mode>
       inline Mask by_mode_bb() const noexcept { return byModeBB[static_cast<u32>(Mode)]; }
       template <MediaType Type>
       inline Mask by_type_bb() const noexcept { return byTypeBB[static_cast<u32>(Type)]; }
       template <MediaTier Tier>
       inline Mask by_tier_bb() const noexcept { return byTierBB[static_cast<u32>(Tier)]; }
       template <DynamicNature Nature>
       inline Mask by_nature_bb() const noexcept { return byNatureBB[static_cast<u32>(Nature)]; }
       template <DynamicAxis Axis>
       inline Mask by_dynamic_axis_bb() const noexcept { return byDynamicAxisBB[static_cast<u32>(Axis)]; }
       inline const ByModeMask<Mask>& by_mode_bb_array() const noexcept { return byModeBB; }
       inline const ByTypeMask<Mask>& by_type_bb_array() const noexcept { return byTypeBB; }
       inline const ByTierMask<Mask>& by_tier_bb_array() const noexcept { return byTierBB; }
       inline const ByDynamicNatureMask<Mask>& by_nature_bb_array() const noexcept { return byNatureBB; }
       inline const ByDynamicAxisMask<Mask>& by_dynamic_axis_bb_array() const noexcept { return byDynamicAxisBB; }
       inline u32 varying_props_mask(u32 slot) const noexcept { return static_cast<u32>(staticLayout[slot & (MAX_TIMELINE_SLOTS - 1u)].varyingProps); }
       inline Mask chunk_kf_active_bb() const noexcept { return lastQueriedChunkKfActiveBB; }
       inline Key chunk_base_zobrist() const noexcept { return lastQueriedChunkBaseZobrist; }
       inline Key running_dynamic_zobrist() const noexcept { return runningDynamicZobrist; }
        // Hashes quantized interpolated geometry/opacity/rotation for kinematic cache validation.
       inline u64 kinematic_state_key(u32 slot, TimePoint nowMs) const
       {
           constexpr float KINEMATIC_GEOM_QUANT    = 16.0f;
           constexpr float KINEMATIC_OPACITY_QUANT = 1024.0f;
           Rect<float> box;
           float op = 1.0f, rot = 0.0f;
           interpolate_kinematic(slot, nowMs, box, op, rot);
           const auto q = [](float v, float s) noexcept -> u64
           { return static_cast<u64>(static_cast<i64>(v * s)); };
           u64 h = HASH_GOLDEN_64;
           h = (h ^ q(box.x0, KINEMATIC_GEOM_QUANT)) * SPLITMIX_MUL1;
           h = (h ^ q(box.y0, KINEMATIC_GEOM_QUANT)) * SPLITMIX_MUL1;
           h = (h ^ q(box.x1, KINEMATIC_GEOM_QUANT)) * SPLITMIX_MUL1;
           h = (h ^ q(box.y1, KINEMATIC_GEOM_QUANT)) * SPLITMIX_MUL1;
           h = (h ^ q(op, KINEMATIC_OPACITY_QUANT)) * SPLITMIX_MUL1;
           h = (h ^ q(rot, KINEMATIC_GEOM_QUANT)) * SPLITMIX_MUL1;
           return h ^ (h >> 31);
       }
       // Generates the ordered display command list for a precomputed state.
       u32 generate_display_list(StateInfo<Mask> *targetSt);
       // Returns a read-only reference to the stored item record.
       inline const TimelineItem &item(u32 slot) const noexcept { return items[slot]; }
       // Returns cached static bounds for the slot.
       inline const Rect<float> &static_aabb(u32 slot) const noexcept { return staticLayout[slot].aabb; }
       inline void set_slot_top_level(u32 slot, u16 topLevel, bool isGroupChild) noexcept
       {
           const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
           staticLayout[s].topLevelIndex = static_cast<u8>(topLevel);
           staticLayout[s].isGroupChild = isGroupChild ? 1u : 0u;
       }
       inline u16 slot_top_level(u32 slot) const noexcept
       {
           return staticLayout[slot & (MAX_TIMELINE_SLOTS - 1u)].topLevelIndex;
       }
       inline bool slot_is_group_child(u32 slot) const noexcept
       {
           return staticLayout[slot & (MAX_TIMELINE_SLOTS - 1u)].isGroupChild != 0u;
       }
       inline void set_top_level_count(u32 count) noexcept { topLevelCount = count; }
       inline u32 get_top_level_count() const noexcept { return topLevelCount; }


        // Stockfish-Style Compile-Time Slot Evaluator: 0 runtime type/mode branches inside hot path.
       template <TrackMode Mode, MediaType Type>
       inline void evaluate_slot_T(u32 slot, TimePoint nowMs)
       {
           constexpr u16 PERMILLE_MAX_U16 = static_cast<u16>(PERMILLE_MAX);
           using Traits = MediaTrackTraits<Type>;
           const TimelineItem &it = items[slot];
           Evaluated &ev = evaluatedSlots[slot];
           ev.slot = static_cast<u16>(slot);
           ev.zRank = static_cast<u16>(it.zIndex);
           ev.filter = 0;
           constexpr float kModeParam = (Mode == TrackMode::Static) ? 0.0f : 1.0f;
           ev.customParams[0] = kModeParam;
            const auto &sl = staticLayout[slot];
            if constexpr (!Traits::kIsVisual)
           {
               ev.isVisible = 0;
               return;
           }
           else if constexpr (Mode == TrackMode::Static)
           {
               ev.aabb = sl.aabb;
               ev.opacity = 1.0f;
               ev.rotateDeg = it.rotateDeg;
               ev.opCode = Traits::kOpCode;
               if constexpr (Traits::kIsBackground)
               {
                   ev.resourceId = resolve_bass_bg_color(slot, nowMs);
                   ev.flags = 1;
               }
               else
               {
                   if constexpr (Traits::kIsText)
                   {
                       const auto &st = activeStyleEnv.targets[static_cast<u32>(MediaType::Text)];
                       const auto &stL = activeStyleEnv.targets[static_cast<u32>(MediaType::Lyric)];
                       if (st.has_prop(text_reg::COLOR))
                           ev.resourceId = st.template get_value<u32, text_reg::COLOR>();
                       else if (stL.has_prop(text_reg::COLOR))
                           ev.resourceId = stL.template get_value<u32, text_reg::COLOR>();
                       else if (it.resourceId != 0)
                           ev.resourceId = it.resourceId;
                       else
                           ev.resourceId = resolve_text_color(slot, nowMs);
                       ev.flags = PERMILLE_MAX_U16;
                       if (st.has_prop(text_reg::OPACITY)) ev.opacity *= st.template get_value<float, text_reg::OPACITY>();
                   }
                   else
                   {
                       ev.resourceId = it.resourceId;
                       ev.flags = sl.rotateFlags;
                       if constexpr (Traits::kIsPath)
                       {
                           ev.resourceId = resolve_path_fill_color(slot, nowMs);
                           const auto &st = activeStyleEnv.targets[static_cast<u32>(MediaType::Path)];
                           if (st.has_prop(text_reg::OPACITY)) ev.opacity *= st.template get_value<float, text_reg::OPACITY>();
                       }
                   }
                   if constexpr (Traits::kIsImage || Traits::kIsVideo)
                   {
                       const auto &st = activeStyleEnv.targets[static_cast<u32>(it.type)];
                       if (st.has_prop(text_reg::OPACITY)) ev.opacity *= st.template get_value<float, text_reg::OPACITY>();
                   }
                   if constexpr (Traits::kIsImage)
                   {
                       ev.filter = sl.maskFilter;
                   }
               }
               ev.isVisible = (ev.opacity > OPACITY_EPSILON) ? 1u : 0u;
           }
           else if constexpr (Mode == TrackMode::Harmonic)
           {
               const float relMs = (nowMs > it.startMs) ? static_cast<float>(nowMs - it.startMs) : 0.0f;
               const float spinAngle = fast_fmod360(relMs * (it.spinSpeed * INV_MS_PER_SEC_F32));
               const float totalRot = fast_fmod360(spinAngle + it.rotateDeg);
                ev.aabb = sl.aabb;
               ev.opacity = 1.0f;
               ev.rotateDeg = totalRot;
               ev.resourceId = it.resourceId;
               ev.opCode = Traits::kOpCode;
               if constexpr (Traits::kIsText)
               {
                   const auto &st = activeStyleEnv.targets[static_cast<u32>(MediaType::Text)];
                   const auto &stL = activeStyleEnv.targets[static_cast<u32>(MediaType::Lyric)];
                   if (st.has_prop(text_reg::COLOR))
                       ev.resourceId = st.template get_value<u32, text_reg::COLOR>();
                   else if (stL.has_prop(text_reg::COLOR))
                       ev.resourceId = stL.template get_value<u32, text_reg::COLOR>();
                   else if (it.resourceId != 0)
                       ev.resourceId = it.resourceId;
                   else
                       ev.resourceId = resolve_text_color(slot, nowMs);
                   ev.flags = PERMILLE_MAX_U16;
                   if (st.has_prop(text_reg::OPACITY)) ev.opacity *= st.template get_value<float, text_reg::OPACITY>();
               }
               else
               {
                   ev.flags = static_cast<u16>(totalRot);
                   if constexpr (Traits::kIsPath)
                   {
                       ev.resourceId = resolve_path_fill_color(slot, nowMs);
                       const auto &st = activeStyleEnv.targets[static_cast<u32>(MediaType::Path)];
                       if (st.has_prop(text_reg::OPACITY)) ev.opacity *= st.template get_value<float, text_reg::OPACITY>();
                   }
               }
               if constexpr (Traits::kIsImage || Traits::kIsVideo)
               {
                   const auto &st = activeStyleEnv.targets[static_cast<u32>(it.type)];
                   if (st.has_prop(text_reg::OPACITY)) ev.opacity *= st.template get_value<float, text_reg::OPACITY>();
               }
               if constexpr (Traits::kIsImage)
               {
                   ev.filter = sl.maskFilter;
               }
               ev.isVisible = (ev.opacity > OPACITY_EPSILON) ? 1u : 0u;
           }
           else if constexpr (Mode == TrackMode::Kinematic)
           {
               const KeyframeTrack &hdr = tracks[slot];
               Rect<float> box = sl.aabb;
               float op = baseSlotBlocks[slot].template get_value<float, 4>(6);
               float rot = it.rotateDeg;
               u32 animFillPacked = 0u;
               float animBlur = 0.0f;
               bool isRamping = false;
               const i32 relMs = static_cast<i32>(nowMs - it.startMs);
               TimePoint plateauRelEnd = 0;
               if (evaluate_unified_kinematic(slot, relMs, box, op, rot, animFillPacked, animBlur, isRamping, &plateauRelEnd))
               {
                   if (isRamping) {
                       rampingSlotsBB.set_bit(slot);
                       kfPlateauBB.clear_bit(slot);
                   } else {
                       rampingSlotsBB.clear_bit(slot);
                       const TimePoint absPlateauEnd = it.startMs + plateauRelEnd;
                       kfPlateauEndMs[slot] = absPlateauEnd;
                       if (absPlateauEnd > nowMs) {
                           kfPlateauBB.set_bit(slot);
                           if (absPlateauEnd < minKfPlateauEndMs) minKfPlateauEndMs = absPlateauEnd;
                       } else {
                           kfPlateauBB.clear_bit(slot);
                       }
                   }
               }
               ev.aabb = box;
               ev.opacity = op;
               ev.rotateDeg = rot;
               ev.resourceId = it.resourceId;
               if (hdr.activeColorKfMask & 4u)
               {
                   ev.customParams[1] = animBlur;
               }
                if constexpr (Traits::kIsText)
               {
                   const auto &st = activeStyleEnv.targets[static_cast<u32>(MediaType::Text)];
                   const auto &stL = activeStyleEnv.targets[static_cast<u32>(MediaType::Lyric)];
                   if (hdr.activeColorKfMask & 1u)
                       ev.resourceId = animFillPacked;
                   else if (st.has_prop(text_reg::COLOR))
                       ev.resourceId = st.template get_value<u32, text_reg::COLOR>();
                   else if (stL.has_prop(text_reg::COLOR))
                       ev.resourceId = stL.template get_value<u32, text_reg::COLOR>();
                   else if (it.resourceId != 0)
                       ev.resourceId = it.resourceId;
                   else
                       ev.resourceId = resolve_text_color(slot, nowMs);
                   u16 progressPermille = PERMILLE_MAX_U16;
                   u16 filterPacked = 0;
                   evaluate_text_state(slot, nowMs, progressPermille, filterPacked);
                   ev.opCode = CMD_DRAW_TEXT;
                   ev.flags = progressPermille;
                   ev.filter = filterPacked;
                   if (st.has_prop(text_reg::OPACITY)) ev.opacity *= st.template get_value<float, text_reg::OPACITY>();
               }
               else
               {
                   ev.opCode = Traits::kOpCode;
                   if constexpr (Traits::kIsBackground)
                   {
                       ev.resourceId = (hdr.activeColorKfMask & 1u)
                                           ? animFillPacked
                                           : resolve_bass_bg_color(slot, nowMs);
                   }
                   else if constexpr (Traits::kIsPath)
                   {
                       ev.resourceId = (hdr.activeColorKfMask & 1u)
                                           ? animFillPacked
                                           : resolve_path_fill_color(slot, nowMs);
                       const auto &st = activeStyleEnv.targets[static_cast<u32>(MediaType::Path)];
                       if (st.has_prop(text_reg::OPACITY)) ev.opacity *= st.template get_value<float, text_reg::OPACITY>();
                   }
                   ev.flags = static_cast<u16>(fast_fmod360(rot));
                   if constexpr (Traits::kIsImage)
                   {
                       ev.filter = sl.maskFilter;
                   }
                   if constexpr (Traits::kIsImage || Traits::kIsVideo)
                   {
                       const auto &st = activeStyleEnv.targets[static_cast<u32>(it.type)];
                       if (st.has_prop(text_reg::OPACITY)) ev.opacity *= st.template get_value<float, text_reg::OPACITY>();
                   }
               }
               ev.isVisible = (op > OPACITY_EPSILON) ? 1u : 0u;
           }
           else if constexpr (Mode == TrackMode::Procedural)
           {
               if constexpr (Traits::kIsText)
               {
                   u16 progressPermille = PERMILLE_MAX_U16;
                   u16 filterPacked = 0;
                   evaluate_text_state(slot, nowMs, progressPermille, filterPacked);
                   ev.aabb = sl.aabb;
                   ev.opacity = 1.0f;
                   ev.rotateDeg = it.rotateDeg;
                   const auto &st = activeStyleEnv.targets[static_cast<u32>(MediaType::Text)];
                   const auto &stL = activeStyleEnv.targets[static_cast<u32>(MediaType::Lyric)];
                   if (st.has_prop(text_reg::COLOR))
                       ev.resourceId = st.template get_value<u32, text_reg::COLOR>();
                   else if (stL.has_prop(text_reg::COLOR))
                       ev.resourceId = stL.template get_value<u32, text_reg::COLOR>();
                   else if (it.resourceId != 0)
                       ev.resourceId = it.resourceId;
                   else
                       ev.resourceId = resolve_text_color(slot, nowMs);
                   if (st.has_prop(text_reg::OPACITY)) ev.opacity *= st.template get_value<float, text_reg::OPACITY>();
                   ev.opCode = CMD_DRAW_TEXT;
                   ev.flags = progressPermille;
                   ev.filter = filterPacked;
                   ev.isVisible = (ev.opacity > OPACITY_EPSILON) ? 1u : 0u;
               }
               else if constexpr (Traits::kIsBackground)
               {
                   ev.aabb = sl.aabb;
                   ev.opacity = 1.0f;
                   ev.rotateDeg = 0.0f;
                   ev.resourceId = resolve_bass_bg_color(slot, nowMs);
                   ev.opCode = CMD_DRAW_RECT;
                   ev.flags = 1;
                   ev.filter = 0;
                   ev.isVisible = 1;
               }
               else
               {
                   ev.aabb = sl.aabb;
                   ev.opacity = 1.0f;
                   ev.rotateDeg = it.rotateDeg;
                   ev.resourceId = it.resourceId;
                   ev.opCode = Traits::kOpCode;
                   ev.flags = sl.rotateFlags;
                   if constexpr (Traits::kIsPath)
                   {
                       ev.resourceId = resolve_path_fill_color(slot, nowMs);
                       const auto &st = activeStyleEnv.targets[static_cast<u32>(MediaType::Path)];
                       if (st.has_prop(text_reg::OPACITY)) ev.opacity *= st.template get_value<float, text_reg::OPACITY>();
                   }
                   if constexpr (Traits::kIsImage || Traits::kIsVideo)
                   {
                       const auto &st = activeStyleEnv.targets[static_cast<u32>(it.type)];
                       if (st.has_prop(text_reg::OPACITY)) ev.opacity *= st.template get_value<float, text_reg::OPACITY>();
                   }
                   if constexpr (Traits::kIsImage)
                   {
                       ev.filter = sl.maskFilter;
                   }
                   else
                   {
                       ev.filter = 0;
                   }
                   ev.isVisible = (ev.opacity > OPACITY_EPSILON) ? 1u : 0u;
               }
           }
            evalVisibleBB.set_bit(slot, (ev.isVisible != 0u) & (ev.opacity > OPACITY_EPSILON));
       }
        template <TrackMode Mode, MediaType Type>
       inline void evaluate_bucket_T(Mask mask, TimePoint nowMs)
       {
           mask.for_each_active([this, nowMs](u32 slot) {
               evaluate_slot_T<Mode, Type>(slot, nowMs);
           });
       }
        template <MediaType Type>
        inline void evaluate_media_bucket_direct(Mask typeMask, TimePoint nowMs)
        {
            const Mask staticMask = typeMask & byModeBB[static_cast<u32>(TrackMode::Static)];
            if (staticMask)
                evaluate_bucket_T<TrackMode::Static, Type>(staticMask, nowMs);

            const Mask harmonicMask = typeMask & byModeBB[static_cast<u32>(TrackMode::Harmonic)];
            if (harmonicMask)
                evaluate_bucket_T<TrackMode::Harmonic, Type>(harmonicMask, nowMs);

            const Mask kinematicMask = typeMask & byModeBB[static_cast<u32>(TrackMode::Kinematic)];
            if (kinematicMask)
                evaluate_bucket_T<TrackMode::Kinematic, Type>(kinematicMask, nowMs);

            const Mask proceduralMask = typeMask & byModeBB[static_cast<u32>(TrackMode::Procedural)];
            if (proceduralMask)
                evaluate_bucket_T<TrackMode::Procedural, Type>(proceduralMask, nowMs);
        }
        inline void dispatch_evaluate_media_bucket(u32 typeIdx, Mask typeMask, TimePoint nowMs)
        {
            switch (typeIdx)
            {
                case static_cast<u32>(MediaType::Background): evaluate_media_bucket_direct<MediaType::Background>(typeMask, nowMs); break;
                case static_cast<u32>(MediaType::Video):      evaluate_media_bucket_direct<MediaType::Video>(typeMask, nowMs); break;
                case static_cast<u32>(MediaType::Image):      evaluate_media_bucket_direct<MediaType::Image>(typeMask, nowMs); break;
                case static_cast<u32>(MediaType::Path):       evaluate_media_bucket_direct<MediaType::Path>(typeMask, nowMs); break;
                case static_cast<u32>(MediaType::Lyric):      evaluate_media_bucket_direct<MediaType::Lyric>(typeMask, nowMs); break;
                case static_cast<u32>(MediaType::Text):       evaluate_media_bucket_direct<MediaType::Text>(typeMask, nowMs); break;
                default: break;
            }
        }
        // Stockfish-Style Runtime Evaluation: Pre-evaluates active visual slots into Evaluated cachelines
        inline void evaluate_runtime_slots(StateInfo<Mask>& targetSt, bool forceStaticEval = true)
        {
            const TimePoint nowMs = targetSt.timeMs;
            // Clear visibility for exited or newly pseudo-culled slots in 1 single bitwise pass
            const Mask hiddenSlots = targetSt.dirtyBB & (~targetSt.activeBB | targetSt.pseudoCulledBB);
            if (__builtin_expect(hiddenSlots.any(), 0))
            {
                hiddenSlots.for_each_active([this](u32 slot) {
                    evaluatedSlots[slot].isVisible = 0;
                });
                evalVisibleBB &= ~hiddenSlots;
            }
            Mask slotsToEvaluate = (targetSt.dirtyBB & targetSt.activeBB & cachedVisualMask) & ~targetSt.pseudoCulledBB;
            if (!forceStaticEval)
            {
                slotsToEvaluate &= ~(byModeBB[static_cast<u32>(TrackMode::Static)] & evalVisibleBB);
            }
            if (!slotsToEvaluate)
                return;

            constexpr u16 VISUAL_TYPES_MASK =
                (1u << static_cast<u32>(MediaType::Background)) |
                (1u << static_cast<u32>(MediaType::Video)) |
                (1u << static_cast<u32>(MediaType::Image)) |
                (1u << static_cast<u32>(MediaType::Path)) |
                (1u << static_cast<u32>(MediaType::Lyric)) |
                (1u << static_cast<u32>(MediaType::Text));

            u16 types = presentTypesMask & VISUAL_TYPES_MASK;
            while (types)
            {
                const u32 typeIdx = static_cast<u32>(__builtin_ctz(types));
                types &= static_cast<u16>(types - 1u);

                const Mask typeMask = slotsToEvaluate & byTypeBB[typeIdx];
                if (!typeMask)
                    continue;

                dispatch_evaluate_media_bucket(typeIdx, typeMask, nowMs);
            }
        }
        // Returns immutable evaluated runtime state for a slot.
       inline const Evaluated &evaluated_slot(u32 slot) const
       {
           return evaluatedSlots[slot & (MAX_TIMELINE_SLOTS - 1u)];
       }
        // Exposes the contiguous 4 KB Evaluated arena for zero-copy consumer reads.
       inline const Evaluated *evaluated_slots_arena() const
       {
           return evaluatedSlots.data();
       }
       // Truy xuất trực tiếp toán tử Monoid MathOp của slot trong O(1)
       inline MathOp slot_math_op(u32 slot) const noexcept
       {
           return evaluatedSlots[slot & (MAX_TIMELINE_SLOTS - 1u)].to_math_op();
       }

       [[nodiscard]] inline const SlotDifferential<CAPACITY>& diff_accumulator() const noexcept { return diffAccumulator; }
       [[nodiscard]] inline SlotDifferential<CAPACITY>& diff_accumulator() noexcept { return diffAccumulator; }
    private:
       u32 generate_history_display_list(StateInfo<Mask>* targetSt);
       [[nodiscard]] inline u32 lower_bound_z_bucket(i32 zIndex) const noexcept
       {
           u32 low = 0;
           u32 high = zBucketCount;
           while (low < high)
           {
               const u32 middle = low + (high - low) / 2u;
               if (zBucketLevels[middle] < zIndex) low = middle + 1u;
               else high = middle;
           }
           return low;
       }
       inline void remove_z_order_slot(u32 slot)
       {
           if (!dirtyGenSlotsBB.test_bit(slot)) return;
           const i32 zIndex = items[slot].zIndex;
           const u32 low = lower_bound_z_bucket(zIndex);
           if (low == zBucketCount || zBucketLevels[low] != zIndex) return;
           zBuckets[low].clear_bit(slot);
           if (zBuckets[low]) return;
           for (u32 next = low + 1u; next < zBucketCount; ++next)
           {
               zBucketLevels[next - 1u] = zBucketLevels[next];
               zBuckets[next - 1u] = zBuckets[next];
           }
           --zBucketCount;
           rebuild_z_shadows();
       }
        inline void insert_z_order_slot(u32 slot)
       {
           if (!dirtyGenSlotsBB.test_bit(slot)) return;
           const i32 zIndex = items[slot].zIndex;
           const u32 low = lower_bound_z_bucket(zIndex);
           if (low < zBucketCount && zBucketLevels[low] == zIndex)
           {
               zBuckets[low].set_bit(slot);
               rebuild_z_shadows();
               return;
           }
           if (zBucketCount == CAPACITY) return;
           for (u32 index = zBucketCount; index > low; --index)
           {
               zBucketLevels[index] = zBucketLevels[index - 1u];
               zBuckets[index] = zBuckets[index - 1u];
           }
           zBucketLevels[low] = zIndex;
           zBuckets[low] = Mask::from_slot(slot);
           ++zBucketCount;
           rebuild_z_shadows();
       }
       MultiArray<TimelineItem, CAPACITY> items;
       MultiArray<i32, CAPACITY> zBucketLevels;
       MultiArray<Mask, CAPACITY> zBuckets;
       MultiArray<Mask, CAPACITY> zShadowBB;
       MultiArray<Bitboard, CAPACITY> slotVisibleTiles;
       MultiArray<Rect<float>, CAPACITY> slotScissorBoxes;
       MultiArray<u8, CAPACITY> zRankToSlot_;
       MultiArray<u8, CAPACITY> slotToZRank_;
       u8 zRankCount_ = 0;
       u8 baseBgInsertRank_ = 0;
       Mask staticOccludedSlotsBB{0ULL};
       Mask staticOffscreenBB{0ULL};
       Mask staticFullscreenBlockersBB{0ULL};
       Mask lastQueriedChunkStaticOccludedBB{0ULL};
       Bitboard lastQueriedChunkStaticOpaqueTiles = 0ULL;
       u32 zBucketCount = 0;

       MultiArray<SlotStaticLayout, CAPACITY> staticLayout;
       MultiArray<KeyframeTrack, CAPACITY> tracks;
       MultiArray<BassBeatParams, CAPACITY> bassParams;
       ByTypeMask<Mask> byTypeBB;
       u16 presentTypesMask = 0;
       ByTierMask<Mask> byTierBB;
       ByModeMask<Mask> byModeBB;
       ByDynamicNatureMask<Mask> byNatureBB;
       ByDynamicAxisMask<Mask> byDynamicAxisBB;
       Mask visualSlotsBB{0ULL};
       Mask textSlotsBB{0ULL};
       Mask audioSlotsBB{0ULL};
       Mask dirtyGenSlotsBB{0ULL};
       Mask continuousDynBB{0ULL};
       Mask cachedVisualMask;
       Mask cachedIntrinsicDynAll;
       Mask cachedKfDynAll;
       Mask cachedAllDyn;
       Mask cachedAllVaryingVisual;
       Mask cachedTransformDynBB{0ULL};
       Mask cachedOpacityDynBB{0ULL};
       Mask cachedContentDynBB{0ULL};
       Mask evalVisibleBB{0ULL};
       Mask customOriginBB{0ULL};
       MultiArray<Evaluated, CAPACITY> evaluatedSlots;
       SlotDifferential<CAPACITY> diffAccumulator;
       MultiArray<TextBlock, CAPACITY> baseSlotBlocks;
       MultiArray<TextBlock, CAPACITY> activeSlotBlocks;
       alignas(64) TextBlock baseRootBlock;
       alignas(64) TextBlock activeRootBlock;
       Key runningDynamicZobrist = 0ULL;
       Key lastQueriedChunkBaseZobrist = 0ULL;
       Mask lastQueriedChunkKfActiveBB{0ULL};
       Mask prevChunkKfActiveBB{0ULL};
       TimePoint overflowValidFromMs = TIME_MAX_SENTINEL_I64;
       TimePoint overflowValidUntilMs = -1;
       Key overflowBaseZobrist = 0ULL;
       Mask overflowActiveBB{0ULL};
       Mask overflowKfActiveBB{0ULL};
       Mask overflowStaticOccludedBB{0ULL};
       Bitboard overflowStaticOpaqueTiles = 0ULL;
       u16 overflowChunkTag = INVALID_U16;
       u8 overflowActiveBgSlot = static_cast<u8>(CAPACITY);
       mutable Mask rampingSlotsBB{0ULL};
       mutable Mask kfPlateauBB{0ULL};
       mutable TimePoint minKfPlateauEndMs = TIME_MAX_SENTINEL_I64;
       MultiArray<TimePoint, CAPACITY> kfPlateauEndMs;       // KHONG CO {}
       mutable MultiArray<HornerMicrocode<8>, CAPACITY> slotHorner; // KHONG CO {} (Pha 5 Live SIMD128 Horner ISA)
       mutable MultiArray<u64, CAPACITY> slotHornerKey{};
       TypedKeyframeArena keyframeArena{};
       u32 topLevelCount = 0;
       alignas(64) ChunkTable<Mask> temporalTable;
       StateInfo<Mask> *st;
       TimePoint currentTimeMs;
       TimePoint projectDurationMs;
       float stageW;
       float stageH;
       u32 epoch = 0;
       u32 topologyEpoch = 0;
       u32 stepEpoch = INVALID_U32;
       u16 lastQueriedChunkTag = INVALID_U16;
       u8 lastQueriedChunkActiveBgSlot = static_cast<u8>(CAPACITY);
       Mask warmDirtySlotsBB{0ULL};
   };
         // Returns the single native position instance shared by pipeline and Wasm exports.
      Position<> *timeline_position_get_instance();
   Position<> *position_get_instance();
   // Clears the singleton and resets its preallocated history ring.
   void timeline_position_reset();
   // Returns the next reusable record from the fixed state-history ring.
   StateInfo<> *timeline_position_alloc_state();
   // Advances the singleton state, reusing it when timestamp and epoch are unchanged.
   StateInfo<> *timeline_position_advance(TimePoint t);
   // Returns or generates the display list for the supplied state record.
   u32 timeline_position_display_list(StateInfo<> *st);
   // Vô hiệu hóa bộ đệm DisplayList khi tua hoặc lặp để ép dựng lại DisplayList hoàn toàn
   void timeline_position_display_list_invalidate();
} // namespace stockcut
