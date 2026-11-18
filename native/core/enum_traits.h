// ============================================================================
// native/core/enum_traits.h
// STOCKFISH-STYLE COMPILE-TIME ENUM REFLECTION & ZERO-OVERHEAD GROUPING
// ----------------------------------------------------------------------------
// Tự động phân loại, lọc và trích xuất parameter pack cho bất kỳ enum nào
// tại compile-time với chi phí runtime = 0 byte & 0 cycles!
// Không bao giờ phải sửa mảng thủ công: Tự động gom nhóm theo predicate/category.
// ============================================================================
#pragma once

#include "types.h"
#include "data_structures.h"
#include <array>
#include <utility>

namespace stockcut {

// ============================================================================
// 1. Generic Enum Pack: Đại diện cho một tập hợp compile-time các giá trị enum
// ============================================================================
template <typename EnumT, EnumT... Values>
struct EnumPack {
    using enum_type = EnumT;
    static constexpr usize count = sizeof...(Values);
    static constexpr MultiArray<EnumT, (count > 0 ? count : 1)> values = {{ Values... }};

    [[nodiscard]] static constexpr bool contains(EnumT val) noexcept {
        return (((val == Values) ? true : false) || ...);
    }

    template <typename Fn>
    static constexpr void for_each(Fn&& fn) noexcept {
        (fn(Values), ...);
    }
};

// ============================================================================
// 2. Generic Tagged Enum Group: Nhóm enum có gắn nhãn Category/Tag
// ============================================================================
template <typename TagT, TagT Tag, typename EnumT, EnumT... Values>
struct TaggedEnumGroup {
    using tag_type = TagT;
    using enum_type = EnumT;
    static constexpr TagT group_tag = Tag;
    static constexpr usize count = sizeof...(Values);
    static constexpr MultiArray<EnumT, (count > 0 ? count : 1)> values = {{ Values... }};

    [[nodiscard]] static constexpr bool contains(EnumT val) noexcept {
        return (((val == Values) ? true : false) || ...);
    }

    template <typename Fn>
    static constexpr void for_each(Fn&& fn) noexcept {
        (fn(Values), ...);
    }
};

// ============================================================================
// 3. Universal Enum Filter: Quét qua dải enum [0..EnumCount) với Predicate bất kỳ
// ============================================================================
template <typename EnumT, usize EnumCount, auto Predicate>
struct AutoEnumFilter {
    template <usize... Is>
    static constexpr usize count_matches(std::index_sequence<Is...>) noexcept {
        return (((Predicate(static_cast<EnumT>(Is))) ? 1 : 0) + ...);
    }

    static constexpr usize matched_count = count_matches(std::make_index_sequence<EnumCount>{});
    static constexpr usize array_capacity = (matched_count > 0 ? matched_count : 1);

    template <usize... Is>
    static constexpr auto extract_matches(std::index_sequence<Is...>) noexcept {
        std::array<EnumT, array_capacity> arr{};
        usize idx = 0;
        ((Predicate(static_cast<EnumT>(Is)) ? (arr[idx++] = static_cast<EnumT>(Is)), 0 : 0), ...);
        return arr;
    }

    static constexpr auto matched_array = extract_matches(std::make_index_sequence<EnumCount>{});

    // Unpack thành EnumPack<EnumT, Values...>
    template <typename Seq>
    struct MakePack;

    template <usize... Is>
    struct MakePack<std::index_sequence<Is...>> {
        using type = EnumPack<EnumT, matched_array[Is]...>;
    };

    using pack = typename MakePack<std::make_index_sequence<matched_count>>::type;

    // Unpack thành template tùy ý TargetTemplate<Values...>
    template <template <EnumT...> class TargetTemplate, typename Seq>
    struct MakeCustomPack;

    template <template <EnumT...> class TargetTemplate, usize... Is>
    struct MakeCustomPack<TargetTemplate, std::index_sequence<Is...>> {
        using type = TargetTemplate<matched_array[Is]...>;
    };

    template <template <EnumT...> class TargetTemplate>
    using instantiate = typename MakeCustomPack<TargetTemplate, std::make_index_sequence<matched_count>>::type;

    // Unpack thành template gắn Tag: TargetTemplate<Tag, Values...>
    template <template <auto, EnumT...> class TargetTemplate, auto Tag, typename Seq>
    struct MakeTaggedPack;

    template <template <auto, EnumT...> class TargetTemplate, auto Tag, usize... Is>
    struct MakeTaggedPack<TargetTemplate, Tag, std::index_sequence<Is...>> {
        using type = TargetTemplate<Tag, matched_array[Is]...>;
    };

    template <template <auto, EnumT...> class TargetTemplate, auto Tag>
    using instantiate_tagged = typename MakeTaggedPack<TargetTemplate, Tag, std::make_index_sequence<matched_count>>::type;
};

// 4. Tiện ích tạo Group tự động từ Predicate
template <typename EnumT, usize EnumCount, auto Predicate>
using AutoFilteredEnumPack = typename AutoEnumFilter<EnumT, EnumCount, Predicate>::pack;

} // namespace stockcut
