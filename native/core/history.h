// ============================================================================
// CoreEngine - History, Heuristic Learning & Gravity Envelopes (Stockfish history.h)
// ----------------------------------------------------------------------------
// [STOCKFISH DATA-STRUCTURE-CENTRIC & AUTONOMOUS STATS]:
// - RelaxedAtomic<T>: Truy cập nguyên tử không khóa std::memory_order_relaxed.
// - StatsEntry<T, D, Shared>: Cấu trúc số liệu tự chủ với toán tử trọng lực <<.
// - BoundedGravityReal: Trọng lực tự co trên số thực với bound D bất biến.
// - UnitGravityEnvelope & ReciprocalFadeEnvelope: Bao hàm suy giảm trơn 0-branch.
// - DynStats<T, SizeMultiplier>: Mảng động căn lề Large Page (Stockfish history.h).
// ============================================================================
#pragma once
#include <atomic>
#include <cmath>
#include <type_traits>
#include "memory.h"

namespace stockcut {

// ----------------------------------------------------------------------------
// 1. RelaxedAtomic<T>: Truy cập nguyên tử không khóa std::memory_order_relaxed
// ----------------------------------------------------------------------------
template<typename T>
struct RelaxedAtomic {
    RelaxedAtomic() noexcept = default;
    constexpr explicit RelaxedAtomic(T init) noexcept : v(init) {}
    RelaxedAtomic(const RelaxedAtomic& other) noexcept : v(other.load()) {}
    RelaxedAtomic& operator=(const RelaxedAtomic& other) noexcept {
        store(other.load());
        return *this;
    }

    [[nodiscard]] inline T load() const noexcept { return v.load(std::memory_order_relaxed); }
    inline void store(T val) noexcept { v.store(val, std::memory_order_relaxed); }
    inline operator T() const noexcept { return load(); }
    inline RelaxedAtomic& operator=(T val) noexcept { store(val); return *this; }

    inline T fetch_add(T val) noexcept { return v.fetch_add(val, std::memory_order_relaxed); }
    inline T fetch_sub(T val) noexcept { return v.fetch_sub(val, std::memory_order_relaxed); }
    inline T operator++() noexcept { return fetch_add(1) + 1; }
    inline T operator--() noexcept { return fetch_sub(1) - 1; }
    inline T operator++(int) noexcept { return fetch_add(1); }
    inline T operator--(int) noexcept { return fetch_sub(1); }

private:
    std::atomic<T> v{T{}};
};

// ----------------------------------------------------------------------------
// 2. StatsEntry<T, D, Shared>: Cấu trúc số liệu tự chủ với toán tử trọng lực <<
// ----------------------------------------------------------------------------
template<typename T, int D, bool Shared = false>
struct StatsEntry {
private:
    std::conditional_t<Shared, RelaxedAtomic<T>, T> entry_{};
public:
    constexpr StatsEntry() noexcept = default;
    constexpr StatsEntry(T initVal) noexcept : entry_(initVal) {}
    inline void operator=(const T& v) noexcept { entry_ = v; }
    inline operator T() const noexcept { return entry_; }

    inline void operator<<(int delta) noexcept {
        const int clamped = (delta < -D) ? -D : (delta > D ? D : delta);
        const int absClamped = clamped >= 0 ? clamped : -clamped;
        const T val = static_cast<T>(entry_);
        entry_ = static_cast<T>(val + clamped - val * absClamped / D);
    }
};

// ----------------------------------------------------------------------------
// 3. BoundedGravityReal: Trọng lực tự co trên số thực với bound D bất biến
// ----------------------------------------------------------------------------
template<typename Real = float>
struct BoundedGravityReal {
    Real value = Real(0);
    Real invD  = Real(1);

    constexpr BoundedGravityReal() noexcept = default;
    constexpr explicit BoundedGravityReal(Real boundD, Real initVal = Real(0)) noexcept
        : value(initVal), invD(boundD > Real(0) ? Real(1) / boundD : Real(0)) {}

    inline void operator<<(Real delta) noexcept {
        const Real boundD = (invD > Real(0)) ? (Real(1) / invD) : Real(1);
        const Real d = (delta < -boundD) ? -boundD : (delta > boundD ? boundD : delta);
        const Real absDelta = (d >= Real(0)) ? d : -d;
        value += d - value * absDelta * invD;
    }

    inline void update_with_preclamped(Real delta, Real absDelta) noexcept {
        value += delta - value * absDelta * invD;
    }

    constexpr operator Real() const noexcept { return value; }
};

// ----------------------------------------------------------------------------
// 4. UnitGravityEnvelope: Bao hàm trọng lực đơn vị [-1, 1]
// ----------------------------------------------------------------------------
struct UnitGravityEnvelope {
    float value = 0.0f;

    constexpr UnitGravityEnvelope() noexcept = default;
    constexpr explicit UnitGravityEnvelope(float initVal) noexcept : value(initVal) {}

    inline void operator<<(float delta) noexcept {
        const float d = clamp(delta, -1.0f, 1.0f);
        const float target = static_cast<float>(d > 0.0f);
        const float absD = abs_val(d);
        value += absD * (target - value);
    }

    constexpr operator float() const noexcept { return value; }
};

// ----------------------------------------------------------------------------
// 5. ReciprocalFadeEnvelope: Đường cong mờ dần đa thức Hermite
// ----------------------------------------------------------------------------
struct ReciprocalFadeEnvelope {
    float invFade = 0.0f;
    float bias    = 1.0f;

    static constexpr ReciprocalFadeEnvelope bake(float fadeMs) noexcept {
        const bool hasFade = (fadeMs > 0.0f);
        return {
            hasFade ? (1.0f / fadeMs) : 0.0f,
            hasFade ? 0.0f : 1.0f
        };
    }

    [[nodiscard]] inline float evaluate(float elapsedMs, float remainingMs) const noexcept {
        const float minEdge = min(elapsedMs, remainingMs);
        const float rawU = bias + minEdge * invFade;
        const float u = clamp(rawU, 0.0f, 1.0f);
        return u * u * (3.0f - 2.0f * u);
    }

    [[nodiscard]] inline float evaluate_f64(double elapsedMs, double remainingMs, float fadeMs) const noexcept {
        const double b = static_cast<double>(bias);
        const double active = 1.0 - b;
        const double safeFade = static_cast<double>(fadeMs) * active + b;
        const double minEdge = min(elapsedMs, remainingMs) * active;
        const double rawF = b + minEdge / safeFade;
        const double fNorm = rawF < 0.0 ? 0.0 : (rawF > 1.0 ? 1.0 : rawF);
        return static_cast<float>(fNorm * fNorm * (3.0 - 2.0 * fNorm));
    }
};

// ----------------------------------------------------------------------------
// 6. DynStats<T, SizeMultiplier>: Mảng động căn lề Large Page (Stockfish history.h)
// ----------------------------------------------------------------------------
template<typename T, int SizeMultiplier = 1>
struct DynStats {
    DynStats() : size_(0) {}
    explicit DynStats(usize s) {
        size_ = s * SizeMultiplier;
        data_ = make_unique_large_page<T[]>(size_);
    }
    void allocate(usize s) {
        size_ = s * SizeMultiplier;
        data_ = make_unique_large_page<T[]>(size_);
    }
    [[nodiscard]] usize size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0 || !data_; }
    [[nodiscard]] T& operator[](usize index) noexcept {
        return data_.get()[index];
    }
    [[nodiscard]] const T& operator[](usize index) const noexcept {
        return data_.get()[index];
    }
    [[nodiscard]] T* get() noexcept { return data_.get(); }
    [[nodiscard]] const T* get() const noexcept { return data_.get(); }
private:
    usize size_;
    LargePagePtr<T[]> data_;
};

// ----------------------------------------------------------------------------
// 7. RcuDualBank<T>: Lock-Free Read-Copy-Update Dual-Epoch Utility (VideoOS Kernel)
// Zero-mutex, zero-branch acquire/release bank flip with 64B cacheline isolation.
// ----------------------------------------------------------------------------
template <typename T>
struct alignas(64) RcuDualBank {
    alignas(64) MultiArray<T, 2> banks{};
    alignas(64) std::atomic<u32> activeIdx{0u};
    RelaxedAtomic<u64>           epoch{1ULL};

    constexpr RcuDualBank() noexcept = default;

    // Hot Reader Path (60/120/240 FPS): 1 acquire load, 0 locks, 0 branches
    [[nodiscard]] [[gnu::always_inline]] inline const T& read() const noexcept {
        return banks[activeIdx.load(std::memory_order_acquire) & 1u];
    }

    // Writer / Editor Path: direct mutable access to inactive shadow bank
    [[nodiscard]] [[gnu::always_inline]] inline T& staging() noexcept {
        return banks[(activeIdx.load(std::memory_order_relaxed) ^ 1u) & 1u];
    }

    // Copy active state into shadow bank before incremental mutation
    inline T& begin_update_copy() noexcept {
        const u32 cur = activeIdx.load(std::memory_order_relaxed) & 1u;
        const u32 nxt = cur ^ 1u;
        banks[nxt] = banks[cur];
        return banks[nxt];
    }

    // Atomic frame-boundary publication: 1 release store, 0 stalls
    inline u64 publish() noexcept {
        const u32 nxt = (activeIdx.load(std::memory_order_relaxed) ^ 1u) & 1u;
        activeIdx.store(nxt, std::memory_order_release);
        return epoch.fetch_add(1ULL) + 1ULL;
    }

    [[nodiscard]] [[gnu::always_inline]] inline u64 current_epoch() const noexcept {
        return epoch.load();
    }
};

} // namespace stockcut
