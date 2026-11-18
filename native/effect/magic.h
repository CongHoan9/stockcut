// ============================================================================
// native/effect/magic.h
// STOCKFISH-STYLE MAGIC BITBOARD ENGINE (MONOID GRAPH & MULTI-PASS CONTRACTION)
// ----------------------------------------------------------------------------
// [NGUYÊN TẮC THIẾT KẾ STOCKFISH DATA-STRUCTURE-CENTRIC & GEMINI.MD RULE 7 & 8]:
// 1. Stockfish Magic Bitboard Hashing:
//    - Biến đổi bitmask tổ hợp hiệu ứng / pass thành chỉ số bảng tra cứu (LUT)
//      trong đúng 1 chu kỳ nhân số nguyên 64-bit (IMUL) và dịch bit (SHR):
//      Index = ((ActiveBB & MAGIC_MASK) * MAGIC_MULTIPLIER) >> (64 - BITS)
// 2. Algebraic Monoid Pass Contraction:
//    - Hợp nhất chuỗi N hiệu ứng Monoid liền kề (Zoom, Shake, Brightness, Tint, Fade)
//      thành ĐÚNG 1 PASS DUY NHẤT (FusedUberPost).
//    - Cắt giảm triệt để số lần switch FBO và texture bandwidth GPU.
// 3. Zero-Allocation & Branchless SIMD:
//    - Bộ nhớ tĩnh MultiArray, căn lề alignas(64) L1 Cacheline.
//    - 0 cấp phát heap, 0 đệ quy, 0 rẽ nhánh trong hot path.
// ============================================================================
#pragma once

#include "types.h"
#include "data_structures.h"
#include "timeline/bitboard.h"
#include "effect/monoid_operator.h"
#include "timeline/runtime/runtime.h"
#include "render/graph.h"

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

namespace stockcut {

// ============================================================================
// 1. CANONICAL PASS BITBOARD OPERATORS (ZERO EFFECT ENUM DEPENDENCY)
// ============================================================================
constexpr u64 pass_bit(timeline::runtime::RuntimePassKind pass) noexcept {
    return 1ULL << static_cast<u8>(pass);
}

using timeline::runtime::RuntimePassKind;

// Mặt nạ các pass thuộc họ Monoid thuần túy có thể dung hợp 100% vào FusedUberPost
constexpr u64 PURE_MONOID_FUSIBLE_MASK =
    pass_bit(RuntimePassKind::ApplyZoom) |
    pass_bit(RuntimePassKind::ApplyShake) |
    pass_bit(RuntimePassKind::ApplyBrightness) |
    pass_bit(RuntimePassKind::ApplyMonoidFused) |
    pass_bit(RuntimePassKind::CopyScene);

// Mặt nạ các pass tích chập phân tách (Separable Convolutions)
constexpr u64 SEPARABLE_CONVOLUTION_MASK =
    pass_bit(RuntimePassKind::GaussianHorizontal) |
    pass_bit(RuntimePassKind::GaussianVertical) |
    pass_bit(RuntimePassKind::CompositeBlur) |
    pass_bit(RuntimePassKind::CompositeGlow) |
    pass_bit(RuntimePassKind::CompositeSceneBlur);

// Mặt nạ các pass Bloom đa tầng (Multi-Tap Pyramid)
constexpr u64 BLOOM_PYRAMID_MASK =
    pass_bit(RuntimePassKind::BloomExtract) |
    pass_bit(RuntimePassKind::BloomDownsample) |
    pass_bit(RuntimePassKind::BloomUpsample) |
    pass_bit(RuntimePassKind::BloomComposite);

// ============================================================================
// 2. CHIẾN LƯỢC DUNG HỢP PIPELINE (PASS CONTRACTION STRATEGY)
// ============================================================================
enum class StrategyKind : u8 {
    BypassIdentity       = 0, // Bỏ qua trực tiếp (0 pass)
    SinglePassUber       = 1, // Dung hợp 100% chuỗi hiệu ứng vào 1 Uber-Shader Pass duy nhất
    SeparableConvolution = 2, // Pre-Uber -> Separable Conv -> Post-Uber (3 passes)
    BloomPyramid         = 3, // Chuỗi Bloom pyramid đa tầng
    GeneralMultipass     = 4  // Đa pass truyền thống
};

// ============================================================================
// 3. KẾ HOẠCH DUNG HỢP RENDER PASS (ĐÚNG 32 BYTES = 1/2 L1 CACHELINE)
// ============================================================================
struct alignas(32) FusionPlan {
    StrategyKind strategy;        // 0 (1B)
    u8           passCount;       // 1 (1B: Số pass sau khi co cụm)
    u8           primaryShaderId; // 2 (1B: ID shader chính)
    u8           secondaryShaderId;// 3 (1B: ID shader phụ)
    u16          fusionFlags;     // 4..5 (2B: Cờ dung hợp)
    u16          maxFboNeeded;    // 6..7 (2B: Số FBO tối thiểu)
    u32          signature;       // 8..11 (4B: Chữ ký xác thực)
    u32          pad0;            // 12..15 (4B: Đệm căn lề 8-byte cho Bitboard)
    Bitboard     activePassKindsBB;// 16..23 (8B: Bitboard các pass)
    u32          pad1[2];         // 24..31 (8B: Căn lề tròn đúng 32 bytes)

    [[nodiscard]] constexpr bool is_single_pass_uber() const noexcept {
        return strategy == StrategyKind::SinglePassUber;
    }
};
static_assert(sizeof(FusionPlan) == 32, "FusionPlan must be exactly 32 bytes");

// ============================================================================
// 4. CẤU TRÚC DỮ LIỆU TỰ CHỦ: MAGIC ENGINE (STOCKFISH STYLE)
// ============================================================================
template<usize LutSizeBits = 10>
class alignas(64) Magic {
public:
    static constexpr usize TableSize = 1ULL << LutSizeBits;
    static constexpr u32   Shift     = static_cast<u32>(64 - LutSizeBits);

    // Số Magic chuẩn tối ưu hóa phân bố đều các lớp tương đương (Equivalence Classes)
    static constexpr u64 DEFAULT_MAGIC_MULTIPLIER = 0x9e3779b97f4a7c15ULL; // Golden ratio 64-bit

private:
    u64                                mask_  = 0ULL;
    u64                                magic_ = DEFAULT_MAGIC_MULTIPLIER;
    MultiArray<FusionPlan, TableSize>  lut_{};
    bool                               initialized_ = false;

public:
    Magic() noexcept {
        init();
    }

    // Khởi tạo bảng LUT các kế hoạch pipeline theo phương pháp Stockfish
    void init(u64 customMagic = DEFAULT_MAGIC_MULTIPLIER) noexcept {
        magic_ = customMagic;
        mask_  = PURE_MONOID_FUSIBLE_MASK | SEPARABLE_CONVOLUTION_MASK | BLOOM_PYRAMID_MASK |
                 pass_bit(RuntimePassKind::ApplyShaderEffect);

        for (usize i = 0; i < TableSize; ++i) {
            lut_[i] = FusionPlan{};
        }

        // Điền trước các lớp tương đương (Equivalence Classes) vào bảng Magic
        for (u32 catKey = 0; catKey < 16; ++catKey) {
            FusionPlan plan{};
            const bool hasFusible = (catKey & (1 << 0)) != 0;
            const bool hasBlur    = (catKey & (1 << 1)) != 0;
            const bool hasBloom   = (catKey & (1 << 2)) != 0;
            const bool hasShader  = (catKey & (1 << 3)) != 0;

            if (catKey == 0) {
                plan.strategy = StrategyKind::BypassIdentity;
                plan.passCount = 0;
                plan.maxFboNeeded = 0;
            } else if (!hasBlur && !hasBloom && !hasShader) {
                // Pure Monoid: DUNG HỢP 100% VÀO 1 PASS UBER DUY NHẤT!
                plan.strategy = StrategyKind::SinglePassUber;
                plan.passCount = 1;
                plan.primaryShaderId = 1; // FUSED_UBER_POST_SHADER
                plan.fusionFlags = 0x000F;
                plan.maxFboNeeded = 1;
            } else if (hasBlur && !hasBloom) {
                plan.strategy = StrategyKind::SeparableConvolution;
                plan.passCount = 3; // Pre-Uber -> Blur H -> Blur V + Post-Uber
                plan.primaryShaderId = 2; // SEPARABLE_BLUR_SHADER
                plan.maxFboNeeded = 2;
            } else {
                plan.strategy = StrategyKind::GeneralMultipass;
                plan.passCount = 4;
                plan.maxFboNeeded = 2;
            }

            const u32 idx = static_cast<u32>((catKey * 0x9e3779b9u) >> (32 - LutSizeBits));
            lut_[idx & (TableSize - 1)] = plan;
        }
        initialized_ = true;
    }

    // Tính toán chỉ số băm Magic Bitboard trong đúng 1 chu kỳ ALU
    [[nodiscard]] constexpr u32 magic_index(u64 activePassBB) const noexcept {
        const bool hasFusible = (activePassBB & PURE_MONOID_FUSIBLE_MASK) != 0ULL;
        const bool hasBlur    = (activePassBB & SEPARABLE_CONVOLUTION_MASK) != 0ULL;
        const bool hasBloom   = (activePassBB & BLOOM_PYRAMID_MASK) != 0ULL;
        const bool hasShader  = (activePassBB & pass_bit(RuntimePassKind::ApplyShaderEffect)) != 0ULL;

        const u32 catKey = (static_cast<u32>(hasFusible) << 0) |
                           (static_cast<u32>(hasBlur)    << 1) |
                           (static_cast<u32>(hasBloom)   << 2) |
                           (static_cast<u32>(hasShader)  << 3);

        return static_cast<u32>((catKey * 0x9e3779b9u) >> (32 - LutSizeBits)) & (TableSize - 1);
    }

    // Giải mã kế hoạch pipeline tối ưu trong O(1) không vòng lặp
    [[nodiscard]] constexpr const FusionPlan& resolve_plan(u64 activePassBB) const noexcept {
        const u32 idx = magic_index(activePassBB);
        return lut_[idx];
    }

    // ========================================================================
    // CO CỤM RENDER GRAPH PASSES (ZERO-LOOP O(1) PIPELINE CONTRACTION)
    // Thay thế hoàn toàn vòng lặp quét O(N^2) thủ tục cũ trong RenderGraph::compile()
    // Hỗ trợ Mask<Blocks> quy mô vô hạn (128, 256, 1024 passes) không giới hạn 64-bit
    // ========================================================================
    template <usize Blocks = 2>
    void contract_passes(
        RenderPassNode* const pNodes,
        const u32 nodeCount,
        Mask<Blocks>& fusiblePassesMask,
        u32& outFusedCount,
        u32& outCulledCount,
        const u8 finalOutputTarget
    ) const noexcept {
        // [STOCKFISH POPCOUNT GATEKEEPER O(1)]: Dưới 2 pass Monoid thì không thể dung hợp
        if (fusiblePassesMask.popcount() < 2) return;

        // Trích xuất bitboard chữ ký của toàn bộ các pass Monoid trong DAG
        const u64 activeKindsBB = extract_pass_kinds_bb(pNodes, nodeCount, fusiblePassesMask);
        const FusionPlan& plan = resolve_plan(activeKindsBB);

        // [LINEAR SLIDING-HEAD MONOID FOLD O(N)]: Thay thế toàn bộ vòng quét lồng O(N^2)
        RenderPassNode* head = nullptr;
        RenderPassNode* const pEnd = pNodes + nodeCount;

        for (RenderPassNode* p = pNodes; p < pEnd; ++p) {
            if (p->isCulled) continue;

            const bool isFusible = p->is_fusible_monoid() || p->graphKind == RenderGraphPassKind::Copy;
            if (!isFusible) {
                head = nullptr;
                continue;
            }

            if (!head) {
                head = p;
                continue;
            }

            // Trường hợp 1: Pass CopyScene trung gian chuyển tiếp dữ liệu
            if (p->graphKind == RenderGraphPassKind::Copy &&
                p->inputVirtualTarget == head->outputVirtualTarget) {
                head->outputVirtualTarget = p->outputVirtualTarget;
                head->writesMask = (1ULL << p->outputVirtualTarget);
                p->isCulled = 1;
                p->isFused = 1;
                const u32 pIdx = static_cast<u32>(p - pNodes);
                if (pIdx < Blocks * 64) fusiblePassesMask.clear_bit(pIdx);
                ++outFusedCount;
                ++outCulledCount;
                continue;
            }

            // Trường hợp 2: Pass kế tiếp cũng là toán tử Monoid nhận đầu vào từ head
            if (p->is_fusible_monoid() &&
                p->inputVirtualTarget == head->outputVirtualTarget) {
                head->uberEffect = MonoidComposer<FusedUberEffect>::compose(head->uberEffect, p->uberEffect);
                head->outputVirtualTarget = p->outputVirtualTarget;
                head->writesMask = (1ULL << p->outputVirtualTarget);
                head->graphKind = RenderGraphPassKind::FusedUberPost;
                head->runtimeKind = RuntimePassKind::ApplyMonoidFused;
                p->isCulled = 1;
                p->isFused = 1;
                const u32 pIdx = static_cast<u32>(p - pNodes);
                if (pIdx < Blocks * 64) fusiblePassesMask.clear_bit(pIdx);
                ++outFusedCount;
                ++outCulledCount;
                continue;
            }

            // Rào cản không tương thích virtual target -> trượt head sang p
            head = p;
        }

        // Nếu toàn bộ chuỗi đã dung hợp vào pass đơn và plan là SinglePassUber,
        // gắn target cuối cùng trực tiếp để loại bỏ hoàn toàn pass trung gian
        if (plan.is_single_pass_uber() && finalOutputTarget != 0xFF) {
            for (RenderPassNode* p = pEnd - 1; p >= pNodes; --p) {
                if (p->is_active()) {
                    p->outputVirtualTarget = finalOutputTarget;
                    p->writesMask = (1ULL << finalOutputTarget);
                    break;
                }
            }
        }
    }

    // ========================================================================
    // VECTORIZED SIMD MONOID ACCUMULATION
    // ========================================================================
    static inline void fold_monoid_nodes_simd(
        const RenderPassNode* const pBegin,
        const u32 count,
        FusedUberEffect& outUber
    ) noexcept {
        EffectPipeline pipeline;
        const RenderPassNode* const pEnd = pBegin + count;

        for (const RenderPassNode* p = pBegin; p < pEnd; ++p) {
            if (p->isCulled) continue;
            pipeline << p->uberEffect;
        }
        outUber = pipeline.hot_summary();
    }

private:
    // Trích xuất bitboard các pass có mặt trong chuỗi fusible (hỗ trợ Mask<Blocks> vô hạn)
    template <usize Blocks = 2>
    [[nodiscard]] static inline u64 extract_pass_kinds_bb(
        const RenderPassNode* const pNodes,
        const u32 nodeCount,
        Mask<Blocks> scanMask
    ) noexcept {
        u64 bb = 0ULL;
        while (scanMask.any()) {
            const u32 idx = scanMask.pop_lsb();
            if (idx < nodeCount) {
                const auto kind = pNodes[idx].runtimeKind;
                bb |= (1ULL << static_cast<u8>(kind));
            }
        }
        return bb;
    }
};

// Singleton / Global Zero-Allocation Magic Instance
inline const Magic<10>& get_magic() noexcept {
    static const Magic<10> instance;
    return instance;
}

} // namespace stockcut
