// ============================================================================
// CoreEngine - Direct QuadCommand (64 B) & Evaluated -> GPU Stream
// ----------------------------------------------------------------------------
// Eliminates intermediate DisplayCommand -> DrawCommand (152 B) -> RenderCommand
// (264 B) copies and duplicate 1.62 MB cmdTT_ cache. Reads Evaluated
// directly from unified Position<Mask> in Z-bucket order.
// ============================================================================
#pragma once

#include "types.h"
#include "data_structures.h"
#include "math_utils.h"
#include "core/display_list.h"
#include "timeline/position.h"
#include <cmath>

namespace stockcut {

constexpr u32 QUAD_MASK_KIND_BITS    = 0xFu;
constexpr u32 QUAD_MASK_RADIUS_SHIFT = 4u;
constexpr u32 QUAD_MASK_RADIUS_MASK  = 0xFFFu;

struct alignas(64) QuadCommand {
    float quadX;        // 0..3:   Viewport quad X
    float quadY;        // 4..7:   Viewport quad Y
    float quadW;        // 8..11:  Viewport quad width
    float quadH;        // 12..15: Viewport quad height
    float pivotX;       // 16..19: Rotation pivot X (viewport px)
    float pivotY;       // 20..23: Rotation pivot Y (viewport px)
    float rotRad;       // 24..27: Rotation angle (radians)
    float spinRad;      // 28..31: Spin rate/angle (radians)
    float red;          // 32..35: Premul red   (or mask hole ratio)
    float green;        // 36..39: Premul green (or mask circle flag)
    float blue;         // 40..43: Premul blue
    float alpha;        // 44..47: Premul alpha (or opacity for masked)
    u32   textureId;    // 48..51: Bound OpenGL texture ID or glyph atlas
    u16   slot;         // 52..53: Position slot (0..63) or ROOT_SLOT_SENTINEL
    u16   resourceSlot; // 54..55: CachedGlyphRun index (1..64)
    u8    opCode;       // 56:     CMD_DRAW_*
    u8    renderMode;   // 57:     QUAD_MODE_*
    u8    revealMode;   // 58:     Text reveal mode (0..3)
    u8    visible;      // 59:     Viewport visibility flag
    i32   revealRelMs;  // 60..63: Elapsed ms or permille for text reveal

    [[nodiscard]] inline Rect<float> viewport_aabb() const noexcept {
        if (__builtin_expect(rotRad != 0.0f, 0)) {
            const float radius = (quadW + quadH) * 0.5f; // Manhattan Pseudo-Radius Bound O(1) - Zero sqrt
            return {pivotX - radius, pivotY - radius, pivotX + radius, pivotY + radius};
        }
        return {quadX, quadY, quadX + quadW, quadY + quadH};
    }
};
static_assert(sizeof(QuadCommand) == 64, "QuadCommand must be exactly 1 L1 cacheline (64 bytes)");

class alignas(64) QuadStream {
public:
    static constexpr u32 CAPACITY = MAX_DISPLAY_COMMANDS;

    using SlotMask = Mask<((MAX_TIMELINE_SLOTS + 63u) / 64u)>;

    inline void reset() noexcept {
        count_ = 0;
        culledCount_ = 0;
        reuseValid_ = false;
        cmds_[0].opCode = static_cast<u8>(CMD_SENTINEL_END);
        cmds_[0].slot = ROOT_SLOT_SENTINEL;
        cmds_[0].quadW = 0.0f;
        cmds_[0].quadH = 0.0f;
    }

    [[nodiscard]] inline bool has_sentinel() const noexcept {
        return cmds_[count_].opCode == static_cast<u8>(CMD_SENTINEL_END);
    }

    [[nodiscard]] inline u32 count() const noexcept { return count_; }
    [[nodiscard]] inline u32 culled_count() const noexcept { return culledCount_; }
    [[nodiscard]] inline QuadCommand* data() noexcept { return cmds_.data(); }
    [[nodiscard]] inline const QuadCommand* data() const noexcept { return cmds_.data(); }
    [[nodiscard]] inline QuadCommand& operator[](u32 idx) noexcept { return cmds_[idx]; }
    [[nodiscard]] inline const QuadCommand& operator[](u32 idx) const noexcept { return cmds_[idx]; }

    [[nodiscard]] inline u32 compute_static_key(
        u32 prefixCount,
        float bgR, float bgG, float bgB, float bgA
    ) const noexcept {
        const u32 safeCount = prefixCount < count_ ? prefixCount : count_;
        const auto quantByte = [](float value) noexcept -> u32 {
            const float clamped = clamp(value * 255.0f, 0.0f, 255.0f);
            return static_cast<u32>(clamped);
        };
        u32 hash = FNV1A_32_OFFSET ^
            (quantByte(bgR) << 16) ^ (quantByte(bgG) << 8) ^
            quantByte(bgB) ^ (quantByte(bgA) << 24);
        const auto mix = [&hash](float value, float scale) noexcept {
            const float scaled = value * scale;
            const float bounded = clamp(scaled, -2147483640.0f, 2147483640.0f);
            hash = (hash ^ static_cast<u32>(static_cast<i32>(bounded))) * FNV1A_32_PRIME;
        };
        u32 idx = 0;
        for (const QuadCommand* cmd = cmds_.data(); cmd->opCode != static_cast<u8>(CMD_SENTINEL_END) && idx < safeCount; ++cmd, ++idx) {
            mix(cmd->quadX, 16.0f);
            mix(cmd->quadY, 16.0f);
            mix(cmd->quadW, 16.0f);
            mix(cmd->quadH, 16.0f);
            mix(cmd->rotRad, 1024.0f);
            mix(cmd->spinRad, 1024.0f);
            mix(cmd->red, 255.0f);
            mix(cmd->green, 255.0f);
            mix(cmd->blue, 255.0f);
            mix(cmd->alpha, 255.0f);
            hash = (hash ^ static_cast<u32>(cmd->renderMode)) * FNV1A_32_PRIME;
            hash = (hash ^ cmd->textureId) * FNV1A_32_PRIME;
            hash = (hash ^ static_cast<u32>(cmd->slot)) * FNV1A_32_PRIME;
        }
        return hash;
    }

    template <bool HasState = true, typename Mask, typename TextBinderFn, typename TexLookupFn>
    inline u32 build_from_position(
        Position<Mask>& pos,
        const StateInfo<Mask>* st,
        u32 width,
        u32 height,
        double nowMs,
        TextBinderFn&& textBinder,
        TexLookupFn&& texLookup
    ) noexcept {
        if (width == 0u || height == 0u) {
            reset();
            return 0u;
        }

        const Mask active = [st, &pos]() {
            if constexpr (HasState) {
                assert(st != nullptr);
                return st->activeBB;
            } else {
                return (st != nullptr) ? st->activeBB : pos.active_items();
            }
        }();
        const Mask rawOccludedSlotsBB = [st]() {
            if constexpr (HasState) {
                assert(st != nullptr);
                return st->occludedSlotsBB;
            } else {
                return (st != nullptr) ? st->occludedSlotsBB : Mask{};
            }
        }();
        const u64 rawUpperOpaqueTiles = [st]() {
            if constexpr (HasState) {
                assert(st != nullptr);
                return st->occludedBB.value;
            } else {
                return (st != nullptr) ? st->occludedBB.value : 0ULL;
            }
        }();
        const Mask rawOccludedActive = rawOccludedSlotsBB & active;
        const u32 curTopologyEpoch = pos.topology_epoch();
        const u32 curRootColor = pos.active_root_color_packed();
        const Mask textOrLyricBB = pos.template by_type_bb<MediaType::Text>() | pos.template by_type_bb<MediaType::Lyric>();
        const SlotMask activeSlotMask = to_slot_mask(active);
        const SlotMask rawOccSlotMask = to_slot_mask(rawOccludedActive);
        const SlotMask textActiveMask = to_slot_mask(active & textOrLyricBB);

        const bool sameHeaderTopology =
            reuseValid_ &&
            (HasState ? true : (st != nullptr)) &&
            (&pos == lastPosPtr_) &&
            (curTopologyEpoch == lastTopologyEpoch_) &&
            (width == lastWidth_) &&
            (height == lastHeight_) &&
            (curRootColor == lastRootColor_) &&
            (activeSlotMask == lastActiveBB_) &&
            (rawOccSlotMask == lastRawOccludedBB_) &&
            (rawUpperOpaqueTiles == lastRawUpperOpaqueTiles_);

        if (__builtin_expect(sameHeaderTopology, 1)) {
            // Stage 1 (EXACT_REUSE): Zero loops, zero float divisions & zero writes when no slot is dirty and visual state key matches.
            if (!st->dirtyBB.any() &&
                st->visualExactKey != 0ULL &&
                st->visualExactKey == lastVisualExactKey_ &&
                !textActiveMask.any()) {
                return count_;
            }
        }

        const float widthF = static_cast<float>(width);
        const float heightF = static_cast<float>(height);
        const float baseW = pos.stage_w() > 0.0f ? pos.stage_w() : widthF;
        const float baseH = pos.stage_h() > 0.0f ? pos.stage_h() : heightF;
        const float sx = widthF / baseW;
        const float sy = heightF / baseH;
        const TimePoint nowTp = static_cast<TimePoint>(nowMs);
        const Evaluated* evaluatedSlots = pos.evaluated_slots_arena();
        const Mask visualMask = [st, &active, &pos]() {
            if constexpr (HasState) {
                assert(st != nullptr);
                return active & pos.eval_visible_bb();
            } else {
                return (st != nullptr) ? (active & pos.eval_visible_bb()) : active;
            }
        }();
        const Mask customOriginMask = pos.custom_origin_slots();

        const auto resolve_quad_step1 = [&](
            u32 slot,
            const Evaluated& ev,
            float& quadX, float& quadY, float& quadW, float& quadH,
            float& pivotX, float& pivotY, float& rotRad,
            u16& glyphSlot, u32& atlasTex
        ) noexcept -> bool {
            Rect<float> box = ev.aabb;
            glyphSlot = 0u;
            atlasTex = 0u;
            if (ev.opCode == CMD_DRAW_TEXT) {
                textBinder(slot, sx, glyphSlot, atlasTex);
                if (pos.mode_of(slot) != TrackMode::Kinematic) {
                    box = pos.evaluated_slot(slot).aabb;
                }
            }
            quadX = box.x0 * sx;
            quadY = box.y0 * sy;
            quadW = stockcut::max(0.0f, (box.x1 - box.x0) * sx);
            quadH = stockcut::max(0.0f, (box.y1 - box.y0) * sy);

            float ox = 0.0f, oy = 0.0f;
            if (customOriginMask.test_bit(slot) && pos.get_slot_custom_origin(slot, ox, oy)) {
                pivotX = ox * sx;
                pivotY = oy * sy;
            } else {
                pivotX = quadX + quadW * 0.5f;
                pivotY = quadY + quadH * 0.5f;
            }

            float rotDeg = ev.rotateDeg;
            if ((ev.opCode == CMD_DRAW_IMAGE || ev.opCode == CMD_DRAW_VIDEO) && rotDeg == 0.0f) {
                rotDeg = static_cast<float>(ev.flags);
            }
            rotRad = rotDeg * DEG_TO_RAD_F32;

            Rect<float> aabb;
            if (__builtin_expect(rotRad != 0.0f, 0)) {
                const float maxDx = stockcut::max(std::abs(quadX - pivotX), std::abs(quadX + quadW - pivotX));
                const float maxDy = stockcut::max(std::abs(quadY - pivotY), std::abs(quadY + quadH - pivotY));
                const float radius = sqrt_f32(maxDx * maxDx + maxDy * maxDy);
                aabb = {pivotX - radius, pivotY - radius, pivotX + radius, pivotY + radius};
            } else {
                aabb = {quadX, quadY, quadX + quadW, quadY + quadH};
            }
            return (aabb.x1 > 0.0f && aabb.x0 < widthF && aabb.y1 > 0.0f && aabb.y0 < heightF);
        };

        const auto commit_quad_sink = [](QuadCommand& cmd, u32 slot) noexcept {
            cmd.quadX = 0.0f;
            cmd.quadY = 0.0f;
            cmd.quadW = 0.0f;
            cmd.quadH = 0.0f;
            cmd.pivotX = 0.0f;
            cmd.pivotY = 0.0f;
            cmd.rotRad = 0.0f;
            cmd.spinRad = 0.0f;
            cmd.red = 0.0f;
            cmd.green = 0.0f;
            cmd.blue = 0.0f;
            cmd.alpha = 0.0f;
            cmd.textureId = 0u;
            cmd.slot = static_cast<u16>(slot);
            cmd.resourceSlot = 0u;
            cmd.opCode = CMD_DRAW_RECT;
            cmd.renderMode = QUAD_MODE_COLOR;
            cmd.revealMode = 0u;
            cmd.visible = 0u;
            cmd.revealRelMs = 0;
        };

        const auto commit_quad_step2 = [&](
            QuadCommand& cmd,
            u32 slot,
            const Evaluated& ev,
            float quadX, float quadY, float quadW, float quadH,
            float pivotX, float pivotY, float rotRad,
            u16 glyphSlot, u32 atlasTex
        ) noexcept {
            cmd.quadX = quadX;
            cmd.quadY = quadY;
            cmd.quadW = quadW;
            cmd.quadH = quadH;
            cmd.pivotX = pivotX;
            cmd.pivotY = pivotY;
            cmd.rotRad = rotRad;
            cmd.spinRad = 0.0f;
            cmd.opCode = static_cast<u8>(ev.opCode);
            cmd.slot = static_cast<u16>(slot);
            cmd.visible = 1u;

            if (ev.opCode == CMD_DRAW_TEXT) {
                cmd.resourceSlot = glyphSlot;
                cmd.textureId = atlasTex;
                cmd.renderMode = QUAD_MODE_TEXT;
                cmd.revealMode = pos.slot_block(slot).template get_value<u8, text_bit::REVEAL_MODE_0, 4>();
                cmd.revealRelMs = static_cast<i32>(nowMs - static_cast<double>(pos.item(slot).startMs));
                const ColorRGBA pm = pos.resolve_slot_color(slot, nowTp).premultiply_alpha(ev.opacity);
                cmd.red = pm.r; cmd.green = pm.g; cmd.blue = pm.b; cmd.alpha = pm.a;
                const u32 rawBg = pos.resolve_text_bg_color(slot);
                if ((rawBg >> 24) != 0u) {
                    ColorRGBA bg = ColorRGBA::from_packed(rawBg);
                    bg.a *= ev.opacity;
                    cmd.spinRad = std::bit_cast<float>(bg.to_packed());
                }
            } else if (ev.opCode == CMD_DRAW_IMAGE || ev.opCode == CMD_DRAW_VIDEO) {
                cmd.textureId = texLookup(slot, ev.resourceId);
                cmd.resourceSlot = 0u;
                cmd.revealMode = 0u;
                cmd.revealRelMs = 0;
                if (ev.opCode == CMD_DRAW_IMAGE && (ev.filter & QUAD_MASK_KIND_BITS) != 0u) {
                    cmd.renderMode = QUAD_MODE_MASKED_TEXTURE;
                    cmd.red = static_cast<float>((ev.filter >> QUAD_MASK_RADIUS_SHIFT) & QUAD_MASK_RADIUS_MASK) /
                              static_cast<float>(PERMILLE_MAX);
                    cmd.green = (ev.filter & ITEM_FLAG_MASK_CIRCLE) ? 1.0f : 0.0f;
                    cmd.blue = 1.0f;
                    cmd.alpha = ev.opacity;
                } else {
                    cmd.renderMode = QUAD_MODE_TEXTURE;
                    cmd.red = 1.0f; cmd.green = 1.0f; cmd.blue = 1.0f; cmd.alpha = ev.opacity;
                }
            } else if (ev.opCode == CMD_DRAW_PATH) {
                cmd.textureId = texLookup(slot, 0u);
                cmd.renderMode = (cmd.textureId != 0u) ? QUAD_MODE_TEXTURE : QUAD_MODE_COLOR;
                cmd.resourceSlot = 0u;
                cmd.revealMode = 0u;
                cmd.revealRelMs = 0;
                const ColorRGBA pm = pos.resolve_slot_color(slot, nowTp).premultiply_alpha(ev.opacity);
                cmd.red = pm.r; cmd.green = pm.g; cmd.blue = pm.b; cmd.alpha = pm.a;
            } else {
                cmd.renderMode = QUAD_MODE_COLOR;
                cmd.textureId = 0u;
                cmd.resourceSlot = 0u;
                cmd.revealMode = 0u;
                cmd.revealRelMs = 0;
                const ColorRGBA pm = pos.resolve_slot_color(slot, nowTp).premultiply_alpha(ev.opacity);
                cmd.red = pm.r; cmd.green = pm.g; cmd.blue = pm.b; cmd.alpha = pm.a;
            }
        };

        const Mask bgTypeBB = pos.template by_type_bb<MediaType::Background>();
        const Mask opaqueCandidateBB = visualMask & bgTypeBB;
        const SlotMask dirtySlotMask = [st]() {
            if constexpr (HasState) {
                assert(st != nullptr);
                return to_slot_mask(st->dirtyBB);
            } else {
                return (st != nullptr) ? to_slot_mask(st->dirtyBB) : SlotMask{};
            }
        }();
        constexpr u32 kDrawableOpMask =
            (1u << CMD_DRAW_RECT) | (1u << CMD_DRAW_TEXT) |
            (1u << CMD_DRAW_IMAGE) | (1u << CMD_DRAW_VIDEO) | (1u << CMD_DRAW_PATH);

        if (__builtin_expect(sameHeaderTopology, 1)) {
            // Stage 2 (DIRTY_IN_PLACE): Patch dirty visual slots in-place via Sentinel Sink
            const SlotMask resolvedVisMask = to_slot_mask(visualMask) & ~lastResolvedOccludedBB_;
            const SlotMask dirtyOpaqueCheck = to_slot_mask(st->dirtyBB & opaqueCandidateBB);
            const bool unallocatedBecameVisible =
                ((dirtySlotMask | textActiveMask) & resolvedVisMask & ~lastEmittedBB_).any();



            if (!dirtyOpaqueCheck.any() && !unallocatedBecameVisible) {
                SlotMask patchMask = (dirtySlotMask | textActiveMask) & lastEmittedBB_;
                bool fallbackFull = false;
                QuadCommand* const outCmds = cmds_.data();

                patchMask.for_each_active([&](u32 slot) {
                    if (fallbackFull) return;
                    const Evaluated& ev = evaluatedSlots[slot];
                    const bool isDrawable = (ev.opCode < 32u) && ((kDrawableOpMask & (1u << ev.opCode)) != 0u);
                    const bool preVis = (ev.isVisible != 0u) & (ev.opacity > OPACITY_EPSILON) & isDrawable;
                    const bool wasEmitted = lastEmittedBB_.test_bit(slot);

                    if (!preVis) {
                        if (wasEmitted) {
                            const u16 cmdIdx = slotToCmd_[slot];
                            if (cmdIdx < count_) {
                                commit_quad_sink(outCmds[cmdIdx], slot);
                            } else {
                                fallbackFull = true;
                            }
                        }
                        return;
                    }
                    float qX = 0.0f, qY = 0.0f, qW = 0.0f, qH = 0.0f, pX = 0.0f, pY = 0.0f, rot = 0.0f;
                    u16 gSlot = 0u;
                    u32 aTex = 0u;
                    const bool inFrustum = resolve_quad_step1(slot, ev, qX, qY, qW, qH, pX, pY, rot, gSlot, aTex);
                    if (!inFrustum) {
                        if (wasEmitted) {
                            const u16 cmdIdx = slotToCmd_[slot];
                            if (cmdIdx < count_) {
                                commit_quad_sink(outCmds[cmdIdx], slot);
                            } else {
                                fallbackFull = true;
                            }
                        }
                        return;
                    }
                    if (!wasEmitted) {
                        fallbackFull = true;
                        return;
                    }
                    const u16 cmdIdx = slotToCmd_[slot];
                    if (cmdIdx >= count_) {
                        fallbackFull = true;
                        return;
                    }
                    commit_quad_step2(outCmds[cmdIdx], slot, ev, qX, qY, qW, qH, pX, pY, rot, gSlot, aTex);
                });
                if (!fallbackFull) {
                    lastVisualExactKey_ = st->visualExactKey;
                    lastResolvedVisibleBB_ = resolvedVisMask;
                    outCmds[count_].opCode = static_cast<u8>(CMD_SENTINEL_END);
                    outCmds[count_].slot = ROOT_SLOT_SENTINEL;
                    outCmds[count_].quadW = 0.0f;
                    outCmds[count_].quadH = 0.0f;
                    return count_;
                }
            }
        }

        // Stage 3/4: Full Rebuild
        count_ = 0u;
        culledCount_ = 0u;
        u64 upperOpaqueTiles = rawUpperOpaqueTiles;
        Mask occludedSlotsBB = rawOccludedActive;
        const u32 bucketCount = pos.z_bucket_count();
        const float stageW = pos.stage_w();
        const float stageH = pos.stage_h();

        // Multi-layer composite occlusion sweep (top-to-bottom):
        // Fast Gatekeeper skips the sweep in O(1) when neither prebaked occlusion nor opaque candidates are active.
        if (upperOpaqueTiles != ~0ULL && (upperOpaqueTiles != 0ULL || opaqueCandidateBB.any())) {
            bool resolvedByFastBg = false;
            if (upperOpaqueTiles == 0ULL) {
                bool allFullscreenOpaque = true;
                Mask bgCheck = opaqueCandidateBB & ~occludedSlotsBB;
                Mask shadowUnion{};
                while (bgCheck) {
                    const u32 bgSlot = bgCheck.pop_lsb();
                    const Evaluated& ev = evaluatedSlots[bgSlot];
                    if (ev.isVisible == 0u || ev.opacity <= OPACITY_EPSILON) continue;
                    if (ev.opacity >= OPACITY_OPAQUE_MIN &&
                        ev.rotateDeg == 0.0f &&
                        ev.filter == 0u &&
                        ev.aabb.x0 <= 0.0f && ev.aabb.y0 <= 0.0f &&
                        ev.aabb.x1 >= stageW && ev.aabb.y1 >= stageH) {
                        upperOpaqueTiles = ~0ULL;
                        shadowUnion |= pos.z_shadow_bb(bgSlot);
                    } else {
                        allFullscreenOpaque = false;
                        break;
                    }
                }
                if (allFullscreenOpaque) {
                    occludedSlotsBB |= (active & shadowUnion);
                    resolvedByFastBg = true;
                }
            }
            if (!resolvedByFastBg) {
                for (i32 level = static_cast<i32>(bucketCount) - 1; level >= 0; --level) {
                    const Mask levelScope = (upperOpaqueTiles != 0ULL) ? visualMask : (visualMask & bgTypeBB);
                    Mask activeAtLevel = pos.z_bucket(static_cast<u32>(level)) & levelScope & ~occludedSlotsBB;
                    u64 levelOpaqueTiles = 0ULL;
                    while (activeAtLevel) {
                        const u32 slot = activeAtLevel.pop_lsb();
                        const Evaluated& ev = evaluatedSlots[slot];
                        if (ev.isVisible == 0u || ev.opacity <= OPACITY_EPSILON) continue;
                        const bool isOpaque =
                            (ev.opacity >= OPACITY_OPAQUE_MIN) &
                            (ev.rotateDeg == 0.0f) &
                            (ev.filter == 0u) &
                            (bgTypeBB.test_bit(slot) |
                             ((ev.opCode == CMD_DRAW_RECT) & ((ev.resourceId >> 24) == INVALID_U8)));

                        if (upperOpaqueTiles != 0ULL) {
                            u64 slotTiles;
                            if (__builtin_expect(ev.rotateDeg != 0.0f, 0)) {
                                const float rdx = (ev.aabb.x1 - ev.aabb.x0) * 0.5f;
                                const float rdy = (ev.aabb.y1 - ev.aabb.y0) * 0.5f;
                                const float rad = sqrt_f32(rdx * rdx + rdy * rdy);
                                const float cx = (ev.aabb.x0 + ev.aabb.x1) * 0.5f;
                                const float cy = (ev.aabb.y0 + ev.aabb.y1) * 0.5f;
                                slotTiles = Bitboard::FromAABBT<Bitboard::SpatialCoverage::ExactAABB>(
                                    cx - rad, cy - rad, cx + rad, cy + rad, stageW, stageH).value;
                            } else {
                                slotTiles = Bitboard::FromAABBT<Bitboard::SpatialCoverage::ExactAABB>(
                                    ev.aabb.x0, ev.aabb.y0, ev.aabb.x1, ev.aabb.y1, stageW, stageH).value;
                            }
                            if ((slotTiles & ~upperOpaqueTiles) == 0ULL) {
                                occludedSlotsBB.set_bit(slot);
                                continue;
                            }
                        }

                        if (isOpaque) {
                            levelOpaqueTiles |= Bitboard::FromAABBT<Bitboard::SpatialCoverage::ConservativeInner>(
                                ev.aabb.x0, ev.aabb.y0, ev.aabb.x1, ev.aabb.y1, stageW, stageH).value;
                        }
                    }
                    upperOpaqueTiles |= levelOpaqueTiles;
                    if (upperOpaqueTiles == ~0ULL) {
                        for (i32 rem = level - 1; rem >= 0; --rem) {
                            occludedSlotsBB |= (active & pos.z_bucket(static_cast<u32>(rem)));
                        }
                        break;
                    }
                }
            }
        }

        const Mask visibleActive = visualMask & ~occludedSlotsBB;
        bool baseBackgroundEmitted = (upperOpaqueTiles == ~0ULL);
        QuadCommand* const outCmds = cmds_.data();
        u32 outCount = 0u;
        u32 culled = 0u;
        SlotMask emittedSlotsBB{};

        const auto emitRootBg = [&]() noexcept {
            if (outCount >= CAPACITY) return;
            const ColorRGBA bgCol = pos.active_root_color().premultiply_alpha(1.0f);
            QuadCommand& bg = outCmds[outCount++];
            bg.quadX = 0.0f;
            bg.quadY = 0.0f;
            bg.quadW = widthF;
            bg.quadH = heightF;
            bg.pivotX = widthF * 0.5f;
            bg.pivotY = heightF * 0.5f;
            bg.rotRad = 0.0f;
            bg.spinRad = 0.0f;
            bg.red = bgCol.r;
            bg.green = bgCol.g;
            bg.blue = bgCol.b;
            bg.alpha = bgCol.a;
            bg.textureId = 0u;
            bg.slot = ROOT_SLOT_SENTINEL;
            bg.resourceSlot = 0u;
            bg.opCode = CMD_DRAW_RECT;
            bg.renderMode = QUAD_MODE_COLOR;
            bg.revealMode = 0u;
            bg.visible = 1u;
            bg.revealRelMs = 0;
        };

        const auto emitVisibleSlot = [&](u32 slot) noexcept {
            const Evaluated& ev = evaluatedSlots[slot];
            if (ev.isVisible == 0u || ev.opacity <= OPACITY_EPSILON) return;
            if (ev.opCode >= 32u || (kDrawableOpMask & (1u << ev.opCode)) == 0u) return;

            float quadX = 0.0f, quadY = 0.0f, quadW = 0.0f, quadH = 0.0f;
            float pivotX = 0.0f, pivotY = 0.0f, rotRad = 0.0f;
            u16 glyphSlot = 0u;
            u32 atlasTex = 0u;
            if (!resolve_quad_step1(slot, ev, quadX, quadY, quadW, quadH, pivotX, pivotY, rotRad, glyphSlot, atlasTex)) {
                ++culled;
                return;
            }

            commit_quad_step2(outCmds[outCount], slot, ev, quadX, quadY, quadW, quadH, pivotX, pivotY, rotRad, glyphSlot, atlasTex);
            if (outCount < CAPACITY) {
                if (slot < MAX_TIMELINE_SLOTS) {
                    slotToCmd_[slot] = static_cast<u16>(outCount);
                    emittedSlotsBB.set_bit(slot);
                }
                ++outCount;
            }
        };

        if (bucketCount <= 4u) {
            constexpr i32 BASE_BACKGROUND_Z = -1;
            for (u32 level = 0u; level < bucketCount; ++level) {
                if (!baseBackgroundEmitted && pos.z_bucket_level(level) >= BASE_BACKGROUND_Z) {
                    emitRootBg();
                    baseBackgroundEmitted = true;
                }
                Mask activeAtLevel = pos.z_bucket(level) & visibleActive;
                activeAtLevel.for_each_active(emitVisibleSlot);
            }
        } else {
            u64 zRankMask = 0ULL;
            visibleActive.for_each_active([&](u32 s) {
                zRankMask |= (1ULL << pos.slot_to_z_rank(s));
            });
            const u32 bgInsertRank = pos.base_bg_insert_rank();
            while (zRankMask) {
                const u32 rank = static_cast<u32>(__builtin_ctzll(zRankMask));
                zRankMask &= (zRankMask - 1ULL);
                if (!baseBackgroundEmitted && rank >= bgInsertRank) {
                    emitRootBg();
                    baseBackgroundEmitted = true;
                }
                emitVisibleSlot(pos.z_rank_to_slot(rank));
            }
        }

        if (!baseBackgroundEmitted) {
            emitRootBg();
        }
        outCmds[outCount].opCode = static_cast<u8>(CMD_SENTINEL_END);
        outCmds[outCount].slot = ROOT_SLOT_SENTINEL;
        outCmds[outCount].quadW = 0.0f;
        outCmds[outCount].quadH = 0.0f;
        count_ = outCount;
        culledCount_ = culled;

        if (st != nullptr) {
            reuseValid_ = true;
            lastPosPtr_ = &pos;
            lastTopologyEpoch_ = curTopologyEpoch;
            lastWidth_ = width;
            lastHeight_ = height;
            lastRootColor_ = curRootColor;
            lastVisualExactKey_ = st->visualExactKey;
            lastActiveBB_ = activeSlotMask;
            lastRawOccludedBB_ = rawOccSlotMask;
            lastRawUpperOpaqueTiles_ = rawUpperOpaqueTiles;
            lastResolvedOccludedBB_ = to_slot_mask(occludedSlotsBB);
            lastResolvedVisibleBB_ = to_slot_mask(visibleActive);
            lastEmittedBB_ = emittedSlotsBB;
        } else {
            reuseValid_ = false;
        }
        return outCount;
    }

private:
    template <typename SrcMask>
    static inline SlotMask to_slot_mask(const SrcMask& src) noexcept {
        SlotMask dst;
        constexpr usize minWords = (SrcMask::BLOCK_COUNT < SlotMask::BLOCK_COUNT) ? SrcMask::BLOCK_COUNT : SlotMask::BLOCK_COUNT;
        for (usize b = 0; b < minWords; ++b) {
            dst.words[b] = src.word(b);
        }
        return dst;
    }

    Sentinel<QuadCommand, CAPACITY, 0, 1> cmds_;
    MultiArray<u16, MAX_TIMELINE_SLOTS> slotToCmd_;
    u32 count_ = 0;
    u32 culledCount_ = 0;
    bool reuseValid_ = false;
    const void* lastPosPtr_ = nullptr;
    u32 lastTopologyEpoch_ = INVALID_U32;
    u32 lastWidth_ = 0u;
    u32 lastHeight_ = 0u;
    u32 lastRootColor_ = 0u;
    Key lastVisualExactKey_ = 0ULL;
    SlotMask lastActiveBB_{};
    SlotMask lastRawOccludedBB_{};
    u64 lastRawUpperOpaqueTiles_ = 0ULL;
    SlotMask lastResolvedOccludedBB_{};
    SlotMask lastResolvedVisibleBB_{};
    SlotMask lastEmittedBB_{};
};

} // namespace stockcut
