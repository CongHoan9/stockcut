// ============================================================================
// CoreEngine - Universal Anonymous Register & Bit Block (Absolute Anonymous)
// ----------------------------------------------------------------------------
// Implements the architecture specified in CANH_GIOI_VO_DANH_TUYET_DOI_MASTER.md:
// 1. Block<FloatCount, BitBlocks, RegBlocks>:
//    - Mask<BitBlocks> bits   : Discrete bools, bit-packed enums, mode flags
//    - Mask<RegBlocks> active : Presence / keyframed float register bitmask
//    - MultiArray<float, N> reg: Continuous 16B-aligned float registers (NO {})
// 2. Canonical Zero-Decoding Raw Coercion:
//    - ONLY get_value<T, int Size>(usize addr) and set_value<T>(usize addr, T val)
//    - NO typed wrappers, NO view structs, NO intermediate decoding helpers
// 3. 4-Tier Effect & MediaType Specializations (Zero-Waste L1 Cache Density)
// 4. Sparse Dynamic Slicing & Static Step Keyframe Track (32B Header)
// ============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>
#include <type_traits>
#include <bit>
#include "../core/types.h"
#include "../core/memory.h"
#include "../core/data_structures.h"
#include "../core/nested_mark.h"

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

namespace stockcut {

// ============================================================================
// 1. UNIVERSAL ANONYMOUS BLOCK: Block<FloatCount, BitBlocks, RegBlocks>
// All physical bit & register meanings are documented in:
// BIT_AND_REGISTER_DICTIONARY.txt (Zero-Enum Architecture)
// ============================================================================
template <
    usize FloatCount,
    usize BitBlocks = 1,
    usize RegBlocks = (FloatCount + 63) / 64
>
struct alignas(16) Block {
    static_assert((FloatCount % 4) == 0, "FloatCount must be vec4-aligned for SIMD128/std140");
    static_assert(BitBlocks >= 1, "BitBlocks must be at least 1");
    static_assert(RegBlocks >= 1, "RegBlocks must be at least 1");

    static constexpr usize kFloatCount = FloatCount;
    static constexpr usize kBitBlocks  = BitBlocks;
    static constexpr usize kRegBlocks  = RegBlocks;

    Mask<BitBlocks> bits;              // Tier 1: Bool, Enum (1..6 bit), Flags, Mode
    Mask<RegBlocks> active;            // Tier 2: Presence / Active Float Mask
    MultiArray<float, FloatCount> reg; // Tier 3: Continuous float registers (NO {})

    // ------------------------------------------------------------------------
    // STOCKFISH-STYLE UNIFIED DIRECT REGISTER & BIT ACCESS (RAW, DECISIVE, ZERO-FLAG)
    // - set_value<Addr, BitWidth>(val) / set_value(addr, val): Direct raw store
    // - get_value<T, Addr, BitWidth>() / get_value<T>(addr): Direct raw fetch
    // ------------------------------------------------------------------------
    template <usize Addr, usize BitWidth = 0, typename T>
    [[gnu::always_inline]] inline void set_value(T val) noexcept {
        if constexpr (BitWidth == 1 || (BitWidth == 0 && std::is_same_v<T, bool>)) {
            bits.set_bit(static_cast<u32>(Addr), static_cast<bool>(val));
        } else if constexpr (BitWidth > 1) {
            constexpr u64 mask = (1ULL << BitWidth) - 1ULL;
            constexpr usize wIdx = Addr >> 6;
            constexpr usize shift = Addr & 63u;
            bits.words[wIdx] = (bits.words[wIdx] & ~(mask << shift)) | ((static_cast<u64>(val) & mask) << shift);
        } else if constexpr (std::is_same_v<T, float>) {
            reg[Addr] = val;
        } else if constexpr (sizeof(T) == sizeof(float)) {
            reg[Addr] = std::bit_cast<float>(val);
        } else {
            std::memcpy(&reg[Addr], &val, sizeof(T));
        }
    }

    template <typename T>
    [[gnu::always_inline]] inline void set_value(usize addr, T val) noexcept {
        if constexpr (std::is_same_v<T, bool>) {
            bits.set_bit(static_cast<u32>(addr), val);
        } else if constexpr (std::is_same_v<T, float>) {
            reg[addr] = val;
        } else if constexpr (sizeof(T) == sizeof(float)) {
            reg[addr] = std::bit_cast<float>(val);
        } else {
            std::memcpy(&reg[addr], &val, sizeof(T));
        }
    }

    [[gnu::always_inline]] inline void set_value(usize bitAddr, u8 bitWidth, u64 val) noexcept {
        const u64 mask = (1ULL << bitWidth) - 1ULL;
        const usize wIdx = bitAddr >> 6;
        const usize shift = bitAddr & 63u;
        bits.words[wIdx] = (bits.words[wIdx] & ~(mask << shift)) | ((val & mask) << shift);
    }

    template <usize Addr, typename T>
    [[gnu::always_inline]] inline void set_preset_value(T val) noexcept {
        set_value<Addr>(val);
        active.set_bit(static_cast<u32>(Addr));
    }

    [[nodiscard]] [[gnu::always_inline]] inline bool has_prop(u32 regAddr) const noexcept {
        return active.test_bit(regAddr);
    }

    [[nodiscard]] [[gnu::always_inline]] inline bool has_any_prop() const noexcept {
        return (active.words[0] | bits.words[0]) != 0ULL;
    }

    [[gnu::always_inline]] inline void merge_style(const Block& base, const Block& preset) noexcept {
        *this = base;
        constexpr u64 kRegMask = (FloatCount >= 64) ? ~0ULL : ((1ULL << FloatCount) - 1ULL);
        u64 rMask = preset.active.words[0] & kRegMask;
        while (rMask) {
            const u32 r = BitOps<u64>::pop_lsb(rMask);
            reg[r] = preset.reg[r];
        }
        if (preset.active.test_bit(6)) {
            reg[6] = base.reg[6] * preset.reg[6];
        }
    }

    template <typename T, usize Addr, usize BitWidth = 0>
    [[nodiscard]] [[gnu::always_inline]] inline T get_value() const noexcept {
        if constexpr (BitWidth == 1 || (BitWidth == 0 && std::is_same_v<T, bool>)) {
            return static_cast<T>(bits.test_bit(static_cast<u32>(Addr)));
        } else if constexpr (BitWidth > 1) {
            constexpr u64 mask = (1ULL << BitWidth) - 1ULL;
            return static_cast<T>((bits.words[Addr >> 6] >> (Addr & 63u)) & mask);
        } else if constexpr (sizeof(T) == sizeof(float)) {
            return std::bit_cast<T>(reg[Addr]);
        } else {
            T out;
            std::memcpy(&out, &reg[Addr], sizeof(T));
            return out;
        }
    }

    template <usize Addr, typename T = float>
    [[nodiscard]] [[gnu::always_inline]] inline T get_value() const noexcept {
        return get_value<T, Addr, 0>();
    }

    template <typename T, int Size = static_cast<int>(sizeof(T))>
    [[nodiscard]] [[gnu::always_inline]] inline T get_value(usize addr) const noexcept {
        if constexpr (std::is_same_v<T, bool>) {
            return bits.test_bit(static_cast<u32>(addr));
        } else if constexpr (sizeof(T) == sizeof(float) && Size == sizeof(float)) {
            return std::bit_cast<T>(reg[addr]);
        } else {
            T out;
            std::memcpy(&out, &reg[addr], Size);
            return out;
        }
    }

    inline void clear() noexcept {
        bits.clear();
        active.clear();
        std::memset(reg.data(), 0, sizeof(float) * FloatCount);
    }

    // ------------------------------------------------------------------------
    // SIMD128 ZERO-BRANCH XOR FIELD DIFF (UPGRADE UTILITY ON EXISTING Block<>)
    // Returns exact bitmask of float registers / discrete bits that differ.
    // ------------------------------------------------------------------------
    [[nodiscard]] [[gnu::always_inline]] inline Mask<RegBlocks> diff_reg_mask(const Block& other) const noexcept {
        Mask<RegBlocks> diff{};
#if defined(__wasm_simd128__)
        #pragma clang loop unroll(full)
        for (usize i = 0; i < FloatCount; i += 4) {
            const v128_t va = wasm_v128_load(&reg[i]);
            const v128_t vb = wasm_v128_load(&other.reg[i]);
            const u64 laneBits = static_cast<u64>(wasm_i32x4_bitmask(wasm_i32x4_ne(va, vb)));
            diff.words[i >> 6] |= (laneBits << (i & 63u));
        }
#else
        #pragma clang loop unroll(full)
        for (usize i = 0; i < FloatCount; ++i) {
            const u32 a = std::bit_cast<u32>(reg[i]);
            const u32 b = std::bit_cast<u32>(other.reg[i]);
            diff.words[i >> 6] |= (static_cast<u64>(a != b) << (i & 63u));
        }
#endif
        return diff;
    }

    [[nodiscard]] [[gnu::always_inline]] inline Mask<BitBlocks> diff_bit_mask(const Block& other) const noexcept {
        Mask<BitBlocks> diff{};
        #pragma clang loop unroll(full)
        for (usize w = 0; w < BitBlocks; ++w) {
            diff.words[w] = bits.words[w] ^ other.bits.words[w];
        }
        return diff;
    }
};

// ============================================================================
// 3. RIGHT-SIZED TEMPLATE SPECIALIZATIONS (MEDIA TYPES & 4 EFFECT TIERS)
// ============================================================================
using TextBlock = Block<24, 1>; // 112 bytes

namespace effect {
    using Small  = Block<16, 1>; // 80 bytes  (<= 16 floats: 164/215 effects)
    using Medium = Block<32, 1>; // 144 bytes (<= 32 floats: 36/215 effects)
    using Large  = Block<64, 2>; // 288 bytes (<= 64 floats: 14/215 effects)
    using Huge   = Block<96, 2>; // 416 bytes (<= 96 floats: _LayerStyles2, _CPUSaber)
}

using EffectBlock = effect::Huge;

static_assert(sizeof(Block<16, 1>) == 80,  "Block<16, 1> must be 80 bytes");
static_assert(sizeof(TextBlock)    == 112, "TextBlock must be 112 bytes");
static_assert(sizeof(EffectBlock)  == 416, "EffectBlock must be 416 bytes");

struct RectF {
    float left;
    float top;
    float width;
    float height;
};
static_assert(sizeof(RectF) == 16 && std::is_trivially_copyable_v<RectF>, "RectF must be 16-byte trivially copyable");

// ============================================================================
// 4. ZERO-SEMANTIC SIMD128 BLOCK INTERPOLATION (1 LOOP FOR ENTIRE ENGINE)
// ============================================================================
template <typename T>
[[gnu::always_inline]] inline T load_unaligned(const void* src) noexcept {
    T val;
    std::memcpy(&val, src, sizeof(T));
    return val;
}

template <typename T>
[[gnu::always_inline]] inline void store_unaligned(void* dst, const T& val) noexcept {
    std::memcpy(dst, &val, sizeof(T));
}

template <usize FloatCount, usize BitBlocks, usize RegBlocks>
[[gnu::always_inline]] inline void interpolate_block(
    const Block<FloatCount, BitBlocks, RegBlocks>& a,
    const Block<FloatCount, BitBlocks, RegBlocks>& b,
    float alpha,
    Block<FloatCount, BitBlocks, RegBlocks>& out
) noexcept {
    // 1. Discrete Bit Tier: Step semantics from keyframe 'a'
    out.bits = a.bits;
    out.active = a.active | b.active;

    // 2. Continuous Float Tier: Unrolled SIMD128 linear interpolation (Decisive, Branchless)
#if defined(__wasm_simd128__)
    const v128_t vAlpha = wasm_f32x4_splat(alpha);
    #pragma clang loop unroll(full)
    for (usize i = 0; i < FloatCount; i += 4) {
        const v128_t va = wasm_v128_load(&a.reg[i]);
        const v128_t vb = wasm_v128_load(&b.reg[i]);
        const v128_t vo = wasm_f32x4_add(va, wasm_f32x4_mul(wasm_f32x4_sub(vb, va), vAlpha));
        wasm_v128_store(&out.reg[i], vo);
    }
#else
    #pragma clang loop unroll(full)
    for (usize i = 0; i < FloatCount; ++i) {
        out.reg[i] = a.reg[i] + (b.reg[i] - a.reg[i]) * alpha;
    }
#endif
}

} // namespace stockcut
