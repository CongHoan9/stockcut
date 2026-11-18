// ============================================================================
// CoreEngine - Cellular Sheaf Cohomology for Dynamic Scene & Layout Routing
// ----------------------------------------------------------------------------
// [BẢN ĐẶC TẢ KIẾN TRÚC]: KIEN_TRUC_RENDER_LUONG_TU_MASTER.md (GEMINI.md Rule 7 & 12)
// Chiết xuất TƯ DUY LƯỢNG TỬ và cấu trúc hóa đặc thù cho bài toán Render Video:
// - SceneSheafRouter<Domain, MaxNodes, Dimensions>: Hỗ trợ cả Fast và Quantum domain.
// - Cuống (Stalk F(v)): Không gian trạng thái SceneState tại mỗi layer / container.
// - Ánh xạ giới hạn (Sheaf Restriction Map rho_{v -> e}): Toán tử hoán vị và xoay pha.
// - Nhất quán đối đồng điều (Cohomological Harmony): Khi H^1(X; F) = 0, toàn bộ bố cục
//   và liên kết đa miền (Audio - Text - Shader) hài hòa 100%, 0 chu kỳ reflow thừa.
// ============================================================================
#pragma once

#include "quantum_wave.h"
#include "../core/types.h"
#include "../core/data_structures.h"

namespace stockcut {

// ----------------------------------------------------------------------------
// 1. ÁNH XẠ GIỚI HẠN BÓ (Sheaf Restriction Map)
// ----------------------------------------------------------------------------
struct alignas(16) SceneSheafRestriction {
    i16 spatial_permute{0};                  // Hoán vị vòng bit thể hiện quan hệ cha - con hoặc bù dịch
    u8  quantum_phase{0};                    // Pha lượng tử ràng buộc (0, pi/2, pi, 3pi/2)
    u64 channel_mask{0xFFFFFFFFFFFFFFFFULL}; // Mặt nạ kênh chọn lọc

    template <ExecutionDomain Domain = ExecutionDomain::Quantum, usize Dimensions = 4096>
    inline SceneState<Domain, Dimensions> apply(const SceneState<Domain, Dimensions>& input) const noexcept {
        SceneState<Domain, Dimensions> transformed = input;
        if (spatial_permute != 0) {
            transformed.permute(spatial_permute);
        }
        if (quantum_phase != 0) {
            transformed.rotate_phase(quantum_phase);
        }
        if (channel_mask != 0xFFFFFFFFFFFFFFFFULL) {
            if constexpr (Domain == ExecutionDomain::Fast) {
                for (usize w = 0; w < SceneState<Domain, Dimensions>::WORDS; ++w) {
                    const u64 m = (w & 1) ? channel_mask : ~channel_mask;
                    transformed.bits[w] &= m;
                }
            } else {
                for (usize w = 0; w < SceneState<Domain, Dimensions>::WORDS; ++w) {
                    const u64 m = (w & 1) ? channel_mask : ~channel_mask;
                    transformed.real_pos[w] &= m;
                    transformed.real_neg[w] &= m;
                    transformed.imag_pos[w] &= m;
                    transformed.imag_neg[w] &= m;
                }
            }
        }
        return transformed;
    }
};

// ----------------------------------------------------------------------------
// 2. BỘ ĐIỀU PHỐI ĐỐI ĐỒNG ĐIỀU BÓ TẾ BÀO (Scene Sheaf Router)
// ----------------------------------------------------------------------------
template <ExecutionDomain Domain = ExecutionDomain::Quantum, usize MaxNodes = 64, usize Dimensions = 4096>
class alignas(64) SceneSheafRouter {
private:
    // Tận dụng Kẻ Gác Cổng Sentinel Slot (MaxNodes + 1): Slot MaxNodes là Black-Hole Sink
    // triệt tiêu 100% rẽ nhánh kiểm tra biên trong vòng lặp nóng (0 lệnh if)
    // TUYỆT ĐỐI KHÔNG DÙNG {} để tránh ép CPU memset zero-init 133 KB bộ nhớ khi tạo instance
    MultiArray<SceneState<Domain, Dimensions>, MaxNodes + 1> stalks_;
    MultiArray<SceneSheafRestriction, MaxNodes + 1> restrictions_;
    u64 activeNodesMask_ = 0ULL;

public:
    constexpr SceneSheafRouter() noexcept = default;

    inline void clear() noexcept {
        for (usize i = 0; i <= MaxNodes; ++i) {
            stalks_[i].clear();
        }
        restrictions_.clear();
        activeNodesMask_ = 0ULL;
    }

    inline void set_node(u32 idx, const SceneState<Domain, Dimensions>& wave, const SceneSheafRestriction& r) noexcept {
        const bool valid = (idx < MaxNodes);
        const u32 safeIdx = valid ? idx : static_cast<u32>(MaxNodes);
        stalks_[safeIdx] = wave;
        restrictions_[safeIdx] = r;
        activeNodesMask_ |= (static_cast<u64>(valid) << (safeIdx & 63));
        stalks_[MaxNodes].clear();
    }

    [[nodiscard]] inline const SceneState<Domain, Dimensions>& get_stalk(u32 idx) const noexcept {
        const u32 safeIdx = (idx < MaxNodes) ? idx : static_cast<u32>(MaxNodes);
        return stalks_[safeIdx];
    }

    [[nodiscard]] inline const SceneSheafRestriction& get_restriction(u32 idx) const noexcept {
        const u32 safeIdx = (idx < MaxNodes) ? idx : static_cast<u32>(MaxNodes);
        return restrictions_[safeIdx];
    }

    [[nodiscard]] inline u64 active_mask() const noexcept {
        return activeNodesMask_;
    }

    // ------------------------------------------------------------------------
    // KIỂM ĐỊNH HÀI HÒA ĐỐI ĐỒNG ĐIỀU (Cohomological Harmony: H^1 = 0)
    // Đo lường sai lệch đối đồng điều trên cạnh e = (nodeA, nodeB):
    // delta = || rho_A(s_A) - rho_B(s_B) ||
    // Nếu delta <= harmony_threshold: Hai nút hài hòa hoàn hảo, không có xung đột!
    // 100% Branchless: Tuyến tính hóa qua Sentinel Sink Slot (0 lệnh if)
    // ------------------------------------------------------------------------
    [[nodiscard]] inline bool is_cohomologically_harmonious(
        u32 nodeA,
        u32 nodeB,
        u32 harmony_threshold = 0
    ) const noexcept {
        const bool valid = (nodeA < MaxNodes) & (nodeB < MaxNodes);
        const u32 safeA = valid ? nodeA : static_cast<u32>(MaxNodes);
        const u32 safeB = valid ? nodeB : static_cast<u32>(MaxNodes);

        const auto projA = restrictions_[safeA].template apply<Domain, Dimensions>(stalks_[safeA]);
        const auto projB = restrictions_[safeB].template apply<Domain, Dimensions>(stalks_[safeB]);

        auto diff = projB;
        if constexpr (Domain == ExecutionDomain::Quantum) {
            diff.rotate_phase(2); // Nhân với -1 (Pha pi)
            diff += projA;
        } else {
            diff.cull(projA);
        }

        return valid & (diff.count_active() <= harmony_threshold);
    }

    // ------------------------------------------------------------------------
    // BƯỚC KHUẾCH TÁN SHEAF LAPLACIAN (Sheaf Laplacian Diffusion Step)
    // Tự động hấp thu vào Sentinel Sink Slot nếu biên không hợp lệ (0 lệnh if)
    // ------------------------------------------------------------------------
    inline void sheaf_diffusion_step(u32 nodeA, u32 nodeB) noexcept {
        const bool valid = (nodeA < MaxNodes) & (nodeB < MaxNodes);
        const u32 safeA = valid ? nodeA : static_cast<u32>(MaxNodes);
        const u32 safeB = valid ? nodeB : static_cast<u32>(MaxNodes);

        const auto projA = restrictions_[safeA].template apply<Domain, Dimensions>(stalks_[safeA]);
        const auto projB = restrictions_[safeB].template apply<Domain, Dimensions>(stalks_[safeB]);

        stalks_[safeA] += projB;
        stalks_[safeB] += projA;
        stalks_[MaxNodes].clear();
    }
};

} // namespace stockcut
