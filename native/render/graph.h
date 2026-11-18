// ============================================================================
// native/render/graph.h
// TRỤ CỘT 2: RENDER GRAPH (FRAMEGRAPH) & TRANSIENT MEMORY ALIASING
// ----------------------------------------------------------------------------
// Kiến trúc đồ họa AAA (id Tech 7 / DOOM Eternal, Frostbite FrameGraph):
// 1. Phân tách Virtual Resources (Tài nguyên ảo) & Physical Targets (FBO vật lý).
// 2. Dead Code Elimination (DCE): Cắt tỉa nhánh chết ngược từ Target đầu ra.
// 3. Monoid Pass Contraction: Hợp nhất chuỗi N hiệu ứng Monoid liền kề thành 1 pass.
// 4. Transient Memory Aliasing: Tái sử dụng tối đa FBO trong Tile SRAM/L2 cache.
// 5. Zero-Allocation: 100% MultiArray tĩnh, con trỏ Stockfish thuần túy.
// ============================================================================
#pragma once

#include "types.h"
#include "data_structures.h"
#include "core/nested_mark.h"
#include "timeline/bitboard.h"
#include "effect/monoid_operator.h"
#include "timeline/runtime/runtime.h"

namespace stockcut {

constexpr u32 MAX_RENDER_GRAPH_PASSES = 128;
constexpr usize RENDER_GRAPH_PASS_BLOCKS = (MAX_RENDER_GRAPH_PASSES + 63) / 64;
constexpr u32 MAX_VIRTUAL_TARGETS = 32;
constexpr u32 MAX_PHYSICAL_FBOS = 8;
constexpr u8 TARGET_FINAL_OUTPUT = 0;
constexpr u8 TARGET_INVALID = 0xFF;

// ============================================================================
// RenderGraphPassKind: Định danh ngữ nghĩa của RenderPass trong DAG
// ============================================================================
enum class RenderGraphPassKind : u8 {
    Clear = 0,
    RasterLayers,
    FusedUberPost,   // Single-Pass Monoid Uber-Effect (Affine UV + Color Gain/Emission)
    BloomExtract,
    BloomDownsample,
    BloomUpsample,
    BloomComposite,
    BlurHorizontal,
    BlurVertical,
    BlurComposite,
    ShaderCustom,
    Copy,
    Count
};

// ============================================================================
// RenderPassNode (128 bytes = ĐÚNG 2 L1 CACHELINES / 8 SIMD REGISTERS)
// ============================================================================
struct alignas(64) RenderPassNode {
    timeline::runtime::RuntimePassKind runtimeKind; // 0     (1B)
    RenderGraphPassKind graphKind;                  // 1     (1B)
    u8 inputVirtualTarget;                          // 2     (1B)
    u8 outputVirtualTarget;                         // 3     (1B)
    u8 inputPhysicalFbo;                            // 4     (1B)
    u8 outputPhysicalFbo;                           // 5     (1B)
    u8 kernelIndex;                                 // 6     (1B)
    u8 isCulled;                                    // 7     (1B: 1 = Cắt tỉa bởi DCE / Fusion)
    u8 isFused;                                     // 8     (1B: 1 = Đã dung hợp vào node trước)
    u8 isIdentity;                                  // 9     (1B: 1 = Không biến đổi)
    u8 pad0[2];                                     // 10..11(2B)
    u32 slot;                                       // 12..15(4B)
    u32 width;                                      // 16..19(4B)
    u32 height;                                     // 20..23(4B)
    float radius;                                   // 24..27(4B)
    float intensity;                                // 28..31(4B)
    float parameter0;                               // 32..35(4B)
    float parameter1;                               // 36..39(4B)
    float parameter2;                               // 40..43(4B)
    float parameter3;                               // 44..47(4B)
    Bitboard readsMask;                             // 48..55(8B)
    Bitboard writesMask;                            // 56..63(8B)
    FusedUberEffect uberEffect;                     // 64..127(64B: Payload Monoid dung hợp)

    [[nodiscard]] constexpr bool is_fusible_monoid() const noexcept {
        constexpr u32 FUSIBLE_MASK =
            (1u << static_cast<u8>(timeline::runtime::RuntimePassKind::ApplyZoom)) |
            (1u << static_cast<u8>(timeline::runtime::RuntimePassKind::ApplyShake)) |
            (1u << static_cast<u8>(timeline::runtime::RuntimePassKind::ApplyBrightness)) |
            (1u << static_cast<u8>(timeline::runtime::RuntimePassKind::ApplyMonoidFused));
        return (graphKind == RenderGraphPassKind::FusedUberPost) ||
               ((FUSIBLE_MASK & (1u << static_cast<u8>(runtimeKind))) != 0);
    }

    [[nodiscard]] constexpr bool is_active() const noexcept {
        return isCulled == 0u;
    }
};
static_assert(sizeof(RenderPassNode) == 128, "RenderPassNode must be exactly 128 bytes");

// ============================================================================
// TargetLifetime: Vòng đời tài nguyên ảo [firstWrite, lastRead]
// ============================================================================
struct alignas(8) TargetLifetime {
    u16 firstWritePass; // Chỉ số pass đầu tiên ghi vào target này
    u16 lastReadPass;   // Chỉ số pass cuối cùng đọc target này
    u8 readCount;       // Số lượng pass đang đọc target này
    u8 assignedFbo;     // FBO vật lý được cấp phát (0..MAX_PHYSICAL_FBOS-1)
    u8 isReferenced;    // 1 nếu target còn tồn tại trong DAG
    u8 pad;
};
static_assert(sizeof(TargetLifetime) == 8, "TargetLifetime must be 8 bytes");

// ============================================================================
// RenderGraph: BỘ BIÊN DỊCH VÀ ĐIỀU PHỐI ĐỒ HỌA KHÔNG PHÂN MẢNH
// ============================================================================
struct PassAssignment {
    u8 inputFbo;
    u8 outputFbo;
    u8 isCulled;
    u8 isFused;
};

class RenderGraph {
public:
    MultiArray<RenderPassNode, MAX_RENDER_GRAPH_PASSES> nodes;
    MultiArray<TargetLifetime, MAX_VIRTUAL_TARGETS> virtualLifetimes{};
    MultiArray<PassAssignment, MAX_RENDER_GRAPH_PASSES> cachedAssignments;
    u64 lastGraphTopologyHash = 0ULL;
    u32 lastNodeCount = 0;
    u8  lastFinalOutputTarget = 255;
    u32 nodeCount = 0;
    u32 activeNodeCount = 0;
    u32 physicalFboCount = 0;
    u32 fusedPassCount = 0;
    u32 culledPassCount = 0;
    Mask<RENDER_GRAPH_PASS_BLOCKS> fusiblePassesMask{};
    Bitboard referencedVirtualTargetsBB{0ULL};

    // Reset O(1) cấu trúc đồ họa mà không cấp phát heap
    [[gnu::always_inline]] inline void reset() noexcept {
        nodeCount = 0;
        activeNodeCount = 0;
        physicalFboCount = 0;
        fusedPassCount = 0;
        culledPassCount = 0;
        fusiblePassesMask.clear();
        for (Bitboard bb = referencedVirtualTargetsBB; bb;) {
            const u32 v = bb.pop_lsb();
            virtualLifetimes[v] = TargetLifetime{};
        }
        referencedVirtualTargetsBB = 0ULL;
    }

    // Thêm một node pass mới vào DAG
    [[gnu::always_inline]] inline RenderPassNode& add_pass(
        timeline::runtime::RuntimePassKind kind,
        u8 inTarget,
        u8 outTarget,
        u32 width,
        u32 height
    ) noexcept {
        if (nodeCount >= MAX_RENDER_GRAPH_PASSES) {
            return nodes[MAX_RENDER_GRAPH_PASSES - 1];
        }
        RenderPassNode& node = nodes[nodeCount++];
        node.runtimeKind = kind;
        node.inputVirtualTarget = inTarget;
        node.outputVirtualTarget = outTarget;
        node.inputPhysicalFbo = (inTarget < MAX_PHYSICAL_FBOS) ? inTarget : 0;
        node.outputPhysicalFbo = (outTarget < MAX_PHYSICAL_FBOS) ? outTarget : 0;
        node.kernelIndex = 0;
        node.isCulled = 0;
        node.isFused = 0;
        node.isIdentity = 0;
        node.slot = 0;
        node.width = width;
        node.height = height;
        node.radius = 0.0f;
        node.intensity = 1.0f;
        node.parameter0 = 0.0f;
        node.parameter1 = 0.0f;
        node.parameter2 = 0.0f;
        node.parameter3 = 0.0f;
        node.readsMask = (inTarget != TARGET_INVALID && inTarget < 64) ? Bitboard(1ULL << inTarget) : Bitboard(0ULL);
        node.writesMask = (outTarget != TARGET_INVALID && outTarget < 64) ? Bitboard(1ULL << outTarget) : Bitboard(0ULL);
        node.uberEffect = FusedUberEffect::identity();

        switch (kind) {
            case timeline::runtime::RuntimePassKind::ClearScene:
                node.graphKind = RenderGraphPassKind::Clear;
                break;
            case timeline::runtime::RuntimePassKind::DrawLayer:
                node.graphKind = RenderGraphPassKind::RasterLayers;
                break;
            case timeline::runtime::RuntimePassKind::ApplyZoom:
            case timeline::runtime::RuntimePassKind::ApplyShake:
            case timeline::runtime::RuntimePassKind::ApplyBrightness:
            case timeline::runtime::RuntimePassKind::ApplyMonoidFused:
                node.graphKind = RenderGraphPassKind::FusedUberPost;
                break;
            case timeline::runtime::RuntimePassKind::BloomExtract:
                node.graphKind = RenderGraphPassKind::BloomExtract;
                break;
            case timeline::runtime::RuntimePassKind::BloomDownsample:
                node.graphKind = RenderGraphPassKind::BloomDownsample;
                break;
            case timeline::runtime::RuntimePassKind::BloomUpsample:
                node.graphKind = RenderGraphPassKind::BloomUpsample;
                break;
            case timeline::runtime::RuntimePassKind::BloomComposite:
                node.graphKind = RenderGraphPassKind::BloomComposite;
                break;
            case timeline::runtime::RuntimePassKind::GaussianHorizontal:
                node.graphKind = RenderGraphPassKind::BlurHorizontal;
                break;
            case timeline::runtime::RuntimePassKind::GaussianVertical:
                node.graphKind = RenderGraphPassKind::BlurVertical;
                break;
            case timeline::runtime::RuntimePassKind::CompositeBlur:
            case timeline::runtime::RuntimePassKind::CompositeGlow:
            case timeline::runtime::RuntimePassKind::CompositeSceneBlur:
                node.graphKind = RenderGraphPassKind::BlurComposite;
                break;
            case timeline::runtime::RuntimePassKind::CopyScene:
                node.graphKind = RenderGraphPassKind::Copy;
                break;
            default:
                node.graphKind = RenderGraphPassKind::ShaderCustom;
                break;
        }
        if (node.is_fusible_monoid() && (nodeCount - 1) < MAX_RENDER_GRAPH_PASSES) {
            fusiblePassesMask.set_bit(nodeCount - 1);
        }
        return node;
    }

    // Khởi tạo đồ họa từ Frame có sẵn (Zero-Allocation Generic Template)
    template <typename FrameType>
    [[gnu::always_inline]] inline void build_from_frame(const FrameType& frame) noexcept {
        reset();
        const timeline::runtime::RenderPass* const pBegin = frame.passes.data();
        const timeline::runtime::RenderPass* const pEnd = pBegin + frame.passCount;

        for (const timeline::runtime::RenderPass* p = pBegin; p < pEnd; ++p) {
            RenderPassNode& node = add_pass(
                p->kind, p->inputTarget, p->outputTarget, p->width, p->height
            );
            node.slot = p->slot;
            node.kernelIndex = p->kernelIndex;
            node.radius = p->radius;
            node.intensity = p->intensity;
            node.parameter0 = p->parameter0;
            node.parameter1 = p->parameter1;
            node.parameter2 = p->parameter2;
            node.parameter3 = p->parameter3;

            // Chuyển hóa tham số sang toán tử Monoid tương ứng
            if (p->kind == timeline::runtime::RuntimePassKind::ApplyZoom) {
                node.uberEffect.uvTransform = Transform2D::from_zoom(
                    p->radius, p->intensity * DEG_TO_RAD_F32, p->kernelIndex != 0,
                    static_cast<float>(p->width), static_cast<float>(p->height)
                );
            } else if (p->kind == timeline::runtime::RuntimePassKind::ApplyShake) {
                node.uberEffect.uvTransform = Transform2D::from_shake(
                    p->radius, p->intensity, p->parameter0, p->parameter1,
                    static_cast<float>(p->width), static_cast<float>(p->height)
                );
            } else if (p->kind == timeline::runtime::RuntimePassKind::ApplyBrightness) {
                node.uberEffect.colorTransfer = ColorTransferOp::from_brightness(p->intensity);
            }
        }
    }

    // Biên dịch DAG đồ họa qua 3 giai đoạn:
    // 1. Dead Code Elimination (DCE)
    // 2. Monoid Pass Contraction (Pillar 1 + 2)
    // 3. Transient Memory Aliasing (Physical FBO Allocation)
    void compile(u8 finalOutputTarget = TARGET_FINAL_OUTPUT) noexcept;

    // Xuất kết quả DAG đã tối ưu ngược lại vào Frame (Zero-Allocation Generic Template)
    template <typename FrameType>
    [[gnu::always_inline]] inline void emit_to_frame(FrameType& frame) const noexcept {
        u32 outPassCount = 0;
        frame.passKindMask.clear();
        for (u32 i = 0; i < nodeCount; ++i) {
            const RenderPassNode& node = nodes[i];
            if (node.isCulled) continue;
            if (outPassCount >= timeline::runtime::RUNTIME_RENDER_PASS_CAPACITY - 1) break;

            frame.passKindMask.set(node.runtimeKind);
            timeline::runtime::RenderPass& pass = frame.passes[outPassCount++];
            pass.kind = node.runtimeKind;
            pass.inputTarget = node.inputPhysicalFbo;
            pass.outputTarget = node.outputPhysicalFbo;
            pass.kernelIndex = node.kernelIndex;
            pass.slot = node.slot;
            pass.width = node.width;
            pass.height = node.height;
            if (node.runtimeKind == timeline::runtime::RuntimePassKind::ApplyMonoidFused) {
                pass.radius = node.uberEffect.uvTransform.m00;
                pass.intensity = node.uberEffect.uvTransform.m01;
                pass.parameter0 = node.uberEffect.uvTransform.m10;
                pass.parameter1 = node.uberEffect.uvTransform.m11;
                pass.parameter2 = node.uberEffect.uvTransform.tx;
                pass.parameter3 = node.uberEffect.uvTransform.ty;
            } else {
                pass.radius = node.radius;
                pass.intensity = node.intensity;
                pass.parameter0 = node.parameter0;
                pass.parameter1 = node.parameter1;
                pass.parameter2 = node.parameter2;
                pass.parameter3 = node.parameter3;
            }
        }
        frame.passes[outPassCount].kind = timeline::runtime::RuntimePassKind::Sentinel;
        frame.passCount = outPassCount;
    }

    // Thống kê hiệu năng
    [[nodiscard]] constexpr u32 get_node_count() const noexcept { return nodeCount; }
    [[nodiscard]] constexpr u32 get_active_count() const noexcept { return activeNodeCount; }
    [[nodiscard]] constexpr u32 get_fused_count() const noexcept { return fusedPassCount; }
    [[nodiscard]] constexpr u32 get_culled_count() const noexcept { return culledPassCount; }
    [[nodiscard]] constexpr u32 get_physical_fbo_count() const noexcept { return physicalFboCount; }

private:
    void dead_code_elimination(u8 finalOutputTarget) noexcept;
    void monoid_pass_contraction(u8 finalOutputTarget = TARGET_FINAL_OUTPUT) noexcept;
    void transient_memory_aliasing() noexcept;
};

} // namespace stockcut
