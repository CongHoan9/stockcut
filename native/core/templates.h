// ============================================================================
// CoreEngine - Hierarchical Template System (Stockfish Compact Arena + Bitmask)
// ============================================================================
#pragma once

#include "types.h"
#include "memory.h"
#include "data_structures.h"
#include "string_parse.h"
#include "timeline/bitboard.h"
#include "property/property_types.h"
#include "effect/monoid_operator.h"

namespace stockcut {

// Compact 16-byte property payload stored lazily inside TemplateArena::propPool_
struct TemplatePropEntry {
    double numVal = 0.0;
    u32 strOffset = INVALID_U32;
    u16 strLen = 0;
    u8 isNumeric = 0;
    u8 pad = 0;
};
static_assert(sizeof(TemplatePropEntry) == 16, "TemplatePropEntry must be 16 bytes");

class alignas(64) TemplateNode {
public:
    static constexpr u32 MAX_PROPS = 80u;

    MultiArray<char, 64> id;               // KHONG CO {}
    MultiArray<char, 32> type;             // KHONG CO {}
    u32 uid = 0;

    TemplateNode* parent = nullptr;
    Bitboard assignedSlotsBB{0ULL};
    Bitboard allDependentSlotsBB{0ULL};

    Mask<2> definedMask{};
    Mask<2> resolvedMask{};
    MultiArray<u16, MAX_PROPS> definedEntryIdx;  // KHONG CO {}
    MultiArray<u16, MAX_PROPS> resolvedEntryIdx; // KHONG CO {}
    EffectPipeline bakedFxPipeline{};

    template <AnyGroup GroupT>
    inline void set_baked_fx_group(const GroupT& grp) noexcept {
        if constexpr (std::is_same_v<typename GroupT::value_type, FusedUberEffect>) {
            bakedFxPipeline.group = grp;
        }
    }

    [[nodiscard]] inline const EffectPipeline& get_baked_fx_pipeline() const noexcept {
        return bakedFxPipeline;
    }

    [[nodiscard]] inline const EffectGroup& get_baked_fx_group() const noexcept {
        return bakedFxPipeline.group;
    }

    inline void clear() noexcept {
        id[0] = '\0';
        type[0] = '\0';
        uid = 0;
        parent = nullptr;
        assignedSlotsBB = Bitboard(0ULL);
        allDependentSlotsBB = Bitboard(0ULL);
        definedMask.clear();
        resolvedMask.clear();
        bakedFxPipeline.reset();
    }

    template <bool IsNumeric>
    void set_defined_property(prop::PropertyId propId, const char* propName, const char* propVal, double num = 0.0) noexcept;

    void blend_hierarchy() noexcept {
        resolvedMask = definedMask;
        copy_masked_indices(definedMask, definedEntryIdx);

        for (TemplateNode* curr = this->parent; curr; curr = curr->parent) {
            const Mask<2> missingFromCurr = curr->definedMask & (~resolvedMask);
            if (missingFromCurr.any()) {
                copy_masked_indices(missingFromCurr, curr->definedEntryIdx);
                resolvedMask |= missingFromCurr;
            }
            if (!curr->bakedFxPipeline.is_identity()) {
                bakedFxPipeline << curr->bakedFxPipeline;
            }
        }
    }

    void blend_diff(const Mask<2>& diffMask) noexcept {
        if (diffMask.empty()) return;

        diffMask.for_each_active([&](u32 pId) {
            if (pId >= MAX_PROPS) return;
            if (definedMask.test_bit(pId)) {
                resolvedEntryIdx[pId] = definedEntryIdx[pId];
                resolvedMask.set_bit(pId);
                return;
            }
            for (TemplateNode* c = this->parent; c; c = c->parent) {
                if (c->definedMask.test_bit(pId)) {
                    resolvedEntryIdx[pId] = c->definedEntryIdx[pId];
                    resolvedMask.set_bit(pId);
                    return;
                }
            }
            resolvedMask.clear_bit(pId);
            resolvedEntryIdx[pId] = INVALID_U16;
        });
    }

    [[nodiscard]] const TemplatePropEntry* get_resolved(prop::PropertyId propId) const noexcept;
    [[nodiscard]] const char* get_resolved_string(prop::PropertyId propId, const char* fallback = "") const noexcept;
    [[nodiscard]] double get_resolved_number(prop::PropertyId propId, double fallback = 0.0) const noexcept;

private:
    inline void copy_masked_indices(
        const Mask<2>& mask,
        const MultiArray<u16, MAX_PROPS>& src
    ) noexcept {
        mask.for_each_active([&](u32 pId) {
            if (pId < MAX_PROPS) resolvedEntryIdx[pId] = src[pId];
        });
    }
};

class TemplateArena {
public:
    static constexpr u32 MAX_TEMPLATES = 128u;
    static constexpr u32 INITIAL_PROP_CAP = 512u;
    static constexpr u32 INITIAL_STR_CAP  = 16384u;

    static TemplateArena& instance() noexcept;

    void reset() noexcept;
    TemplateNode* allocate_template(const char* name, const char* type = "") noexcept;
    TemplateNode* find_template(const char* name) noexcept;
    const TemplateNode* find_template(const char* name) const noexcept;
    bool link_parent(TemplateNode* child, TemplateNode* parent) noexcept;
    void bake_all_templates() noexcept;
    bool update_template_diff(TemplateNode* target, const Mask<2>& diffMask, Bitboard& outMutatedSlots) noexcept;

    u16 alloc_prop_entry(const char* propVal, bool isNumeric, double numVal) noexcept;
    const TemplatePropEntry* entry_at(u16 idx) const noexcept {
        return (idx < propCount_ && propPool_) ? &propPool_[idx] : nullptr;
    }
    const char* string_at(u32 offset) const noexcept {
        return (offset < strCursor_ && strPool_) ? (strPool_.get() + offset) : "";
    }

    u32 count() const noexcept { return count_; }
    TemplateNode* get(u32 idx) noexcept { return (idx < count_) ? &templates_[idx] : nullptr; }
    const TemplateNode* get(u32 idx) const noexcept { return (idx < count_) ? &templates_[idx] : nullptr; }

private:
    TemplateArena() noexcept { reset(); }
    MultiArray<TemplateNode, MAX_TEMPLATES> templates_; // KHONG CO {} (O(1) reset, allocate_template() clears on claim)
    u32 count_ = 0;
    AlignedPtr<TemplatePropEntry[]> propPool_;
    u32 propCap_ = 0;
    u32 propCount_ = 0;
    AlignedPtr<char[]> strPool_;
    u32 strCap_ = 0;
    u32 strCursor_ = 0;
};

template <bool IsNumeric>
inline void TemplateNode::set_defined_property(prop::PropertyId propId, const char* /*propName*/, const char* propVal, double num) noexcept {
    if (propId >= MAX_PROPS) return;
    const u16 idx = TemplateArena::instance().alloc_prop_entry(propVal, IsNumeric, IsNumeric ? num : 0.0);
    if (idx == INVALID_U16) return;
    definedMask.set_bit(propId);
    definedEntryIdx[propId] = idx;
}

inline const TemplatePropEntry* TemplateNode::get_resolved(prop::PropertyId propId) const noexcept {
    if (propId >= MAX_PROPS || !resolvedMask.test_bit(propId)) return nullptr;
    return TemplateArena::instance().entry_at(resolvedEntryIdx[propId]);
}

inline const char* TemplateNode::get_resolved_string(prop::PropertyId propId, const char* fallback) const noexcept {
    const TemplatePropEntry* e = get_resolved(propId);
    if (!e || e->strOffset == INVALID_U32 || e->strLen == 0) return fallback;
    return TemplateArena::instance().string_at(e->strOffset);
}

inline double TemplateNode::get_resolved_number(prop::PropertyId propId, double fallback) const noexcept {
    const TemplatePropEntry* e = get_resolved(propId);
    return (e && e->isNumeric) ? e->numVal : fallback;
}

} // namespace stockcut
