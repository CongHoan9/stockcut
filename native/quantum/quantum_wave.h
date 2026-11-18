// ============================================================================
// CoreEngine - Unified SceneState<Domain, Dimensions> & BitOps<T>
// ----------------------------------------------------------------------------
// [BẢN ĐẶC TẢ KIẾN TRÚC]: KIEN_TRUC_RENDER_LUONG_TU_MASTER.md (GEMINI.md Rule 7 & 12)
// Chiết xuất TƯ DUY LƯỢNG TỬ và cấu trúc hóa đặc thù cho bài toán Render Video:
// - BitOps<T>: Toàn bộ thao tác bit nguyên tử thống nhất template <T> (POPCNT, LSB, MSB).
// - SceneState<Domain, Dimensions>: Trừu tượng hóa đồng nhất cho cả 2 chế độ:
//     * ExecutionDomain::Fast: 1-Bitplane classical flat bitboard (siêu tốc, trực tiếp).
//     * ExecutionDomain::Quantum: 2-Bitplane 4-QPSK phasor (chồng chập & giao thoa triệt tiêu).
// - Kích thước chuẩn hóa: Dimensions = 4096 (tương ứng lưới màn hình 64x64 tiles = 2KB L1 Cache).
// ============================================================================
#pragma once

#include "../core/types.h"
#include "../core/data_structures.h"
#include "../core/hash_primitives.h"
#include <cstring>
#include <algorithm>
#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

namespace stockcut {

// ----------------------------------------------------------------------------
// Chế độ tính toán không gian trạng thái
// ----------------------------------------------------------------------------
enum class ExecutionDomain : u8 {
    Fast,    // 1-Bitplane classical flat bitboard (Trực tiếp, 1 bit/chiều)
    Quantum  // 2-Bitplane 4-QPSK phasor (Lượng tử, 2 bit/chiều, có pha và giao thoa)
};

// ----------------------------------------------------------------------------
// 1. BitOps<T>: Toán tử Bit Nguyên Tử Thống Nhất Template <T>
// Đã được định vị chuẩn mực tại native/core/data_structures.h (Rule 7 & 8).
// Triệt tiêu 100% các hàm freestanding rời rạc (fast_popcount64, quantum_popcount64).
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// 2. SceneState<Domain, Dimensions>: Cấu trúc Không Gian Trạng Thái Thống Nhất
// ----------------------------------------------------------------------------
template <ExecutionDomain Domain = ExecutionDomain::Quantum, usize Dimensions = 4096>
struct alignas(64) SceneState;

// ----------------------------------------------------------------------------
// 2a. Chuyên biệt hóa: ExecutionDomain::Fast (Cổ điển 1-Bitplane Siêu Tốc)
// ----------------------------------------------------------------------------
template <usize Dimensions>
struct alignas(64) SceneState<ExecutionDomain::Fast, Dimensions> {
    static constexpr usize BITS = Dimensions;
    static constexpr usize WORDS = (Dimensions + 63) / 64;

    MultiArray<u64, WORDS> bits;

    constexpr SceneState() noexcept {
        clear();
    }

    inline void clear() noexcept {
        bits.clear();
    }

    inline void set_bit(usize dim, bool val = true) noexcept {
        if (dim >= BITS) return;
        const usize w = dim / 64;
        const u64 mask = 1ULL << (dim % 64);
        if (val) bits[w] |= mask;
        else bits[w] &= ~mask;
    }

    inline void set_phase(usize dim, u8 /*phase*/, i8 amp = 1) noexcept {
        set_bit(dim, amp > 0);
    }

    [[nodiscard]] inline u32 count_active() const noexcept {
        u32 total = 0;
        for (usize w = 0; w < WORDS; ++w) {
            total += BitOps<u64>::popcount64(bits[w]);
        }
        return total;
    }

    [[nodiscard]] inline u32 popcount() const noexcept {
        return count_active();
    }

    // Gộp trạng thái cổ điển (Bitwise OR)
    inline SceneState& operator+=(const SceneState& other) noexcept {
#if defined(__wasm_simd128__)
        v128_t* const __restrict__ d = reinterpret_cast<v128_t*>(bits.data());
        const v128_t* const __restrict__ s = reinterpret_cast<const v128_t*>(other.bits.data());
        constexpr usize VEC_WORDS = WORDS / 2;
        #pragma clang loop unroll_count(4)
        for (usize v = 0; v < VEC_WORDS; ++v) {
            d[v] = wasm_v128_or(d[v], s[v]);
        }
        if constexpr ((WORDS % 2) != 0) {
            bits[WORDS - 1] |= other.bits[WORDS - 1];
        }
#else
        #pragma clang loop unroll_count(4)
        for (usize w = 0; w < WORDS; ++w) {
            bits[w] |= other.bits[w];
        }
#endif
        return *this;
    }

    inline SceneState operator+(const SceneState& other) const noexcept {
        SceneState copy = *this;
        copy += other;
        return copy;
    }

    // Cắt tỉa che khuất cổ điển (A & ~B)
    inline void cull(const SceneState& occluder) noexcept {
#if defined(__wasm_simd128__)
        v128_t* const __restrict__ d = reinterpret_cast<v128_t*>(bits.data());
        const v128_t* const __restrict__ s = reinterpret_cast<const v128_t*>(occluder.bits.data());
        constexpr usize VEC_WORDS = WORDS / 2;
        #pragma clang loop unroll_count(4)
        for (usize v = 0; v < VEC_WORDS; ++v) {
            d[v] = wasm_v128_andnot(d[v], s[v]);
        }
        if constexpr ((WORDS % 2) != 0) {
            bits[WORDS - 1] &= ~occluder.bits[WORDS - 1];
        }
#else
        #pragma clang loop unroll_count(4)
        for (usize w = 0; w < WORDS; ++w) {
            bits[w] &= ~occluder.bits[w];
        }
#endif
    }

    inline void destructive_filter(const SceneState& forbidden) noexcept {
        cull(forbidden);
    }

    inline void rotate_phase(u8 /*phase_shift*/) noexcept {
        // Fast domain không có pha ảo
    }

    inline void permute(int shift) noexcept {
        int s = shift % static_cast<int>(BITS);
        if (s < 0) s += static_cast<int>(BITS);
        if (s == 0) return;

        const usize word_shift = static_cast<usize>(s) / 64;
        const usize bit_shift = static_cast<usize>(s) % 64;

        MultiArray<u64, WORDS> temp{};
        for (usize i = 0; i < WORDS; ++i) {
            const usize target_idx = (i + word_shift) % WORDS;
            temp[target_idx] |= (bits[i] << bit_shift);
            if (bit_shift > 0) {
                const usize next_idx = (target_idx + 1) % WORDS;
                temp[next_idx] |= (bits[i] >> (64 - bit_shift));
            }
        }
        bits = temp;
    }

    inline void unitary_transform(int perm_shift, u8 phase_shift) noexcept {
        permute(perm_shift);
        rotate_phase(phase_shift);
    }

    [[nodiscard]] inline u64 transition_probability(const SceneState& other) const noexcept {
        u64 match = 0;
        for (usize w = 0; w < WORDS; ++w) {
            match += BitOps<u64>::popcount64(bits[w] & other.bits[w]);
        }
        return match * match;
    }
};

// ----------------------------------------------------------------------------
// 2b. Chuyên biệt hóa: ExecutionDomain::Quantum (2-Bitplane 4-QPSK Phasor Lượng Tử)
// ----------------------------------------------------------------------------
template <usize Dimensions>
struct alignas(64) SceneState<ExecutionDomain::Quantum, Dimensions> {
    static constexpr usize BITS = Dimensions;
    static constexpr usize WORDS = (Dimensions + 63) / 64;

    // Bitplane phần thực: pos (+1, pha 0), neg (-1, pha pi)
    MultiArray<u64, WORDS> real_pos;
    MultiArray<u64, WORDS> real_neg;

    // Bitplane phần ảo: pos (+i, pha pi/2), neg (-i, pha 3pi/2)
    MultiArray<u64, WORDS> imag_pos;
    MultiArray<u64, WORDS> imag_neg;

    constexpr SceneState() noexcept {
        clear();
    }

    inline void clear() noexcept {
        real_pos.clear();
        real_neg.clear();
        imag_pos.clear();
        imag_neg.clear();
    }

    inline void set_phase(usize dim, u8 phase, i8 amp = 1) noexcept {
        if (dim >= BITS || amp == 0) return;
        const usize w = dim / 64;
        const u64 mask = 1ULL << (dim % 64);

        real_pos[w] &= ~mask;
        real_neg[w] &= ~mask;
        imag_pos[w] &= ~mask;
        imag_neg[w] &= ~mask;

        const u8 p = phase & 3;
        if (amp > 0) {
            switch (p) {
                case 0: real_pos[w] |= mask; break; // +1
                case 1: imag_pos[w] |= mask; break; // +i
                case 2: real_neg[w] |= mask; break; // -1
                case 3: imag_neg[w] |= mask; break; // -i
            }
        } else {
            switch (p) {
                case 0: real_neg[w] |= mask; break;
                case 1: imag_neg[w] |= mask; break;
                case 2: real_pos[w] |= mask; break;
                case 3: imag_pos[w] |= mask; break;
            }
        }
    }

    inline void set_bit(usize dim, bool val = true) noexcept {
        set_phase(dim, 0, val ? 1 : 0);
    }

    [[nodiscard]] inline u32 count_active() const noexcept {
        u32 total = 0;
        #pragma clang loop unroll_count(4)
        for (usize w = 0; w < WORDS; ++w) {
            total += BitOps<u64>::popcount64(real_pos[w] | real_neg[w] | imag_pos[w] | imag_neg[w]);
        }
        return total;
    }

    [[nodiscard]] inline u32 popcount() const noexcept {
        return count_active();
    }

    // GIAO THOA TRIỆT TIÊU TỰ ĐỘNG (Destructive Interference)
    // Delta theta = pi => (+1) + (-1) = 0 bitwise
    inline SceneState& operator+=(const SceneState& wave) noexcept {
#if defined(__wasm_simd128__)
        v128_t* const __restrict__ rp_ptr = reinterpret_cast<v128_t*>(real_pos.data());
        v128_t* const __restrict__ rn_ptr = reinterpret_cast<v128_t*>(real_neg.data());
        v128_t* const __restrict__ ip_ptr = reinterpret_cast<v128_t*>(imag_pos.data());
        v128_t* const __restrict__ in_ptr = reinterpret_cast<v128_t*>(imag_neg.data());

        const v128_t* const __restrict__ wrp_ptr = reinterpret_cast<const v128_t*>(wave.real_pos.data());
        const v128_t* const __restrict__ wrn_ptr = reinterpret_cast<const v128_t*>(wave.real_neg.data());
        const v128_t* const __restrict__ wip_ptr = reinterpret_cast<const v128_t*>(wave.imag_pos.data());
        const v128_t* const __restrict__ win_ptr = reinterpret_cast<const v128_t*>(wave.imag_neg.data());

        constexpr usize VEC_WORDS = WORDS / 2;
        #pragma clang loop unroll_count(4)
        for (usize v = 0; v < VEC_WORDS; ++v) {
            const v128_t r_pos = wasm_v128_or(rp_ptr[v], wrp_ptr[v]);
            const v128_t r_neg = wasm_v128_or(rn_ptr[v], wrn_ptr[v]);
            const v128_t cancel_r = wasm_v128_and(r_pos, r_neg);
            rp_ptr[v] = wasm_v128_andnot(r_pos, cancel_r);
            rn_ptr[v] = wasm_v128_andnot(r_neg, cancel_r);

            const v128_t i_pos = wasm_v128_or(ip_ptr[v], wip_ptr[v]);
            const v128_t i_neg = wasm_v128_or(in_ptr[v], win_ptr[v]);
            const v128_t cancel_i = wasm_v128_and(i_pos, i_neg);
            ip_ptr[v] = wasm_v128_andnot(i_pos, cancel_i);
            in_ptr[v] = wasm_v128_andnot(i_neg, cancel_i);
        }
        if constexpr ((WORDS % 2) != 0) {
            constexpr usize w = WORDS - 1;
            const u64 r_pos = real_pos[w] | wave.real_pos[w];
            const u64 r_neg = real_neg[w] | wave.real_neg[w];
            const u64 cancel_r = r_pos & r_neg;
            real_pos[w] = r_pos & ~cancel_r;
            real_neg[w] = r_neg & ~cancel_r;

            const u64 i_pos = imag_pos[w] | wave.imag_pos[w];
            const u64 i_neg = imag_neg[w] | wave.imag_neg[w];
            const u64 cancel_i = i_pos & i_neg;
            imag_pos[w] = i_pos & ~cancel_i;
            imag_neg[w] = i_neg & ~cancel_i;
        }
#else
        #pragma clang loop unroll_count(4)
        for (usize w = 0; w < WORDS; ++w) {
            const u64 r_pos = real_pos[w] | wave.real_pos[w];
            const u64 r_neg = real_neg[w] | wave.real_neg[w];
            const u64 cancel_r = r_pos & r_neg;
            real_pos[w] = r_pos & ~cancel_r;
            real_neg[w] = r_neg & ~cancel_r;

            const u64 i_pos = imag_pos[w] | wave.imag_pos[w];
            const u64 i_neg = imag_neg[w] | wave.imag_neg[w];
            const u64 cancel_i = i_pos & i_neg;
            imag_pos[w] = i_pos & ~cancel_i;
            imag_neg[w] = i_neg & ~cancel_i;
        }
#endif
        return *this;
    }

    inline SceneState operator+(const SceneState& wave) const noexcept {
        SceneState copy = *this;
        copy += wave;
        return copy;
    }

    // TOÁN TỬ XOAY PHA LƯỢNG TỬ (Unitary Phase Rotation e^{i*theta})
    // In-place 128-bit SIMD register rotation: 0 heap, 0 stack copy, 100% CPU register resident
    inline void rotate_phase(u8 phase_shift) noexcept {
        const u8 p = phase_shift & 3;
        if (p == 0) return;

#if defined(__wasm_simd128__)
        v128_t* const __restrict__ rp_ptr = reinterpret_cast<v128_t*>(real_pos.data());
        v128_t* const __restrict__ rn_ptr = reinterpret_cast<v128_t*>(real_neg.data());
        v128_t* const __restrict__ ip_ptr = reinterpret_cast<v128_t*>(imag_pos.data());
        v128_t* const __restrict__ in_ptr = reinterpret_cast<v128_t*>(imag_neg.data());
        constexpr usize VEC_WORDS = WORDS / 2;

        if (p == 2) {
            #pragma clang loop unroll_count(4)
            for (usize v = 0; v < VEC_WORDS; ++v) {
                const v128_t rp = rp_ptr[v];
                rp_ptr[v] = rn_ptr[v];
                rn_ptr[v] = rp;

                const v128_t ip = ip_ptr[v];
                ip_ptr[v] = in_ptr[v];
                in_ptr[v] = ip;
            }
            if constexpr ((WORDS % 2) != 0) {
                std::swap(real_pos[WORDS - 1], real_neg[WORDS - 1]);
                std::swap(imag_pos[WORDS - 1], imag_neg[WORDS - 1]);
            }
            return;
        }

        if (p == 1) {
            #pragma clang loop unroll_count(4)
            for (usize v = 0; v < VEC_WORDS; ++v) {
                const v128_t rp = rp_ptr[v];
                const v128_t rn = rn_ptr[v];
                const v128_t ip = ip_ptr[v];
                const v128_t in = in_ptr[v];
                rp_ptr[v] = in;
                rn_ptr[v] = ip;
                ip_ptr[v] = rp;
                in_ptr[v] = rn;
            }
            if constexpr ((WORDS % 2) != 0) {
                constexpr usize w = WORDS - 1;
                const u64 rp = real_pos[w];
                const u64 rn = real_neg[w];
                const u64 ip = imag_pos[w];
                const u64 in = imag_neg[w];
                real_pos[w] = in;
                real_neg[w] = ip;
                imag_pos[w] = rp;
                imag_neg[w] = rn;
            }
            return;
        }

        #pragma clang loop unroll_count(4)
        for (usize v = 0; v < VEC_WORDS; ++v) {
            const v128_t rp = rp_ptr[v];
            const v128_t rn = rn_ptr[v];
            const v128_t ip = ip_ptr[v];
            const v128_t in = in_ptr[v];
            rp_ptr[v] = ip;
            rn_ptr[v] = in;
            ip_ptr[v] = rn;
            in_ptr[v] = rp;
        }
        if constexpr ((WORDS % 2) != 0) {
            constexpr usize w = WORDS - 1;
            const u64 rp = real_pos[w];
            const u64 rn = real_neg[w];
            const u64 ip = imag_pos[w];
            const u64 in = imag_neg[w];
            real_pos[w] = ip;
            real_neg[w] = in;
            imag_pos[w] = rn;
            imag_neg[w] = rp;
        }
#else
        if (p == 2) {
            #pragma clang loop unroll_count(4)
            for (usize w = 0; w < WORDS; ++w) {
                std::swap(real_pos[w], real_neg[w]);
                std::swap(imag_pos[w], imag_neg[w]);
            }
            return;
        }

        if (p == 1) {
            #pragma clang loop unroll_count(4)
            for (usize w = 0; w < WORDS; ++w) {
                const u64 rp = real_pos[w];
                const u64 rn = real_neg[w];
                const u64 ip = imag_pos[w];
                const u64 in = imag_neg[w];
                real_pos[w] = in;
                real_neg[w] = ip;
                imag_pos[w] = rp;
                imag_neg[w] = rn;
            }
            return;
        }

        #pragma clang loop unroll_count(4)
        for (usize w = 0; w < WORDS; ++w) {
            const u64 rp = real_pos[w];
            const u64 rn = real_neg[w];
            const u64 ip = imag_pos[w];
            const u64 in = imag_neg[w];
            real_pos[w] = ip;
            real_neg[w] = in;
            imag_pos[w] = rn;
            imag_neg[w] = rp;
        }
#endif
    }

    inline void permute(int shift) noexcept {
        int s = shift % static_cast<int>(BITS);
        if (s < 0) s += static_cast<int>(BITS);
        if (s == 0) return;

        const usize word_shift = static_cast<usize>(s) / 64;
        const usize bit_shift = static_cast<usize>(s) % 64;

        auto permute_array = [&](MultiArray<u64, WORDS>& arr) {
            MultiArray<u64, WORDS> temp{};
            for (usize i = 0; i < WORDS; ++i) {
                const usize target_idx = (i + word_shift) % WORDS;
                temp[target_idx] |= (arr[i] << bit_shift);
                if (bit_shift > 0) {
                    const usize next_idx = (target_idx + 1) % WORDS;
                    temp[next_idx] |= (arr[i] >> (64 - bit_shift));
                }
            }
            arr = temp;
        };

        permute_array(real_pos);
        permute_array(real_neg);
        permute_array(imag_pos);
        permute_array(imag_neg);
    }

    inline void unitary_transform(int perm_shift, u8 phase_shift) noexcept {
        permute(perm_shift);
        rotate_phase(phase_shift);
    }

    // Tích vô hướng lượng tử (Inner Product) nung chảy trong 1 vòng lặp duy nhất
    inline void inner_product(const SceneState& other, i64& out_re, i64& out_im) const noexcept {
        i64 r_r = 0, i_i = 0, r_i = 0, i_r = 0;

        #pragma clang loop unroll_count(4)
        for (usize w = 0; w < WORDS; ++w) {
            const u64 a_rp = real_pos[w];
            const u64 a_rn = real_neg[w];
            const u64 a_ip = imag_pos[w];
            const u64 a_in = imag_neg[w];

            const u64 b_rp = other.real_pos[w];
            const u64 b_rn = other.real_neg[w];
            const u64 b_ip = other.imag_pos[w];
            const u64 b_in = other.imag_neg[w];

            // Re * Re
            const u64 rr_pos = (a_rp & b_rp) | (a_rn & b_rn);
            const u64 rr_neg = (a_rp & b_rn) | (a_rn & b_rp);
            r_r += static_cast<i64>(BitOps<u64>::popcount64(rr_pos)) - static_cast<i64>(BitOps<u64>::popcount64(rr_neg));

            // Im * Im
            const u64 ii_pos = (a_ip & b_ip) | (a_in & b_in);
            const u64 ii_neg = (a_ip & b_in) | (a_in & b_ip);
            i_i += static_cast<i64>(BitOps<u64>::popcount64(ii_pos)) - static_cast<i64>(BitOps<u64>::popcount64(ii_neg));

            // Re * Im
            const u64 ri_pos = (a_rp & b_ip) | (a_rn & b_in);
            const u64 ri_neg = (a_rp & b_in) | (a_rn & b_ip);
            r_i += static_cast<i64>(BitOps<u64>::popcount64(ri_pos)) - static_cast<i64>(BitOps<u64>::popcount64(ri_neg));

            // Im * Re
            const u64 ir_pos = (a_ip & b_rp) | (a_in & b_rn);
            const u64 ir_neg = (a_ip & b_rn) | (a_in & b_rp);
            i_r += static_cast<i64>(BitOps<u64>::popcount64(ir_pos)) - static_cast<i64>(BitOps<u64>::popcount64(ir_neg));
        }

        out_re = r_r + i_i;
        out_im = r_i - i_r;
    }

    // QUY TẮC BORN: P = |<phi|psi>|^2 = Re^2 + Im^2
    [[nodiscard]] inline u64 transition_probability(const SceneState& other) const noexcept {
        i64 re = 0, im = 0;
        inner_product(other, re, im);
        return static_cast<u64>(re * re + im * im);
    }

    // BỘ LỌC TRIỆT TIÊU RÀNG BUỘC CẤM / CHE KHUẤT (Occlusion Culling)
    inline void destructive_filter(const SceneState& forbidden_constraint) noexcept {
        SceneState opposite = forbidden_constraint;
        opposite.rotate_phase(2); // Phase pi (Nghịch đảo)
        *this += opposite;
    }

    inline void cull(const SceneState& occluder) noexcept {
        destructive_filter(occluder);
    }
};

// ----------------------------------------------------------------------------
// Type Aliases Tương Thích & Tiện Dụng Cho Toàn Bộ Engine:
// ----------------------------------------------------------------------------
template <usize Dimensions = 4096>
using QuantumSceneWave = SceneState<ExecutionDomain::Quantum, Dimensions>;

template <usize Dimensions = 4096>
using FastSceneBitboard = SceneState<ExecutionDomain::Fast, Dimensions>;

} // namespace stockcut
