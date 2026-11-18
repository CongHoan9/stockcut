// ============================================================================
// CoreEngine - Nanosecond Timeline Engine & Staged Display Pipeline
// ----------------------------------------------------------------------------
// [STOCKFISH-STYLE STAGED DISPLAY GENERATION ARCHITECTURE]:
// 1. Phased Staged Execution (Học từ Stockfish MovePicker & generate<GenType>):
//    - Stage 1: GenType::EXACT_REUSE (Tương đương MAIN_TT)
//      0 loops, 0 memory writes, instant return hoặc O(1) TT blit.
//    - Stage 2: GenType::DIRTY_IN_PLACE (Tương đương QUIETS / vi sai)
//      Topology Z bất biến, chỉ vá in-place đúng các slot dirty (0 reset, 0 bucket scan).
//    - Stage 3: GenType::OCCLUSION_CULL (Tương đương EVASIONS / beta cutoff)
//      Screen 100% occluded by opaque blocker, culling 100% layer dưới và base bg.
//    - Stage 4: GenType::FULL_REBUILD (Tương đương NON_EVASIONS)
//      Front-to-Back Beta Cutoff Sweep + Back-to-Front Command Emit.
// 2. Self-Contained Display<Mask> struct: Hợp nhất toàn bộ logic generate vào struct
// 3. Compile-Time Specialization (generate<GenType, bool IsLive>)
// ============================================================================
#include "position.h"

namespace stockcut {

// 1. Các chế độ sinh Display List theo phân tầng nghiệp vụ Stockfish (GenType)
enum GenType : u8 {
    EXACT_REUSE = 0,    // Stage 1: Zobrist exact match hoặc Transposition Table Hit (0 loops, 0 rebuild)
    DIRTY_IN_PLACE = 1, // Stage 2: Topology Z không đổi, chỉ vá slot dirty in-place (0 reset, 0 bucket scan)
    OCCLUSION_CULL = 2, // Stage 3: Opaque Blocker che toàn màn hình, cull triệt để Z thấp hơn và base background
    FULL_REBUILD = 3    // Stage 4: Chuyển cảnh / seek / thay đổi topology slot: Sweep Front-to-Back + Emit Back-to-Front
};

enum class OcclusionCullingPolicy : u8 {
    Disabled = 0,
    FrontToBackBetaCutoff = 1
};

enum class BaseBackgroundPolicy : u8 {
    SkipIfFullyOccluded = 0,
    AlwaysEmit = 1
};

// ----------------------------------------------------------------------------
// Cấu trúc Display tự chủ theo Stockfish Data-Structure-Centric
// Hợp nhất toàn bộ luồng phát sinh lệnh generate<GenType, IsLive> trực tiếp vào Display!
// ----------------------------------------------------------------------------
template <typename Mask = Mask<1>>
struct Display {
    Mask lastActiveBB;
    Mask lastOccludedBB;
    u64 lastOpaqueTiles = 0ULL;
    u32 lastEpoch = INVALID_U32;
    u32 lastCmdCount = 0;
    u32 lastDlGen = 0;
    Key lastVisualExactKey = 0ULL;
    bool slotMapValid = false;
    Mask lastVisibleBB;
    MultiArray<u16, Mask::BITS> slotToCmd;
    u16 killerBlockerSlot = INVALID_U16; // Stockfish Killer Blocker Heuristic (O(1) Beta Cutoff)

    Display() {
        slotToCmd.fill(INVALID_U16);
    }

    inline void invalidate() noexcept {
        lastActiveBB.clear();
        lastOccludedBB.clear();
        lastOpaqueTiles = 0ULL;
        lastEpoch = INVALID_U32;
        lastCmdCount = 0;
        lastDlGen = 0;
        lastVisualExactKey = 0ULL;
        slotMapValid = false;
        lastVisibleBB.clear();
        slotToCmd.fill(INVALID_U16);
        killerBlockerSlot = INVALID_U16;
    }

    // ------------------------------------------------------------------------
    // Các phương thức generate chuyên biệt hóa theo từng Stage
    // ------------------------------------------------------------------------
    template <bool IsLive = true,
              OcclusionCullingPolicy Culling = OcclusionCullingPolicy::FrontToBackBetaCutoff,
              BaseBackgroundPolicy BackgroundPolicy = BaseBackgroundPolicy::SkipIfFullyOccluded>
    inline u32 generate_exact_reuse(
        Position<Mask>* pos,
        StateInfo<Mask>* targetSt,
        Mask active,
        u32 curEpoch) noexcept
    {
        (void)pos; (void)active; (void)curEpoch;
        targetSt->occludedBB = Bitboard(lastOpaqueTiles);
        targetSt->occludedSlotsBB = lastOccludedBB;
        targetSt->commandCount = lastCmdCount;
        targetSt->pad = lastDlGen;
        targetSt->dlReady = 1;
        return lastCmdCount;
    }

    template <bool IsLive = true,
              OcclusionCullingPolicy Culling = OcclusionCullingPolicy::FrontToBackBetaCutoff,
              BaseBackgroundPolicy BackgroundPolicy = BaseBackgroundPolicy::SkipIfFullyOccluded>
    inline u32 generate_dirty_in_place(
        Position<Mask>* pos,
        StateInfo<Mask>* targetSt,
        Mask active,
        u32 curEpoch) noexcept
    {
        (void)curEpoch;
        const Evaluated* evaluatedSlots = pos->evaluated_slots_arena();
        Mask dirty = targetSt->dirtyBB & active & lastVisibleBB & ~(targetSt->occludedSlotsBB | targetSt->pseudoCulledBB);
        dirty.for_each_active([&](u32 slot) {
            const u16 cmdIdx = slotToCmd[slot];
            if (cmdIdx != INVALID_U16) {
                DisplayCommand* cmd = display_list_get_command(cmdIdx);
                if (cmd) {
                    const Evaluated& ev = evaluatedSlots[slot];
                    cmd->x0 = ev.aabb.x0;
                    cmd->y0 = ev.aabb.y0;
                    cmd->x1 = ev.aabb.x1;
                    cmd->y1 = ev.aabb.y1;
                    cmd->opacity = ev.opacity;
                    cmd->flags = ev.flags;
                    cmd->resourceId = ev.resourceId;
                    cmd->filterSlot = static_cast<u16>(ev.filter);
                }
            }
        });
        if constexpr (IsLive) {
            lastVisualExactKey = targetSt->visualExactKey;
        }
        targetSt->occludedBB = Bitboard(lastOpaqueTiles);
        targetSt->occludedSlotsBB = lastOccludedBB;
        targetSt->commandCount = lastCmdCount;
        targetSt->pad = lastDlGen;
        targetSt->dlReady = 1;
        return lastCmdCount;
    }

    template <bool IsLive = true,
              OcclusionCullingPolicy Culling = OcclusionCullingPolicy::FrontToBackBetaCutoff,
              BaseBackgroundPolicy BackgroundPolicy = BaseBackgroundPolicy::SkipIfFullyOccluded>
    inline u32 generate_occlusion_cull(
        Position<Mask>* pos,
        StateInfo<Mask>* targetSt,
        Mask active,
        u32 curEpoch) noexcept
    {
        display_list_reset();
        targetSt->commandCount = 0;

        const Evaluated* evaluatedSlots = pos->evaluated_slots_arena();
        const u32 bucketCount = pos->z_bucket_count();
        const u64 accumulatedOpaqueTiles = ~0ULL;
        const Mask occludedSlotsBB = targetSt->occludedSlotsBB & active;
        targetSt->occludedBB = Bitboard(accumulatedOpaqueTiles);
        targetSt->occludedSlotsBB = occludedSlotsBB;
        const Mask visibleActive = active & ~(occludedSlotsBB | targetSt->pseudoCulledBB);

        if constexpr (IsLive) {
            lastVisibleBB.clear();
            slotToCmd.fill(INVALID_U16);
        }

        // 100% occluded: Bỏ qua hoàn toàn việc phát sinh Base Background
        for (u32 level = 0; level < bucketCount; ++level) {
            Mask activeAtLevel = pos->z_bucket(level) & visibleActive;
            if (!activeAtLevel) continue;
            activeAtLevel.for_each_active([&](u32 slot) {
                const Evaluated& ev = evaluatedSlots[slot];
                const u32 cmdIdx = display_list_get_count();
                const bool pushed = (ev.isVisible != 0) & (ev.opacity > OPACITY_EPSILON);
                targetSt->commandCount += display_list_push_if(
                    pushed,
                    ev.opCode, ev.flags, ev.resourceId,
                    ev.aabb.x0, ev.aabb.y0, ev.aabb.x1, ev.aabb.y1,
                    ev.opacity, ev.slot, static_cast<u16>(ev.filter)
                );
                if (pushed) {
                    slotToCmd[slot] = static_cast<u16>(cmdIdx);
                    if constexpr (IsLive) lastVisibleBB.set_bit(slot);
                }
            });
        }

        targetSt->pad = display_list_generation();
        targetSt->dlReady = 1;

        if constexpr (IsLive) {
            lastActiveBB = active;
            lastOccludedBB = occludedSlotsBB;
            lastOpaqueTiles = accumulatedOpaqueTiles;
            lastEpoch = curEpoch;
            lastCmdCount = targetSt->commandCount;
            lastVisualExactKey = targetSt->visualExactKey;
            lastDlGen = targetSt->pad;
            slotMapValid = true;
        }

        return targetSt->commandCount;
    }

    template <bool IsLive = true,
              OcclusionCullingPolicy Culling = OcclusionCullingPolicy::FrontToBackBetaCutoff,
              BaseBackgroundPolicy BackgroundPolicy = BaseBackgroundPolicy::SkipIfFullyOccluded>
    inline u32 generate_full_rebuild(
        Position<Mask>* pos,
        StateInfo<Mask>* targetSt,
        Mask active,
        u32 curEpoch) noexcept
    {
        display_list_reset();
        targetSt->commandCount = 0;

        const Evaluated* evaluatedSlots = pos->evaluated_slots_arena();
        const u32 bucketCount = pos->z_bucket_count();
        const float stageW = pos->stage_w();
        const float stageH = pos->stage_h();
        const Mask bgTypeBB = pos->template by_type_bb<MediaType::Background>();

        // PASS 1: FRONT-TO-BACK BETA-CUTOFF OCCLUSION SWEEP
        u64 accumulatedOpaqueTiles = targetSt->occludedBB.value;
        Mask occludedSlotsBB = targetSt->occludedSlotsBB & active;

        if constexpr (Culling == OcclusionCullingPolicy::FrontToBackBetaCutoff) {
            if (__builtin_expect(accumulatedOpaqueTiles == ~0ULL, 1)) {
                goto occlusion_sweep_done;
            }

            // [STOCKFISH KILLER BLOCKER HEURISTIC]:
            // Kiểm tra trước slot đã từng gây Full Beta Cutoff ở frame trước -> O(1) cutoff!
            if (killerBlockerSlot != INVALID_U16 && active.test_bit(killerBlockerSlot)) {
                const Evaluated& kev = evaluatedSlots[killerBlockerSlot];
                const bool isKillerOpaque = (kev.isVisible != 0) &
                    (kev.opacity >= OPACITY_OPAQUE_MIN) &
                    (kev.rotateDeg == 0.0f) &
                    (kev.filter == 0) &
                    (bgTypeBB.test_bit(killerBlockerSlot) |
                     ((kev.opCode == CMD_DRAW_RECT) & ((kev.resourceId >> 24) == INVALID_U8)));
                if (isKillerOpaque) {
                    const u64 killerTiles = Bitboard::FromAABBT<SpatialCoverage::ConservativeInner>(
                        kev.aabb.x0, kev.aabb.y0, kev.aabb.x1, kev.aabb.y1, stageW, stageH
                    ).value;
                    if (killerTiles == ~0ULL) {
                        accumulatedOpaqueTiles = ~0ULL;
                        occludedSlotsBB |= (active & pos->z_shadow_bb(killerBlockerSlot));
                        goto occlusion_sweep_done;
                    }
                }
            }

            const Mask dynVisual = active & targetSt->dynamicVisualBB;
            if (dynVisual) {
                for (i32 level = static_cast<i32>(bucketCount) - 1; level >= 0; --level) {
                    Mask activeAtLevel = pos->z_bucket(static_cast<u32>(level)) & dynVisual;
                    while (activeAtLevel) {
                        const u32 slot = activeAtLevel.pop_lsb();

                        const Evaluated& ev = evaluatedSlots[slot];
                        const bool isOpaque = (ev.isVisible != 0) &
                            (ev.opacity >= OPACITY_OPAQUE_MIN) &
                            (ev.rotateDeg == 0.0f) &
                            (ev.filter == 0) &
                            (bgTypeBB.test_bit(slot) |
                             ((ev.opCode == CMD_DRAW_RECT) & ((ev.resourceId >> 24) == INVALID_U8)));

                        const u64 opaqueMask = branchless_mask<u64>(isOpaque);
                        const u64 innerTiles = opaqueMask & Bitboard::FromAABBT<SpatialCoverage::ConservativeInner>(
                            ev.aabb.x0, ev.aabb.y0, ev.aabb.x1, ev.aabb.y1, stageW, stageH
                        ).value;
                        accumulatedOpaqueTiles |= innerTiles;
                        if (innerTiles == ~0ULL) {
                            killerBlockerSlot = static_cast<u16>(slot);
                            occludedSlotsBB |= (active & pos->z_shadow_bb(slot));
                            goto occlusion_sweep_done;
                        }
                    }
                }
            }
        }
    occlusion_sweep_done:

        targetSt->occludedBB = Bitboard(accumulatedOpaqueTiles);
        targetSt->occludedSlotsBB = occludedSlotsBB;
        const Mask visibleActive = active & ~(occludedSlotsBB | targetSt->pseudoCulledBB);

        // PASS 2: BACK-TO-FRONT COMMAND EMIT (CHUẨN OPENGL / WEBGL ALPHA BLEND)
        if constexpr (IsLive) {
            lastVisibleBB.clear();
            slotToCmd.fill(INVALID_U16);
        }

        constexpr i32 BASE_BACKGROUND_Z = -1;
        const bool omitBaseBg = (BackgroundPolicy == BaseBackgroundPolicy::SkipIfFullyOccluded) &&
                                (accumulatedOpaqueTiles == ~0ULL);
        bool baseBackgroundEmitted = omitBaseBg;

        for (u32 level = 0; level < bucketCount; ++level) {
            const bool atOrAboveBaseZ = (pos->z_bucket_level(level) >= BASE_BACKGROUND_Z);
            const bool emitBgNow = (!baseBackgroundEmitted) & atOrAboveBaseZ;
            targetSt->commandCount += display_list_push_if(
                emitBgNow,
                CMD_DRAW_RECT, 0, pos->active_root_color_packed(),
                0.0f, 0.0f, stageW, stageH,
                1.0f, ROOT_SLOT_SENTINEL, 0
            );
            baseBackgroundEmitted |= atOrAboveBaseZ;

            Mask activeAtLevel = pos->z_bucket(level) & visibleActive;
            activeAtLevel.for_each_active([&](u32 slot) {
                const Evaluated& ev = evaluatedSlots[slot];
                const u32 cmdIdx = display_list_get_count();
                const bool pushed = (ev.isVisible != 0) & (ev.opacity > OPACITY_EPSILON);
                targetSt->commandCount += display_list_push_if(
                    pushed,
                    ev.opCode, ev.flags, ev.resourceId,
                    ev.aabb.x0, ev.aabb.y0, ev.aabb.x1, ev.aabb.y1,
                    ev.opacity, ev.slot, static_cast<u16>(ev.filter)
                );
                if (pushed) {
                    slotToCmd[slot] = static_cast<u16>(cmdIdx);
                    if constexpr (IsLive) lastVisibleBB.set_bit(slot);
                }
            });
        }
        targetSt->commandCount += display_list_push_if(
            !baseBackgroundEmitted,
            CMD_DRAW_RECT, 0, pos->active_root_color_packed(),
            0.0f, 0.0f, stageW, stageH,
            1.0f, ROOT_SLOT_SENTINEL, 0
        );
        targetSt->pad = display_list_generation();
        targetSt->dlReady = 1;

        if constexpr (IsLive) {
            lastActiveBB = active;
            lastOccludedBB = occludedSlotsBB;
            lastOpaqueTiles = accumulatedOpaqueTiles;
            lastEpoch = curEpoch;
            lastCmdCount = targetSt->commandCount;
            lastVisualExactKey = targetSt->visualExactKey;
            lastDlGen = targetSt->pad;
            slotMapValid = true;
        }

        return targetSt->commandCount;
    }

    // ------------------------------------------------------------------------
    // Quyết định Stage phân tầng tương tự MovePicker trong Stockfish (O(1))
    // ------------------------------------------------------------------------
    template <bool IsLive = true>
    [[nodiscard]] [[gnu::always_inline]] inline GenType decide_stage(
        Position<Mask>* pos,
        StateInfo<Mask>* targetSt,
        Mask active,
        u32 curEpoch) const noexcept
    {
        if constexpr (IsLive) {
            // Stage 1: Exact Key Reuse
            if ((lastCmdCount > 0) &&
                (curEpoch == lastEpoch) &&
                (active == lastActiveBB) &&
                (targetSt->visualExactKey != 0ULL) &&
                (targetSt->visualExactKey == lastVisualExactKey) &&
                (lastDlGen == display_list_generation())) {
                return GenType::EXACT_REUSE;
            }

            // Stage 2: Dirty In-Place Patching (Topology Z và Occlusion không đổi)
            const bool canPatchInPlace = slotMapValid &&
                                         (lastCmdCount > 0) &&
                                         (curEpoch == lastEpoch) &&
                                         (active == lastActiveBB) &&
                                         (lastDlGen == display_list_generation()) &&
                                         (targetSt->occludedSlotsBB == lastOccludedBB) &&
                                         (targetSt->occludedBB.value == lastOpaqueTiles);
            if (canPatchInPlace) {
                bool visChanged = false;
                const Evaluated* evaluatedSlots = pos->evaluated_slots_arena();
                Mask dirty = targetSt->dirtyBB & active & ~(targetSt->occludedSlotsBB | targetSt->pseudoCulledBB);
                dirty.for_each_active([&](u32 slot) {
                    const bool isVis = (evaluatedSlots[slot].isVisible != 0);
                    const bool wasVis = lastVisibleBB.test_bit(slot);
                    visChanged |= (isVis ^ wasVis);
                });
                if (!visChanged) {
                    return GenType::DIRTY_IN_PLACE;
                }
            }
        }

        // Stage 3: Full Occlusion Cutoff
        if (targetSt->occludedBB.value == ~0ULL) {
            return GenType::OCCLUSION_CULL;
        }

        // Stage 4: Full Rebuild
        return GenType::FULL_REBUILD;
    }

    // ------------------------------------------------------------------------
    // Hợp nhất hàm generate<GenType, bool IsLive> trực tiếp vào Display<Mask>!
    // ------------------------------------------------------------------------
    template <GenType Stage, bool IsLive = true,
              OcclusionCullingPolicy Culling = OcclusionCullingPolicy::FrontToBackBetaCutoff,
              BaseBackgroundPolicy BackgroundPolicy = BaseBackgroundPolicy::SkipIfFullyOccluded>
    inline u32 generate(
        Position<Mask>* pos,
        StateInfo<Mask>* targetSt,
        Mask active,
        u32 curEpoch) noexcept
    {
        if constexpr (Stage == GenType::EXACT_REUSE)
            return generate_exact_reuse<IsLive, Culling, BackgroundPolicy>(pos, targetSt, active, curEpoch);
        else if constexpr (Stage == GenType::DIRTY_IN_PLACE)
            return generate_dirty_in_place<IsLive, Culling, BackgroundPolicy>(pos, targetSt, active, curEpoch);
        else if constexpr (Stage == GenType::OCCLUSION_CULL)
            return generate_occlusion_cull<IsLive, Culling, BackgroundPolicy>(pos, targetSt, active, curEpoch);
        else
            return generate_full_rebuild<IsLive, Culling, BackgroundPolicy>(pos, targetSt, active, curEpoch);
    }
};

// ----------------------------------------------------------------------------
// DisplayPipeline Điều Phối Chính (MovePicker-Style Stage Dispatcher)
// ----------------------------------------------------------------------------
template <typename Mask = Mask<1>,
          OcclusionCullingPolicy Culling = OcclusionCullingPolicy::FrontToBackBetaCutoff,
          BaseBackgroundPolicy BackgroundPolicy = BaseBackgroundPolicy::SkipIfFullyOccluded>
struct DisplayPipeline {
    static inline Display<Mask> s_display;

    template <bool IsLive = true>
    static inline u32 build(Position<Mask>* pos, StateInfo<Mask>* targetSt) {
        if (!pos || !targetSt) return 0;

        const Mask active = targetSt->activeBB;
        const u32 curEpoch = pos->configuration_epoch();

        const GenType stage = s_display.template decide_stage<IsLive>(pos, targetSt, active, curEpoch);

        switch (stage) {
            case GenType::EXACT_REUSE:
                return s_display.template generate<GenType::EXACT_REUSE, IsLive, Culling, BackgroundPolicy>(
                    pos, targetSt, active, curEpoch);

            case GenType::DIRTY_IN_PLACE:
                return s_display.template generate<GenType::DIRTY_IN_PLACE, IsLive, Culling, BackgroundPolicy>(
                    pos, targetSt, active, curEpoch);

            case GenType::OCCLUSION_CULL:
                return s_display.template generate<GenType::OCCLUSION_CULL, IsLive, Culling, BackgroundPolicy>(
                    pos, targetSt, active, curEpoch);

            case GenType::FULL_REBUILD:
            default:
                return s_display.template generate<GenType::FULL_REBUILD, IsLive, Culling, BackgroundPolicy>(
                    pos, targetSt, active, curEpoch);
        }
    }

    static inline void invalidate() noexcept {
        s_display.invalidate();
    }
};

template <typename Mask = Mask<1>>
using TimelineDisplayPipeline = DisplayPipeline<Mask,
    OcclusionCullingPolicy::FrontToBackBetaCutoff, BaseBackgroundPolicy::SkipIfFullyOccluded>;

template <typename Mask>
u32 Position<Mask>::generate_display_list(StateInfo<Mask>* targetSt) {
    if (targetSt && targetSt != state()) return generate_history_display_list(targetSt);
    const u32 count = TimelineDisplayPipeline<Mask>::template build<true>(this, targetSt);
    display_list_terminate_sentinel();
    return count;
}

// Keep the geometry snapshot and its stack space off the live playback path.
template <typename Mask>
[[gnu::noinline]] u32 Position<Mask>::generate_history_display_list(StateInfo<Mask>* targetSt) {
    // A history entry stores membership and time, but the evaluated geometry arena
    // belongs to the current position. Materialize the requested time for this draw.
    const auto currentSlots = evaluatedSlots;
    auto historical = *targetSt;
    historical.dirtyBB = historical.activeBB;
    evaluate_runtime_slots(historical);
    const u32 count = TimelineDisplayPipeline<Mask>::template build<false>(this, &historical);
    display_list_terminate_sentinel();
    evaluatedSlots = currentSlots;
    targetSt->commandCount = count;
    targetSt->pad = historical.pad;
    targetSt->dlReady = historical.dlReady;
    return count;
}

// Explicit template instantiations
template class Position<Mask<1>>;
template class Position<Mask<2>>;
template struct DisplayPipeline<Mask<1>>;
template struct DisplayPipeline<Mask<2>>;

void timeline_position_display_list_invalidate() {
    TimelineDisplayPipeline<Mask<1>>::invalidate();
    TimelineDisplayPipeline<Mask<2>>::invalidate();
}

} // namespace stockcut
