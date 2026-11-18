// ============================================================================
// CoreEngine - Central Project Coordinator (Stockfish Position Heritage)
// ============================================================================
#pragma once

#include "types.h"
#include "undo.h"
#include "nnue_project.h"

namespace stockcut {

class CutProject {
public:
    static CutProject& instance() noexcept;

    template <EditAction Action>
    bool do_edit(const char* payload, u32 len, HistoryDirties* outDirty = nullptr) noexcept;

    template <bool IsRedo>
    bool step_edit(HistoryDirties* outDirty = nullptr) noexcept;

    bool can_undo() const noexcept;
    bool can_redo() const noexcept;

private:
    CutProject() noexcept = default;
};

inline CutProject& cutProject() noexcept {
    return CutProject::instance();
}

template<>
bool CutProject::do_edit<CONFIG>(const char* payload, u32 len, HistoryDirties* outDirty) noexcept;

template<>
bool CutProject::do_edit<TIMELINE>(const char* payload, u32 len, HistoryDirties* outDirty) noexcept;

template<>
bool CutProject::do_edit<TEMPLATE>(const char* payload, u32 len, HistoryDirties* outDirty) noexcept;

template<>
bool CutProject::do_edit<FULL_PROJECT>(const char* payload, u32 len, HistoryDirties* outDirty) noexcept;

inline bool dispatch_do_edit(u32 actionType, const char* payload, u32 len, HistoryDirties* outDirty = nullptr) noexcept {
    switch (actionType) {
        case CONFIG:       return cutProject().do_edit<CONFIG>(payload, len, outDirty);
        case TIMELINE:     return cutProject().do_edit<TIMELINE>(payload, len, outDirty);
        case TEMPLATE:     return cutProject().do_edit<TEMPLATE>(payload, len, outDirty);
        case FULL_PROJECT: return cutProject().do_edit<FULL_PROJECT>(payload, len, outDirty);
        default:           return false;
    }
}

} // namespace stockcut
