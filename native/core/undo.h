// ============================================================================
// CoreEngine - Project History & StateInfo Chain (Stockfish Heritage)
// ============================================================================
#pragma once

#include "types.h"
#include "data_structures.h"
#include "nnue_project.h"
#include <cstdlib>
#include <cstring>

namespace stockcut {

enum EditAction : u32 {
    TIMELINE       = 0,
    TEMPLATE       = 1,
    CONFIG         = 2,
    FULL_PROJECT   = 3,
    EDIT_ACTION_NB = 4
};

struct UndoNode {
    EditAction action = TIMELINE;
    u32 stepId = 0;
    Key projectKey = 0ULL;
    i64 timestampMs = 0;

    UndoNode* previous = nullptr;
    UndoNode* next = nullptr;

    HistoryDirties dirty{};

    char* snapshot = nullptr;
    u32 snapshotLen = 0;

    void set_snapshot(const char* data, u32 len) noexcept {
        if (snapshot) {
            std::free(snapshot);
            snapshot = nullptr;
            snapshotLen = 0;
        }
        if (data && len > 0) {
            snapshot = static_cast<char*>(std::malloc(len + 1));
            if (snapshot) {
                std::memcpy(snapshot, data, len);
                snapshot[len] = '\0';
                snapshotLen = len;
            }
        }
    }

    void clear() noexcept {
        if (snapshot) {
            std::free(snapshot);
            snapshot = nullptr;
            snapshotLen = 0;
        }
        dirty.clear();
        previous = nullptr;
        next = nullptr;
        action = TIMELINE;
        stepId = 0;
        projectKey = 0ULL;
        timestampMs = 0;
    }
};

class ProjectHistory {
public:
    static constexpr u32 MAX_HISTORY_STEPS = 64u;

    static ProjectHistory& instance() noexcept;

    void init() noexcept;

    template <EditAction Action>
    UndoNode* allocate_step() noexcept {
        UndoNode* st = &pool_[poolCursor_ & (MAX_HISTORY_STEPS - 1u)];
        ++poolCursor_;
        st->clear();
        st->action = Action;
        st->stepId = nextStepId_++;
        return st;
    }

    void commit_step(UndoNode* st) noexcept;

    template <bool Forward>
    UndoNode* step() noexcept {
        if constexpr (Forward) {
            if (!can_redo()) return nullptr;
            current_ = current_->next;
        } else {
            if (!can_undo()) return nullptr;
            current_ = current_->previous;
        }
        return current_;
    }

    bool can_undo() const noexcept {
        return current_ != nullptr && current_->previous != nullptr;
    }
    bool can_redo() const noexcept {
        return current_ != nullptr && current_->next != nullptr;
    }

    UndoNode* current_state() noexcept { return current_; }
    const UndoNode* current_state() const noexcept { return current_; }

    u32 step_count() const noexcept { return stepCount_; }

private:
    ProjectHistory() noexcept { init(); }
    ~ProjectHistory() noexcept;

    MultiArray<UndoNode, MAX_HISTORY_STEPS> pool_; // KHONG CO {} (init() clears every slot)
    u32 poolCursor_ = 0;
    UndoNode* current_ = nullptr;
    UndoNode* head_ = nullptr;
    u32 stepCount_ = 0;
    u32 nextStepId_ = 1;
};

} // namespace stockcut
