// ============================================================================
// CoreEngine - Vector 2D Path Types & Architecture (Stockfish DOD)
// ----------------------------------------------------------------------------
// [STOCKFISH DATA-STRUCTURE-CENTRIC & 2D VECTOR SPECIFICATION]:
// - Thiết kế phân tầng hỗ trợ nguyên mẫu đồ họa vector 2D (<Path>) chuẩn CSS/SVG.
// - Hoàn toàn độc lập, div làm container cha chứa các path.
// - 100% Zero-Allocation trong vòng lặp runtime 60 FPS, căn lề 64 bytes L1 Cache.
// - Không sử dụng {} trên khai báo mảng để triệt tiêu áp lực memset.
// ============================================================================
#pragma once
#include "../core/types.h"
#include "../core/memory.h"
#include <algorithm>
#include <cstring>
#include <cmath>

namespace stockcut {
namespace path {

// ----------------------------------------------------------------------------
// 1. CÁC HẰNG SỐ & ENUM CHUẨN CSS / SVG CHO VECTOR PATH
// ----------------------------------------------------------------------------
enum class PathFillRule : u8 {
    NonZero = 0,
    EvenOdd = 1
};

enum class PathLineCap : u8 {
    Butt   = 0,
    Round  = 1,
    Square = 2
};

enum class PathLineJoin : u8 {
    Miter = 0,
    Round = 1,
    Bevel = 2
};

constexpr usize MAX_PATH_STRING_CAP = 512;

// ----------------------------------------------------------------------------
// 2. PathStringArena: Bộ Đệm Chuỗi Lệnh SVG d Contiguous (Zero Allocation)
// ----------------------------------------------------------------------------
class alignas(64) PathStringArena {
public:
    MultiArray<char, MAX_TIMELINE_SLOTS, MAX_PATH_STRING_CAP> strings; // KHONG CO {}
    MultiArray<u16, MAX_TIMELINE_SLOTS> lengths;                       // KHONG CO {}

    static PathStringArena& instance() noexcept {
        static PathStringArena s_arena;
        return s_arena;
    }

    inline void set_path(u32 slot, const char* str, usize len) noexcept {
        const u32 safeSlot = slot & (MAX_TIMELINE_SLOTS - 1u);
        const usize copyLen = std::min(len, static_cast<usize>(MAX_PATH_STRING_CAP - 1));
        std::memcpy(strings[safeSlot].data(), str, copyLen);
        strings[safeSlot][copyLen] = '\0';
        lengths[safeSlot] = static_cast<u16>(copyLen);
    }

    [[nodiscard]] inline const char* get_path(u32 slot) const noexcept {
        return strings[slot & (MAX_TIMELINE_SLOTS - 1u)].data();
    }

    [[nodiscard]] inline u16 get_length(u32 slot) const noexcept {
        return lengths[slot & (MAX_TIMELINE_SLOTS - 1u)];
    }

    inline void clear_slot(u32 slot) noexcept {
        const u32 safeSlot = slot & (MAX_TIMELINE_SLOTS - 1u);
        strings[safeSlot][0] = '\0';
        lengths[safeSlot] = 0;
    }

    inline void reset() noexcept {
        for (usize i = 0; i < MAX_TIMELINE_SLOTS; ++i) {
            strings[i][0] = '\0';
            lengths[i] = 0;
        }
    }
};

// ----------------------------------------------------------------------------
// 4. FastPathBoundsEstimator: Tính Toán Bounding Box Tự Nhiên Không Cấp Phát
// ----------------------------------------------------------------------------
struct FastPathBoundsEstimator {
    static void compute_bounds(const char* d, usize len, float outBounds[4]) noexcept {
        outBounds[0] = 0.0f; // minX
        outBounds[1] = 0.0f; // minY
        outBounds[2] = 0.0f; // maxX
        outBounds[3] = 0.0f; // maxY

        if (!d || len == 0) return;

        float curX = 0.0f;
        float curY = 0.0f;
        float minX = 1e9f;
        float minY = 1e9f;
        float maxX = -1e9f;
        float maxY = -1e9f;
        bool hasPoints = false;

        usize idx = 0;
        while (idx < len && d[idx] != '\0') {
            // Bỏ qua khoảng trắng và dấu phẩy
            while (idx < len && (d[idx] == ' ' || d[idx] == '\t' || d[idx] == '\r' || d[idx] == '\n' || d[idx] == ',')) {
                ++idx;
            }
            if (idx >= len || d[idx] == '\0') break;

            const char cmd = d[idx];
            if ((cmd >= 'A' && cmd <= 'Z') || (cmd >= 'a' && cmd <= 'z')) {
                ++idx;
            }

            // Đọc các cặp số tọa độ
            while (idx < len && d[idx] != '\0') {
                while (idx < len && (d[idx] == ' ' || d[idx] == '\t' || d[idx] == '\r' || d[idx] == '\n' || d[idx] == ',')) {
                    ++idx;
                }
                if (idx >= len || d[idx] == '\0') break;
                if ((d[idx] >= 'A' && d[idx] <= 'Z') || (d[idx] >= 'a' && d[idx] <= 'z')) {
                    break; // Gặp lệnh tiếp theo
                }

                char* nextPtr = nullptr;
                const float val1 = std::strtof(d + idx, &nextPtr);
                if (nextPtr == d + idx) break;
                idx = static_cast<usize>(nextPtr - d);

                while (idx < len && (d[idx] == ' ' || d[idx] == '\t' || d[idx] == '\r' || d[idx] == '\n' || d[idx] == ',')) {
                    ++idx;
                }
                float val2 = 0.0f;
                if (idx < len && ((d[idx] >= '0' && d[idx] <= '9') || d[idx] == '-' || d[idx] == '+' || d[idx] == '.')) {
                    val2 = std::strtof(d + idx, &nextPtr);
                    if (nextPtr != d + idx) {
                        idx = static_cast<usize>(nextPtr - d);
                    }
                }

                const bool isRelative = (cmd >= 'a' && cmd <= 'z');
                if (cmd == 'H' || cmd == 'h') {
                    curX = isRelative ? (curX + val1) : val1;
                } else if (cmd == 'V' || cmd == 'v') {
                    curY = isRelative ? (curY + val1) : val1;
                } else {
                    curX = isRelative ? (curX + val1) : val1;
                    curY = isRelative ? (curY + val2) : val2;
                }

                minX = std::min(minX, curX);
                minY = std::min(minY, curY);
                maxX = std::max(maxX, curX);
                maxY = std::max(maxY, curY);
                hasPoints = true;
            }
        }

        if (hasPoints) {
            outBounds[0] = minX;
            outBounds[1] = minY;
            outBounds[2] = std::max(minX, maxX);
            outBounds[3] = std::max(minY, maxY);
        }
    }
};

} // namespace path
} // namespace stockcut
