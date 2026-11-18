// ============================================================================
// CoreEngine - Dirty Rect Coalescing Engine
// ============================================================================
#include "dirty_rect.h"
#include "data_structures.h"
#include "math_utils.h"
#include "timeline/bitboard.h"

namespace stockcut {

alignas(32) static MultiArray<Rect<double>, MAX_DIRTY_RECTS_CAP> g_dirtyRects{};
alignas(32) static double g_area = 0.0;

Rect<double>* dirty_get_buf() {
    return g_dirtyRects.data();
}

double* dirty_get_area_ptr() {
    return &g_area;
}

usize dirty_coalesce(usize count, double W, double H) {
    if (count == 0) {
        g_area = 0.0;
        return 0;
    }
    if (count > MAX_DIRTY_RECTS_CAP) count = MAX_DIRTY_RECTS_CAP;

    Rect<double>* rects = g_dirtyRects.data();
    MultiArray<u64, MAX_DIRTY_RECTS_CAP> tileMasks{};
    usize n = 0;

    for (Rect<double> *p = rects, *const end = rects + count; p < end; ++p) {
        p->x0 = stockcut::max(0.0, p->x0);
        p->y0 = stockcut::max(0.0, p->y0);
        p->x1 = stockcut::min(W, p->x1);
        p->y1 = stockcut::min(H, p->y1);
        if (p->isValid()) {
            rects[n] = *p;
            tileMasks[n] = Bitboard::FromAABBT<SpatialCoverage::ConservativeOuter>(
                p->x0, p->y0, p->x1, p->y1, W, H);
            ++n;
        }
    }
    if (n == 0) {
        g_area = 0.0;
        return 0;
    }

    bool again = true;
    while (again) {
        again = false;
        for (usize i = 0; i < n && !again; ++i) {
            for (usize j = i + 1; j < n; ++j) {
                if ((tileMasks[i] & tileMasks[j]) == 0ULL) continue;
                if (rects[i].intersects(rects[j])) {
                    rects[i].uniteWith(rects[j]);
                    tileMasks[i] = Bitboard::FromAABBT<SpatialCoverage::ConservativeOuter>(
                        rects[i].x0, rects[i].y0, rects[i].x1, rects[i].y1, W, H);
                    --n;
                    rects[j] = rects[n];
                    tileMasks[j] = tileMasks[n];
                    again = true;
                    break;
                }
            }
        }
    }

    while (n > MAX_COALESCED_RECTS) {
        usize bi = 0, bj = 1;
        double bestCost = DBL_MAX_VAL;
        for (usize i = 0; i < n; ++i) {
            const double areaA = rects[i].area();
            for (usize j = i + 1; j < n; ++j) {
                const double ux = stockcut::max(rects[i].x1, rects[j].x1) - stockcut::min(rects[i].x0, rects[j].x0);
                const double uy = stockcut::max(rects[i].y1, rects[j].y1) - stockcut::min(rects[i].y0, rects[j].y0);
                const double cost = (ux * uy) - areaA - rects[j].area();
                if (cost < bestCost) {
                    bestCost = cost;
                    bi = i;
                    bj = j;
                }
            }
        }
        rects[bi].uniteWith(rects[bj]);
        tileMasks[bi] = Bitboard::FromAABBT<SpatialCoverage::ConservativeOuter>(
            rects[bi].x0, rects[bi].y0, rects[bi].x1, rects[bi].y1, W, H);
        --n;
        rects[bj] = rects[n];
        tileMasks[bj] = tileMasks[n];
        for (usize i = 0; i < n; ++i) {
            if (i == bi) continue;
            if ((tileMasks[bi] & tileMasks[i]) == 0ULL) continue;
            if (rects[bi].intersects(rects[i])) {
                rects[bi].uniteWith(rects[i]);
                tileMasks[bi] = Bitboard::FromAABBT<SpatialCoverage::ConservativeOuter>(
                    rects[bi].x0, rects[bi].y0, rects[bi].x1, rects[bi].y1, W, H);
                --n;
                rects[i] = rects[n];
                tileMasks[i] = tileMasks[n];
                if (bi == n) bi = i;
                i = static_cast<usize>(-1);
            }
        }
    }

    double totalArea = 0.0;
    for (const Rect<double> *p = rects, *const end = rects + n; p < end; ++p) {
        totalArea += p->area();
    }
    g_area = totalArea;
    return n;
}

} // namespace stockcut
