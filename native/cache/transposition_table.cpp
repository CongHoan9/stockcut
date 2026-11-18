// ============================================================================
// CoreEngine - Stockfish-Grade Universal Transposition Table Implementation
// ============================================================================
#include "transposition_table.h"

namespace stockcut::cache {

alignas(64) static DisplayListTT g_displayListTT{};

DisplayListTT& get_global_display_list_tt() noexcept {
    return g_displayListTT;
}

void tt_clear() noexcept {
    g_displayListTT.clear();
}

void tt_next_generation() noexcept {
    g_displayListTT.advance_generation();
}

usize tt_memory_bytes() noexcept {
    return sizeof(g_displayListTT);
}

} // namespace stockcut::cache

namespace stockcut {
void tt_clear() noexcept {
    cache::tt_clear();
}
void tt_next_generation() noexcept {
    cache::tt_next_generation();
}
usize tt_memory_bytes() noexcept {
    return cache::tt_memory_bytes();
}
} // namespace stockcut
