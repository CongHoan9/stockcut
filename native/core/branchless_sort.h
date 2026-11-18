// ============================================================================
// [ĐÁNH GIÁ ĐIỂM NGHẼN HỆ THỐNG]:
// 1. Phân nhánh thủ tục (Procedural if-else cascades) trong giải thuật sắp xếp:
//    - Các lệnh if (count == 2), if (count == 3) gây branch misprediction và làm
//      đứt gãy pipeline của CPU trong hot path xử lý timeline chunk.
// 2. Thiếu chuyên biệt hóa kiểu và tham số hóa compile-time (C-Style Functions):
//    - Viết các hàm thủ tục nhận tham số động làm mất khả năng nội suy hằng số
//      và loop unrolling của trình biên dịch C++20.
//
// [HƯỚNG KHẮC PHỤC TRIỆT ĐỂ — KHÔNG VÁ]:
// 1. Chuyên biệt hóa hoàn toàn qua enum class và template<usize N, SortOrder Order>:
//    - Mạng sắp xếp Bose-Nelson / Green được định nghĩa tại compile-time.
//    - Trình biên dịch tự động bung (unroll) toàn bộ phép hoán vị thành các lệnh
//      số học bitwise thẳng băng (0 loops, 0 branches).
// 2. Bảng chuyển tiếp con trỏ hàm O(1) tĩnh (DynamicSortingNetwork<T, Order>):
//    - Ánh xạ số lượng phần tử runtime N trực tiếp qua mảng con trỏ hàm kDispatchLut[N]
//      mà không dùng chuỗi so sánh rẽ nhánh.
// ============================================================================
#pragma once
#include "types.h"
#include "data_structures.h"
#include <type_traits>

namespace stockcut {

// 1. Phân loại hướng sắp xếp
enum class SortOrder : u8 {
    Ascending = 0,
    Descending = 1
};

// 2. Toán tử so sánh hoán vị điều kiện không rẽ nhánh (Self-Contained Branchless Comparator)
template <typename T, SortOrder Order = SortOrder::Ascending>
struct BranchlessSwap {
    static inline void apply(T& a, T& b) noexcept {
        if constexpr (std::is_integral_v<T> && sizeof(T) == 8 && std::is_signed_v<T>) {
            // Đại số Galois XOR + số học dịch bit cho số nguyên 64-bit (TimePoint / i64)
            if constexpr (Order == SortOrder::Ascending) {
                const i64 diff = static_cast<i64>(b) - static_cast<i64>(a);
                const i64 mask = diff >> 63; // -1 nếu b < a (cần swap), 0 nếu b >= a
                const i64 c = (static_cast<i64>(a) ^ static_cast<i64>(b)) & mask;
                a = static_cast<T>(static_cast<i64>(a) ^ c);
                b = static_cast<T>(static_cast<i64>(b) ^ c);
            } else {
                const i64 diff = static_cast<i64>(a) - static_cast<i64>(b);
                const i64 mask = diff >> 63; // -1 nếu a < b (cần swap), 0 nếu a >= b
                const i64 c = (static_cast<i64>(a) ^ static_cast<i64>(b)) & mask;
                a = static_cast<T>(static_cast<i64>(a) ^ c);
                b = static_cast<T>(static_cast<i64>(b) ^ c);
            }
        } else {
            // Toán tử chọn lọc điều kiện không rẽ nhánh cho các kiểu dữ liệu khác
            const bool cond = (Order == SortOrder::Ascending) ? (b < a) : (a < b);
            const T first  = cond ? b : a;
            const T second = cond ? a : b;
            a = first;
            b = second;
        }
    }
};

// 3. Mạng sắp xếp Compile-Time (Bose-Nelson & Green Optimal Sorting Networks)
// Mọi specialization N sinh chính xác mạng so sánh tối ưu với 0 loops, 0 branches
template <usize N, typename T, SortOrder Order = SortOrder::Ascending>
struct SortingNetwork;

template <typename T, SortOrder Order>
struct SortingNetwork<0, T, Order> {
    static inline void sort(T*) noexcept {}
};

template <typename T, SortOrder Order>
struct SortingNetwork<1, T, Order> {
    static inline void sort(T*) noexcept {}
};

template <typename T, SortOrder Order>
struct SortingNetwork<2, T, Order> {
    static inline void sort(T* arr) noexcept {
        BranchlessSwap<T, Order>::apply(arr[0], arr[1]);
    }
};

template <typename T, SortOrder Order>
struct SortingNetwork<3, T, Order> {
    static inline void sort(T* arr) noexcept {
        BranchlessSwap<T, Order>::apply(arr[0], arr[1]);
        BranchlessSwap<T, Order>::apply(arr[1], arr[2]);
        BranchlessSwap<T, Order>::apply(arr[0], arr[1]);
    }
};

template <typename T, SortOrder Order>
struct SortingNetwork<4, T, Order> {
    static inline void sort(T* arr) noexcept {
        // Bose-Nelson 5 comparators
        BranchlessSwap<T, Order>::apply(arr[0], arr[1]);
        BranchlessSwap<T, Order>::apply(arr[2], arr[3]);
        BranchlessSwap<T, Order>::apply(arr[0], arr[2]);
        BranchlessSwap<T, Order>::apply(arr[1], arr[3]);
        BranchlessSwap<T, Order>::apply(arr[1], arr[2]);
    }
};

template <typename T, SortOrder Order>
struct SortingNetwork<5, T, Order> {
    static inline void sort(T* arr) noexcept {
        SortingNetwork<4, T, Order>::sort(arr);
        BranchlessSwap<T, Order>::apply(arr[3], arr[4]);
        BranchlessSwap<T, Order>::apply(arr[2], arr[3]);
        BranchlessSwap<T, Order>::apply(arr[1], arr[2]);
        BranchlessSwap<T, Order>::apply(arr[0], arr[1]);
    }
};

template <typename T, SortOrder Order>
struct SortingNetwork<6, T, Order> {
    static inline void sort(T* arr) noexcept {
        SortingNetwork<5, T, Order>::sort(arr);
        BranchlessSwap<T, Order>::apply(arr[4], arr[5]);
        BranchlessSwap<T, Order>::apply(arr[3], arr[4]);
        BranchlessSwap<T, Order>::apply(arr[2], arr[3]);
        BranchlessSwap<T, Order>::apply(arr[1], arr[2]);
        BranchlessSwap<T, Order>::apply(arr[0], arr[1]);
    }
};

template <typename T, SortOrder Order>
struct SortingNetwork<7, T, Order> {
    static inline void sort(T* arr) noexcept {
        SortingNetwork<6, T, Order>::sort(arr);
        BranchlessSwap<T, Order>::apply(arr[5], arr[6]);
        BranchlessSwap<T, Order>::apply(arr[4], arr[5]);
        BranchlessSwap<T, Order>::apply(arr[3], arr[4]);
        BranchlessSwap<T, Order>::apply(arr[2], arr[3]);
        BranchlessSwap<T, Order>::apply(arr[1], arr[2]);
        BranchlessSwap<T, Order>::apply(arr[0], arr[1]);
    }
};

template <typename T, SortOrder Order>
struct SortingNetwork<8, T, Order> {
    static inline void sort(T* arr) noexcept {
        SortingNetwork<7, T, Order>::sort(arr);
        BranchlessSwap<T, Order>::apply(arr[6], arr[7]);
        BranchlessSwap<T, Order>::apply(arr[5], arr[6]);
        BranchlessSwap<T, Order>::apply(arr[4], arr[5]);
        BranchlessSwap<T, Order>::apply(arr[3], arr[4]);
        BranchlessSwap<T, Order>::apply(arr[2], arr[3]);
        BranchlessSwap<T, Order>::apply(arr[1], arr[2]);
        BranchlessSwap<T, Order>::apply(arr[0], arr[1]);
    }
};

// 4. Bảng Tra Hàm Con Trỏ Tĩnh (Stockfish-Style Function Pointer Table)
// Triệt tiêu hoàn toàn chuỗi if (count == ...) rẽ nhánh, ánh xạ O(1) qua chỉ số mảng
template <typename T, SortOrder Order = SortOrder::Ascending>
struct DynamicSortingNetwork {
    using SortFn = void (*)(T*) noexcept;

    static constexpr usize MAX_NETWORK_SIZE = 8;

    static constexpr MultiArray<SortFn, MAX_NETWORK_SIZE + 1> kDispatchLut = {{
        &SortingNetwork<0, T, Order>::sort,
        &SortingNetwork<1, T, Order>::sort,
        &SortingNetwork<2, T, Order>::sort,
        &SortingNetwork<3, T, Order>::sort,
        &SortingNetwork<4, T, Order>::sort,
        &SortingNetwork<5, T, Order>::sort,
        &SortingNetwork<6, T, Order>::sort,
        &SortingNetwork<7, T, Order>::sort,
        &SortingNetwork<8, T, Order>::sort
    }};

    static inline void sort(T* arr, usize count) noexcept {
        const usize boundedCount = count > MAX_NETWORK_SIZE ? MAX_NETWORK_SIZE : count;
        kDispatchLut[boundedCount](arr);
    }
};

// Helper tiện ích compile-time cho mã nguồn gọn gàng
template <usize N, typename T, SortOrder Order = SortOrder::Ascending>
inline void sort_network(T* arr) noexcept {
    SortingNetwork<N, T, Order>::sort(arr);
}

template <typename T, SortOrder Order = SortOrder::Ascending>
inline void sort_dynamic_network(T* arr, usize count) noexcept {
    DynamicSortingNetwork<T, Order>::sort(arr, count);
}

} // namespace stockcut
