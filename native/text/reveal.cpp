// ============================================================================
// CoreEngine - Text Reveal Evaluation & Segment Owner Mapping
// ============================================================================
#include "reveal.h"
#include "math_utils.h"

namespace stockcut {
namespace {

TextRevealBuffer g_textRevealBuffer{};

[[nodiscard]] inline bool codepoints_match_at(
    const TextRevealBuffer& buffer,
    u32 layoutIdx,
    u32 segOffset,
    u32 length
) noexcept {
    for (u32 j = 0; j < length; ++j) {
        if (buffer.layoutCodepoints[layoutIdx + j] != buffer.segmentCodepoints[segOffset + j]) {
            return false;
        }
    }
    return true;
}

} // namespace

TextRevealBuffer* text_reveal_get_buffer() { return &g_textRevealBuffer; }
u32 text_reveal_get_capacity() { return MAX_TEXT_REVEAL_CHARS; }
u32 text_reveal_get_segment_capacity() { return MAX_TEXT_REVEAL_SEGMENTS; }
u32 text_reveal_get_never_flag() { return TEXT_REVEAL_FLAG_NEVER; }
u32 text_reveal_get_tilde_flag() { return TEXT_REVEAL_FLAG_TILDE; }

u32 text_reveal_evaluate(u32 count, double now) {
    if (count > MAX_TEXT_REVEAL_CHARS) return 0u;
    TextRevealBuffer& buffer = g_textRevealBuffer;
    for (u32 i = 0; i < count; ++i) {
        const u32 flags = buffer.flags[i];
        if (flags & TEXT_REVEAL_FLAG_NEVER) {
            buffer.visible[i] = 0u;
            buffer.alpha[i]   = 0.0;
            continue;
        }
        const double t0   = buffer.t0[i];
        const double fade = buffer.fade[i];
        bool visible      = now >= t0;
        double alpha      = fade > 0.0 ? clamp((now - t0) / fade, 0.0, 1.0) : 1.0;
        if ((flags & TEXT_REVEAL_FLAG_TILDE) && now >= buffer.dieT[i]) {
            const double dieT    = buffer.dieT[i];
            const double dieFade = buffer.dieFade[i];
            const double out     = dieFade > 0.0 ? 1.0 - clamp((now - dieT) / dieFade, 0.0, 1.0) : 0.0;
            if (out < alpha) alpha = out;
            if (out <= 0.0) visible = false;
        }
        buffer.visible[i] = visible ? 1u : 0u;
        buffer.alpha[i]   = alpha;
    }
    return 1u;
}

u32 text_reveal_map_owners(u32 layoutCount, u32 segmentCount, u32 segmentCodepointCount) {
    if (layoutCount > MAX_TEXT_REVEAL_CHARS ||
        segmentCount > MAX_TEXT_REVEAL_SEGMENTS ||
        segmentCodepointCount > MAX_TEXT_REVEAL_CHARS)
        return 0u;
    TextRevealBuffer& buffer = g_textRevealBuffer;
    for (u32 i = 0; i < layoutCount; ++i) {
        buffer.owner[i] = -1;
        buffer.jpos[i]  = 0u;
    }
    double cursor = 0.0;
    for (u32 segmentIndex = 0; segmentIndex < segmentCount; ++segmentIndex) {
        const u32 offset = buffer.segmentOffsets[segmentIndex];
        const u32 length = buffer.segmentLengths[segmentIndex];
        if (offset > segmentCodepointCount || length > segmentCodepointCount - offset) {
            return 0u;
        }
        const double requestedAt = buffer.segmentAt[segmentIndex];
        double start = -1.0;
        if (requestedAt >= 0.0) {
            start = requestedAt;
        } else if (length <= layoutCount) {
            const double lastStart = static_cast<double>(layoutCount) - length;
            for (double candidate = cursor; candidate <= lastStart; candidate += 1.0) {
                if (candidate < 0.0 || (length != 0u && candidate != static_cast<u32>(candidate))) {
                    continue;
                }
                if (codepoints_match_at(buffer, static_cast<u32>(candidate), offset, length)) {
                    start = candidate;
                    break;
                }
            }
            if (start < 0.0) {
                for (u32 candidate = 0; candidate <= layoutCount - length; ++candidate) {
                    if (codepoints_match_at(buffer, candidate, offset, length)) {
                        start = static_cast<double>(candidate);
                        break;
                    }
                }
            }
        }
        if (start >= 0.0) {
            if (start <= layoutCount && start == static_cast<double>(static_cast<u32>(start))) {
                const u32 index = static_cast<u32>(start);
                for (u32 j = 0; j < length && index + j < layoutCount; ++j) {
                    if (buffer.owner[index + j] < 0) {
                        buffer.owner[index + j] = static_cast<i32>(segmentIndex);
                        buffer.jpos[index + j]  = j;
                    }
                }
            }
            cursor = start + length;
        }
    }
    i32 currentOwner = 0;
    u32 currentJpos  = 0u;
    for (u32 i = 0; i < layoutCount; ++i) {
        if (buffer.owner[i] >= 0) {
            currentOwner = buffer.owner[i];
            currentJpos  = buffer.jpos[i];
        } else {
            buffer.owner[i] = currentOwner;
            buffer.jpos[i]  = currentJpos + 1u;
        }
    }
    return 1u;
}

WASM_EXPORT(wasm_text_reveal_get_buffer_ptr) TextRevealBuffer* wasm_text_reveal_get_buffer_ptr() { return text_reveal_get_buffer(); }
WASM_EXPORT(wasm_text_reveal_get_capacity) u32 wasm_text_reveal_get_capacity() { return text_reveal_get_capacity(); }
WASM_EXPORT(wasm_text_reveal_get_segment_capacity) u32 wasm_text_reveal_get_segment_capacity() { return text_reveal_get_segment_capacity(); }
WASM_EXPORT(wasm_text_reveal_get_never_flag) u32 wasm_text_reveal_get_never_flag() { return text_reveal_get_never_flag(); }
WASM_EXPORT(wasm_text_reveal_get_tilde_flag) u32 wasm_text_reveal_get_tilde_flag() { return text_reveal_get_tilde_flag(); }
WASM_EXPORT(wasm_text_reveal_evaluate) u32 wasm_text_reveal_evaluate(u32 count, double now) { return text_reveal_evaluate(count, now); }
WASM_EXPORT(wasm_text_reveal_map_owners) u32 wasm_text_reveal_map_owners(u32 layoutCount, u32 segmentCount, u32 segmentCodepointCount) {
    return text_reveal_map_owners(layoutCount, segmentCount, segmentCodepointCount);
}

} // namespace stockcut
