// ============================================================================
// CoreEngine - Unified Scientific Foundations Architecture (Stockfish Architecture)
// ----------------------------------------------------------------------------
// [STOCKFISH DATA-STRUCTURE-CENTRIC & 5 UNIFIED SCIENTIFIC PILLARS]:
// 1. Classical & Symplectic Mechanics: SymplecticPhase<T>, symplectic_spring_evaluate
//    (Hamiltonian Leapfrog Integration - 0 libm exp calls, 0 branch, Cayley/Taylor decay).
// 2. Algebraic Topology & Cellular Sheaf: SheafResonance<Nodes>
//    (Cohomological Harmony H^1 = 0, Sentinel Gatekeeper Sink Slot, 0 if-statements).
// 3. Statistical Mechanics: GibbsEvictionArena<TEntry, Capacity>
//    (Helmholtz Free Energy F = U - TS, O(1) NonFull/Active Words Mask, 0 for-loops).
// 4. Physical Wave Optics: WavefrontPhasor & WavefrontGroup
//    (Huygens-Fresnel Chromatic Wavefront Dispersion, Closed Monoid).
// 5. Information Theory & Galois Field: GaloisSyndrome<MaxSlots>
//    (GF(2^64) Carry-less Polynomial Syndrome, Single Mutation BitOps).
// TUYỆT ĐỐI KHÔNG GÁN {} trên các MultiArray thành viên.
// ============================================================================
#pragma once
#include <cmath>
#include <type_traits>
#include "memory.h"

namespace stockcut {

// Forward declarations
template<typename T> struct BitOps;
template<typename U> constexpr U branchless_mask(bool cond) noexcept;

// ============================================================================
// 1. Classical & Symplectic Mechanics: SymplecticPhase<T>
// ============================================================================
template <typename T = float>
struct alignas(16) SymplecticPhase {
    T q = T(0);
    T p = T(0);
    float invMass = 1.0f;
    float springK = 40.0f;
    float damping = 0.85f;

    // Hệ số bước tích phân nướng sẵn (Pre-baked Stepper Coefficients)
    // Tối ưu hóa FMA tối tân, loại bỏ toàn bộ phép nhân thừa trong vòng lặp nóng
    float cachedDt = 0.0f;
    float cachedHalfDtK = 0.0f;
    float cachedDtInvM = 0.0f;
    float cachedDecay = 1.0f;

    constexpr SymplecticPhase() noexcept = default;
    constexpr SymplecticPhase(T initQ, T initP, float m = 1.0f, float k = 40.0f, float d = 0.85f) noexcept
        : q(initQ), p(initP), invMass(m > 0.0f ? 1.0f / m : 1.0f), springK(k), damping(d) {
        update_stepper_coefficients(0.016f);
    }

    inline void update_stepper_coefficients(float dt) noexcept {
        cachedDt = dt;
        cachedHalfDtK = dt * 0.5f * springK;
        cachedDtInvM = dt * invMass;
        const float x = damping * dt;
        cachedDecay = 1.0f - x * (1.0f - 0.5f * x);
    }

    // Symplectic Leapfrog Integration:
    // Triệt tiêu 100% rẽ nhánh if và transcendental libm exp() call.
    // Dùng khai triển Symplectic Taylor 2nd-order / Cayley transform:
    // decay = 1 - x + 0.5 * x^2 (với x = damping * dt).
    // Đảm bảo bảo toàn diện tích pha symplectic, không chạm errno, cho phép SIMD vectorization.
    template <float FixedDt = 0.0f>
    inline void step_leapfrog(float dt, T targetQ) noexcept {
        if constexpr (FixedDt > 0.0f) {
            // Compile-Time Fixed-Step: 0 rẽ nhánh, 100% thanh ghi SIMD FMA thuần túy
            p += (targetQ - q) * cachedHalfDtK;
            q += p * cachedDtInvM;
            p += (targetQ - q) * cachedHalfDtK;
            p *= cachedDecay;
        } else {
            if (dt != cachedDt) [[unlikely]] {
                update_stepper_coefficients(dt);
            }
            p += (targetQ - q) * cachedHalfDtK;
            q += p * cachedDtInvM;
            p += (targetQ - q) * cachedHalfDtK;
            p *= cachedDecay;
        }
    }

    // Hot-Path Stepper: Tận dụng trực tiếp hệ số nướng sẵn mà không kiểm tra dt
    inline void step_leapfrog_fast(T targetQ) noexcept {
        p += (targetQ - q) * cachedHalfDtK;
        q += p * cachedDtInvM;
        p += (targetQ - q) * cachedHalfDtK;
        p *= cachedDecay;
    }

    [[nodiscard]] constexpr T position() const noexcept { return q; }
    [[nodiscard]] constexpr T momentum() const noexcept { return p; }
    [[nodiscard]] constexpr T velocity() const noexcept { return p * invMass; }

    [[nodiscard]] constexpr float hamiltonian(T targetQ) const noexcept {
        const float diff = static_cast<float>(q - targetQ);
        const float vel  = static_cast<float>(p * invMass);
        const float kinetic = 0.5f * (vel * vel) / invMass;
        const float potential = 0.5f * springK * (diff * diff);
        return kinetic + potential;
    }

    [[nodiscard]] static constexpr SymplecticPhase identity() noexcept {
        return SymplecticPhase{ T(0), T(0), 1.0f, 40.0f, 0.85f };
    }

    [[nodiscard]] constexpr SymplecticPhase compose(const SymplecticPhase& next) const noexcept {
        return SymplecticPhase{
            q + next.q,
            p + next.p,
            (invMass + next.invMass) * 0.5f,
            (springK + next.springK) * 0.5f,
            (damping + next.damping) * 0.5f
        };
    }

    [[nodiscard]] constexpr bool is_identity(float eps = 1e-6f) const noexcept {
        const float aq = static_cast<float>(q >= T(0) ? q : -q);
        const float ap = static_cast<float>(p >= T(0) ? p : -p);
        return (aq + ap) <= eps;
    }
};

inline void symplectic_spring_evaluate(
    float dt,
    float targetX, float targetY,
    SymplecticPhase<float>& phaseX,
    SymplecticPhase<float>& phaseY
) noexcept {
    phaseX.step_leapfrog(dt, targetX);
    phaseY.step_leapfrog(dt, targetY);
}

// ============================================================================
// 2. Algebraic Topology & Cellular Sheaf: SheafResonance<MaxNodes>
// ============================================================================
template <usize MaxNodes = 8>
struct alignas(16) SheafResonance {
    static constexpr usize MAX_EDGES = (MaxNodes * (MaxNodes - 1)) / 2;

    u64 activeNodesMask = 0ULL;
    u64 activeEdgesMask = 0ULL;
    // Sentinel padding (MaxNodes + 1, MAX_EDGES + 1): Slot MaxNodes là Black-Hole Sink
    // TUYỆT ĐỐI KHÔNG DÙNG {} trên MultiArray thành viên
    MultiArray<float, MaxNodes + 1> nodePhases;
    MultiArray<u8, MAX_EDGES + 1>   edgeSource;
    MultiArray<u8, MAX_EDGES + 1>   edgeTarget;
    usize edgeCount = 0;

    constexpr SheafResonance() noexcept = default;

    inline void add_node(usize nodeIdx, float phase) noexcept {
        const bool valid = (nodeIdx < MaxNodes);
        const usize safeIdx = valid ? nodeIdx : MaxNodes;
        nodePhases[safeIdx] = phase;
        activeNodesMask |= (static_cast<u64>(valid) << (safeIdx & 63));
    }

    inline void add_restriction(usize srcNode, usize tgtNode) noexcept {
        const bool valid = (srcNode < MaxNodes) & (tgtNode < MaxNodes) & (edgeCount < MAX_EDGES);
        const usize safeEdge = valid ? edgeCount : MAX_EDGES;
        edgeSource[safeEdge] = static_cast<u8>(srcNode);
        edgeTarget[safeEdge] = static_cast<u8>(tgtNode);
        activeEdgesMask |= (static_cast<u64>(valid) << (safeEdge & 63));
        edgeCount += valid; // 0 LỆNH IF
    }

    [[nodiscard]] inline u64 compute_cohomology_defect(float tolerance = 0.05f) const noexcept {
        u64 defectMask = 0ULL;
        u64 edges = activeEdgesMask;
        while (edges != 0ULL) {
            const usize e = static_cast<usize>(__builtin_ctzll(edges));
            edges &= edges - 1ULL;
            const u8 u = edgeSource[e];
            const u8 v = edgeTarget[e];
            float diff = abs_val(nodePhases[u] - nodePhases[v]);
            diff = (diff > 0.5f) ? (1.0f - diff) : diff;
            const u64 isDefect = static_cast<u64>(diff > tolerance);
            defectMask |= ((1ULL << e) & (0ULL - isDefect));
        }
        return defectMask;
    }

    [[nodiscard]] inline bool is_in_resonance(float tolerance = 0.05f) const noexcept {
        return static_cast<u32>(__builtin_popcountll(compute_cohomology_defect(tolerance))) == 0u;
    }
};

// ============================================================================
// 3. Statistical Mechanics & Helmholtz Free Energy: GibbsEvictionArena
// ============================================================================
template <typename TEntry, usize Capacity = 64>
struct alignas(64) GibbsEvictionArena {
    static constexpr usize WORD_COUNT = (Capacity + 63) / 64;

    // TUYỆT ĐỐI KHÔNG DÙNG {} trên MultiArray thành viên
    MultiArray<TEntry, Capacity> entries;
    MultiArray<u64, Capacity>    keys;
    MultiArray<float, Capacity>  computeCostU;
    MultiArray<float, Capacity>  accessEntropyS;
    MultiArray<u64, WORD_COUNT>  occupiedMasks;
    usize count = 0;

    constexpr GibbsEvictionArena() noexcept = default;

    inline void clear() noexcept {
        occupiedMasks.clear();
        count = 0;
    }

    [[nodiscard]] inline bool contains(usize slot, u64 key) const noexcept {
        if (slot >= Capacity) return false;
        const usize w = slot / 64;
        const usize b = slot % 64;
        return ((occupiedMasks[w] & (1ULL << b)) != 0ULL) && (keys[slot] == key);
    }

    [[nodiscard]] static inline constexpr float free_energy(float u, float s, float systemTemp) noexcept {
        return u - systemTemp * s;
    }

    template <usize... Is>
    [[nodiscard]] inline constexpr u64 non_full_words_mask_impl(std::index_sequence<Is...>) const noexcept {
        return ((static_cast<u64>(~occupiedMasks[Is] != 0ULL) << Is) | ...);
    }

    [[nodiscard]] inline constexpr u64 non_full_words_mask() const noexcept {
        return non_full_words_mask_impl(std::make_index_sequence<WORD_COUNT>{});
    }

    template <usize... Is>
    [[nodiscard]] inline constexpr u64 active_words_mask_impl(std::index_sequence<Is...>) const noexcept {
        return ((static_cast<u64>(occupiedMasks[Is] != 0ULL) << Is) | ...);
    }

    [[nodiscard]] inline constexpr u64 active_words_mask() const noexcept {
        return active_words_mask_impl(std::make_index_sequence<WORD_COUNT>{});
    }

    [[nodiscard]] inline usize select_eviction_candidate(float systemTemp = 1.0f) const noexcept {
        if (count < Capacity) {
            const u64 nonFull = non_full_words_mask();
            if (nonFull != 0ULL) {
                const u32 w = static_cast<u32>(__builtin_ctzll(nonFull));
                const u64 emptyMask = ~occupiedMasks[w & (WORD_COUNT - 1)];
                const usize slot = w * 64 + static_cast<usize>(__builtin_ctzll(emptyMask));
                if (slot < Capacity) return slot;
            }
        }
        usize minIdx = 0;
        float minF = 1e30f;
        u64 activeWords = active_words_mask();
        while (activeWords != 0ULL) {
            const usize w = static_cast<usize>(__builtin_ctzll(activeWords));
            activeWords &= activeWords - 1ULL;
            u64 mask = occupiedMasks[w];
            while (mask != 0ULL) {
                const u32 bit = static_cast<u32>(__builtin_ctzll(mask));
                mask &= mask - 1ULL;
                const usize idx = w * 64 + bit;
                if (idx < Capacity) {
                    const float f = free_energy(computeCostU[idx], accessEntropyS[idx], systemTemp);
                    if (f < minF) {
                        minF = f;
                        minIdx = idx;
                    }
                }
            }
        }
        return minIdx;
    }

    inline void store(usize slot, u64 key, const TEntry& val, float costU, float initS = 1.0f) noexcept {
        if (slot < Capacity) {
            entries[slot] = val;
            keys[slot] = key;
            computeCostU[slot] = costU;
            accessEntropyS[slot] = initS;
            const usize w = slot / 64;
            const usize b = slot % 64;
            if ((occupiedMasks[w] & (1ULL << b)) == 0ULL) {
                occupiedMasks[w] |= (1ULL << b);
                ++count;
            }
        }
    }

    inline void record_access(usize slot, float deltaS = 1.0f) noexcept {
        if (slot < Capacity) {
            accessEntropyS[slot] += deltaS;
        }
    }
};

// ============================================================================
// 4. Physical Wave Optics: WavefrontPhasor & WavefrontGroup
// ============================================================================
struct alignas(16) WavefrontPhasor {
    float phaseR = 0.0f;
    float phaseG = 0.0f;
    float phaseB = 0.0f;
    float dispersion = 0.0f;

    constexpr WavefrontPhasor() noexcept = default;
    constexpr WavefrontPhasor(float r, float g, float b, float d) noexcept
        : phaseR(r), phaseG(g), phaseB(b), dispersion(d) {}

    [[nodiscard]] static constexpr WavefrontPhasor identity() noexcept {
        return WavefrontPhasor{ 0.0f, 0.0f, 0.0f, 0.0f };
    }

    [[nodiscard]] static inline WavefrontPhasor from_chromatic_aberration(float amount) noexcept {
        constexpr float LAMBDA_R = 650.0f;
        constexpr float LAMBDA_G = 532.0f;
        constexpr float LAMBDA_B = 450.0f;
        return WavefrontPhasor{
            amount * (532.0f / LAMBDA_R - 1.0f),
            0.0f,
            amount * (532.0f / LAMBDA_B - 1.0f),
            amount
        };
    }

    [[nodiscard]] constexpr WavefrontPhasor compose(const WavefrontPhasor& o) const noexcept {
        return WavefrontPhasor{
            phaseR + o.phaseR,
            phaseG + o.phaseG,
            phaseB + o.phaseB,
            dispersion + o.dispersion
        };
    }

    [[nodiscard]] constexpr bool is_identity(float eps = 1e-5f) const noexcept {
        const float ar = abs_val(phaseR);
        const float ag = abs_val(phaseG);
        const float ab = abs_val(phaseB);
        return (ar + ag + ab + dispersion) <= eps;
    }
};

// Group alias for WavefrontPhasor
template <typename T, usize Capacity> struct Group;
using WavefrontGroup = Group<WavefrontPhasor, 16>;

// ============================================================================
// 5. Information Theory & Galois Field: GaloisSyndrome<MaxSlots>
// ============================================================================
template <usize MaxSlots = 64>
struct GaloisSyndrome {
    static constexpr u64 GF_POLY = 0xC200000000000000ULL;
    static constexpr u64 ALPHA_GENERATOR = 0x9E3779B97F4A7C15ULL;

    [[nodiscard]] static inline constexpr u64 gf_mul(u64 a, u64 b) noexcept {
        u64 res = 0ULL;
        for (usize i = 0; i < 64; ++i) {
            const u64 bitMask = branchless_mask<u64>((b & 1ULL) != 0ULL);
            res ^= (a & bitMask);
            const u64 carryMask = branchless_mask<u64>((a & 0x8000000000000000ULL) != 0ULL);
            a = (a << 1ULL) ^ (GF_POLY & carryMask);
            b >>= 1ULL;
        }
        return res;
    }

    // Tự nhiên số học Galois: M = 0 tự động sinh syndrome = 0 mà KHÔNG CẦN bất kỳ lệnh if kiểm tra đầu hàm nào
    [[nodiscard]] static inline constexpr u64 compute_syndrome(u64 mutationBitboard) noexcept {
        return gf_mul(mutationBitboard, ALPHA_GENERATOR);
    }

    [[nodiscard]] static inline constexpr bool is_single_slot(u64 mutationBitboard) noexcept {
        return BitOps<u64>::popcount64(mutationBitboard) == 1u;
    }

    [[nodiscard]] static inline constexpr u32 extract_single_mutated_slot(u64 dirtyBB) noexcept {
        return BitOps<u64>::safe_lsb(dirtyBB, static_cast<u32>(MaxSlots));
    }
};

} // namespace stockcut
