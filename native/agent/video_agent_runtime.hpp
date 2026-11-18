#pragma once
// Deterministic plan/execute/verify/replan controller for a video-editing agent.
#include "../core/types.h"
#include "../core/string_parse.h"
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace bitagent {

using stockcut::u8;
using stockcut::u32;
using stockcut::usize;
using stockcut::isize;

static constexpr u32 kMaxPlanSteps    = 48;
static constexpr u32 kMaxDependencies = 12;
static constexpr u32 kMaxRetries      = 4;
static constexpr usize kMaxTaskBytes  = 4095;
static constexpr usize kMaxPostBytes  = 512;

enum class StepStatus : u32 { Pending = 0, Running = 1, Succeeded = 2, NeedsRepair = 3, Failed = 4, Blocked = 5 };

struct PlanStep {
    u32 id = 0;
    std::string action;
    std::string payload_json;
    std::string postcondition;
    std::string evidence;
    std::string error;
    std::vector<u32> dependencies;
    StepStatus status = StepStatus::Pending;
    u32 attempts = 0;
    u32 max_retries = 2;
    u32 revision = 0;
};

class VideoAgentPlan {
    std::string task_;
    std::vector<PlanStep> steps_;
    std::vector<std::string> allowed_actions_;
    std::string last_error_;

    static bool valid_action(const std::string& s) {
        if (s.empty() || s.size() > 64 || !(s[0] >= 'a' && s[0] <= 'z')) return false;
        for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
        return true;
    }
    struct JsonCheck {
        const std::string& s; usize p = 0;
        void ws() { while (p < s.size() && stockcut::is_ascii_space(s[p])) ++p; }
        bool string() {
            if (p >= s.size() || s[p++] != '"') return false;
            while (p < s.size()) {
                const u8 c = static_cast<u8>(s[p++]);
                if (c == '"') return true;
                if (c < 0x20u) return false;
                if (c == '\\') {
                    if (p >= s.size()) return false;
                    const char e = s[p++];
                    if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't') continue;
                    if (e != 'u' || p + 4 > s.size()) return false;
                    for (u32 i = 0; i < 4; ++i) { if (stockcut::hex_nibble(s[p++]) < 0) return false; }
                }
            }
            return false;
        }
        bool number() {
            const usize b = p; if (p < s.size() && s[p] == '-') ++p;
            if (p >= s.size()) return false;
            if (s[p] == '0') ++p;
            else { if (s[p] < '1' || s[p] > '9') return false; while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p; }
            if (p < s.size() && s[p] == '.') { ++p; const usize q = p; while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p; if (q == p) return false; }
            if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) { ++p; if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p; const usize q = p; while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p; if (q == p) return false; }
            return p > b;
        }
        bool value(u32 depth = 0) {
            if (depth > 16u) return false;
            ws();
            if (p >= s.size()) return false;
            if (s[p] == '"') return string();
            if (s[p] == '{') {
                ++p; ws(); if (p < s.size() && s[p] == '}') { ++p; return true; }
                for (;;) {
                    ws(); if (!string()) return false; ws(); if (p >= s.size() || s[p++] != ':') return false;
                    if (!value(depth + 1u)) return false;
                    ws();
                    if (p >= s.size()) return false;
                    const char c = s[p++];
                    if (c == '}') return true;
                    if (c != ',') return false;
                }
            }
            if (s[p] == '[') {
                ++p; ws(); if (p < s.size() && s[p] == ']') { ++p; return true; }
                for (;;) {
                    if (!value(depth + 1u)) return false; ws(); if (p >= s.size()) return false;
                    const char c = s[p++];
                    if (c == ']') return true;
                    if (c != ',') return false;
                }
            }
            if (s.compare(p, 4, "true") == 0) { p += 4; return true; }
            if (s.compare(p, 5, "false") == 0) { p += 5; return true; }
            if (s.compare(p, 4, "null") == 0) { p += 4; return true; }
            return number();
        }
    };
    static bool valid_payload(const std::string& s) {
        if (s.size() < 2 || s.size() > kMaxTaskBytes || s.front() != '{') return false;
        JsonCheck j{s}; if (!j.value()) return false; j.ws(); return j.p == s.size();
    }
    static bool parse_u32(const std::string& s, u32& out) {
        if (s.empty()) return false;
        char* end = nullptr; const unsigned long v = std::strtoul(s.c_str(), &end, 10);
        if (!end || *end || v == 0 || v > stockcut::INVALID_U32) return false;
        out = static_cast<u32>(v); return true;
    }
    static std::vector<std::string> split(const std::string& s, char delim) {
        std::vector<std::string> out; usize p = 0;
        for (;;) {
            const usize q = s.find(delim, p);
            out.push_back(s.substr(p, q == std::string::npos ? q : q - p));
            if (q == std::string::npos) break;
            p = q + 1;
        }
        return out;
    }
    PlanStep* find(u32 id) {
        for (auto& s : steps_) if (s.id == id) return &s;
        return nullptr;
    }
    const PlanStep* find(u32 id) const {
        for (const auto& s : steps_) if (s.id == id) return &s;
        return nullptr;
    }
    bool deps_succeeded(const PlanStep& s) const {
        for (u32 id : s.dependencies) { const PlanStep* d = find(id); if (!d || d->status != StepStatus::Succeeded) return false; }
        return true;
    }
    bool deps_terminal_failure(const PlanStep& s) const {
        for (u32 id : s.dependencies) { const PlanStep* d = find(id); if (!d || d->status == StepStatus::Failed || d->status == StepStatus::Blocked) return true; }
        return false;
    }
    static std::string trim_cr(std::string s) { if (!s.empty() && s.back() == '\r') s.pop_back(); return s; }
public:
    void clear() { task_.clear(); steps_.clear(); last_error_.clear(); }
    bool begin(const std::string& task) {
        if (task.empty() || task.size() > kMaxTaskBytes) { last_error_ = "task length must be 1..4095 bytes"; return false; }
        clear(); task_ = task; return true;
    }
    const std::string& task() const noexcept { return task_; }
    const std::string& last_error() const noexcept { return last_error_; }
    bool set_action_catalog(const std::vector<std::string>& actions) {
        if (actions.empty() || actions.size() > 128) { last_error_ = "host action catalog is empty or too large"; return false; }
        std::vector<std::string> clean;
        for (const auto& a : actions) {
            if (!valid_action(a) || std::find(clean.begin(), clean.end(), a) != clean.end()) { last_error_ = "invalid or duplicate host action"; return false; }
            clean.push_back(a);
        }
        allowed_actions_.swap(clean); last_error_.clear(); return true;
    }
    const std::vector<PlanStep>& steps() const noexcept { return steps_; }
    const PlanStep* step(u32 id) const noexcept { return find(id); }
    u32 size() const noexcept { return static_cast<u32>(steps_.size()); }

    bool add_step(u32 id, const std::string& action, const std::string& payload,
                  const std::string& postcondition, const std::vector<u32>& deps, u32 maxRetries = 2) {
        if (task_.empty()) { last_error_ = "begin(task) must be called first"; return false; }
        if (!id || steps_.size() >= kMaxPlanSteps || find(id)) { last_error_ = "invalid, duplicate, or excessive step id"; return false; }
        if (!valid_action(action)) { last_error_ = "action must be a lowercase adapter identifier"; return false; }
        if (!allowed_actions_.empty() && std::find(allowed_actions_.begin(), allowed_actions_.end(), action) == allowed_actions_.end()) { last_error_ = "action is not registered by the host adapter"; return false; }
        if (!valid_payload(payload)) { last_error_ = "step payload must be a bounded JSON object"; return false; }
        if (postcondition.empty() || postcondition.size() > kMaxPostBytes) { last_error_ = "a concrete postcondition is required"; return false; }
        if (deps.size() > kMaxDependencies || maxRetries > kMaxRetries) { last_error_ = "dependency/retry limit exceeded"; return false; }
        for (usize i = 0; i < deps.size(); ++i) {
            if (!find(deps[i])) { last_error_ = "dependencies must reference earlier steps"; return false; }
            if (std::find(deps.begin(), deps.begin() + static_cast<isize>(i), deps[i]) != deps.begin() + static_cast<isize>(i)) {
                last_error_ = "duplicate dependency id"; return false;
            }
        }
        PlanStep s; s.id = id; s.action = action; s.payload_json = payload; s.postcondition = postcondition;
        s.dependencies = deps; s.max_retries = maxRetries; steps_.push_back(std::move(s)); last_error_.clear(); return true;
    }

    const PlanStep* claim_next() {
        for (auto& s : steps_) {
            if (s.status != StepStatus::Pending) continue;
            if (deps_terminal_failure(s)) { s.status = StepStatus::Blocked; s.error = "blocked by an unsuccessful dependency"; continue; }
            if (!deps_succeeded(s)) continue;
            s.status = StepStatus::Running; ++s.attempts; last_error_.clear(); return &s;
        }
        return nullptr;
    }

    template <bool ActionSucceeded, bool Verified>
    bool report(u32 id, const std::string& evidenceOrError) {
        PlanStep* s = find(id);
        if (!s || s->status != StepStatus::Running) { last_error_ = "report requires a currently running step"; return false; }
        if constexpr (ActionSucceeded && Verified) {
            if (!evidenceOrError.empty()) {
                s->status = StepStatus::Succeeded; s->evidence = evidenceOrError; s->error.clear(); last_error_.clear(); return true;
            }
        }
        s->evidence.clear();
        s->error = evidenceOrError.empty() ? "execution was not verified against the editor state" : evidenceOrError;
        s->status = (s->attempts <= s->max_retries) ? StepStatus::NeedsRepair : StepStatus::Failed;
        last_error_ = s->error; return true;
    }

    bool revise(u32 id, const std::string& action, const std::string& payload,
                const std::string& postcondition) {
        PlanStep* s = find(id);
        if (!s || s->status != StepStatus::NeedsRepair) { last_error_ = "only a step awaiting repair may be revised"; return false; }
        if (s->attempts > s->max_retries) { s->status = StepStatus::Failed; last_error_ = "retry budget exhausted"; return false; }
        if (!valid_action(action) || (!allowed_actions_.empty() && std::find(allowed_actions_.begin(), allowed_actions_.end(), action) == allowed_actions_.end()) || !valid_payload(payload) || postcondition.empty() || postcondition.size() > kMaxPostBytes) {
            last_error_ = "revised action, JSON payload, or postcondition is invalid"; return false;
        }
        s->action = action; s->payload_json = payload; s->postcondition = postcondition;
        s->status = StepStatus::Pending; s->error.clear(); s->evidence.clear(); ++s->revision; last_error_.clear(); return true;
    }

    bool complete() const {
        if (steps_.empty()) return false;
        for (const auto& s : steps_) if (s.status != StepStatus::Succeeded) return false;
        return true;
    }
    bool has_repair() const { for (const auto& s : steps_) if (s.status == StepStatus::NeedsRepair) return true; return false; }
    bool terminal_failure() const { for (const auto& s : steps_) if (s.status == StepStatus::Failed || s.status == StepStatus::Blocked) return true; return false; }
    u32 ready_count() const {
        u32 n = 0; for (const auto& s : steps_) if (s.status == StepStatus::Pending && deps_succeeded(s)) ++n; return n;
    }

    std::string plan_prompt() const {
        std::string catalog;
        for (usize i = 0; i < allowed_actions_.size(); ++i) { if (i) catalog += " "; catalog += allowed_actions_[i]; }
        return std::string("VIDEO_AGENT_PLAN_V1 create minimal safe video editing plan output only valid JSON object with steps array each step JSON schema {\"id\":1,\"action\":\"registered_action\",\"depends_on\":[],\"params\":{},\"verify\":\"observable editor state\"} output JSON only use only supported actions do not invent item ids timestamps or settings list dependencies first TASK ")
          + task_ + " SUPPORTED_ACTIONS " + catalog;
    }
    std::string repair_prompt(u32 id) const {
        const PlanStep* s = find(id); if (!s) return {};
        std::string out = "VIDEO_AGENT_REPAIR_V1 repair only the failed step preserve verified work output only valid JSON object fields id action params verify TASK ";
        out += task_ + " FAILED_STEP " + std::to_string(s->id) + " OLD_ACTION " + s->action +
          " OLD_PARAMS " + s->payload_json + " REQUIRED_VERIFY " + s->postcondition +
          " ATTEMPT " + std::to_string(s->attempts) + " ERROR " + s->error;
        for (const auto& p : steps_) if (p.status == StepStatus::Succeeded)
            out += " VERIFIED_STEP_" + std::to_string(p.id) + " " + p.action + " " + p.evidence;
        return out;
    }
    bool parse_plan(const std::string& output) {
        const auto lines = split(output, '\n');
        if (lines.empty() || trim_cr(lines.front()) != "PLAN/1") { last_error_ = "missing PLAN/1 header"; return false; }
        VideoAgentPlan candidate; candidate.task_ = task_; candidate.allowed_actions_ = allowed_actions_;
        bool ended = false; u32 parsed = 0;
        for (usize i = 1; i < lines.size(); ++i) {
            const std::string line = trim_cr(lines[i]); if (line.empty()) continue;
            if (line == "END") { ended = true; break; }
            const auto f = split(line, '\t');
            if (f.size() != 6 || f[0] != "STEP") { last_error_ = "malformed STEP line at output row " + std::to_string(i + 1); return false; }
            u32 id = 0; if (!parse_u32(f[1], id)) { last_error_ = "invalid step id"; return false; }
            std::vector<u32> deps;
            if (f[3] != "-") for (const auto& x : split(f[3], ',')) { u32 d = 0; if (!parse_u32(x, d)) { last_error_ = "invalid dependency id"; return false; } deps.push_back(d); }
            if (!candidate.add_step(id, f[2], f[4], f[5], deps, 2)) { last_error_ = candidate.last_error_; return false; }
            ++parsed;
        }
        if (!ended || parsed == 0) { last_error_ = "plan needs at least one step and END"; return false; }
        steps_.swap(candidate.steps_); last_error_.clear(); return true;
    }
    bool parse_repair(const std::string& output) {
        const std::string line = trim_cr(output);
        const auto f = split(line, '\t');
        if (f.size() != 5 || f[0] != "REVISE") { last_error_ = "malformed REVISE response"; return false; }
        u32 id = 0; if (!parse_u32(f[1], id)) { last_error_ = "invalid repaired step id"; return false; }
        return revise(id, f[2], f[3], f[4]);
    }
};

} // namespace bitagent
