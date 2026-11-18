// ============================================================================
// CoreEngine - Hierarchical Template System Implementation
// ============================================================================
#include "templates.h"
#include <cstring>

namespace stockcut {

TemplateArena& TemplateArena::instance() noexcept {
    static TemplateArena s_instance;
    return s_instance;
}

void TemplateArena::reset() noexcept {
    count_ = 0;
    propCount_ = 0;
    strCursor_ = 0;
}

u16 TemplateArena::alloc_prop_entry(const char* propVal, bool isNumeric, double numVal) noexcept {
    if (propCount_ >= 65534u) return INVALID_U16;
    if (propCount_ >= propCap_) {
        const u32 newCap = (propCap_ == 0u) ? INITIAL_PROP_CAP : (propCap_ * 2u);
        AlignedPtr<TemplatePropEntry[]> next = make_unique_aligned<TemplatePropEntry[]>(newCap);
        if (!next) return INVALID_U16;
        if (propPool_ && propCount_ > 0u) {
            std::memcpy(next.get(), propPool_.get(), static_cast<usize>(propCount_) * sizeof(TemplatePropEntry));
        }
        propPool_ = std::move(next);
        propCap_ = newCap;
    }

    const u32 valLen = (propVal && propVal[0]) ? static_cast<u32>(std::strlen(propVal)) : 0u;
    u32 strOff = INVALID_U32;
    if (valLen > 0u) {
        const u32 needed = strCursor_ + valLen + 1u;
        if (needed > strCap_) {
            u32 newCap = (strCap_ == 0u) ? INITIAL_STR_CAP : strCap_;
            while (newCap < needed) newCap *= 2u;
            AlignedPtr<char[]> nextStr = make_unique_aligned<char[]>(newCap);
            if (!nextStr) return INVALID_U16;
            if (strPool_ && strCursor_ > 0u) {
                std::memcpy(nextStr.get(), strPool_.get(), strCursor_);
            }
            strPool_ = std::move(nextStr);
            strCap_ = newCap;
        }
        strOff = strCursor_;
        std::memcpy(strPool_.get() + strOff, propVal, valLen + 1u);
        strCursor_ = needed;
    }

    const u16 idx = static_cast<u16>(propCount_++);
    TemplatePropEntry& e = propPool_[idx];
    e.numVal = isNumeric ? numVal : 0.0;
    e.strOffset = strOff;
    e.strLen = static_cast<u16>(valLen <= 0xFFFFu ? valLen : 0xFFFFu);
    e.isNumeric = isNumeric ? 1u : 0u;
    e.pad = 0u;
    return idx;
}

TemplateNode* TemplateArena::allocate_template(const char* name, const char* type) noexcept {
    if (name && name[0]) {
        if (TemplateNode* existing = find_template(name)) return existing;
    }
    if (count_ >= MAX_TEMPLATES) return nullptr;
    TemplateNode* node = &templates_[count_++];
    node->clear();
    node->uid = count_;
    copy_cstr_bounded(node->id.data(), node->id.size(), name);
    copy_cstr_bounded(node->type.data(), node->type.size(), type);
    return node;
}

TemplateNode* TemplateArena::find_template(const char* name) noexcept {
    return const_cast<TemplateNode*>(static_cast<const TemplateArena*>(this)->find_template(name));
}

const TemplateNode* TemplateArena::find_template(const char* name) const noexcept {
    if (!name || !name[0]) return nullptr;
    for (const TemplateNode* p = templates_.begin(), *const end = p + count_; p < end; ++p) {
        if (cstr_equal(p->id.data(), name)) return p;
    }
    return nullptr;
}

bool TemplateArena::link_parent(TemplateNode* child, TemplateNode* parent) noexcept {
    if (!child) return false;
    for (TemplateNode* check = parent; check; check = check->parent) {
        if (check == child) return false;
    }
    child->parent = parent;
    return true;
}

void TemplateArena::bake_all_templates() noexcept {
    TemplateNode* const first = templates_.begin();
    TemplateNode* const last  = first + count_;
    for (TemplateNode* p = first; p < last; ++p) {
        p->blend_hierarchy();
        p->allDependentSlotsBB = p->assignedSlotsBB;
    }
    for (TemplateNode* p = first; p < last; ++p) {
        for (TemplateNode* curr = p->parent; curr; curr = curr->parent) {
            curr->allDependentSlotsBB |= p->assignedSlotsBB;
        }
    }
}

bool TemplateArena::update_template_diff(TemplateNode* target, const Mask<2>& diffMask, Bitboard& outMutatedSlots) noexcept {
    if (!target) return false;
    target->blend_hierarchy();
    outMutatedSlots |= target->allDependentSlotsBB;

    for (TemplateNode* candidate = templates_.begin(), *const end = candidate + count_; candidate < end; ++candidate) {
        if (candidate == target) continue;
        for (TemplateNode* p = candidate->parent; p; p = p->parent) {
            if (p == target) {
                candidate->blend_diff(diffMask);
                break;
            }
        }
    }
    return true;
}

} // namespace stockcut
