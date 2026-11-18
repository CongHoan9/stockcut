#ifndef STOCKCUT_TIMELINE_SELECTION_OUTLINE_H
#define STOCKCUT_TIMELINE_SELECTION_OUTLINE_H

#include "position.h"
#include <algorithm>
#include <cmath>

namespace stockcut {
namespace timeline {

// ============================================================================
// SELECTION OUTLINE ENGINE (C++ INDEPENDENT INTERNAL ENGINE CORE - GEMINI.md Rule 9)
// - Zero-copy shared memory flat buffer exported to JS
// - Stockfish Data-Structure-Centric (Rule 7): Zero dynamic allocations in hot path
// - Div Container Rule: Div cha là 1 khối thống nhất, các con bên trong tự động
//   tính toán viền để đưa ra viền ngoài cùng. Mọi line bên trong thân bị xóa sạch!
// - Exact intrinsic bounds for Text, Lyric, Image, Video, and Divs
// - Full multi-line Lyric bounding box encompassing all lines
// ============================================================================

struct SelectionSegment {
    float x0;
    float y0;
    float x1;
    float y1;
};

struct SelectionRect {
    float x0;
    float y0;
    float x1;
    float y1;
};

class OutlineEngine {
public:
    static constexpr u32 MAX_OUTLINE_SEGMENTS = 512;
    static constexpr u32 MAX_SELECTION_RECTS  = 128;
    static constexpr u32 MAX_SELECTED_INPUT   = 64;
    static constexpr float EPSILON            = 1e-3f;

    MultiArray<u32, MAX_SELECTED_INPUT> inputIndices; // KHONG CO {}
    MultiArray<SelectionSegment, MAX_OUTLINE_SEGMENTS> segments; // KHONG CO {} (guarded by segmentCount)
    MultiArray<u32, MAX_SELECTED_INPUT> hitIndices; // KHONG CO {} (guarded by hitCount)
    u32 segmentCount = 0;
    u32 hitCount = 0;

    // Evaluates intrinsic slot bounds in O(1) from point-in-time Evaluated
    // Zero-check hot path: Slot index is masked and data is read directly from L1 evaluated cache.
    static inline bool get_slot_bounds(
        Position<>* pos, u32 slot,
        float& outX0, float& outY0, float& outX1, float& outY1, float& outRot
    ) noexcept {
        const u32 s = slot & (MAX_TIMELINE_SLOTS - 1u);
        const Evaluated& ev = pos->evaluated_slot(s);
        if (ev.isVisible == 0) return false;

        const TimelineItem& it = pos->item(s);
        if (it.type == MediaType::Background) return false;

        float x0 = ev.aabb.x0;
        float y0 = ev.aabb.y0;
        float x1 = ev.aabb.x1;
        float y1 = ev.aabb.y1;
        if (x1 - x0 <= EPSILON || y1 - y0 <= EPSILON) return false;

        // Static authoring rotation (from it.rotateDeg, e.g. text rotate: 90)
        // NOT dynamic keyframe rotation or spinSpeed, so bounds never pulse or jitter as timeline plays!
        // Circular items (border-radius: 50% / maskCircle) are radially symmetric, so rotation never changes their bounds.
        const bool isCircle = (it.flags & ITEM_FLAG_MASK_CIRCLE) != 0;
        const float rot = isCircle ? 0.0f : it.rotateDeg;

        if (std::abs(rot) > 0.01f) {
            const float rRad = rot * (3.14159265358979323846f / 180.0f);
            const float cosA = std::abs(std::cos(rRad));
            const float sinA = std::abs(std::sin(rRad));
            const float cx = (x0 + x1) * 0.5f;
            const float cy = (y0 + y1) * 0.5f;
            const float w = x1 - x0;
            const float h = y1 - y0;
            const float halfW = (w * cosA + h * sinA) * 0.5f;
            const float halfH = (w * sinA + h * cosA) * 0.5f;
            x0 = cx - halfW;
            x1 = cx + halfW;
            y0 = cy - halfH;
            y1 = cy + halfH;
        }

        outX0 = x0;
        outY0 = y0;
        outX1 = x1;
        outY1 = y1;
        outRot = 0.0f;
        return true;
    }

    // Gathers all candidate bounding boxes for a top-level item
    static inline u32 get_item_rects(
        Position<>* pos, u32 topLevelIndex,
        SelectionRect* outRects, u32 maxRects
    ) noexcept {
        if (!pos || !outRects || maxRects == 0) return 0;
        u32 count = 0;
        Bitboard occ = pos->populated_slots();
        while (occ && count < maxRects) {
            const u32 s = occ.pop_lsb();
            if (pos->slot_top_level(s) != topLevelIndex) continue;
            float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f, rot = 0.0f;
            if (!get_slot_bounds(pos, s, x0, y0, x1, y1, rot)) continue;
            outRects[count++] = { x0, y0, x1, y1 };
        }
        return count;
    }

    // Boolean Perimeter Union: Removes ALL lines inside the unified perimeter
    static inline u32 compute_union_perimeter(
        const SelectionRect* inRects, u32 inCount,
        SelectionSegment* outSegments, u32 maxSegments
    ) noexcept {
        if (!inRects || inCount == 0 || !outSegments || maxSegments == 0) return 0;

        // 1. Sanitize and filter duplicate rects
        MultiArray<SelectionRect, MAX_SELECTION_RECTS> valid;
        u32 n = 0;
        for (u32 i = 0; i < inCount && n < MAX_SELECTION_RECTS; ++i) {
            const float x0 = std::min(inRects[i].x0, inRects[i].x1);
            const float x1 = std::max(inRects[i].x0, inRects[i].x1);
            const float y0 = std::min(inRects[i].y0, inRects[i].y1);
            const float y1 = std::max(inRects[i].y0, inRects[i].y1);
            if (x1 - x0 <= EPSILON || y1 - y0 <= EPSILON) continue;

            bool dup = false;
            for (u32 v = 0; v < n; ++v) {
                if (std::abs(valid[v].x0 - x0) < EPSILON && std::abs(valid[v].x1 - x1) < EPSILON &&
                    std::abs(valid[v].y0 - y0) < EPSILON && std::abs(valid[v].y1 - y1) < EPSILON) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                valid[n++] = { x0, y0, x1, y1 };
            }
        }
        if (n == 0) return 0;

        // 2. Filter out rectangles completely contained within another rectangle (A subset of B => A U B = B)
        MultiArray<SelectionRect, MAX_SELECTION_RECTS> rects;
        u32 rectCount = 0;
        for (u32 i = 0; i < n; ++i) {
            bool contained = false;
            for (u32 j = 0; j < n; ++j) {
                if (i == j) continue;
                if (valid[i].x0 >= valid[j].x0 - EPSILON &&
                    valid[i].x1 <= valid[j].x1 + EPSILON &&
                    valid[i].y0 >= valid[j].y0 - EPSILON &&
                    valid[i].y1 <= valid[j].y1 + EPSILON) {
                    contained = true;
                    break;
                }
            }
            if (!contained) {
                rects[rectCount++] = valid[i];
            }
        }
        if (rectCount == 0) return 0;

        // If only 1 rect remains, emit its 4 edges directly
        if (rectCount == 1) {
            if (maxSegments < 4) return 0;
            const SelectionRect& r = rects[0];
            outSegments[0] = { r.x0, r.y0, r.x1, r.y0 };
            outSegments[1] = { r.x1, r.y0, r.x1, r.y1 };
            outSegments[2] = { r.x1, r.y1, r.x0, r.y1 };
            outSegments[3] = { r.x0, r.y1, r.x0, r.y0 };
            return 4;
        }

        // Buffer for raw subsegments
        MultiArray<SelectionSegment, MAX_OUTLINE_SEGMENTS> rawH;
        MultiArray<SelectionSegment, MAX_OUTLINE_SEGMENTS> rawV;
        MultiArray<float, MAX_SELECTION_RECTS * 2 + 2> splits;
        u32 rawHCount = 0;
        u32 rawVCount = 0;

        // Process horizontal edges
        for (u32 k = 0; k < rectCount; ++k) {
            const SelectionRect& R = rects[k];
            const float yEdges[2] = { R.y0, R.y1 };
            for (u32 e = 0; e < 2; ++e) {
                const float y = yEdges[e];
                u32 splitCount = 0;
                splits[splitCount++] = R.x0;
                splits[splitCount++] = R.x1;

                for (u32 p = 0; p < rectCount; ++p) {
                    if (p == k) continue;
                    const SelectionRect& P = rects[p];
                    if (y >= P.y0 - EPSILON && y <= P.y1 + EPSILON) {
                        if (P.x0 > R.x0 + EPSILON && P.x0 < R.x1 - EPSILON) {
                            splits[splitCount++] = P.x0;
                        }
                        if (P.x1 > R.x0 + EPSILON && P.x1 < R.x1 - EPSILON) {
                            splits[splitCount++] = P.x1;
                        }
                    }
                }

                // Sort splits
                std::sort(splits.data(), splits.data() + splitCount);

                for (u32 i = 0; i < splitCount - 1 && rawHCount < MAX_OUTLINE_SEGMENTS; ++i) {
                    const float x0 = splits[i];
                    const float x1 = splits[i + 1];
                    if (x1 - x0 <= EPSILON) continue;
                    const float xMid = (x0 + x1) * 0.5f;

                    // Test if strictly inside ANY rectangle of the unified block
                    bool inside = false;
                    for (u32 p = 0; p < rectCount; ++p) {
                        if (p == k) continue;
                        const SelectionRect& P = rects[p];
                        if (xMid > P.x0 + EPSILON && xMid < P.x1 - EPSILON &&
                            y > P.y0 + EPSILON && y < P.y1 - EPSILON) {
                            inside = true;
                            break;
                        }
                        // Shared internal boundary between touching rectangles
                        if (xMid > P.x0 + EPSILON && xMid < P.x1 - EPSILON &&
                            std::abs(y - (e == 0 ? P.y1 : P.y0)) < EPSILON) {
                            inside = true;
                            break;
                        }
                    }

                    if (!inside) {
                        // Check deduplication with already emitted H segment
                        bool dupSeg = false;
                        for (u32 d = 0; d < rawHCount; ++d) {
                            if (std::abs(rawH[d].y0 - y) < EPSILON &&
                                std::abs(rawH[d].x0 - x0) < EPSILON &&
                                std::abs(rawH[d].x1 - x1) < EPSILON) {
                                dupSeg = true;
                                break;
                            }
                        }
                        if (!dupSeg) {
                            rawH[rawHCount++] = { x0, y, x1, y };
                        }
                    }
                }
            }
        }

        // Process vertical edges
        for (u32 k = 0; k < rectCount; ++k) {
            const SelectionRect& R = rects[k];
            const float xEdges[2] = { R.x0, R.x1 };
            for (u32 e = 0; e < 2; ++e) {
                const float x = xEdges[e];
                u32 splitCount = 0;
                splits[splitCount++] = R.y0;
                splits[splitCount++] = R.y1;

                for (u32 p = 0; p < rectCount; ++p) {
                    if (p == k) continue;
                    const SelectionRect& P = rects[p];
                    if (x >= P.x0 - EPSILON && x <= P.x1 + EPSILON) {
                        if (P.y0 > R.y0 + EPSILON && P.y0 < R.y1 - EPSILON) {
                            splits[splitCount++] = P.y0;
                        }
                        if (P.y1 > R.y0 + EPSILON && P.y1 < R.y1 - EPSILON) {
                            splits[splitCount++] = P.y1;
                        }
                    }
                }

                // Sort splits
                std::sort(splits.data(), splits.data() + splitCount);

                for (u32 i = 0; i < splitCount - 1 && rawVCount < MAX_OUTLINE_SEGMENTS; ++i) {
                    const float y0 = splits[i];
                    const float y1 = splits[i + 1];
                    if (y1 - y0 <= EPSILON) continue;
                    const float yMid = (y0 + y1) * 0.5f;

                    bool inside = false;
                    for (u32 p = 0; p < rectCount; ++p) {
                        if (p == k) continue;
                        const SelectionRect& P = rects[p];
                        if (yMid > P.y0 + EPSILON && yMid < P.y1 - EPSILON &&
                            x > P.x0 + EPSILON && x < P.x1 - EPSILON) {
                            inside = true;
                            break;
                        }
                        // Shared internal boundary between touching rectangles
                        if (yMid > P.y0 + EPSILON && yMid < P.y1 - EPSILON &&
                            std::abs(x - (e == 0 ? P.x1 : P.x0)) < EPSILON) {
                            inside = true;
                            break;
                        }
                    }

                    if (!inside) {
                        bool dupSeg = false;
                        for (u32 d = 0; d < rawVCount; ++d) {
                            if (std::abs(rawV[d].x0 - x) < EPSILON &&
                                std::abs(rawV[d].y0 - y0) < EPSILON &&
                                std::abs(rawV[d].y1 - y1) < EPSILON) {
                                dupSeg = true;
                                break;
                            }
                        }
                        if (!dupSeg) {
                            rawV[rawVCount++] = { x, y0, x, y1 };
                        }
                    }
                }
            }
        }

        // Collinear Merging for H segments
        u32 outCount = 0;
        MultiArray<bool, MAX_OUTLINE_SEGMENTS> mergedH;
        if (rawHCount > 0) {
            std::memset(mergedH.data(), 0, rawHCount * sizeof(bool));
        }
        for (u32 i = 0; i < rawHCount && outCount < maxSegments; ++i) {
            if (mergedH[i]) continue;
            float curY = rawH[i].y0;
            float curX0 = rawH[i].x0;
            float curX1 = rawH[i].x1;
            mergedH[i] = true;

            bool expanded = true;
            while (expanded) {
                expanded = false;
                for (u32 j = 0; j < rawHCount; ++j) {
                    if (mergedH[j]) continue;
                    if (std::abs(rawH[j].y0 - curY) < EPSILON) {
                        if (rawH[j].x0 <= curX1 + EPSILON && rawH[j].x1 >= curX0 - EPSILON) {
                            curX0 = std::min(curX0, rawH[j].x0);
                            curX1 = std::max(curX1, rawH[j].x1);
                            mergedH[j] = true;
                            expanded = true;
                        }
                    }
                }
            }
            outSegments[outCount++] = { curX0, curY, curX1, curY };
        }

        // Collinear Merging for V segments
        MultiArray<bool, MAX_OUTLINE_SEGMENTS> mergedV;
        if (rawVCount > 0) {
            std::memset(mergedV.data(), 0, rawVCount * sizeof(bool));
        }
        for (u32 i = 0; i < rawVCount && outCount < maxSegments; ++i) {
            if (mergedV[i]) continue;
            float curX = rawV[i].x0;
            float curY0 = rawV[i].y0;
            float curY1 = rawV[i].y1;
            mergedV[i] = true;

            bool expanded = true;
            while (expanded) {
                expanded = false;
                for (u32 j = 0; j < rawVCount; ++j) {
                    if (mergedV[j]) continue;
                    if (std::abs(rawV[j].x0 - curX) < EPSILON) {
                        if (rawV[j].y0 <= curY1 + EPSILON && rawV[j].y1 >= curY0 - EPSILON) {
                            curY0 = std::min(curY0, rawV[j].y0);
                            curY1 = std::max(curY1, rawV[j].y1);
                            mergedV[j] = true;
                            expanded = true;
                        }
                    }
                }
            }
            outSegments[outCount++] = { curX, curY0, curX, curY1 };
        }

        return outCount;
    }

    // Computes selection outline segments for the given top-level indices at base (0,0)
    u32 compute_outlines(Position<>* pos, u32 count) noexcept {
        segmentCount = 0;
        if (!pos || count == 0) return 0;

        MultiArray<SelectionRect, MAX_SELECTION_RECTS> allRects;
        u32 totalRects = 0;

        for (u32 i = 0; i < count && i < MAX_SELECTED_INPUT; ++i) {
            const u32 topLevelIdx = inputIndices[i];
            const u32 added = get_item_rects(
                pos, topLevelIdx, allRects.data() + totalRects,
                MAX_SELECTION_RECTS - totalRects
            );
            totalRects += added;
        }

        if (totalRects == 0) return 0;

        segmentCount = compute_union_perimeter(
            allRects.data(), totalRects,
            segments.data(), MAX_OUTLINE_SEGMENTS
        );
        return segmentCount;
    }

    // Hit test at point (stageX, stageY) -> returns topLevelIndex or -1
    i32 hit_test(Position<>* pos, float stageX, float stageY) noexcept {
        if (!pos) return -1;
        i32 bestTopLevel = -1;
        i32 bestZ = -999999;
        constexpr float stageW = 1920.0f;
        constexpr float stageH = 1080.0f;

        Bitboard occ = pos->populated_slots();
        while (occ) {
            const u32 s = occ.pop_lsb();
            float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f, rot = 0.0f;
            if (!get_slot_bounds(pos, s, x0, y0, x1, y1, rot)) continue;

            // Exclude full-stage background items (never selectable by click)
            if (x0 <= 2.0f && y0 <= 2.0f && x1 >= stageW - 2.0f && y1 >= stageH - 2.0f) {
                continue;
            }

            if (stageX >= x0 && stageX <= x1 && stageY >= y0 && stageY <= y1) {
                const TimelineItem& it = pos->item(s);
                if (it.zIndex >= bestZ) {
                    bestZ = it.zIndex;
                    bestTopLevel = static_cast<i32>(pos->slot_top_level(s));
                }
            }
        }
        return bestTopLevel;
    }

    // Marquee hit test -> returns hit count, stores indices in hitIndices
    u32 marquee_hit_test(Position<>* pos, float mx0, float my0, float mx1, float my1) noexcept {
        hitCount = 0;
        if (!pos) return 0;

        const float minX = std::min(mx0, mx1);
        const float maxX = std::max(mx0, mx1);
        const float minY = std::min(my0, my1);
        const float maxY = std::max(my0, my1);

        // Minimum 4x4 threshold to avoid accidental marquee triggers on clicks
        if (maxX - minX < 4.0f || maxY - minY < 4.0f) return 0;

        constexpr float stageW = 1920.0f;
        constexpr float stageH = 1080.0f;

        Bitboard occ = pos->populated_slots();
        while (occ && hitCount < MAX_SELECTED_INPUT) {
            const u32 s = occ.pop_lsb();
            float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f, rot = 0.0f;
            if (!get_slot_bounds(pos, s, x0, y0, x1, y1, rot)) continue;

            // Exclude full-stage background items
            if (x0 <= 2.0f && y0 <= 2.0f && x1 >= stageW - 2.0f && y1 >= stageH - 2.0f) {
                continue;
            }

            // Overlap check
            if (x0 < maxX && x1 > minX && y0 < maxY && y1 > minY) {
                const u32 topLevel = static_cast<u32>(pos->slot_top_level(s));
                bool exists = false;
                for (u32 h = 0; h < hitCount; ++h) {
                    if (hitIndices[h] == topLevel) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    hitIndices[hitCount++] = topLevel;
                }
            }
        }
        return hitCount;
    }
};

} // namespace timeline
} // namespace stockcut

#endif // STOCKCUT_TIMELINE_SELECTION_OUTLINE_H
