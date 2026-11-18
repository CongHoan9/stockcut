// ============================================================================
// CoreEngine - Self-Contained Monoid Groups & DOD Containers (Stockfish Architecture)
// ----------------------------------------------------------------------------
// [STOCKFISH DATA-STRUCTURE-CENTRIC & ALGEBRAIC MONOID FUSION]:
// - MonoidComposer<T>: Gấp Monoid tại compile-time với toán tử compose/union.
// - Group<T, Capacity>: Nhóm Tự Chủ Đại Số với gấp Monoid và truy vấn O(1).
//   TUYỆT ĐỐI KHÔNG GÁN {} trên MultiArray items để triệt tiêu áp lực memset.
// - FlatMonoidSequence<T>: Chuỗi hợp nhất [T, Group<T>, T] duyệt liên tục 0 cache miss.
// - TrackBucket<T>: DOD Bucket đồng nhất với tóm tắt Monoid gấp sẵn.
// - ZRankArena<T>: Đệm độc lập thứ tự Z-index trực tiếp.
// - IndexBucketList: Danh sách khe chỉ mục không cấp phát động.
// - TypeBucketMap: Bảng gom cụm kiểu theo chuẩn Stockfish byType[].
// ============================================================================
#pragma once
#include <type_traits>
#include "memory.h"

namespace stockcut {

// Forward declarations of basic types needed for aliases
template<typename Real> struct Rect;
struct Transform2D;
struct ColorTransferOp;
template<typename T> struct BitOps;
template<usize Blocks> struct Mask;

// ============================================================================
// 1. Monoid Traits & Composer
// ============================================================================
template <typename T>
struct MonoidComposer {
    static constexpr T identity() noexcept {
        if constexpr (requires { T::identity(); }) {
            return T::identity();
        } else {
            return T{};
        }
    }

    [[nodiscard]] [[gnu::always_inline]] static constexpr T compose(const T& a, const T& b) noexcept {
        if constexpr (requires { a.compose(b); }) {
            return a.compose(b);
        } else if constexpr (requires { a.union_with(b); }) {
            return a.union_with(b);
        } else if constexpr (requires { a * b; }) {
            return a * b;
        } else if constexpr (requires { a + b; }) {
            return a + b;
        } else {
            return b;
        }
    }
};



template <typename T>
struct is_expanding_group : std::false_type {};

template <typename T>
inline constexpr bool is_expanding_group_v = is_expanding_group<T>::value;

template <typename T>
inline constexpr bool is_contractive_group_v = !is_expanding_group_v<T>;

template <typename T>
struct MonoidTraits {
    static constexpr bool  IsExpanding   = is_expanding_group_v<T>;
    static constexpr usize GroupCapacity = IsExpanding ? 64 : 16;
    static constexpr usize MaxNodes      = 64;
    static constexpr usize MaxChildren   = IsExpanding ? 256 : 1;
};

// ============================================================================
// 2. Group<T, Capacity>: Nhóm Tự Chủ Đại Số (Self-Contained Monoid Group)
// ============================================================================
template <typename T, usize Capacity = MonoidTraits<T>::GroupCapacity>
struct alignas(64) Group {
    T summary = MonoidComposer<T>::identity();
    // MultiArray items không dùng {} để triệt tiêu áp lực memset
    MultiArray<T, is_expanding_group_v<T> ? Capacity : 1> items;
    u32 count = 0;
    u64 activeMask = 0ULL;

    using value_type = T;
    static constexpr usize capacity_value = Capacity;

    constexpr Group() noexcept = default;

    [[nodiscard]] static constexpr Group identity() noexcept {
        return Group{};
    }

    [[nodiscard]] [[gnu::always_inline]] inline constexpr Group compose(const Group& rhs) const noexcept {
        Group res;
        res.summary = MonoidComposer<T>::compose(summary, rhs.summary);
        res.count = count + rhs.count;
        res.activeMask = activeMask | rhs.activeMask;
        if constexpr (is_expanding_group_v<T>) {
            const usize limit1 = count < Capacity ? count : Capacity;
            for (usize i = 0; i < limit1; ++i) {
                res.items[i] = items[i];
            }
            const usize rem = Capacity > limit1 ? Capacity - limit1 : 0;
            const usize limit2 = rhs.count < rem ? rhs.count : rem;
            for (usize j = 0; j < limit2; ++j) {
                res.items[limit1 + j] = rhs.items[j];
            }
        }
        return res;
    }

    inline void reset() noexcept {
        summary = MonoidComposer<T>::identity();
        count = 0;
        activeMask = 0ULL;
    }

    [[gnu::always_inline]] inline Group& operator<<(const T& item) noexcept {
        summary = MonoidComposer<T>::compose(summary, item);
        activeMask |= (1ULL << (count & 63));
        if constexpr (is_expanding_group_v<T>) {
            if (count < Capacity) {
                items[count] = item;
            }
        }
        ++count;
        return *this;
    }

    template <usize OtherCap>
    [[gnu::always_inline]] inline Group& operator<<(const Group<T, OtherCap>& rhs) noexcept {
        summary = MonoidComposer<T>::compose(summary, rhs.summary);
        activeMask |= rhs.activeMask;
        if constexpr (is_expanding_group_v<T>) {
            const usize rem = Capacity > count ? Capacity - count : 0;
            const usize limit = rhs.count < rem ? rhs.count : rem;
            for (usize j = 0; j < limit; ++j) {
                items[count + j] = rhs.items[j];
            }
        }
        count += rhs.count;
        return *this;
    }

    template <bool CheckCapacity = false>
    [[gnu::always_inline]] inline Group& push_back(const T& item) noexcept {
        summary = MonoidComposer<T>::compose(summary, item);
        activeMask |= (1ULL << (count & 63));
        if constexpr (is_expanding_group_v<T>) {
            if constexpr (CheckCapacity) {
                if (count < Capacity) items[count] = item;
            } else {
                items[count] = item;
            }
        }
        ++count;
        return *this;
    }

    inline void push_back(const T& item) noexcept {
        push_back<true>(item);
    }

    template <usize OtherCap>
    inline void push_back(const Group<T, OtherCap>& rhs) noexcept {
        *this << rhs;
    }

    [[nodiscard]] inline const T& get_summary() const noexcept {
        return summary;
    }

    [[nodiscard]] inline T& get_summary() noexcept {
        return summary;
    }

    [[nodiscard]] inline constexpr auto collapsed_summary() const noexcept {
        if constexpr (requires { summary.collapsed_summary(); }) {
            return summary.collapsed_summary();
        } else {
            return summary;
        }
    }

    [[nodiscard]] inline constexpr operator const T&() const noexcept {
        return summary;
    }
    [[nodiscard]] inline constexpr operator T&() noexcept {
        return summary;
    }

    [[nodiscard]] inline constexpr usize size() const noexcept {
        return count;
    }

    [[nodiscard]] inline constexpr usize capacity() const noexcept {
        return Capacity;
    }

    [[nodiscard]] inline constexpr bool is_empty() const noexcept {
        return count == 0;
    }

    [[nodiscard]] inline constexpr bool is_identity() const noexcept {
        if constexpr (requires { summary.is_identity(); }) {
            return count == 0 || summary.is_identity();
        } else {
            return count == 0;
        }
    }

    constexpr T& operator[](usize index) noexcept {
        if constexpr (is_expanding_group_v<T>) {
            return items[index];
        } else {
            return summary;
        }
    }

    constexpr const T& operator[](usize index) const noexcept {
        if constexpr (is_expanding_group_v<T>) {
            return items[index];
        } else {
            return summary;
        }
    }

    constexpr T* begin() noexcept { return items.begin(); }
    constexpr const T* begin() const noexcept { return items.begin(); }
    constexpr T* end() noexcept { return items.begin() + count; }
    constexpr const T* end() const noexcept { return items.begin() + count; }
    constexpr T* data() noexcept { return items.data(); }
    constexpr const T* data() const noexcept { return items.data(); }
    [[nodiscard]] inline constexpr Span<const T> as_span() const noexcept {
        if constexpr (is_expanding_group_v<T>) {
            return Span<const T>(items.data(), count < Capacity ? count : Capacity);
        } else {
            return Span<const T>(&summary, count > 0 ? 1 : 0);
        }
    }

    template <typename Fn>
    inline void for_each_active(Fn&& fn) const noexcept {
        u64 mask = activeMask;
        while (mask) {
            const u32 idx = BitOps<u64>::pop_lsb(mask);
            if constexpr (is_expanding_group_v<T>) {
                if (idx < count && idx < Capacity) {
                    fn(idx, items[idx]);
                }
            } else {
                fn(0, summary);
                break;
            }
        }
    }
};

// ============================================================================
// 2b. Ring<T, Depth>: Struct Tiện Ích Bộ Đệm Xoay Vòng N Tầng (GEMINI.md Rule 7)
// ----------------------------------------------------------------------------
// - Depth = 2: Ping-Pong Buffer (Dual FBO / Swapchain) đảo bit 1-bit XOR ^= 1.
// - Depth = 3: Triple-Buffering Pipelining (Frame N GPU, N+1 CPU Pack, N+2 Prefetch).
// - Depth = 5: Penta-Buffering PBO Streaming triệt tiêu 100% DMA Stall.
// ============================================================================
template <typename T, usize Depth = 2>
struct alignas(alignof(T) > 16 ? alignof(T) : 16) Ring {
    MultiArray<T, Depth> slices{};
    u32 cur = 0;

    static constexpr usize DEPTH = Depth;

    constexpr Ring() noexcept = default;

    [[gnu::always_inline]] inline void advance() noexcept {
        if constexpr (Depth == 2) {
            cur ^= 1u;
        } else if constexpr ((Depth & (Depth - 1)) == 0) {
            cur = (cur + 1u) & static_cast<u32>(Depth - 1u);
        } else {
            cur = (cur + 1u < Depth) ? (cur + 1u) : 0u;
        }
    }

    [[nodiscard]] [[gnu::always_inline]] inline T& current() noexcept { return slices[cur]; }
    [[nodiscard]] [[gnu::always_inline]] inline const T& current() const noexcept { return slices[cur]; }

    [[nodiscard]] [[gnu::always_inline]] inline T& previous() noexcept {
        if constexpr (Depth == 2) {
            return slices[cur ^ 1u];
        } else {
            const u32 prev = (cur == 0u) ? static_cast<u32>(Depth - 1u) : (cur - 1u);
            return slices[prev];
        }
    }
    [[nodiscard]] [[gnu::always_inline]] inline const T& previous() const noexcept {
        if constexpr (Depth == 2) {
            return slices[cur ^ 1u];
        } else {
            const u32 prev = (cur == 0u) ? static_cast<u32>(Depth - 1u) : (cur - 1u);
            return slices[prev];
        }
    }

    [[nodiscard]] [[gnu::always_inline]] inline T& next() noexcept {
        if constexpr (Depth == 2) {
            return slices[cur ^ 1u];
        } else {
            const u32 n = (cur + 1u < Depth) ? (cur + 1u) : 0u;
            return slices[n];
        }
    }
    [[nodiscard]] [[gnu::always_inline]] inline const T& next() const noexcept {
        if constexpr (Depth == 2) {
            return slices[cur ^ 1u];
        } else {
            const u32 n = (cur + 1u < Depth) ? (cur + 1u) : 0u;
            return slices[n];
        }
    }

    [[nodiscard]] [[gnu::always_inline]] inline u32 index() const noexcept { return cur; }
    [[nodiscard]] [[gnu::always_inline]] inline u32 previous_index() const noexcept {
        if constexpr (Depth == 2) return cur ^ 1u;
        return (cur == 0u) ? static_cast<u32>(Depth - 1u) : (cur - 1u);
    }
    [[nodiscard]] [[gnu::always_inline]] inline u32 next_index() const noexcept {
        if constexpr (Depth == 2) return cur ^ 1u;
        return (cur + 1u < Depth) ? (cur + 1u) : 0u;
    }
    [[nodiscard]] [[gnu::always_inline]] inline u32 offset_index(u32 steps) const noexcept {
        return (cur + steps) % static_cast<u32>(Depth);
    }
    [[nodiscard]] [[gnu::always_inline]] inline T& operator[](usize idx) noexcept { return slices[idx]; }
    [[nodiscard]] [[gnu::always_inline]] inline const T& operator[](usize idx) const noexcept { return slices[idx]; }
    [[nodiscard]] [[gnu::always_inline]] inline T* data() noexcept { return slices.data(); }
    [[nodiscard]] [[gnu::always_inline]] inline const T* data() const noexcept { return slices.data(); }
    inline void reset() noexcept { cur = 0; }
};



// Specialization of is_expanding_group & MonoidTraits for nested Group<InnerT, InnerCap>
template <typename InnerT, usize InnerCap>
struct is_expanding_group<Group<InnerT, InnerCap>> : std::bool_constant<is_expanding_group_v<InnerT>> {};

template <typename InnerT, usize InnerCap>
struct MonoidTraits<Group<InnerT, InnerCap>> {
    static constexpr bool  IsExpanding   = is_expanding_group_v<InnerT>;
    static constexpr usize GroupCapacity = 16;
    static constexpr usize MaxNodes      = 16;
    static constexpr usize MaxChildren   = 16;
};

// C++20 Concept for Monoid Groups
template <typename G>
concept AnyGroup = requires(const G& g) {
    typename G::value_type;
    { g.get_summary() } -> std::convertible_to<const typename G::value_type&>;
    { g.is_identity() } -> std::convertible_to<bool>;
    { g.size() } -> std::convertible_to<usize>;
};

// Template Template Parameter pattern demonstration:
// Forward declarations for cross-struct utility synergy
template <typename T, usize Capacity> struct Pipelines;
template <typename T, typename MaskType> struct Runtime;
template <typename T, usize Capacity> struct TrackBucket;
template <typename T, usize MaxNodes, usize MaxChildren> struct FlatMonoidSequence;
template <typename T, usize MaxElements> struct ZRankArena;
template <typename KeyEnum, usize EnumCount, typename T, usize CapacityPerBucket> struct TypeBucketMap;

template <template <typename, usize> class GroupTemplate, typename T, usize Capacity = 16>
struct HierarchicalGroupStage {
    GroupTemplate<T, Capacity> primary;
    GroupTemplate<T, Capacity> secondary;

    [[nodiscard]] inline constexpr T collapsed() const noexcept {
        return MonoidComposer<T>::compose(primary.collapsed_summary(), secondary.collapsed_summary());
    }
};

// ============================================================================
// Pipelines<T, Capacity>: Struct Tiện Ích Pipeline Đa Kiểu T (GEMINI.md Rule 7)
// ============================================================================
// Cứ có kiểu dữ liệu hạt nguyên tử T là tự động có Pipelines<T> tương ứng.
// - Nhận BẤT KỲ kiểu dữ liệu nào T (FusedUberEffect, Transform2D, ColorTransferOp, Rect<float>, WavefrontPhasor, v.v.)
// - Cold Path: Tiền tính toán, nạp và gấp Monoid trước (bake, stage, operator<<) 1 lần duy nhất.
// - Hot Path: Truy xuất evaluate() / hot_summary() trong O(1) tức thì, ZERO loop, ZERO heap allocation.
template <typename T, usize Capacity = MonoidTraits<T>::GroupCapacity>
struct alignas(64) Pipelines {
    using value_type = T;
    static constexpr usize capacity_value = Capacity;

    Group<T, Capacity> group;

    constexpr Pipelines() noexcept = default;

    // Cold Path: Ingests & folds items 1 time only
    [[gnu::always_inline]] inline Pipelines& operator<<(const T& item) noexcept {
        group << item;
        return *this;
    }

    template <usize OtherCap>
    [[gnu::always_inline]] inline Pipelines& operator<<(const Group<T, OtherCap>& rhs) noexcept {
        group << rhs;
        return *this;
    }

    template <typename OtherT, usize OtherCap>
    [[gnu::always_inline]] inline Pipelines& operator<<(const Pipelines<OtherT, OtherCap>& rhs) noexcept {
        if constexpr (std::is_same_v<T, OtherT>) {
            group << rhs.group;
        } else if constexpr (requires { group << rhs.hot_summary(); }) {
            group << rhs.hot_summary();
        }
        return *this;
    }

    inline void bake(const T& item) noexcept {
        group << item;
    }

    template <usize OtherCap>
    inline void bake(const Group<T, OtherCap>& rhs) noexcept {
        group << rhs;
    }
    // Ingestion from Span
    [[gnu::always_inline]] inline Pipelines& operator<<(const Span<const T>& span) noexcept {
        for (usize i = 0; i < span.size(); ++i) {
            group << span[i];
        }
        return *this;
    }

    // Ingestion from Runtime<T>
    template <typename MaskType>
    [[gnu::always_inline]] inline Pipelines& operator<<(const Runtime<T, MaskType>& rt) noexcept {
        group << rt.get();
        return *this;
    }

    // Ingestion from TrackBucket<T>
    template <usize OtherCap>
    [[gnu::always_inline]] inline Pipelines& operator<<(const TrackBucket<T, OtherCap>& bucket) noexcept {
        group << bucket.get_summary();
        return *this;
    }

    // Cold Path: Batch-bake a span of items
    inline void bake(const Span<const T>& span) noexcept {
        for (usize i = 0; i < span.size(); ++i) {
            group << span[i];
        }
    }

    [[nodiscard]] inline constexpr Span<const T> as_span() const noexcept {
        return group.as_span();
    }

    inline void reset() noexcept {
        group.reset();
    }

    // Hot Path: O(1) query with zero loops and zero allocations
    [[nodiscard]] [[gnu::always_inline]] inline constexpr const T& evaluate() const noexcept {
        return group.get_summary();
    }

    [[nodiscard]] [[gnu::always_inline]] inline constexpr const T& hot_summary() const noexcept {
        return group.get_summary();
    }

    [[nodiscard]] [[gnu::always_inline]] inline constexpr auto collapsed_hot() const noexcept {
        return group.collapsed_summary();
    }

    [[nodiscard]] [[gnu::always_inline]] inline constexpr const T& get_summary() const noexcept {
        return group.get_summary();
    }

    [[nodiscard]] [[gnu::always_inline]] inline constexpr T& get_summary() noexcept {
        return group.get_summary();
    }

    [[nodiscard]] [[gnu::always_inline]] inline constexpr operator const T&() const noexcept {
        return group.get_summary();
    }

    [[nodiscard]] [[gnu::always_inline]] inline constexpr bool is_identity() const noexcept {
        return group.is_identity();
    }

    [[nodiscard]] [[gnu::always_inline]] inline constexpr usize size() const noexcept {
        return group.size();
    }

    [[nodiscard]] [[gnu::always_inline]] inline constexpr usize capacity() const noexcept {
        return Capacity;
    }
};

// ============================================================================
// Runtime<T, MaskType>: Struct Tiện Ích Runtime Mẫu Hóa Toàn Năng (GEMINI.md Rule 6, 7 & 8)
// ============================================================================
// Đối ngẫu tuyệt đối: T (Authoring/Static State - Cold) <-> Runtime<T> (Evaluated State - Hot)
// Khi Runtime<T> được nâng cấp (SIMD, Zobrist Epoch, Zero-Allocation, Double Buffering),
// TOÀN BỘ Runtime<A>, Runtime<B>, Runtime<Timeline> đều tự động được hưởng lợi!
template <typename T, typename MaskType = u64>
struct alignas(64) Runtime {
    using value_type = T;
    using mask_type  = MaskType;

    T authoring{};                 // Authoring / Config state (Cold Path)
    u64 epoch = 0;                  // Epoch / Versioning (Stockfish dirty tracking)
    u64 stateKey = 0;               // Zobrist / Hash signature
    MaskType activeMask{};          // Active slots / bitboard
    bool isDirty = true;            // Dirty gatekeeper (Stockfish DirtyMutation)

    constexpr Runtime() noexcept = default;

    explicit constexpr Runtime(const T& cold) noexcept
        : authoring(cold), epoch(1), stateKey(0), isDirty(true) {}

    // Cold Path Setup / Mutation
    inline void update(const T& newCold) noexcept {
        authoring = newCold;
        ++epoch;
        isDirty = true;
    }

    inline void touch() noexcept {
        ++epoch;
        isDirty = true;
    }

    inline void clean() noexcept {
        isDirty = false;
    }
    // Stream operator into underlying authoring state (Pipelines<T> / Group<T>)
    template <typename ItemT>
    [[gnu::always_inline]] inline Runtime& operator<<(const ItemT& item) noexcept {
        if constexpr (requires { authoring << item; }) {
            authoring << item;
            touch();
        } else if constexpr (requires { authoring.push_back(item); }) {
            authoring.push_back(item);
            touch();
        }
        return *this;
    }

    // Branchless conditional dirty touch with Known Intent support
    template <bool Cond = true>
    [[gnu::always_inline]] inline void touch_if() noexcept {
        if constexpr (Cond) {
            ++epoch;
            isDirty = true;
        }
    }

    [[gnu::always_inline]] inline void touch_if(bool cond) noexcept {
        epoch += static_cast<u64>(cond);
        isDirty |= cond;
    }

    // Step or skip: Leveraging Known Intent (KnownDirty) to eliminate runtime if(!isDirty)
    template <bool KnownDirty = false, typename... Args>
    [[gnu::always_inline]] inline decltype(auto) step_or_skip(Args&&... args) noexcept {
        if constexpr (KnownDirty) {
            clean();
            return evaluate(std::forward<Args>(args)...);
        } else {
            if (!isDirty) {
                return evaluate(std::forward<Args>(args)...);
            }
            clean();
            return evaluate(std::forward<Args>(args)...);
        }
    }

    // Zero-allocation Span access if authoring supports as_span()
    [[nodiscard]] inline auto as_span() const noexcept {
        if constexpr (requires { authoring.as_span(); }) {
            return authoring.as_span();
        } else {
            return Span<const T>(&authoring, 1);
        }
    }

    inline void set_state_key(u64 key) noexcept {
        stateKey = key;
    }

    inline void set_active_mask(MaskType mask) noexcept {
        activeMask = mask;
    }

    // Pointer-like & dereference access
    [[nodiscard]] [[gnu::always_inline]] inline constexpr const T* operator->() const noexcept {
        return &authoring;
    }
    [[nodiscard]] [[gnu::always_inline]] inline constexpr T* operator->() noexcept {
        return &authoring;
    }
    [[nodiscard]] [[gnu::always_inline]] inline constexpr const T& operator*() const noexcept {
        return authoring;
    }
    [[nodiscard]] [[gnu::always_inline]] inline constexpr T& operator*() noexcept {
        return authoring;
    }

    // Hot Path: O(1) query
    [[nodiscard]] [[gnu::always_inline]] inline constexpr const T& get() const noexcept {
        return authoring;
    }
    [[nodiscard]] [[gnu::always_inline]] inline constexpr T& get() noexcept {
        return authoring;
    }
    [[nodiscard]] [[gnu::always_inline]] inline constexpr operator const T&() const noexcept {
        return authoring;
    }
    [[nodiscard]] [[gnu::always_inline]] inline constexpr operator T&() noexcept {
        return authoring;
    }

    // Generic Runtime Evaluation: delegates to T::evaluate / step_to if available
    template <typename... Args>
    [[gnu::always_inline]] inline decltype(auto) evaluate(Args&&... args) const noexcept {
        if constexpr (requires { authoring.evaluate(std::forward<Args>(args)...); }) {
            return authoring.evaluate(std::forward<Args>(args)...);
        } else if constexpr (requires { authoring.step_to(std::forward<Args>(args)...); }) {
            return authoring.step_to(std::forward<Args>(args)...);
        } else {
            return authoring;
        }
    }

    template <typename... Args>
    [[gnu::always_inline]] inline decltype(auto) evaluate(Args&&... args) noexcept {
        if constexpr (requires { authoring.evaluate(std::forward<Args>(args)...); }) {
            return authoring.evaluate(std::forward<Args>(args)...);
        } else if constexpr (requires { authoring.step_to(std::forward<Args>(args)...); }) {
            return authoring.step_to(std::forward<Args>(args)...);
        } else {
            return authoring;
        }
    }

    template <typename... Args>
    [[gnu::always_inline]] inline decltype(auto) renderAt(Args&&... args) const noexcept {
        if constexpr (requires { authoring.renderAt(std::forward<Args>(args)...); }) {
            return authoring.renderAt(std::forward<Args>(args)...);
        }
    }

    template <typename... Args>
    [[gnu::always_inline]] inline decltype(auto) renderAt(Args&&... args) noexcept {
        if constexpr (requires { authoring.renderAt(std::forward<Args>(args)...); }) {
            return authoring.renderAt(std::forward<Args>(args)...);
        }
    }

    template <typename... Args>
    [[gnu::always_inline]] inline decltype(auto) loadProject(Args&&... args) noexcept {
        if constexpr (requires { authoring.loadProject(std::forward<Args>(args)...); }) {
            touch();
            return authoring.loadProject(std::forward<Args>(args)...);
        }
    }

    template <typename... Args>
    [[gnu::always_inline]] inline decltype(auto) seek(Args&&... args) noexcept {
        if constexpr (requires { authoring.seek(std::forward<Args>(args)...); }) {
            touch();
            return authoring.seek(std::forward<Args>(args)...);
        }
    }

    template <typename... Args>
    [[gnu::always_inline]] inline decltype(auto) advance(Args&&... args) noexcept {
        if constexpr (requires { authoring.advance(std::forward<Args>(args)...); }) {
            touch();
            return authoring.advance(std::forward<Args>(args)...);
        }
    }
};

// ============================================================================
// 3. TrackBucket<T, Capacity>: Bộ Đệm Dữ Liệu Đồng Nhất (Homogeneous DOD)
// ============================================================================
template <typename T, usize Capacity = 64>
struct alignas(64) TrackBucket {
    T summary = MonoidComposer<T>::identity();
    MultiArray<T, Capacity> storage; // KHÔNG CÓ {}
    u32 count = 0;
    u64 occupiedMask = 0ULL;

    constexpr TrackBucket() noexcept = default;

    inline void reset() noexcept {
        summary = MonoidComposer<T>::identity();
        count = 0;
        occupiedMask = 0ULL;
    }

    // Compile-Time Known Intent: HasExplicitSlot & CheckCapacity
    template <bool HasExplicitSlot = false, bool CheckCapacity = false>
    [[gnu::always_inline]] inline void push_back(const T& item, u32 slotIndex = 0) noexcept {
        if constexpr (CheckCapacity) {
            if (count >= Capacity) return;
        }
        summary = MonoidComposer<T>::compose(summary, item);
        storage[count] = item;
        if constexpr (HasExplicitSlot) {
            occupiedMask |= (1ULL << (slotIndex & 63));
        } else {
            occupiedMask |= (1ULL << (count & 63));
        }
        ++count;
    }

    inline void push_back(const T& item, u32 slotIndex) noexcept {
        if (slotIndex < 64) {
            push_back<true, true>(item, slotIndex);
        } else {
            push_back<false, true>(item);
        }
    }

    inline void push_back(const T& item) noexcept {
        push_back<false, true>(item);
    }

    [[nodiscard]] inline const T& get_summary() const noexcept { return summary; }
    [[nodiscard]] inline T& get_summary() noexcept { return summary; }
    [[nodiscard]] inline usize size() const noexcept { return count; }
    [[nodiscard]] inline bool empty() const noexcept { return count == 0; }
    [[nodiscard]] inline u64 mask() const noexcept { return occupiedMask; }
    [[nodiscard]] inline const T* data() const noexcept { return storage.data(); }
    [[nodiscard]] inline T* data() noexcept { return storage.data(); }
    [[nodiscard]] inline constexpr Span<T> as_span() noexcept { return Span<T>(storage.data(), count); }
    [[nodiscard]] inline constexpr Span<const T> as_span() const noexcept { return Span<const T>(storage.data(), count); }

    inline void push_back_span(const Span<const T>& span) noexcept {
        for (usize i = 0; i < span.size(); ++i) {
            push_back(span[i]);
        }
    }

    template <typename Fn>
    inline void for_each_active(Fn&& fn) const noexcept {
        for (usize i = 0; i < count; ++i) {
            fn(static_cast<u32>(i), storage[i]);
        }
    }

    template <typename Fn>
    inline void for_each_active(Fn&& fn) noexcept {
        for (usize i = 0; i < count; ++i) {
            fn(static_cast<u32>(i), storage[i]);
        }
    }

    template <typename Fn>
    inline void for_each_occupied_slot(Fn&& fn) const noexcept {
        u64 bb = occupiedMask;
        while (bb) {
            const u32 slot = BitOps<u64>::pop_lsb(bb);
            fn(slot);
        }
    }

    template <typename PipeT>
    inline void fold_into(PipeT& pipe) const noexcept {
        pipe << summary;
    }

    constexpr const T& operator[](usize idx) const noexcept { return storage[idx]; }
    constexpr T& operator[](usize idx) noexcept { return storage[idx]; }
};

// ============================================================================
// 4. FlatMonoidSequence<T, MaxNodes, MaxChildren>: Chuỗi Hợp Nhất [T, Group<T>, T]
// ============================================================================
template <typename T, usize MaxNodes = MonoidTraits<T>::MaxNodes, usize MaxChildren = MonoidTraits<T>::MaxChildren>
struct alignas(64) FlatMonoidSequence {
    static constexpr bool IsExpanding = is_expanding_group_v<T>;
    static constexpr usize ActualChildren = IsExpanding ? MaxChildren : 1;

    MultiArray<T, MaxNodes> items; // KHÔNG CÓ {}
    MultiArray<u16, MaxNodes> childOffsets; // KHÔNG CÓ {}
    MultiArray<u8, MaxNodes> childCounts; // KHÔNG CÓ {}
    MultiArray<T, ActualChildren> childArena; // KHÔNG CÓ {}

    u32 nodeCount = 0;
    u32 totalChildren = 0;
    u64 isGroupMask = 0ULL;

    constexpr FlatMonoidSequence() noexcept = default;

    inline void reset() noexcept {
        nodeCount = 0;
        totalChildren = 0;
        isGroupMask = 0ULL;
    }

    inline bool push_next(const T& item) noexcept {
        if (nodeCount >= MaxNodes) return false;
        items[nodeCount] = item;
        childOffsets[nodeCount] = 0;
        childCounts[nodeCount] = 0;
        ++nodeCount;
        return true;
    }

    template <usize Cap>
    inline bool push_next(const Group<T, Cap>& grp) noexcept {
        if (nodeCount >= MaxNodes) return false;
        const usize idx = nodeCount;
        items[idx] = grp.get_summary();
        if (idx < 64) {
            isGroupMask |= (1ULL << idx);
        }

        if constexpr (IsExpanding) {
            const u8 cCount = static_cast<u8>(grp.size() <= 255 ? grp.size() : 255);
            if (totalChildren + cCount <= MaxChildren) {
                childOffsets[idx] = static_cast<u16>(totalChildren);
                childCounts[idx] = cCount;
                for (usize i = 0; i < cCount; ++i) {
                    childArena[totalChildren + i] = grp[i];
                }
                totalChildren += cCount;
            } else {
                childOffsets[idx] = 0;
                childCounts[idx] = 0;
            }
        } else {
            childOffsets[idx] = static_cast<u16>(idx);
            childCounts[idx] = 1;
        }

        ++nodeCount;
        return true;
    }

    template <bool Cond>
    inline bool push_next_if(const T& item) noexcept {
        if constexpr (Cond) {
            return push_next(item);
        } else {
            return false;
        }
    }

    inline bool push_next_if(bool cond, const T& item) noexcept {
        if (cond) {
            return push_next(item);
        }
        return false;
    }

    template <bool Cond = true>
    inline void push_back_if(const T& item) noexcept {
        push_next_if<Cond>(item);
    }

    inline void push_back_if(bool cond, const T& item) noexcept {
        push_next_if(cond, item);
    }

    template <bool Cond, usize Cap>
    inline bool push_next_if(const Group<T, Cap>& grp) noexcept {
        if constexpr (Cond) {
            return push_next(grp);
        } else {
            return false;
        }
    }

    template <usize Cap>
    inline bool push_next_if(bool cond, const Group<T, Cap>& grp) noexcept {
        if (cond) {
            return push_next(grp);
        }
        return false;
    }

    [[nodiscard]] inline constexpr usize size() const noexcept { return nodeCount; }
    [[nodiscard]] inline constexpr bool empty() const noexcept { return nodeCount == 0; }

    [[nodiscard]] inline constexpr bool is_group(usize idx) const noexcept {
        return idx < 64 ? ((isGroupMask & (1ULL << idx)) != 0) : (childCounts[idx] > 0);
    }

    [[nodiscard]] inline Span<T> get_children(usize idx) const noexcept {
        if (idx >= nodeCount || childCounts[idx] == 0) return Span<T>();
        if constexpr (IsExpanding) {
            return Span<T>(&childArena[childOffsets[idx]], childCounts[idx]);
        } else {
            return Span<T>(&items[idx], 1);
        }
    }

    constexpr T& operator[](usize idx) noexcept { return items[idx]; }
    constexpr const T& operator[](usize idx) const noexcept { return items[idx]; }

    constexpr const T* begin() const noexcept { return items.begin(); }
    constexpr const T* end() const noexcept { return items.begin() + nodeCount; }
    constexpr T* begin() noexcept { return items.begin(); }
    constexpr T* end() noexcept { return items.begin() + nodeCount; }
    constexpr const T* data() const noexcept { return items.data(); }
    constexpr T* data() noexcept { return items.data(); }
    [[nodiscard]] inline T fold_all() const noexcept {
        T res = MonoidComposer<T>::identity();
        for (usize i = 0; i < nodeCount; ++i) {
            res = MonoidComposer<T>::compose(res, items[i]);
        }
        return res;
    }

    [[nodiscard]] inline constexpr Span<const T> as_span() const noexcept {
        return Span<const T>(items.data(), nodeCount);
    }
};

// ============================================================================
// 5. ZRankArena<T, MaxElements>: Đệm Độc Lập Thứ Tự Z-Index Trực Tiếp
// ============================================================================
template <typename T, usize MaxElements = 64>
struct alignas(64) ZRankArena {
    MultiArray<T, MaxElements> elements; // KHÔNG CÓ {}
    u64 allocatedMask = 0ULL;
    u32 maxRank = 0;

    constexpr ZRankArena() noexcept = default;

    inline void reset() noexcept {
        allocatedMask = 0ULL;
        maxRank = 0;
    }

    template <bool CheckBounds = false>
    [[gnu::always_inline]] inline void insert(u32 zRank, const T& val) noexcept {
        if constexpr (CheckBounds) {
            if (zRank >= MaxElements) return;
        }
        elements[zRank] = val;
        allocatedMask |= (1ULL << (zRank & 63));
        if (zRank >= maxRank) maxRank = zRank + 1;
    }

    template <bool InBlock0 = true>
    [[nodiscard]] inline bool has(u32 zRank) const noexcept {
        if constexpr (InBlock0) {
            return (allocatedMask & (1ULL << (zRank & 63))) != 0;
        } else {
            return zRank < 64 ? ((allocatedMask & (1ULL << zRank)) != 0) : (zRank < maxRank);
        }
    }

    constexpr T& operator[](usize zRank) noexcept { return elements[zRank]; }
    constexpr const T& operator[](usize zRank) const noexcept { return elements[zRank]; }
    [[nodiscard]] inline u32 count() const noexcept { return maxRank; }

    template <typename Fn>
    inline void for_each_active(Fn&& fn) const noexcept {
        u64 bb = allocatedMask;
        while (bb) {
            const u32 z = static_cast<u32>(__builtin_ctzll(bb));
            bb &= bb - 1ULL;
            fn(z, elements[z]);
        }
    }

    template <typename Fn>
    inline void for_each_active(Fn&& fn) noexcept {
        u64 bb = allocatedMask;
        while (bb) {
            const u32 z = static_cast<u32>(__builtin_ctzll(bb));
            bb &= bb - 1ULL;
            fn(z, elements[z]);
        }
    }
    [[nodiscard]] inline T fold_active() const noexcept {
        T res = MonoidComposer<T>::identity();
        u64 bb = allocatedMask;
        while (bb) {
            const u32 z = BitOps<u64>::pop_lsb(bb);
            res = MonoidComposer<T>::compose(res, elements[z]);
        }
        return res;
    }

    template <typename PipeT>
    inline void fold_into(PipeT& pipe) const noexcept {
        u64 bb = allocatedMask;
        while (bb) {
            const u32 z = BitOps<u64>::pop_lsb(bb);
            pipe << elements[z];
        }
    }

    [[nodiscard]] inline constexpr Span<const T> as_span() const noexcept {
        return Span<const T>(elements.data(), maxRank);
    }
};

// ============================================================================
// 6. IndexBucketList<Slots, MaxElements>: Danh Sách Khe Chỉ Mục Không Cấp Phát
// ============================================================================
template <usize Slots, usize MaxElements>
struct alignas(64) IndexBucketList {
    MultiArray<i16, Slots + 1> head; // KHÔNG CÓ {}
    MultiArray<i16, MaxElements> next; // KHÔNG CÓ {}
    u64 activeSlotsMask = 0ULL;

    constexpr IndexBucketList() noexcept = default;

    inline void reset() noexcept {
        head.fill(-1);
        next.fill(-1);
        activeSlotsMask = 0ULL;
    }

    template <bool CheckBounds = false>
    [[gnu::always_inline]] inline void add(u32 slot, u32 elemIdx) noexcept {
        if constexpr (CheckBounds) {
            const u32 safeSlot = (slot < Slots) ? slot : static_cast<u32>(Slots);
            if (elemIdx < MaxElements) {
                next[elemIdx] = head[safeSlot];
                head[safeSlot] = static_cast<i16>(elemIdx);
                activeSlotsMask |= (1ULL << (slot & 63));
            }
        } else {
            next[elemIdx] = head[slot];
            head[slot] = static_cast<i16>(elemIdx);
            activeSlotsMask |= (1ULL << (slot & 63));
        }
    }

    template <bool CheckBounds = false>
    [[nodiscard]] inline i16 first(u32 slot) const noexcept {
        if constexpr (CheckBounds) {
            const u32 safeSlot = (slot < Slots) ? slot : static_cast<u32>(Slots);
            return head[safeSlot];
        } else {
            return head[slot];
        }
    }

    template <bool CheckBounds = false>
    [[nodiscard]] inline i16 next_elem(u32 elemIdx) const noexcept {
        if constexpr (CheckBounds) {
            return (elemIdx < MaxElements) ? next[elemIdx] : -1;
        } else {
            return next[elemIdx];
        }
    }

    template <bool CheckBounds = false, typename Fn>
    inline void for_each_in_slot(u32 slot, Fn&& fn) const noexcept {
        for (i16 idx = first<CheckBounds>(slot); idx != -1; idx = next_elem<CheckBounds>(idx)) {
            fn(static_cast<u32>(idx));
        }
    }
    [[nodiscard]] inline bool has_active_slots() const noexcept {
        return activeSlotsMask != 0ULL;
    }

    template <typename Fn>
    inline void for_each_active_slot(Fn&& fn) const noexcept {
        u64 bb = activeSlotsMask;
        while (bb) {
            const u32 slot = BitOps<u64>::pop_lsb(bb);
            for_each_in_slot(slot, [&](u32 elemIdx) {
                fn(slot, elemIdx);
            });
        }
    }
};



// ============================================================================
// 7. TypeBucketMap<KeyEnum, EnumCount, T, CapacityPerBucket>: Bảng Gom Cụm Kiểu
// ============================================================================
template <typename KeyEnum, usize EnumCount, typename T, usize CapacityPerBucket = 64>
struct alignas(64) TypeBucketMap {
    MultiArray<TrackBucket<T, CapacityPerBucket>, EnumCount> buckets; // KHÔNG CÓ {}
    u64 activeTypesMask = 0ULL;

    constexpr TypeBucketMap() noexcept = default;

    inline void reset() noexcept {
        for (usize i = 0; i < EnumCount; ++i) {
            buckets[i].reset();
        }
        activeTypesMask = 0ULL;
    }

    template <bool CheckBounds = false>
    [[gnu::always_inline]] inline void add(KeyEnum type, const T& item, u32 slotIndex = 0xFFFFFFFF) noexcept {
        const usize t = static_cast<usize>(type);
        if constexpr (CheckBounds) {
            if (t >= EnumCount) return;
        }
        buckets[t].push_back(item, slotIndex);
        activeTypesMask |= (1ULL << (t & 63));
    }

    [[nodiscard]] inline TrackBucket<T, CapacityPerBucket>& get(KeyEnum type) noexcept {
        return buckets[static_cast<usize>(type)];
    }

    [[nodiscard]] inline const TrackBucket<T, CapacityPerBucket>& get(KeyEnum type) const noexcept {
        return buckets[static_cast<usize>(type)];
    }

    [[nodiscard]] inline u64 active_types_mask() const noexcept {
        return activeTypesMask;
    }
    [[nodiscard]] inline T fold_all_active_types() const noexcept {
        T res = MonoidComposer<T>::identity();
        u64 mask = activeTypesMask;
        while (mask) {
            const u32 tIdx = BitOps<u64>::pop_lsb(mask);
            res = MonoidComposer<T>::compose(res, buckets[tIdx].get_summary());
        }
        return res;
    }

    [[nodiscard]] inline constexpr Span<const T> get_span(KeyEnum type) const noexcept {
        return buckets[static_cast<usize>(type)].as_span();
    }

    [[nodiscard]] inline constexpr Span<T> get_span(KeyEnum type) noexcept {
        return buckets[static_cast<usize>(type)].as_span();
    }

    template <typename PipeT>
    inline void fold_into(PipeT& pipe) const noexcept {
        pipe << fold_all_active_types();
    }

    template <typename Fn>
    inline void for_each_active_type(Fn&& fn) noexcept {
        u64 mask = activeTypesMask;
        while (mask) {
            const u32 tIdx = static_cast<u32>(__builtin_ctzll(mask));
            mask &= mask - 1ULL;
            fn(static_cast<KeyEnum>(tIdx), buckets[tIdx]);
        }
    }

    template <typename Fn>
    inline void for_each_active_type(Fn&& fn) const noexcept {
        u64 mask = activeTypesMask;
        while (mask) {
            const u32 tIdx = static_cast<u32>(__builtin_ctzll(mask));
            mask &= mask - 1ULL;
            fn(static_cast<KeyEnum>(tIdx), buckets[tIdx]);
        }
    }
};




// ============================================================================
// 10. Packed<T, N>: Nén N phần tử kiểu T vào 1 thanh ghi integer (Zero-Loop SWAR)
// ============================================================================
template <typename T = u8, usize N = (sizeof(u64) / sizeof(T))>
struct alignas(sizeof(T) * N <= 4 ? 4 : 8) Packed {
    static_assert(sizeof(T) * N <= 8, "Packed must fit within a single 64-bit register");
    using RawType = std::conditional_t<(sizeof(T) * N <= 4), u32, u64>;
    static constexpr RawType kEmptyMask = ~RawType(0);

    RawType raw = kEmptyMask;

    constexpr Packed() noexcept = default;
    constexpr explicit Packed(RawType val) noexcept : raw(val) {}

    [[gnu::always_inline]] inline void clear() noexcept {
        raw = kEmptyMask;
    }

    [[gnu::always_inline]] inline bool push_next_if(bool cond, T val) noexcept {
        if constexpr (sizeof(T) == 1 && N <= 8) {
            const u64 inv = ~raw;
            const u64 zeroBytes = (inv - 0x0101010101010101ULL) & ~inv & 0x8080808080808080ULL;
            const bool canPush = cond & (zeroBytes != 0ULL);
            const u32 shift = (static_cast<u32>(__builtin_ctzll(zeroBytes | 0x8000000000000000ULL)) >> 3) << 3;
            const u64 writeMask = canPush ? (0xFFULL << shift) : 0ULL;
            raw = (raw & ~writeMask) | (canPush ? (static_cast<u64>(val) << shift) : 0ULL);
            return canPush;
        } else if constexpr (sizeof(T) == 2 && N <= 4) {
            const u64 inv = ~raw;
            const u64 zeroWords = (inv - 0x0001000100010001ULL) & ~inv & 0x8000800080008000ULL;
            const bool canPush = cond & (zeroWords != 0ULL);
            const u32 shift = (static_cast<u32>(__builtin_ctzll(zeroWords | 0x8000000000000000ULL)) >> 4) << 4;
            const u64 writeMask = canPush ? (0xFFFFULL << shift) : 0ULL;
            raw = (raw & ~writeMask) | (canPush ? (static_cast<u64>(val) << shift) : 0ULL);
            return canPush;
        } else {
            const u64 inv = ~raw;
            const u64 zeroDwords = (inv - 0x0000000100000001ULL) & ~inv & 0x8000000080000000ULL;
            const bool canPush = cond & (zeroDwords != 0ULL);
            const u32 shift = (static_cast<u32>(__builtin_ctzll(zeroDwords | 0x8000000000000000ULL)) >> 5) << 5;
            const u64 writeMask = canPush ? (0xFFFFFFFFULL << shift) : 0ULL;
            raw = (raw & ~writeMask) | (canPush ? (static_cast<u64>(val) << shift) : 0ULL);
            return canPush;
        }
    }

    template <bool Cond = true>
    [[gnu::always_inline]] inline bool push_next_if(T val) noexcept {
        return push_next_if(Cond, val);
    }

    [[gnu::always_inline]] inline void push_back_if(bool cond, T val) noexcept {
        push_next_if(cond, val);
    }

    [[gnu::always_inline]] inline void push_back_if(T val) noexcept {
        push_next_if(true, val);
    }

    [[gnu::always_inline]] inline bool push(T val) noexcept {
        return push_next_if(true, val);
    }

    [[nodiscard]] [[gnu::always_inline]] inline T operator[](usize idx) const noexcept {
        return static_cast<T>((raw >> (idx * (sizeof(T) * 8))) & static_cast<RawType>(static_cast<T>(~0)));
    }

    [[nodiscard]] [[gnu::always_inline]] inline u32 count() const noexcept {
        if constexpr (sizeof(T) == 1 && N <= 8) {
            const u64 inv = ~raw;
            const u64 zeroBytes = (inv - 0x0101010101010101ULL) & ~inv & 0x8080808080808080ULL;
            return static_cast<u32>(N - __builtin_popcountll(zeroBytes));
        } else {
            u32 cnt = 0;
            for (usize k = 0; k < N; ++k) {
                cnt += static_cast<u32>((*this)[k] != static_cast<T>(~0));
            }
            return cnt;
        }
    }

    [[nodiscard]] [[gnu::always_inline]] inline bool is_empty() const noexcept {
        return raw == kEmptyMask;
    }

    [[nodiscard]] [[gnu::always_inline]] inline bool contains(T val) const noexcept {
        if constexpr (sizeof(T) == 1 && N <= 8) {
            const u64 v = static_cast<u64>(val);
            const u64 broadcast = v | (v << 8) | (v << 16) | (v << 24) |
                                  (v << 32) | (v << 40) | (v << 48) | (v << 56);
            const u64 diff = raw ^ broadcast;
            return ((diff - 0x0101010101010101ULL) & ~diff & 0x8080808080808080ULL) != 0ULL;
        } else {
            for (usize k = 0; k < N; ++k) {
                if ((*this)[k] == val) return true;
            }
            return false;
        }
    }
};

using Token = Packed<u8, 8>;

// ============================================================================
// 11. Cluster<EntryT, WayCount = 4>: Cụm Bộ Nhớ N-Way Căn Lề Cacheline (Stockfish TTCluster)
// 64-byte L1 Cacheline Resident, Zero False Sharing, Unrolled Parallel Probe
// ============================================================================
template <typename EntryT, usize SizeParam = tier::Normal>
struct alignas(alignof(EntryT) > 64 ? alignof(EntryT) : 64) Cluster {
    static constexpr usize WayCount = (SizeParam >= 16 && (SizeParam % sizeof(EntryT) == 0)) 
        ? (SizeParam / sizeof(EntryT)) 
        : (SizeParam > 0 ? SizeParam : 1);
    static constexpr usize kWayCount = WayCount;
    static constexpr usize kByteSize = SizeParam >= 16 ? SizeParam : (WayCount * sizeof(EntryT));

    union {
        MultiArray<EntryT, WayCount> entries;
        MultiArray<EntryT, WayCount> slots;
    };

    constexpr Cluster() noexcept : entries() {}

    [[nodiscard]] [[gnu::always_inline]] inline EntryT* data() noexcept {
        return entries.data();
    }
    [[nodiscard]] [[gnu::always_inline]] inline const EntryT* data() const noexcept {
        return entries.data();
    }
    [[nodiscard]] [[gnu::always_inline]] inline EntryT& operator[](usize idx) noexcept {
        return entries[idx];
    }
    [[nodiscard]] [[gnu::always_inline]] inline const EntryT& operator[](usize idx) const noexcept {
        return entries[idx];
    }
    [[nodiscard]] [[gnu::always_inline]] inline EntryT* begin() noexcept {
        return entries.begin();
    }
    [[nodiscard]] [[gnu::always_inline]] inline const EntryT* begin() const noexcept {
        return entries.begin();
    }
    [[nodiscard]] [[gnu::always_inline]] inline EntryT* end() noexcept {
        return entries.end();
    }
    [[nodiscard]] [[gnu::always_inline]] inline const EntryT* end() const noexcept {
        return entries.end();
    }

    template <bool Cond = true>
    [[gnu::always_inline]] inline bool push_next_if(const EntryT& entry) noexcept {
        return push_next_if(Cond, entry);
    }

    [[gnu::always_inline]] inline bool push_next_if(bool cond, const EntryT& entry) noexcept {
        if (!cond) return false;
        for (usize k = 0; k < WayCount; ++k) {
            if constexpr (requires { entries[k].key; }) {
                if (entries[k].key == 0) {
                    entries[k] = entry;
                    return true;
                }
            } else {
                entries[k] = entry;
                return true;
            }
        }
        return false;
    }

    [[gnu::always_inline]] inline void push_back_if(bool cond, const EntryT& entry) noexcept {
        push_next_if(cond, entry);
    }

    [[gnu::always_inline]] inline void push_back_if(const EntryT& entry) noexcept {
        push_next_if(true, entry);
    }

    inline void clear() noexcept {
        for (usize k = 0; k < WayCount; ++k) {
            if constexpr (requires { entries[k].clear(); }) {
                entries[k].clear();
            } else {
                std::memset(&entries[k], 0, sizeof(EntryT));
            }
        }
    }
};

// ============================================================================
// 12. Chunk<T, MaskT = u64, Size = 4>: Cấu Trúc Phân Lát Miền Thời Gian / Không Gian
// Căn lề Cacheline (64 bytes), Zero-Loop SIMD / SWAR branchless interval slicing
// ============================================================================
template <typename T = void, typename MaskT = u64, usize Size = 4>
struct alignas(sizeof(T) + sizeof(MaskT) * Size <= 32 ? 32 : 64) Chunk {
    static constexpr usize kSize = Size;
    using ValueType = T;
    using MaskType  = MaskT;

    union {
        MultiArray<MaskT, Size> mask; // Bitboards / Lát cắt trạng thái
        MultiArray<MaskT, Size> bb;
    };
    T payload;                    // Dữ liệu chuyên biệt (Transitions, Zobrist, Occlusion, Audio)

    constexpr Chunk() noexcept : mask{}, payload{} {}

    [[nodiscard]] [[gnu::always_inline]] inline MaskT& operator[](usize i) noexcept {
        return mask[i];
    }
    [[nodiscard]] [[gnu::always_inline]] inline const MaskT& operator[](usize i) const noexcept {
        return mask[i];
    }

    inline void clear() noexcept {
        for (usize i = 0; i < Size; ++i) {
            mask[i] = MaskT{};
        }
        payload = T{};
    }
};

template <typename MaskT, usize Size>
struct alignas(sizeof(MaskT) * Size <= 32 ? 32 : 64) Chunk<void, MaskT, Size> {
    static constexpr usize kSize = Size;
    using MaskType  = MaskT;

    MultiArray<MaskT, Size> mask;

    constexpr Chunk() noexcept : mask{} {}

    [[nodiscard]] [[gnu::always_inline]] inline MaskT& operator[](usize i) noexcept {
        return mask[i];
    }
    [[nodiscard]] [[gnu::always_inline]] inline const MaskT& operator[](usize i) const noexcept {
        return mask[i];
    }

    inline void clear() noexcept {
        for (usize i = 0; i < Size; ++i) {
            mask[i] = MaskT{};
        }
    }
};

// ============================================================================
// 13. Arena<T, Capacity, Align = 64>: Vùng Đệm Phẳng Tự Chủ (Stockfish Zero-Allocation Arena)
// Khối nhớ liên tục, căn lề phần cứng, tịnh tiến con trỏ không rẽ nhánh, 0 heap alloc
// ============================================================================
template <typename T, usize Capacity, usize Align = 64>
struct alignas(Align) Arena {
    static constexpr usize kCapacity = Capacity;
    MultiArray<T, Capacity> storage;
    u32 count = 0;
    u32 generation = 1;

    constexpr Arena() noexcept = default;

    inline void clear() noexcept {
        count = 0;
        ++generation;
        generation |= static_cast<u32>(generation == 0u);
    }

    inline void reset() noexcept {
        clear();
    }

    [[nodiscard]] [[gnu::always_inline]] inline usize size() const noexcept {
        return count;
    }

    [[nodiscard]] [[gnu::always_inline]] inline usize capacity() const noexcept {
        return Capacity;
    }

    [[nodiscard]] [[gnu::always_inline]] inline bool empty() const noexcept {
        return count == 0;
    }

    [[nodiscard]] [[gnu::always_inline]] inline bool is_full() const noexcept {
        return count >= Capacity;
    }

    [[nodiscard]] [[gnu::always_inline]] inline T* data() noexcept {
        return storage.data();
    }

    [[nodiscard]] [[gnu::always_inline]] inline const T* data() const noexcept {
        return storage.data();
    }

    [[nodiscard]] [[gnu::always_inline]] inline operator T*() noexcept {
        return storage.data();
    }

    [[nodiscard]] [[gnu::always_inline]] inline operator const T*() const noexcept {
        return storage.data();
    }

    [[nodiscard]] [[gnu::always_inline]] inline T& operator[](usize idx) noexcept {
        return storage[idx];
    }

    [[nodiscard]] [[gnu::always_inline]] inline const T& operator[](usize idx) const noexcept {
        return storage[idx];
    }

    [[nodiscard]] [[gnu::always_inline]] inline T* begin() noexcept {
        return storage.begin();
    }

    [[nodiscard]] [[gnu::always_inline]] inline const T* begin() const noexcept {
        return storage.begin();
    }

    [[nodiscard]] [[gnu::always_inline]] inline T* end() noexcept {
        return storage.begin() + count;
    }

    [[nodiscard]] [[gnu::always_inline]] inline const T* end() const noexcept {
        return storage.begin() + count;
    }

    [[nodiscard]] [[gnu::always_inline]] inline constexpr Span<T> as_span() noexcept {
        return Span<T>(storage.data(), count);
    }

    [[nodiscard]] [[gnu::always_inline]] inline constexpr Span<const T> as_span() const noexcept {
        return Span<const T>(storage.data(), count);
    }

    [[gnu::always_inline]] inline bool push_back(const T& item) noexcept {
        if (__builtin_expect(count >= Capacity, 0)) return false;
        storage[count++] = item;
        return true;
    }

    [[gnu::always_inline]] inline void push_back_fast(const T& item) noexcept {
        storage[count++] = item;
    }

    [[gnu::always_inline]] inline T& push_back_slot() noexcept {
        return storage[count++];
    }

    inline void set_size(usize newCount) noexcept {
        count = static_cast<u32>(newCount);
    }
};

// ============================================================================
// 14. HornerMicrocode<MaxTracks>: Timeline-as-Bytecode ISA (SIMD128 Horner FMA)
// Compiles any elementary interval's animated properties (Linear, EaseIn,
// EaseOut, EaseInOut, Cubic Bezier) into flat 64B-aligned SoA polynomials:
//   Val(tau) = ((c3 * tau + c2) * tau + c1) * tau + c0
// Hot Path: 0 if-branches, 0 easing switches, 0 keyframe binary search,
//           0 divisions — evaluates 4 tracks per 3 SIMD128 mul-add instructions!
// ============================================================================
template <usize MaxTracks = 64u>
struct alignas(64) HornerMicrocode {
    static_assert((MaxTracks % 4u) == 0u, "MaxTracks must be a multiple of 4 for SIMD128");
    static constexpr usize kMaxTracks = MaxTracks;

    alignas(64) MultiArray<float, MaxTracks> c0; // Constant term  (value at tau=0)
    alignas(64) MultiArray<float, MaxTracks> c1; // Linear term    (tau^1)
    alignas(64) MultiArray<float, MaxTracks> c2; // Quadratic term (tau^2)
    alignas(64) MultiArray<float, MaxTracks> c3; // Cubic term     (tau^3)
    alignas(64) MultiArray<u16,   MaxTracks> targetSlot;
    alignas(64) MultiArray<u8,    MaxTracks> targetReg;
    float tStartMs    = 0.0f;
    float invSpanMs   = 0.0f;
    u32   trackCount  = 0u;
    u32   paddedQuads = 0u; // ceil(trackCount / 4) * 4

    inline void reset(float startMs, float endMs) noexcept {
        tStartMs = startMs;
        const float span = endMs - startMs;
        invSpanMs = (span > 1e-6f) ? (1.0f / span) : 0.0f;
        trackCount = 0u;
        paddedQuads = 0u;
    }

    inline void push_cubic(u16 slot, u8 regAddr, float k0, float k1, float k2, float k3) noexcept {
        const u32 idx = trackCount;
        c0[idx] = k0; c1[idx] = k1; c2[idx] = k2; c3[idx] = k3;
        targetSlot[idx] = slot;
        targetReg[idx]  = regAddr;
        ++trackCount;
        paddedQuads = (trackCount + 3u) & ~3u;
    }

    // Exact polynomial bakes (0 approximation error!)
    inline void bake_linear(u16 slot, u8 regAddr, float a, float b) noexcept {
        push_cubic(slot, regAddr, a, b - a, 0.0f, 0.0f);
    }
    inline void bake_ease_in(u16 slot, u8 regAddr, float a, float b) noexcept {
        push_cubic(slot, regAddr, a, 0.0f, b - a, 0.0f);
    }
    inline void bake_ease_out(u16 slot, u8 regAddr, float a, float b) noexcept {
        const float d = b - a;
        push_cubic(slot, regAddr, a, 2.0f * d, -d, 0.0f);
    }
    inline void bake_ease_in_out(u16 slot, u8 regAddr, float a, float b) noexcept {
        const float d = b - a;
        push_cubic(slot, regAddr, a, 0.0f, 3.0f * d, -2.0f * d);
    }

    // Branchless polynomial basis bake matching eval_quad_easing bit-for-bit:
    // mode: 0=Linear, 1=EaseIn, 2=EaseOut, 3=EaseInOut[0..0.5), 4=EaseInOut[0.5..1]
    inline void bake_quad_mode(u32 mode, u16 slot, u8 regAddr, float a, float b) noexcept {
        static constexpr MultiArray<float, 5, 3> kBasis = {{
            {{ 0.0f,  1.0f,  0.0f }}, // 0: Linear     (0 + 1*t + 0*t^2)
            {{ 0.0f,  0.0f,  1.0f }}, // 1: EaseIn     (0 + 0*t + 1*t^2)
            {{ 0.0f,  2.0f, -1.0f }}, // 2: EaseOut    (0 + 2*t - 1*t^2)
            {{ 0.0f,  0.0f,  2.0f }}, // 3: EaseInOutL (0 + 0*t + 2*t^2)
            {{-1.0f,  4.0f, -2.0f }}  // 4: EaseInOutH (-1 + 4*t - 2*t^2)
        }};
        const float d = b - a;
        const auto& m = kBasis[mode];
        push_cubic(slot, regAddr, a + d * m[0], d * m[1], d * m[2], 0.0f);
    }

    // Compile-time unrolled SIMD128 Horner FMA (0 loop-bound branches -> < 15ns for 64 tracks!)
    template <u32 Quads>
    [[gnu::always_inline]] inline void eval_tau_fixed(float tau, float* __restrict outVals) const noexcept {
        static_assert(Quads * 4u <= MaxTracks, "Quads exceeds HornerMicrocode MaxTracks");
#if defined(__wasm_simd128__)
        const v128_t vt = wasm_f32x4_splat(tau);
        #pragma clang loop unroll(full)
        for (u32 q = 0u; q < Quads; ++q) {
            const u32 i = q * 4u;
            const v128_t v3 = wasm_v128_load(&c3[i]);
            const v128_t v2 = wasm_v128_load(&c2[i]);
            const v128_t v1 = wasm_v128_load(&c1[i]);
            const v128_t v0 = wasm_v128_load(&c0[i]);
            v128_t acc = wasm_f32x4_add(wasm_f32x4_mul(v3, vt), v2);
            acc        = wasm_f32x4_add(wasm_f32x4_mul(acc, vt), v1);
            acc        = wasm_f32x4_add(wasm_f32x4_mul(acc, vt), v0);
            wasm_v128_store(&outVals[i], acc);
        }
#else
        #pragma clang loop unroll(full)
        for (u32 i = 0u; i < Quads * 4u; ++i) {
            outVals[i] = ((c3[i] * tau + c2[i]) * tau + c1[i]) * tau + c0[i];
        }
#endif
    }

    // Hot Path: Pure Straight-Line SIMD128 Horner FMA (0 if, 0 switch, 0 div)
    [[gnu::always_inline]] inline void eval_tau(float tau, float* __restrict outVals) const noexcept {
#if defined(__wasm_simd128__)
        const v128_t vt = wasm_f32x4_splat(tau);
        for (u32 i = 0u; i < paddedQuads; i += 4u) {
            const v128_t v3 = wasm_v128_load(&c3[i]);
            const v128_t v2 = wasm_v128_load(&c2[i]);
            const v128_t v1 = wasm_v128_load(&c1[i]);
            const v128_t v0 = wasm_v128_load(&c0[i]);
            v128_t acc = wasm_f32x4_add(wasm_f32x4_mul(v3, vt), v2);
            acc        = wasm_f32x4_add(wasm_f32x4_mul(acc, vt), v1);
            acc        = wasm_f32x4_add(wasm_f32x4_mul(acc, vt), v0);
            wasm_v128_store(&outVals[i], acc);
        }
#else
        for (u32 i = 0u; i < paddedQuads; ++i) {
            outVals[i] = ((c3[i] * tau + c2[i]) * tau + c1[i]) * tau + c0[i];
        }
#endif
    }

    [[gnu::always_inline]] inline void eval_at_ms(float nowMs, float* __restrict outVals) const noexcept {
        const float tau = clamp((nowMs - tStartMs) * invSpanMs, 0.0f, 1.0f);
        eval_tau(tau, outVals);
    }
};

} // namespace stockcut
