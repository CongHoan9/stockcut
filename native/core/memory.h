// ============================================================================
// CoreEngine - Memory Subsystem & Continuous Allocator (Stockfish memory.h)
// ----------------------------------------------------------------------------
// [STOCKFISH DATA-STRUCTURE-CENTRIC & MEMORY CONTAINERS]:
// - Quản lý bộ nhớ căn lề Cache Line (64 bytes) và Large Pages (2MB/4KB).
// - MultiArray<T, Sizes...>: Mảng phẳng N chiều tại compile-time, 100% liên tục.
//   TUYỆT ĐỐI KHÔNG GÁN {} để tránh ép CPU zero-init vô nghĩa trên stack.
// - SentinelArray<T, Size>: Mảng tĩnh có Kẻ Gác Cổng Hai Đầu (Two-Sided Gatekeeper).
// - ValueList<T, MaxSize>: Danh sách tĩnh với +16 SIMD Tail Padding và push_back_if.
// - LinearStream<T, MaxCount>: Dãy con trỏ liên tục không rẽ nhánh (Continuous Pointer-Bump).
// - Span<T>: View bộ nhớ phẳng 16-byte không cấp phát động.
// ============================================================================
#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
    #if !defined(NOMINMAX)
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace stockcut {

using u64   = unsigned long long;
using u32   = unsigned int;
using u16   = unsigned short;
using u8    = unsigned char;
using usize = __SIZE_TYPE__;

void* std_aligned_alloc(usize alignment, usize size);
void  std_aligned_free(void* ptr);
void* aligned_large_pages_alloc(usize size);
void  aligned_large_pages_free(void* mem);

// ----------------------------------------------------------------------------
// Stockfish memory_deleter & memory_deleter_array (memory.h)
// ----------------------------------------------------------------------------
template<typename T, typename FREE_FUNC>
void memory_deleter(T* ptr, FREE_FUNC free_func) {
    if (!ptr) return;
    if constexpr (!std::is_trivially_destructible_v<T>)
        ptr->~T();
    free_func(ptr);
}

template<typename T, typename FREE_FUNC>
void memory_deleter_array(T* ptr, FREE_FUNC free_func) {
    if (!ptr) return;
    const usize array_offset = std::max(sizeof(usize), alignof(T));
    char* raw_memory = reinterpret_cast<char*>(ptr) - array_offset;
    if constexpr (!std::is_trivially_destructible_v<T>) {
        const usize size = *reinterpret_cast<usize*>(raw_memory);
        for (usize i = size; i-- > 0;)
            ptr[i].~T();
    }
    free_func(raw_memory);
}

template<typename T, typename ALLOC_FUNC, typename... Args>
inline std::enable_if_t<!std::is_array_v<T>, T*> memory_allocator(ALLOC_FUNC alloc_func, Args&&... args) {
    void* raw_memory = alloc_func(sizeof(T));
    if (raw_memory == nullptr) return nullptr;
    return new (raw_memory) T(std::forward<Args>(args)...);
}

template<typename T, typename ALLOC_FUNC>
inline std::enable_if_t<std::is_array_v<T>, std::remove_extent_t<T>*>
memory_allocator(ALLOC_FUNC alloc_func, usize num) {
    using ElementType = std::remove_extent_t<T>;
    const usize array_offset = std::max(sizeof(usize), alignof(ElementType));
    const usize bytes = array_offset + num * sizeof(ElementType);
    char* raw_memory = reinterpret_cast<char*>(alloc_func(bytes));
    if (raw_memory == nullptr) return nullptr;
    new (raw_memory) usize(num);
    for (usize i = 0; i < num; ++i)
        new (raw_memory + array_offset + i * sizeof(ElementType)) ElementType();
    return reinterpret_cast<ElementType*>(raw_memory + array_offset);
}

// ----------------------------------------------------------------------------
// Stockfish LargePagePtr & make_unique_large_page (memory.h)
// ----------------------------------------------------------------------------
template<typename T>
struct LargePageDeleter {
    void operator()(T* ptr) const { memory_deleter<T>(ptr, aligned_large_pages_free); }
};

template<typename T>
struct LargePageArrayDeleter {
    void operator()(T* ptr) const { memory_deleter_array<T>(ptr, aligned_large_pages_free); }
};

template<typename T>
using LargePagePtr =
    std::conditional_t<std::is_array_v<T>,
                       std::unique_ptr<T, LargePageArrayDeleter<std::remove_extent_t<T>>>,
                       std::unique_ptr<T, LargePageDeleter<T>>>;

template<typename T, typename... Args>
std::enable_if_t<!std::is_array_v<T>, LargePagePtr<T>> make_unique_large_page(Args&&... args) {
    T* obj = memory_allocator<T>(aligned_large_pages_alloc, std::forward<Args>(args)...);
    return LargePagePtr<T>(obj);
}

template<typename T>
std::enable_if_t<std::is_array_v<T>, LargePagePtr<T>> make_unique_large_page(usize num) {
    using ElementType = std::remove_extent_t<T>;
    ElementType* memory = memory_allocator<T>(aligned_large_pages_alloc, num);
    return LargePagePtr<T>(memory);
}

// ----------------------------------------------------------------------------
// Stockfish AlignedPtr & make_unique_aligned (memory.h)
// ----------------------------------------------------------------------------
template<typename T>
struct AlignedDeleter {
    void operator()(T* ptr) const { memory_deleter<T>(ptr, std_aligned_free); }
};

template<typename T>
struct AlignedArrayDeleter {
    void operator()(T* ptr) const { memory_deleter_array<T>(ptr, std_aligned_free); }
};

template<typename T>
using AlignedPtr =
    std::conditional_t<std::is_array_v<T>,
                       std::unique_ptr<T, AlignedArrayDeleter<std::remove_extent_t<T>>>,
                       std::unique_ptr<T, AlignedDeleter<T>>>;

template<typename T, typename... Args>
std::enable_if_t<!std::is_array_v<T>, AlignedPtr<T>> make_unique_aligned(Args&&... args) {
    const auto func = [](usize size) { return std_aligned_alloc(alignof(T) > 64 ? alignof(T) : 64, size); };
    T* obj = memory_allocator<T>(func, std::forward<Args>(args)...);
    return AlignedPtr<T>(obj);
}

template<typename T>
std::enable_if_t<std::is_array_v<T>, AlignedPtr<T>> make_unique_aligned(usize num) {
    using ElementType = std::remove_extent_t<T>;
    const auto func = [](usize size) { return std_aligned_alloc(alignof(ElementType) > 64 ? alignof(ElementType) : 64, size); };
    ElementType* memory = memory_allocator<T>(func, num);
    return AlignedPtr<T>(memory);
}

// ============================================================================
// 1. Sentinel<T, Size, HeadPad, TailPad>: Lớp Cha Gốc Của Mọi Collector
// ----------------------------------------------------------------------------
// - HeadPad = 0, TailPad = 0: Mảng phẳng chuẩn (MultiArray), 0 byte overhead.
// - HeadPad = 0, TailPad = 1: Mảng có Lính Canh Đuôi / Hố Đen (TailSentinel / Sink).
// - HeadPad = 1, TailPad = 1: Mảng Kẻ Gác Cổng Hai Đầu (Two-Sided Moment Gatekeeper).
// - HeadPad = 0, TailPad = 16: Đệm Đuôi SIMD & Push Không Rẽ Nhánh (ValueList / LinearStream).
// ============================================================================
template<typename T, usize Size, usize HeadPad = 0, usize TailPad = 0>
struct Sentinel {
    static constexpr usize CAPACITY    = Size;
    static constexpr usize HEAD_PAD    = HeadPad;
    static constexpr usize TAIL_PAD    = TailPad;
    static constexpr usize RAW_SIZE    = HeadPad + Size + TailPad;
    static constexpr usize BYTE_SIZE   = Size * sizeof(T);
    static constexpr usize BLOCK_COUNT = (Size + 63) / 64;

    T storage[RAW_SIZE]; // KHÔNG CÓ {} để bảo toàn nguyên tắc Zero-Init của Stockfish

    [[gnu::always_inline]] constexpr T& operator[](usize index) noexcept {
        return storage[HeadPad + index];
    }
    [[gnu::always_inline]] constexpr const T& operator[](usize index) const noexcept {
        return storage[HeadPad + index];
    }
    [[gnu::always_inline]] constexpr void fill(const T& val) noexcept {
        for (T* p = storage + HeadPad, *const e = storage + HeadPad + Size; p < e; ++p) {
            *p = val;
        }
    }
    [[gnu::always_inline]] constexpr void clear() noexcept {
        std::memset(storage, 0, sizeof(storage));
    }
    [[gnu::always_inline]] constexpr usize size() const noexcept { return Size; }
    [[gnu::always_inline]] constexpr T* data() noexcept { return storage + HeadPad; }
    [[gnu::always_inline]] constexpr const T* data() const noexcept { return storage + HeadPad; }
    [[gnu::always_inline]] constexpr const T* begin() const noexcept { return storage + HeadPad; }
    [[gnu::always_inline]] constexpr const T* end() const noexcept { return storage + HeadPad + Size; }
    [[gnu::always_inline]] constexpr T* begin() noexcept { return storage + HeadPad; }
    [[gnu::always_inline]] constexpr T* end() noexcept { return storage + HeadPad + Size; }

    // --- Quản lý Lính Canh Đầu & Đuôi tự chủ ---
    [[gnu::always_inline]] constexpr T& left_sentinel() noexcept {
        static_assert(HeadPad >= 1, "left_sentinel requires HeadPad >= 1");
        return storage[0];
    }
    [[gnu::always_inline]] constexpr const T& left_sentinel() const noexcept {
        static_assert(HeadPad >= 1, "left_sentinel requires HeadPad >= 1");
        return storage[0];
    }
    [[gnu::always_inline]] constexpr T& right_sentinel(usize count = Size) noexcept {
        static_assert(TailPad >= 1, "right_sentinel requires TailPad >= 1");
        return storage[HeadPad + count];
    }
    [[gnu::always_inline]] constexpr const T& right_sentinel(usize count = Size) const noexcept {
        static_assert(TailPad >= 1, "right_sentinel requires TailPad >= 1");
        return storage[HeadPad + count];
    }
    [[gnu::always_inline]] constexpr T* sentinel_begin() noexcept { return storage; }
    [[gnu::always_inline]] constexpr const T* sentinel_begin() const noexcept { return storage; }
    [[gnu::always_inline]] constexpr T& sentinel_at(usize rawIdx) noexcept { return storage[rawIdx]; }
    [[gnu::always_inline]] constexpr const T& sentinel_at(usize rawIdx) const noexcept { return storage[rawIdx]; }

    [[gnu::always_inline]] constexpr void seal_bounds(
        usize activeCount, const T& headVal, const T& tailVal
    ) noexcept {
        static_assert(HeadPad >= 1 && TailPad >= 1, "seal_bounds requires HeadPad >= 1 and TailPad >= 1");
        storage[0] = headVal;
        storage[HeadPad + activeCount] = tailVal;
    }

    [[gnu::always_inline]] constexpr void seal_tail(usize activeCount, const T& tailVal) noexcept {
        static_assert(TailPad >= 1, "seal_tail requires TailPad >= 1");
        storage[HeadPad + activeCount] = tailVal;
    }

    [[gnu::always_inline]] constexpr void write_or_sink(bool cond, usize index, const T& val) noexcept {
        static_assert(TailPad >= 1, "write_or_sink requires TailPad >= 1");
        storage[cond ? (HeadPad + index) : (HeadPad + Size)] = val;
    }

    // --- Truy vấn Khoảng Khắc tự chủ (Self-Managed Moment Horizon & Two-Sided Walk) ---
    template<typename CursorT, typename TimeT, typename TimeProj>
    [[gnu::always_inline]] static constexpr const T* step_raw_moment(
        const T* rawBase, CursorT& cursorRawIdx, TimeT t, TimeProj&& proj, bool* outChanged = nullptr
    ) noexcept {
        const T* kf = rawBase + cursorRawIdx;
        const auto t0 = proj(*kf);
        const auto t1 = proj(*(kf + 1));
        if (__builtin_expect(static_cast<u64>(t - t0) < static_cast<u64>(t1 - t0), 1)) {
            if (outChanged) *outChanged = false;
            return kf;
        }
        while (t < proj(*kf))        --kf;
        while (t >= proj(*(kf + 1))) ++kf;
        cursorRawIdx = static_cast<CursorT>(kf - rawBase);
        if (outChanged) *outChanged = true;
        return kf;
    }

    template<typename TimeT, typename TimeProj>
    [[nodiscard]] [[gnu::always_inline]] constexpr u32 walk_cursor(
        u32 hintIdx, TimeT t, TimeProj proj
    ) const noexcept {
        u32 cur = hintIdx;
        step_raw_moment(storage, cur, t, proj);
        return cur;
    }

    template <typename Mask, typename Op>
    [[gnu::always_inline]] inline void for_each_active(const Mask& mask, Op&& op) const noexcept {
        mask.for_each_active([this, &op](u32 idx) {
            if constexpr (Size < Mask::BITS) {
                if (idx < Size) op(storage[HeadPad + idx], idx);
            } else {
                op(storage[HeadPad + idx], idx);
            }
        });
    }

    template <typename Mask, typename Op>
    [[gnu::always_inline]] inline void for_each_active_mut(const Mask& mask, Op&& op) noexcept {
        mask.for_each_active([this, &op](u32 idx) {
            if constexpr (Size < Mask::BITS) {
                if (idx < Size) op(storage[HeadPad + idx], idx);
            } else {
                op(storage[HeadPad + idx], idx);
            }
        });
    }
};

// ============================================================================
// 2. MultiArray<T, Sizes...>: Kế thừa trực tiếp từ Sentinel (0 Byte Overhead)
// ============================================================================
template<typename T, usize Size, usize... Sizes>
struct MultiArray : public Sentinel<MultiArray<T, Sizes...>, Size, 0, 0> {
    using ChildType = MultiArray<T, Sizes...>;

    constexpr MultiArray() noexcept = default;
    constexpr MultiArray(const ChildType (&arr)[Size]) noexcept {
        for (usize i = 0; i < Size; ++i) {
            this->storage[i] = arr[i];
        }
    }

    constexpr void fill(const T& val) noexcept {
        for (usize i = 0; i < Size; ++i) {
            this->storage[i].fill(val);
        }
    }
};

template<typename T, usize Size>
struct MultiArray<T, Size> : public Sentinel<T, Size, 0, 0> {
    constexpr MultiArray() noexcept = default;
    constexpr MultiArray(const T (&arr)[Size]) noexcept {
        for (usize i = 0; i < Size; ++i) {
            this->storage[i] = arr[i];
        }
    }

    constexpr operator T*() noexcept { return this->storage; }
    constexpr operator const T*() const noexcept { return this->storage; }
};

// ============================================================================
// 3. ValueList<T, MaxSize>: Kế thừa trực tiếp từ Sentinel<T, MaxSize, 0, 16>
// ============================================================================
template<typename T, usize MaxSize>
class alignas(alignof(T) > 16 ? alignof(T) : 16) ValueList : public Sentinel<T, MaxSize, 0, 16> {
    using Base = Sentinel<T, MaxSize, 0, 16>;
    usize size_ = 0;
public:
    constexpr ValueList() noexcept = default;
    constexpr usize size() const noexcept { return size_; }
    constexpr bool  empty() const noexcept { return size_ == 0; }
    constexpr usize capacity() const noexcept { return MaxSize; }
    constexpr void  clear() noexcept { size_ = 0; }

    template <bool Cond>
    constexpr void push_back_if(const T& value) noexcept {
        if constexpr (Cond) {
            push_back(value);
        }
    }
    template <bool Cond>
    constexpr bool push_next_if(const T& value) noexcept {
        if constexpr (Cond) {
            return push_next(value);
        } else {
            return false;
        }
    }
    constexpr void push_back_if(const T& value, bool cond) noexcept {
        this->storage[size_] = value;
        size_ += (cond && (size_ < MaxSize));
    }
    constexpr void push_back_if(bool cond, const T& value) noexcept {
        this->storage[size_] = value;
        size_ += (cond && (size_ < MaxSize));
    }
    constexpr bool push_next_if(bool cond, const T& value) noexcept {
        const bool canPush = cond && (size_ < MaxSize);
        this->storage[size_] = value;
        size_ += canPush;
        return canPush;
    }
    constexpr void push_back(const T& value) noexcept {
        const bool canPush = (size_ < MaxSize);
        this->storage[size_] = value;
        size_ += canPush;
    }
    constexpr bool push_next(const T& value) noexcept {
        const bool canPush = (size_ < MaxSize);
        this->storage[size_] = value;
        size_ += canPush;
        return canPush;
    }
    constexpr T* make_space(usize count) noexcept {
        T* res = this->storage + size_;
        size_ += count;
        return res;
    }
    constexpr const T* begin() const noexcept { return this->storage; }
    constexpr const T* end() const noexcept { return this->storage + size_; }
    constexpr T*       begin() noexcept { return this->storage; }
    constexpr T*       end() noexcept { return this->storage + size_; }
};

// ============================================================================
// 4. LinearStream<T, MaxCount>: Kế thừa trực tiếp từ Sentinel<T, MaxCount, 0, 16>
// ============================================================================
template<typename T, usize MaxCount>
class alignas(64) LinearStream : public Sentinel<T, MaxCount, 0, 16> {
    using Base = Sentinel<T, MaxCount, 0, 16>;
    T* cursor_ = this->storage;
public:
    constexpr LinearStream() noexcept = default;

    template <typename U = T>
    inline void terminate_sentinel(u16 sentinelOp = 0xFFu) noexcept {
        if constexpr (requires(U u) { u.opCode; }) {
            cursor_->opCode = static_cast<decltype(cursor_->opCode)>(sentinelOp);
        }
    }

    inline void reset() noexcept {
        cursor_ = this->storage;
        terminate_sentinel();
    }

    [[nodiscard]] inline usize size() const noexcept {
        return static_cast<usize>(cursor_ - this->storage);
    }

    [[nodiscard]] inline bool empty() const noexcept {
        return cursor_ == this->storage;
    }

    [[nodiscard]] constexpr usize capacity() const noexcept {
        return MaxCount;
    }

    [[nodiscard]] inline T* cursor() noexcept {
        return cursor_;
    }

    [[nodiscard]] inline const T* cursor() const noexcept {
        return cursor_;
    }

    inline bool push_next(const T& val) noexcept {
        const bool canPush = (cursor_ < this->storage + MaxCount);
        *cursor_ = val;
        cursor_ += canPush;
        return canPush;
    }

    inline void push_next_fast(const T& val) noexcept {
        *cursor_++ = val;
    }

    inline T* next_slot() noexcept {
        T* slot = cursor_;
        cursor_ += (cursor_ < this->storage + MaxCount);
        return slot;
    }

    template <bool Cond>
    inline T* next_slot_if() noexcept {
        if constexpr (Cond) {
            return next_slot();
        } else {
            return cursor_;
        }
    }

    inline T* next_slot_if(bool cond) noexcept {
        T* slot = cursor_;
        cursor_ += (cond && (cursor_ < this->storage + MaxCount));
        return slot;
    }

    inline T* next_slot_fast() noexcept {
        return cursor_++;
    }

    template <bool Cond>
    inline bool push_next_if(const T& val) noexcept {
        if constexpr (Cond) {
            return push_next(val);
        } else {
            return false;
        }
    }

    inline bool push_next_if(bool cond, const T& val) noexcept {
        const bool canPush = cond && (cursor_ < this->storage + MaxCount);
        *cursor_ = val;
        cursor_ += canPush;
        return canPush;
    }

    inline void advance(usize count) noexcept {
        cursor_ += count;
    }

    inline void set_size(usize s) noexcept {
        cursor_ = this->storage + (s <= MaxCount ? s : MaxCount);
    }

    inline T* begin() noexcept { return this->storage; }
    inline T* end() noexcept { return cursor_; }
    inline const T* begin() const noexcept { return this->storage; }
    inline const T* end() const noexcept { return cursor_; }
};

// ============================================================================
// 5. Span<T>: View bộ nhớ phẳng 16-byte không cấp phát động
// ============================================================================
template <typename T>
struct Span {
    const T* ptr = nullptr;
    usize len = 0;

    constexpr Span() noexcept = default;
    constexpr Span(const T* p, usize l) noexcept : ptr(p), len(l) {}

    template <usize N>
    constexpr Span(const MultiArray<T, N>& arr) noexcept : ptr(arr.data()), len(N) {}

    [[nodiscard]] constexpr usize size() const noexcept { return len; }
    [[nodiscard]] constexpr bool empty() const noexcept { return len == 0; }
    [[nodiscard]] constexpr const T* data() const noexcept { return ptr; }
    [[nodiscard]] constexpr const T* begin() const noexcept { return ptr; }
    [[nodiscard]] constexpr const T* end() const noexcept { return ptr + len; }

    constexpr const T& operator[](usize idx) const noexcept { return ptr[idx]; }

    // 1. Compile-Time Known Intent: Subspan with compile-time Offset & Count (Zero Runtime Math)
    template <usize Offset, usize Count = usize(-1)>
    [[nodiscard]] constexpr Span<T> subspan() const noexcept {
        if constexpr (Offset == 0 && Count == usize(-1)) {
            return *this;
        } else if constexpr (Count == usize(-1)) {
            return Span<T>(ptr + Offset, len - Offset);
        } else {
            return Span<T>(ptr + Offset, Count);
        }
    }

    // 2. Intent-Specialized Subspan:
    // IsZeroOffset: If caller knows offset == 0, eliminates 'ptr + offset' arithmetic.
    // CheckBounds: If caller already knows bounds are safe (default), eliminates 'offset >= len' branch!
    template <bool IsZeroOffset = false, bool CheckBounds = false>
    [[nodiscard]] constexpr Span<T> subspan(usize offset, usize count = usize(-1)) const noexcept {
        if constexpr (CheckBounds) {
            if constexpr (IsZeroOffset) {
                const usize actualCount = count < len ? count : len;
                return Span<T>(ptr, actualCount);
            } else {
                if (offset >= len) return Span<T>(nullptr, 0);
                const usize available = len - offset;
                const usize actualCount = count < available ? count : available;
                return Span<T>(ptr + offset, actualCount);
            }
        } else {
            if constexpr (IsZeroOffset) {
                const usize actualCount = (count == usize(-1)) ? len : count;
                return Span<T>(ptr, actualCount);
            } else {
                const usize actualCount = (count == usize(-1)) ? (len - offset) : count;
                return Span<T>(ptr + offset, actualCount);
            }
        }
    }

    [[nodiscard]] constexpr const T& front() const noexcept { return ptr[0]; }
    [[nodiscard]] constexpr const T& back() const noexcept { return ptr[len > 0 ? len - 1 : 0]; }
    [[nodiscard]] constexpr bool contains(usize idx) const noexcept { return idx < len; }
};

} // namespace stockcut
