// ============================================================================
// CoreEngine - Self-Contained Nested Mark Architecture & Contiguous Arenas
// ----------------------------------------------------------------------------
// [STOCKFISH DATA-STRUCTURE-CENTRIC & ALGEBRAIC MONOID FUSION]:
// - Phá bỏ hoàn toàn xiềng xích 64-slot (Mark64) bằng mô hình đệ quy lồng ghép:
//   * MaskEnd<Type>: Cốt chặn lá cơ sở (Terminal Leaf) 64 phần tử liền kề.
//   * MaskChild<Type, Child>: Khối ghép nối mở rộng (Chained Node) 64 + Child::CAPACITY.
//   * MaskArena<Type, Blocks>: Bộ điều phối tạo Arena phẳng liên tục tại Compile-Time.
//   * Mask<Blocks>: Cấu trúc Bitmask mở rộng tự chủ thay thế Bitboard đơn vô hướng.
// - Tính toán thuộc tính Mark đại số (+, -, *, /) hoàn toàn tại thời điểm biên dịch.
// - Truy xuất trạng thái tại Root bằng phép OR tóm tắt (Summary) và Dịch bit (Presence)
//   giúp Kẻ Gác Cổng Vật Lý (Physical Gatekeeper) kiểm tra vô hạn layer trong O(1).
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>
#include "types.h"
#include "memory.h"

namespace stockcut {

// ============================================================================
// 1. MaskEnd<Type>: Khối cơ sở và cốt chặn cuối cùng (Terminal Leaf 64 Slots)
// ============================================================================
template <typename Type>
struct alignas(64) MaskEnd {
    Type raw[64];
    u64  mask{0ULL};

    // Thuộc tính đại số tính tại Compile-Time của cốt chặn:
    static constexpr usize BLOCK_COUNT   = 1;
    static constexpr usize CAPACITY      = 64;
    static constexpr usize PREV_CAPACITY = 0;
    static constexpr usize BLOCKS        = 1;
    static constexpr usize BYTE_SIZE     = CAPACITY * sizeof(Type);
    static constexpr u64   BLOCK_MASK    = 1ULL;

    [[nodiscard]] [[gnu::always_inline]] inline constexpr u64 summary() const noexcept {
        return mask;
    }

    [[nodiscard]] [[gnu::always_inline]] inline constexpr u64 presence() const noexcept {
        return (mask != 0ULL) ? 1ULL : 0ULL;
    }

    [[gnu::always_inline]] inline void set_mark(usize slot, bool val = true) noexcept {
        const u64 s = slot & 63;
        const u64 m = 1ULL << s;
        mask = (mask & ~m) | (static_cast<u64>(val) << s);
    }

    [[nodiscard]] [[gnu::always_inline]] inline constexpr bool test_mark(usize slot) const noexcept {
        return (mask & (1ULL << (slot & 63))) != 0ULL;
    }

    [[gnu::always_inline]] inline void clear_marks() noexcept {
        mask = 0ULL;
    }

    template <typename Op, bool IsNonZero = false>
    [[gnu::always_inline]] inline void for_each_active(Op&& op, u32 base = 0) const noexcept {
        if (mask == 0ULL) return;
        u64 act = mask;
        while (act) {
            const u32 bit = BitOps<u64>::pop_lsb(act);
            if constexpr (!IsNonZero) {
                op(raw[bit], bit);
            } else {
                op(raw[bit], base | bit);
            }
        }
    }

    template <typename Op, bool IsNonZero = false>
    [[gnu::always_inline]] inline void for_each_active_mut(Op&& op, u32 base = 0) noexcept {
        if (mask == 0ULL) return;
        u64 act = mask;
        while (act) {
            const u32 bit = BitOps<u64>::pop_lsb(act);
            if constexpr (!IsNonZero) {
                op(raw[bit], bit);
            } else {
                op(raw[bit], base | bit);
            }
        }
    }

    // Truy cập con trỏ phẳng O(1)
    [[nodiscard]] [[gnu::always_inline]] inline Type* data() noexcept { return raw; }
    [[nodiscard]] [[gnu::always_inline]] inline const Type* data() const noexcept { return raw; }

    [[nodiscard]] [[gnu::always_inline]] inline Type& operator[](usize idx) noexcept {
        return raw[idx & 63];
    }
    [[nodiscard]] [[gnu::always_inline]] inline const Type& operator[](usize idx) const noexcept {
        return raw[idx & 63];
    }
};

// ============================================================================
// 2. MaskChild<Type, Child>: Khối ghép nối mở rộng đệ quy (Contiguous Chained Node)
// ============================================================================
template <typename Type, typename Child>
struct alignas(64) MaskChild {
    Type  raw[64];
    u64   mask{0ULL};
    Child child;

    // Thao tác đại số (+, -, *, /) với Child::* tại thời điểm biên dịch:
    static constexpr usize BLOCK_COUNT   = 1 + Child::BLOCK_COUNT;          // Phép cộng +
    static constexpr usize CAPACITY      = 64 + Child::CAPACITY;            // Phép cộng + (64 + 64 + ...)
    static constexpr usize PREV_CAPACITY = CAPACITY - 64;                   // Phép trừ -
    static constexpr usize BLOCKS        = CAPACITY / 64;                   // Phép chia /
    static constexpr usize BYTE_SIZE     = CAPACITY * sizeof(Type);         // Phép nhân *
    static constexpr usize MARK_STRIDE   = sizeof(MaskChild);               // Khoảng cách giữa 2 local_mark
    static constexpr u64   BLOCK_MASK    = (Child::BLOCK_MASK << 1) | 1ULL; // Dịch bit và bật mask

    // KẾT HỢP MONOID TẠI ROOT: Phép OR phủ toàn phần gom toàn bộ con cháu trong O(1)
    [[nodiscard]] [[gnu::always_inline]] inline constexpr u64 summary() const noexcept {
        return mask | child.summary();
    }

    // KẾT HỢP NÂNG TẦNG CHỈ MỤC: Bit k đại diện cho Block k
    [[nodiscard]] [[gnu::always_inline]] inline constexpr u64 presence() const noexcept {
        const u64 self_bit = (mask != 0ULL) ? 1ULL : 0ULL;
        return self_bit | (child.presence() << 1);
    }

    [[gnu::always_inline]] inline void set_mark(usize slot, bool val = true) noexcept {
        if (slot < 64) {
            const u64 s = slot & 63;
            const u64 m = 1ULL << s;
            mask = (mask & ~m) | (static_cast<u64>(val) << s);
        } else {
            child.set_mark(slot - 64, val);
        }
    }

    [[nodiscard]] [[gnu::always_inline]] inline constexpr bool test_mark(usize slot) const noexcept {
        if (slot < 64) return (mask & (1ULL << slot)) != 0ULL;
        return child.test_mark(slot - 64);
    }

    [[gnu::always_inline]] inline void clear_marks() noexcept {
        mask = 0ULL;
        child.clear_marks();
    }

    // Truy cập con trỏ phẳng O(1) tuyệt đối nhờ cấu trúc bộ nhớ liên khối không phân mảnh
    [[nodiscard]] [[gnu::always_inline]] inline Type* data() noexcept {
        return reinterpret_cast<Type*>(raw);
    }
    [[nodiscard]] [[gnu::always_inline]] inline const Type* data() const noexcept {
        return reinterpret_cast<const Type*>(raw);
    }

    template <typename Op, bool IsNonZero = false>
    [[gnu::always_inline]] inline void for_each_active(Op&& op, u32 base = 0) const noexcept {
        // Physical Gatekeeper tai Compile-Time: Chi root (!IsNonZero) moi sinh ma kiem tra summary()
        if constexpr (!IsNonZero) {
            if (summary() == 0ULL) return;
        }
        if (mask != 0ULL) {
            u64 act = mask;
            while (act) {
                const u32 bit = BitOps<u64>::pop_lsb(act);
                if constexpr (!IsNonZero) {
                    op(raw[bit], bit);
                } else {
                    op(raw[bit], base | bit);
                }
            }
        }
        child.template for_each_active<Op, true>(std::forward<Op>(op), base + 64);
    }

    template <typename Op, bool IsNonZero = false>
    [[gnu::always_inline]] inline void for_each_active_mut(Op&& op, u32 base = 0) noexcept {
        // Physical Gatekeeper tai Compile-Time: Chi root (!IsNonZero) moi sinh ma kiem tra summary()
        if constexpr (!IsNonZero) {
            if (summary() == 0ULL) return;
        }
        if (mask != 0ULL) {
            u64 act = mask;
            while (act) {
                const u32 bit = BitOps<u64>::pop_lsb(act);
                if constexpr (!IsNonZero) {
                    op(raw[bit], bit);
                } else {
                    op(raw[bit], base | bit);
                }
            }
        }
        child.template for_each_active_mut<Op, true>(std::forward<Op>(op), base + 64);
    }

    [[nodiscard]] [[gnu::always_inline]] inline Type& operator[](usize idx) noexcept {
        if (idx < 64) return raw[idx];
        return child[idx - 64];
    }
    [[nodiscard]] [[gnu::always_inline]] inline const Type& operator[](usize idx) const noexcept {
        if (idx < 64) return raw[idx];
        return child[idx - 64];
    }
};

// ============================================================================
// 3. MaskArena<T, Capacity, InlineLimitBytes>: Cây Bitmask Phân Cấp Radix-64 O(1)
// ----------------------------------------------------------------------------
// [STOCKFISH DATA-STRUCTURE-CENTRIC & STRICTLY CONTIGUOUS DATA ARENA]:
// - Xóa bỏ 100% bẫy đệ quy lồng nhau và bẫy gián đoạn bộ nhớ của MaskChild.
// - Vùng Mask phân cấp (kMaskBytes) nằm ở đầu, vùng dữ liệu T phẳng (Capacity * sizeof(T))
//   nằm liên tục 100% ở nửa sau, căn lề 64 bytes (Cacheline aligned).
// - operator[](i) đạt O(1) phẳng tuyệt đối, tương thích SIMD và GPU buffer blit.
// - any() đạt O(1) chỉ đọc đúng 1 từ 64-bit ở Root, skip toàn bộ nhánh rỗng.
// - set_mark(i, v) có cơ chế cắt tỉa (pruning): dừng ngay khi trạng thái rỗng không đổi.
// - Chặn đứng buffer overflow: kiểm tra biên i < Capacity ở mọi hàm thao tác.
// - Cung cấp đầy đủ const/non-const overloads và zero-allocation clear_marks().
// ============================================================================
template <typename T, usize Capacity, usize InlineLimitBytes = 64 * 1024>
struct alignas(64) MaskArena {
    static_assert(std::is_trivially_copyable_v<T> && Capacity >= 1,
                  "T must be trivially copyable and Capacity >= 1");
    static constexpr usize kMaxLevels = 11; // 64^11 > 2^64

    // 1. Tính toán số tầng tại Compile-Time (Radix-64 Tree)
    static constexpr usize kLevels = [] {
        usize n = Capacity, l = 0;
        for (;;) { ++l; n = (n + 63) / 64; if (n <= 1) break; }
        return l;
    }();

    // 2. Tính offset từng tầng: kOffset[l] = word offset của tầng l
    static constexpr auto kOffset = [] {
        MultiArray<usize, kMaxLevels + 1> o{};
        usize n = Capacity, off = 0;
        for (usize l = 0; l < kLevels; ++l) { 
            o[l] = off; 
            n = (n + 63) / 64; 
            off += n; 
        }
        o[kLevels] = off; // Tổng số từ 64-bit của toàn bộ cây mask
        return o;
    }();

    // Căn lề kMaskBytes lên đúng bội số 64 bytes để data() luôn đạt chuẩn Cacheline
    static constexpr usize kMaskWords = kOffset[kLevels];
    static constexpr usize kMaskBytes = ((kMaskWords * sizeof(u64)) + 63) & ~usize(63);
    static constexpr usize kDataBytes = Capacity * sizeof(T);
    static constexpr usize kTotal     = kMaskBytes + kDataBytes;
    static constexpr bool  kInline    = kTotal <= InlineLimitBytes;

    // Hằng số đại số tương thích hệ sinh thái Stockfish
    static constexpr usize CAPACITY    = Capacity;
    static constexpr usize BLOCK_COUNT = (Capacity + 63) / 64;
    static constexpr usize BLOCKS      = BLOCK_COUNT;
    static constexpr usize BYTE_SIZE   = kDataBytes;

    // Ngăn chặn copy ngầm định bất nhất giữa Inline và Dynamic
    MaskArena(const MaskArena&) = delete;
    MaskArena& operator=(const MaskArena&) = delete;

    MaskArena(MaskArena&& o) noexcept {
        if constexpr (kInline) {
            std::memcpy(inlineStorage.data(), o.inlineStorage.data(), kTotal);
        } else {
            storage = std::move(o.storage);
        }
    }

    MaskArena& operator=(MaskArena&& o) noexcept {
        if (this != &o) {
            if constexpr (kInline) {
                std::memcpy(inlineStorage.data(), o.inlineStorage.data(), kTotal);
            } else {
                storage = std::move(o.storage);
            }
        }
        return *this;
    }

    MaskArena() noexcept {
        if constexpr (!kInline) {
            storage = make_unique_aligned<u8[]>(kTotal);
        }
        clear_marks();
    }

    // --- Bộ điều phối con trỏ căn lề Cacheline ---
    [[gnu::always_inline]] inline u8* base() noexcept { 
        if constexpr (kInline) return inlineStorage.data(); 
        else return storage.get(); 
    }
    [[gnu::always_inline]] inline const u8* base() const noexcept { 
        if constexpr (kInline) return inlineStorage.data(); 
        else return storage.get(); 
    }

    [[gnu::always_inline]] inline u64* mask() noexcept { 
        return reinterpret_cast<u64*>(base()); 
    }
    [[gnu::always_inline]] inline const u64* mask() const noexcept { 
        return reinterpret_cast<const u64*>(base()); 
    }

    [[gnu::always_inline]] inline T* data() noexcept { 
        return reinterpret_cast<T*>(base() + kMaskBytes); 
    }
    [[gnu::always_inline]] inline const T* data() const noexcept { 
        return reinterpret_cast<const T*>(base() + kMaskBytes); 
    }

    // --- Truy cập mảng phẳng O(1) ---
    [[nodiscard]] [[gnu::always_inline]] inline T& operator[](usize i) noexcept { 
        return data()[i]; 
    }
    [[nodiscard]] [[gnu::always_inline]] inline const T& operator[](usize i) const noexcept { 
        return data()[i]; 
    }

    // --- Kiểm tra trạng thái O(1) ---
    [[nodiscard]] [[gnu::always_inline]] inline bool test_mark(usize i) const noexcept {
        if (i >= Capacity) [[unlikely]] return false;
        return (mask()[kOffset[0] + (i >> 6)] >> (i & 63)) & 1ULL;
    }

    [[nodiscard]] [[gnu::always_inline]] inline bool any() const noexcept {
        return mask()[kOffset[kLevels - 1]] != 0ULL;
    }

    [[nodiscard]] [[gnu::always_inline]] inline bool empty() const noexcept {
        return !any();
    }

    [[nodiscard]] [[gnu::always_inline]] inline u64 summary() const noexcept {
        return any() ? 1ULL : 0ULL;
    }

    [[nodiscard]] [[gnu::always_inline]] inline u64 presence() const noexcept {
        if constexpr (kLevels >= 2) {
            return mask()[kOffset[1]]; // Tầng 1 đại diện cho các block 64 của tầng 0
        } else {
            return (mask()[0] != 0ULL) ? 1ULL : 0ULL;
        }
    }

    // --- Xóa sạch mask O(kMaskBytes) ---
    [[gnu::always_inline]] inline void clear_marks() noexcept {
        std::memset(base(), 0, kMaskBytes);
    }

    [[gnu::always_inline]] inline void clear() noexcept {
        clear_marks();
    }

    // --- Đột biến vi sai phân cấp O(kLevels) ---
    [[gnu::always_inline]] inline void set_mark(usize i, bool v = true) noexcept {
        if (i >= Capacity) [[unlikely]] return; // Chặn đứng buffer overflow
        u64* m = mask();
        #pragma clang loop unroll(full)
        for (usize l = 0; l < kLevels; ++l) {
            u64& w = m[kOffset[l] + (i >> 6)];
            const bool was = (w != 0ULL);
            const usize s = i & 63;
            w = (w & ~(1ULL << s)) | (static_cast<u64>(v) << s);
            const bool now = (w != 0ULL);
            if (was == now) break; // Trạng thái rỗng không đổi: dừng lập tức
            i >>= 6; 
            v = now;
        }
    }

    // --- Duyệt qua các phần tử active (bỏ qua toàn bộ nhánh rỗng) ---
    template <class Op>
    [[gnu::always_inline]] inline void for_each_active(Op&& op) noexcept {
        T* d = data();
        walk<kLevels - 1>(0, [&](usize i) {
            if constexpr (std::is_invocable_v<Op, T&, u32>) {
                op(d[i], static_cast<u32>(i));
            } else if constexpr (std::is_invocable_v<Op, T&>) {
                op(d[i]);
            } else {
                op(static_cast<u32>(i));
            }
        });
    }

    template <class Op>
    [[gnu::always_inline]] inline void for_each_active(Op&& op) const noexcept {
        const T* d = data();
        walk<kLevels - 1>(0, [&](usize i) {
            if constexpr (std::is_invocable_v<Op, const T&, u32>) {
                op(d[i], static_cast<u32>(i));
            } else if constexpr (std::is_invocable_v<Op, const T&>) {
                op(d[i]);
            } else {
                op(static_cast<u32>(i));
            }
        });
    }

private:
    template <usize Lv, class F>
    [[gnu::always_inline]] inline void walk(usize wordIdx, F&& f) const noexcept {
        u64 a = mask()[kOffset[Lv] + wordIdx];
        while (a) {
            const usize bit = BitOps<u64>::pop_lsb(a);
            const usize child = (wordIdx << 6) | bit;
            if constexpr (Lv == 0) {
                if (child < Capacity) [[likely]] f(child);
            } else {
                walk<Lv - 1>(child, f);
            }
        }
    }

    struct EmptyStorage {};
    template <usize Bytes>
    struct alignas(64) AlignedBytes {
        u8 raw[Bytes];
        [[gnu::always_inline]] inline u8* data() noexcept { return raw; }
        [[gnu::always_inline]] inline const u8* data() const noexcept { return raw; }
    };

    [[no_unique_address]] std::conditional_t<kInline, AlignedBytes<kInline ? kTotal : 1>, EmptyStorage> inlineStorage{};
    [[no_unique_address]] std::conditional_t<!kInline, AlignedPtr<u8[]>, EmptyStorage> storage{};
};

// ============================================================================
// 4. Mask<Blocks>: Cấu trúc Bitmask mở rộng tự chủ thay thế Bitboard đơn vô hướng
// ============================================================================
template <usize Blocks = 1>
struct alignas(8) Mask {
    MultiArray<u64, Blocks> words;

    static constexpr usize BLOCK_COUNT = Blocks;
    static constexpr usize BITS        = Blocks * 64;

    constexpr Mask() noexcept {
        if constexpr (Blocks == 1) {
            words[0] = 0ULL;
        } else if constexpr (Blocks == 2) {
            words[0] = 0ULL;
            words[1] = 0ULL;
        } else {
            #pragma clang loop unroll(full)
            for (usize b = 0; b < Blocks; ++b) words[b] = 0ULL;
        }
    }

    constexpr Mask(u64 single_word) noexcept {
        words[0] = single_word;
        if constexpr (Blocks == 2) {
            words[1] = 0ULL;
        } else if constexpr (Blocks > 2) {
            #pragma clang loop unroll(full)
            for (usize b = 1; b < Blocks; ++b) words[b] = 0ULL;
        }
    }

    [[gnu::always_inline]] inline void clear() noexcept {
        if constexpr (Blocks == 1) {
            words[0] = 0ULL;
        } else if constexpr (Blocks == 2) {
            words[0] = 0ULL;
            words[1] = 0ULL;
        } else {
            #pragma clang loop unroll(full)
            for (usize b = 0; b < Blocks; ++b) words[b] = 0ULL;
        }
    }

    // Tóm tắt Monoid toàn phần trong O(1): nếu summary() == 0, toàn bộ bits rỗng
    [[nodiscard]] [[gnu::always_inline]] inline constexpr u64 summary() const noexcept {
        if constexpr (Blocks == 1) {
            return words[0];
        } else if constexpr (Blocks == 2) {
            return words[0] | words[1];
        } else {
            u64 s = 0ULL;
            #pragma clang loop unroll(full)
            for (usize b = 0; b < Blocks; ++b) {
                s |= words[b];
            }
            return s;
        }
    }

    // Chỉ mục Block Presence: Bit b = (words[b] != 0)
    [[nodiscard]] [[gnu::always_inline]] inline constexpr u64 presence() const noexcept {
        if constexpr (Blocks == 1) {
            return (words[0] != 0ULL) ? 1ULL : 0ULL;
        } else if constexpr (Blocks == 2) {
            return ((words[0] != 0ULL) ? 1ULL : 0ULL) | (((words[1] != 0ULL) ? 1ULL : 0ULL) << 1);
        } else {
            u64 p = 0ULL;
            #pragma clang loop unroll(full)
            for (usize b = 0; b < Blocks; ++b) {
                p |= ((words[b] != 0ULL) ? 1ULL : 0ULL) << b;
            }
            return p;
        }
    }

    [[gnu::always_inline]] inline void set_bit(usize slot, bool val = true) noexcept {
        if constexpr (Blocks == 1) {
            const u64 s = slot & 63u;
            const u64 m = 1ULL << s;
            words[0] = (words[0] & ~m) | (static_cast<u64>(val) << s);
        } else if constexpr (Blocks == 2) {
            const usize w = (slot >> 6) & 1u;
            const u64 s = slot & 63u;
            const u64 m = 1ULL << s;
            words[w] = (words[w] & ~m) | (static_cast<u64>(val) << s);
        } else {
            const usize w = slot >> 6;
            if (w < Blocks) {
                const u64 s = slot & 63u;
                const u64 m = 1ULL << s;
                words[w] = (words[w] & ~m) | (static_cast<u64>(val) << s);
            }
        }
    }

    [[gnu::always_inline]] inline void clear_bit(usize slot) noexcept {
        if constexpr (Blocks == 1) {
            words[0] &= ~(1ULL << (slot & 63));
        } else if constexpr (Blocks == 2) {
            if (slot < 64) {
                words[0] &= ~(1ULL << slot);
            } else if (slot < 128) {
                words[1] &= ~(1ULL << (slot & 63));
            }
        } else {
            const usize w = slot >> 6;
            if (w < Blocks) {
                words[w] &= ~(1ULL << (slot & 63));
            }
        }
    }

    [[nodiscard]] [[gnu::always_inline]] inline constexpr bool test_bit(usize slot) const noexcept {
        if constexpr (Blocks == 1) {
            return (words[0] & (1ULL << (slot & 63))) != 0ULL;
        } else if constexpr (Blocks == 2) {
            return (words[(slot >> 6) & 1u] & (1ULL << (slot & 63u))) != 0ULL;
        } else {
            const usize w = slot >> 6;
            if (w < Blocks) {
                return (words[w] & (1ULL << (slot & 63))) != 0ULL;
            }
            return false;
        }
    }

    // Cầu dao cảm nhận quang học (Temporal Breaker) tích hợp trực tiếp trong Mask
    template <typename Real>
    [[gnu::always_inline]] inline void test_delta(usize slot, Real cur, Real prev, Real eps = static_cast<Real>(0.001)) noexcept {
        const Real diff = cur - prev;
        const Real absDiff = diff < static_cast<Real>(0) ? -diff : diff;
        set_bit(slot, absDiff > eps);
    }

    [[nodiscard]] [[gnu::always_inline]] inline bool empty() const noexcept {
        return summary() == 0ULL;
    }

    [[nodiscard]] [[gnu::always_inline]] inline bool any() const noexcept {
        return summary() != 0ULL;
    }

    [[nodiscard]] [[gnu::always_inline]] explicit constexpr operator bool() const noexcept {
        return summary() != 0ULL;
    }

    [[nodiscard]] [[gnu::always_inline]] constexpr bool operator!() const noexcept {
        return summary() == 0ULL;
    }

    [[nodiscard]] [[gnu::always_inline]] inline u32 popcount() const noexcept {
        if constexpr (Blocks == 1) {
            return BitOps<u64>::popcount64(words[0]);
        } else if constexpr (Blocks == 2) {
            return BitOps<u64>::popcount64(words[0]) + BitOps<u64>::popcount64(words[1]);
        } else {
            u32 total = 0;
            #pragma clang loop unroll(full)
            for (usize b = 0; b < Blocks; ++b) {
                total += BitOps<u64>::popcount64(words[b]);
            }
            return total;
        }
    }

    // Stockfish pop_lsb: trích xuất slot bit thấp nhất trên toàn bộ các block và xóa bit đó
    [[nodiscard]] [[gnu::always_inline]] inline u32 pop_lsb() noexcept {
        if constexpr (Blocks == 1) {
            return BitOps<u64>::pop_lsb(words[0]);
        } else if constexpr (Blocks == 2) {
            if (words[0] != 0ULL) {
                return BitOps<u64>::pop_lsb(words[0]);
            }
            if (words[1] != 0ULL) {
                return 64u | BitOps<u64>::pop_lsb(words[1]);
            }
            return INVALID_U32;
        } else {
            #pragma clang loop unroll(full)
            for (usize b = 0; b < Blocks; ++b) {
                if (words[b] != 0ULL) {
                    const u32 bit = BitOps<u64>::pop_lsb(words[b]);
                    return static_cast<u32>((b << 6) | bit);
                }
            }
            return INVALID_U32;
        }
    }

    // Direct Zero-Allocation Active Visitor
    template <typename Op>
    [[gnu::always_inline]] inline void for_each_active(Op&& op) const noexcept {
        u64 w0 = words[0];
        while (w0) {
            op(BitOps<u64>::pop_lsb(w0));
        }
        if constexpr (Blocks == 2) {
            u64 w1 = words[1];
            while (w1) {
                op(64u | BitOps<u64>::pop_lsb(w1));
            }
        } else if constexpr (Blocks > 2) {
            #pragma clang loop unroll(full)
            for (usize b = 1; b < Blocks; ++b) {
                u64 wb = words[b];
                const u32 base = static_cast<u32>(b << 6);
                while (wb) {
                    op(base | BitOps<u64>::pop_lsb(wb));
                }
            }
        }
    }

    // Sinh bitmask chỉ có đúng 1 slot kích hoạt
    static inline constexpr Mask from_slot(usize slot) noexcept {
        Mask res;
        if constexpr (Blocks == 1) {
            res.words[0] = (1ULL << (slot & 63));
        } else if constexpr (Blocks == 2) {
            if (slot < 64) {
                res.words[0] = (1ULL << slot);
            } else if (slot < 128) {
                res.words[1] = (1ULL << (slot & 63));
            }
        } else {
            const usize b = slot >> 6;
            if (b < Blocks) {
                res.words[b] = (1ULL << (slot & 63));
            }
        }
        return res;
    }

    // Truy cập theo Block
    [[nodiscard]] [[gnu::always_inline]] inline constexpr u64 word(usize b) const noexcept {
        if constexpr (Blocks == 1) {
            return (b == 0) ? words[0] : 0ULL;
        } else if constexpr (Blocks == 2) {
            return (b == 0) ? words[0] : ((b == 1) ? words[1] : 0ULL);
        } else {
            return (b < Blocks) ? words[b] : 0ULL;
        }
    }
    [[nodiscard]] [[gnu::always_inline]] inline constexpr u64& word(usize b) noexcept {
        return words[b];
    }

    // So sánh bằng
    [[nodiscard]] [[gnu::always_inline]] constexpr bool operator==(const Mask& rhs) const noexcept {
        if constexpr (Blocks == 1) {
            return words[0] == rhs.words[0];
        } else if constexpr (Blocks == 2) {
            return (words[0] == rhs.words[0]) && (words[1] == rhs.words[1]);
        } else {
            #pragma clang loop unroll(full)
            for (usize b = 0; b < Blocks; ++b) {
                if (words[b] != rhs.words[b]) return false;
            }
            return true;
        }
    }
    [[nodiscard]] [[gnu::always_inline]] constexpr bool operator!=(const Mask& rhs) const noexcept {
        return !(*this == rhs);
    }
    [[nodiscard]] [[gnu::always_inline]] constexpr bool operator==(u64 rhs) const noexcept {
        if constexpr (Blocks == 1) {
            return words[0] == rhs;
        } else if constexpr (Blocks == 2) {
            return (words[0] == rhs) && (words[1] == 0ULL);
        } else {
            if (words[0] != rhs) return false;
            #pragma clang loop unroll(full)
            for (usize b = 1; b < Blocks; ++b) {
                if (words[b] != 0ULL) return false;
            }
            return true;
        }
    }
    [[nodiscard]] [[gnu::always_inline]] constexpr bool operator!=(u64 rhs) const noexcept {
        return !(*this == rhs);
    }
    [[nodiscard]] [[gnu::always_inline]] friend constexpr bool operator==(u64 lhs, const Mask& rhs) noexcept {
        return rhs == lhs;
    }
    [[nodiscard]] [[gnu::always_inline]] friend constexpr bool operator!=(u64 lhs, const Mask& rhs) noexcept {
        return rhs != lhs;
    }

    // Toán tử Bitwise Monoid giữa các Mask
    [[gnu::always_inline]] inline Mask& operator|=(const Mask& rhs) noexcept {
        if constexpr (Blocks == 1) {
            words[0] |= rhs.words[0];
        } else if constexpr (Blocks == 2) {
            words[0] |= rhs.words[0];
            words[1] |= rhs.words[1];
        } else {
            #pragma clang loop unroll(full)
            for (usize b = 0; b < Blocks; ++b) words[b] |= rhs.words[b];
        }
        return *this;
    }
    [[gnu::always_inline]] inline Mask& operator&=(const Mask& rhs) noexcept {
        if constexpr (Blocks == 1) {
            words[0] &= rhs.words[0];
        } else if constexpr (Blocks == 2) {
            words[0] &= rhs.words[0];
            words[1] &= rhs.words[1];
        } else {
            #pragma clang loop unroll(full)
            for (usize b = 0; b < Blocks; ++b) words[b] &= rhs.words[b];
        }
        return *this;
    }
    [[gnu::always_inline]] inline Mask& operator^=(const Mask& rhs) noexcept {
        if constexpr (Blocks == 1) {
            words[0] ^= rhs.words[0];
        } else if constexpr (Blocks == 2) {
            words[0] ^= rhs.words[0];
            words[1] ^= rhs.words[1];
        } else {
            #pragma clang loop unroll(full)
            for (usize b = 0; b < Blocks; ++b) words[b] ^= rhs.words[b];
        }
        return *this;
    }
    [[nodiscard]] [[gnu::always_inline]] inline Mask operator~() const noexcept {
        Mask res;
        if constexpr (Blocks == 1) {
            res.words[0] = ~words[0];
        } else if constexpr (Blocks == 2) {
            res.words[0] = ~words[0];
            res.words[1] = ~words[1];
        } else {
            #pragma clang loop unroll(full)
            for (usize b = 0; b < Blocks; ++b) res.words[b] = ~words[b];
        }
        return res;
    }
    [[nodiscard]] [[gnu::always_inline]] inline Mask operator|(const Mask& rhs) const noexcept {
        Mask res;
        if constexpr (Blocks == 1) {
            res.words[0] = words[0] | rhs.words[0];
        } else if constexpr (Blocks == 2) {
            res.words[0] = words[0] | rhs.words[0];
            res.words[1] = words[1] | rhs.words[1];
        } else {
            #pragma clang loop unroll(full)
            for (usize b = 0; b < Blocks; ++b) res.words[b] = words[b] | rhs.words[b];
        }
        return res;
    }
    [[nodiscard]] [[gnu::always_inline]] inline Mask operator&(const Mask& rhs) const noexcept {
        Mask res;
        if constexpr (Blocks == 1) {
            res.words[0] = words[0] & rhs.words[0];
        } else if constexpr (Blocks == 2) {
            res.words[0] = words[0] & rhs.words[0];
            res.words[1] = words[1] & rhs.words[1];
        } else {
            #pragma clang loop unroll(full)
            for (usize b = 0; b < Blocks; ++b) res.words[b] = words[b] & rhs.words[b];
        }
        return res;
    }
    [[nodiscard]] [[gnu::always_inline]] inline Mask operator^(const Mask& rhs) const noexcept {
        Mask res;
        if constexpr (Blocks == 1) {
            res.words[0] = words[0] ^ rhs.words[0];
        } else if constexpr (Blocks == 2) {
            res.words[0] = words[0] ^ rhs.words[0];
            res.words[1] = words[1] ^ rhs.words[1];
        } else {
            #pragma clang loop unroll(full)
            for (usize b = 0; b < Blocks; ++b) res.words[b] = words[b] ^ rhs.words[b];
        }
        return res;
    }

    // Toán tử Hiệu tập hợp (A \ B): Giữ lại các bit thuộc *this nhưng không thuộc rhs
    [[nodiscard]] [[gnu::always_inline]] inline Mask operator-(const Mask& rhs) const noexcept {
        return (*this) & (~rhs);
    }

    // ------------------------------------------------------------------------
    // CÁC PHÉP ĐẠI SỐ KHÔNG GIAN & THỨ TỰ (ZERO-LOOP SPATIAL & Z-ORDER ALGEBRA)
    // ------------------------------------------------------------------------
    [[nodiscard]] [[gnu::always_inline]] static constexpr Mask below(usize k) noexcept {
        Mask res;
        if constexpr (Blocks == 1) {
            res.words[0] = (k >= 64) ? ~0ULL : ((1ULL << (k & 63)) - 1ULL);
        } else if constexpr (Blocks == 2) {
            if (k >= 128) {
                res.words[0] = ~0ULL;
                res.words[1] = ~0ULL;
            } else if (k >= 64) {
                res.words[0] = ~0ULL;
                res.words[1] = (1ULL << (k - 64)) - 1ULL;
            } else {
                res.words[0] = (1ULL << (k & 63)) - 1ULL;
                res.words[1] = 0ULL;
            }
        } else {
            const usize wordIdx = k / 64;
            const usize bitOffset = k % 64;
            for (usize i = 0; i < wordIdx && i < Blocks; ++i) res.words[i] = ~0ULL;
            if (wordIdx < Blocks) res.words[wordIdx] = (1ULL << bitOffset) - 1ULL;
        }
        return res;
    }

    [[nodiscard]] [[gnu::always_inline]] static constexpr Mask above(usize k) noexcept {
        Mask res;
        if constexpr (Blocks == 1) {
            res.words[0] = (k >= 63) ? 0ULL : ((~0ULL) << ((k + 1) & 63));
        } else if constexpr (Blocks == 2) {
            if (k >= 127) {
                res.words[0] = 0ULL;
                res.words[1] = 0ULL;
            } else if (k >= 64) {
                res.words[0] = 0ULL;
                res.words[1] = (~0ULL) << (k - 63);
            } else {
                res.words[0] = (~0ULL) << ((k + 1) & 63);
                res.words[1] = ~0ULL;
            }
        } else {
            const usize wordIdx = (k + 1) / 64;
            const usize bitOffset = (k + 1) % 64;
            if (wordIdx < Blocks) {
                res.words[wordIdx] = (~0ULL) << bitOffset;
                for (usize i = wordIdx + 1; i < Blocks; ++i) res.words[i] = ~0ULL;
            }
        }
        return res;
    }

    [[nodiscard]] [[gnu::always_inline]] static constexpr Mask range(usize low, usize high) noexcept {
        return below(high + 1) & ~below(low);
    }

    // Lấy chỉ mục phần tử CAO NHẤT (Top / MSB): 1 lệnh LZCNT / BSR (Thay cho duyệt ngược)
    // Triết lý Stockfish Compile-Time Information Propagation:
    // - NonEmpty = true: Caller cam kết mask khác rỗng -> 0 IF, 0 BRANCH, 1 lệnh phần cứng trực tiếp!
    // - NonEmpty = false: Caller cần kiểm tra an toàn -> branchless fallback.
    template <bool NonEmpty = true>
    [[nodiscard]] [[gnu::always_inline]] inline i32 top() const noexcept {
        if constexpr (NonEmpty) {
            if constexpr (Blocks == 1) {
                assert(words[0] != 0ULL);
                return static_cast<i32>(63 - __builtin_clzll(words[0]));
            } else if constexpr (Blocks == 2) {
                assert(words[0] != 0ULL || words[1] != 0ULL);
                const u64 w0 = words[0];
                const u64 w1 = words[1];
                const u32 offset = (w1 != 0ULL) ? 64u : 0u;
                const u64 activeWord = (w1 != 0ULL) ? w1 : w0;
                return static_cast<i32>(offset + (63 - __builtin_clzll(activeWord)));
            } else {
                for (i32 w = static_cast<i32>(Blocks) - 1; w >= 0; --w) {
                    if (words[w] != 0ULL) {
                        return (w * 64) + static_cast<i32>(63 - __builtin_clzll(words[w]));
                    }
                }
                return -1;
            }
        } else {
            if (!any()) return -1;
            return top<true>();
        }
    }

    // Lấy chỉ mục phần tử THẤP NHẤT (Bottom / LSB): 1 lệnh TZCNT / BSF (Thay cho duyệt xuôi)
    template <bool NonEmpty = true>
    [[nodiscard]] [[gnu::always_inline]] inline i32 bottom() const noexcept {
        if constexpr (NonEmpty) {
            if constexpr (Blocks == 1) {
                assert(words[0] != 0ULL);
                return static_cast<i32>(__builtin_ctzll(words[0]));
            } else if constexpr (Blocks == 2) {
                assert(words[0] != 0ULL || words[1] != 0ULL);
                const u64 w0 = words[0];
                const u64 w1 = words[1];
                const u32 offset = (w0 == 0ULL) ? 64u : 0u;
                const u64 activeWord = (w0 == 0ULL) ? w1 : w0;
                return static_cast<i32>(offset + __builtin_ctzll(activeWord));
            } else {
                for (usize w = 0; w < Blocks; ++w) {
                    if (words[w] != 0ULL) {
                        return static_cast<i32>(w * 64 + __builtin_ctzll(words[w]));
                    }
                }
                return -1;
            }
        } else {
            if (!any()) return -1;
            return bottom<true>();
        }
    }

    // Khử che khuất tức thì: Xóa sạch toàn bộ phần tử nằm dưới phần tử che cao nhất
    template <bool HasOccluders = false>
    [[nodiscard]] [[gnu::always_inline]] inline Mask cull_below_highest(const Mask& occluders) const noexcept {
        if constexpr (HasOccluders) {
            const i32 topOcc = occluders.template top<true>();
            return (*this) & ~below(static_cast<usize>(topOcc));
        } else {
            if (!occluders.any()) return *this;
            return cull_below_highest<true>(occluders);
        }
    }

    // Toán tử Bitwise với u64 vô hướng (Zero-Cost Backward Compatibility)
    [[nodiscard]] [[gnu::always_inline]] inline Mask operator&(u64 rhs) const noexcept {
        Mask res;
        res.words[0] = words[0] & rhs;
        return res;
    }
    [[nodiscard]] [[gnu::always_inline]] inline Mask operator|(u64 rhs) const noexcept {
        Mask res = *this;
        res.words[0] |= rhs;
        return res;
    }
    [[gnu::always_inline]] inline Mask& operator&=(u64 rhs) noexcept {
        words[0] &= rhs;
        if constexpr (Blocks > 1) {
            #pragma clang loop unroll(full)
            for (usize b = 1; b < Blocks; ++b) words[b] = 0ULL;
        }
        return *this;
    }
    [[gnu::always_inline]] inline Mask& operator|=(u64 rhs) noexcept {
        words[0] |= rhs;
        return *this;
    }
    [[nodiscard]] [[gnu::always_inline]] inline friend Mask operator&(u64 lhs, const Mask& rhs) noexcept {
        return rhs & lhs;
    }
    [[nodiscard]] [[gnu::always_inline]] inline friend Mask operator|(u64 lhs, const Mask& rhs) noexcept {
        return rhs | lhs;
    }

    // Tương thích ngược với u64 khi Blocks == 1
    constexpr operator u64() const noexcept {
        return words[0];
    }
};

} // namespace stockcut
