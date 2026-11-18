// ============================================================================
// CoreEngine - Project History Implementation (Stockfish StateInfo Storage)
// ============================================================================
#include "undo.h"

namespace stockcut {

ProjectHistory& ProjectHistory::instance() noexcept {
    static ProjectHistory s_instance;
    return s_instance;
}

void ProjectHistory::init() noexcept {
    for (UndoNode* p = pool_.begin(); p < pool_.end(); ++p) {
        p->clear();
    }
    poolCursor_ = 0;
    current_ = nullptr;
    head_ = nullptr;
    stepCount_ = 0;
    nextStepId_ = 1;

    UndoNode* baseline = allocate_step<CONFIG>();
    baseline->dirty.clear();
    head_ = baseline;
    current_ = baseline;
}

ProjectHistory::~ProjectHistory() noexcept {
    for (UndoNode* p = pool_.begin(); p < pool_.end(); ++p) {
        p->clear();
    }
}

void ProjectHistory::commit_step(UndoNode* st) noexcept {
    if (!st) return;

    if (!head_) {
        head_ = st;
        current_ = st;
        st->previous = nullptr;
        st->next = nullptr;
        stepCount_ = 1;
        return;
    }

    if (current_) {
        st->previous = current_;
        st->next = nullptr;
        current_->next = st;
        current_ = st;
    } else {
        st->previous = nullptr;
        st->next = head_;
        head_->previous = st;
        current_ = st;
    }
    ++stepCount_;
}

} // namespace stockcut
