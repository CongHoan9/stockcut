// ============================================================================
// CoreEngine - CutProject Coordinator Implementation (Stockfish Position Heritage)
// ============================================================================
#include "cut_project.h"
#include "manager.h"
#include "templates.h"
#include "string_parse.h"
#include "timeline/project/json_reader.h"
#include "timeline/project/json_buffer.h"
#include "property/property_types.h"
#include <cstring>

namespace stockcut {
namespace {
constexpr u32 CONFIG_SNAPSHOT_BUF_BYTES = 1024u;
constexpr u32 TEMPLATE_DIRTY_BIT        = 2u;

inline bool isReservedTemplateKey(const char* key) noexcept {
    return cstr_equal(key, "name") || cstr_equal(key, "id") ||
           cstr_equal(key, "class") || cstr_equal(key, "type") ||
           cstr_equal(key, "parent");
}
} // namespace

CutProject& CutProject::instance() noexcept {
    static CutProject s_instance;
    return s_instance;
}

bool CutProject::can_undo() const noexcept {
    return ProjectHistory::instance().can_undo();
}

bool CutProject::can_redo() const noexcept {
    return ProjectHistory::instance().can_redo();
}

template<>
bool CutProject::do_edit<CONFIG>(const char* payload, u32 len, HistoryDirties* outDirty) noexcept {
    if (!payload || len == 0u) return false;

    ConfigOptionsMap& cfg = engineManager().config_options();
    if (!cfg.load_json(payload, len)) return false;

    UndoNode* newSt = ProjectHistory::instance().allocate_step<CONFIG>();
    MultiArray<char, CONFIG_SNAPSHOT_BUF_BYTES> snapBuf;
    const u32 sLen = cfg.to_json(snapBuf.data(), CONFIG_SNAPSHOT_BUF_BYTES);
    newSt->set_snapshot(snapBuf.data(), sLen);
    newSt->dirty.clear();

    ProjectHistory::instance().commit_step(newSt);
    if (outDirty) outDirty->clear();
    return true;
}

template<>
bool CutProject::do_edit<TIMELINE>(const char* payload, u32 len, HistoryDirties* outDirty) noexcept {
    if (!payload || len == 0u) return false;

    char* buf = timeline::project::projectJsonBuffer();
    const u32 cap = timeline::project::projectJsonCapacity();
    if (len >= cap) return false;

    std::memcpy(buf, payload, len);
    buf[len] = '\0';

    UndoNode* newSt = ProjectHistory::instance().allocate_step<TIMELINE>();
    newSt->set_snapshot(payload, len);
    newSt->dirty.mutatedSlotsBB = NNUECutProjectBrain::instance().last_dirty_bb();
    newSt->projectKey = NNUECutProjectBrain::instance().project_key();

    ProjectHistory::instance().commit_step(newSt);
    if (outDirty) *outDirty = newSt->dirty;
    return true;
}

template<>
bool CutProject::do_edit<TEMPLATE>(const char* payload, u32 len, HistoryDirties* outDirty) noexcept {
    if (!payload || len == 0u) return false;

    UndoNode* newSt = ProjectHistory::instance().allocate_step<TEMPLATE>();
    newSt->set_snapshot(payload, len);
    newSt->dirty.clear();
    newSt->dirty.configDirtyMask |= TEMPLATE_DIRTY_BIT;

    timeline::project::JsonReader json(payload, len);
    if (json.isObjectDocument()) {
        u32 nameStart = 0, nameEnd = 0;
        MultiArray<char, 64> tmplName;
        tmplName[0] = '\0';
        if (json.field(0, "name", nameStart, nameEnd) ||
            json.field(0, "id", nameStart, nameEnd) ||
            json.field(0, "class", nameStart, nameEnd)) {
            json.readStringValue(nameStart, nameEnd, tmplName.data(), tmplName.size());
        }

        TemplateNode* target = TemplateArena::instance().find_template(tmplName.data());
        if (target) {
            Mask<2> diffMask{};
            u32 cursor = 1;
            while (cursor < len - 1) {
                u32 kStart = 0, kEnd = 0, vStart = 0, vEnd = 0;
                if (!json.nextObjectField(cursor, len - 1, kStart, kEnd, vStart, vEnd)) break;
                MultiArray<char, 48> key;
                key[0] = '\0';
                json.readStringValue(kStart, kEnd, key.data(), key.size());
                if (isReservedTemplateKey(key.data())) continue;

                MultiArray<char, 64> val;
                val[0] = '\0';
                json.readStringValue(vStart, vEnd, val.data(), val.size());
                double numVal = 0.0;
                const bool isNum = json.numberField(0, key.data(), numVal);

                const prop::PropertyId pId = prop::find_prop_id(key.data());
                if (pId < TemplateNode::MAX_PROPS) {
                    if (isNum) {
                        target->set_defined_property<true>(pId, key.data(), val.data(), numVal);
                    } else {
                        target->set_defined_property<false>(pId, key.data(), val.data(), 0.0);
                    }
                    diffMask.set_bit(pId);
                }
            }

            Bitboard mutatedSlots{0ULL};
            TemplateArena::instance().update_template_diff(target, diffMask, mutatedSlots);
            newSt->dirty.mutatedSlotsBB |= mutatedSlots;
        }
    }

    ProjectHistory::instance().commit_step(newSt);
    if (outDirty) *outDirty = newSt->dirty;
    return true;
}

template<>
bool CutProject::do_edit<FULL_PROJECT>(const char* payload, u32 len, HistoryDirties* outDirty) noexcept {
    if (!payload || len == 0u) return false;

    engineManager().config_options().load_json(payload, len);

    char* buf = timeline::project::projectJsonBuffer();
    const u32 cap = timeline::project::projectJsonCapacity();
    if (len >= cap) return false;

    std::memcpy(buf, payload, len);
    buf[len] = '\0';

    UndoNode* newSt = ProjectHistory::instance().allocate_step<FULL_PROJECT>();
    newSt->set_snapshot(payload, len);
    newSt->dirty.mutatedSlotsBB = NNUECutProjectBrain::instance().last_dirty_bb();
    newSt->projectKey = NNUECutProjectBrain::instance().project_key();

    ProjectHistory::instance().commit_step(newSt);
    if (outDirty) *outDirty = newSt->dirty;
    return true;
}

template <bool IsRedo>
bool CutProject::step_edit(HistoryDirties* outDirty) noexcept {
    UndoNode* target = ProjectHistory::instance().step<IsRedo>();
    if (!target) return false;

    if (target->action == CONFIG) {
        if (target->snapshot) {
            engineManager().config_options().restore(target->snapshot, target->snapshotLen);
        }
        if (outDirty) outDirty->clear();
    } else if (target->action == TIMELINE || target->action == FULL_PROJECT) {
        if (target->snapshot && target->snapshotLen > 0u) {
            char* buf = timeline::project::projectJsonBuffer();
            const u32 cap = timeline::project::projectJsonCapacity();
            const u32 copyLen = target->snapshotLen < cap ? target->snapshotLen : cap - 1u;
            std::memcpy(buf, target->snapshot, copyLen);
            buf[copyLen] = '\0';
            Position<>* pos = timeline_position_get_instance();
            const double stageW = (pos && pos->stage_w() > 0.0f) ? static_cast<double>(pos->stage_w()) : DEFAULT_STAGE_WIDTH;
            const double stageH = (pos && pos->stage_h() > 0.0f) ? static_cast<double>(pos->stage_h()) : DEFAULT_STAGE_HEIGHT;
            engineManager().doProjectJsonMove(copyLen, stageW, stageH);
        }
        if (outDirty) *outDirty = target->dirty;
    } else if (target->action == TEMPLATE) {
        if (outDirty) *outDirty = target->dirty;
    }

    return true;
}

template bool CutProject::step_edit<false>(HistoryDirties*) noexcept;
template bool CutProject::step_edit<true>(HistoryDirties*) noexcept;

} // namespace stockcut
