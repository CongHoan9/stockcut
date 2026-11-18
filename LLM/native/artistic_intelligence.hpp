#pragma once
// Bit-LLM v21 Artistic Intelligence Layer
// Explicit creative intent + preview-measurement critic + pairwise preference learning.
// This evaluates supplied, measurable evidence. It does not claim subjective emotion or objective beauty.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

namespace bitart {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
constexpr u32 kIntentDims = 16;
constexpr u32 kFeatureDims = 24;
constexpr u32 kCandidateSlots = 16;
constexpr u32 kStrategyCount = 4;
constexpr u32 kStateMagic = 0x31524142u; // BAR1
constexpr u32 kStateVersion = 1;

static constexpr const char *kIntentNames[kIntentDims] = {
    "cinematic", "minimalist", "dynamic", "luxury", "playful", "melancholic", "energetic", "calm",
    "warm", "cool", "high_contrast", "saturated", "typography", "closeup", "smooth_motion", "rhythmic_sync"};
static constexpr const char *kFeatureNames[kFeatureDims] = {
    "brightness", "contrast", "saturation", "warmth", "hue_coherence", "color_entropy",
    "composition_balance", "subject_saliency", "negative_space", "clutter", "edge_density", "motion_smoothness",
    "motion_energy", "cut_rate", "beat_sync", "audio_dynamics", "shot_scale_variance", "transition_density",
    "temporal_continuity", "text_legibility", "focus_consistency", "novelty", "visual_complexity", "exposure_stability"};

struct Candidate {
    u32 id = 0;
    bool used = false;
    std::array<float, kFeatureDims> f{};
    float score = -1.0f;
};

struct Strategy {
    const char *id;
    const char *name;
    const char *brief;
    std::array<float, kFeatureDims> target;
};

static constexpr Strategy kStrategies[kStrategyCount] = {
    {"minimal_cinematic", "Minimal Cinematic",
     "Use deliberate pacing, restrained transitions, generous negative space, stable exposure and clear subject hierarchy. Avoid decoration that does not strengthen the story.",
     {{0.52f,0.58f,0.38f,0.52f,0.76f,0.36f,0.78f,0.78f,0.82f,0.12f,0.30f,0.82f,0.30f,0.18f,0.42f,0.38f,0.34f,0.12f,0.88f,0.82f,0.86f,0.48f,0.28f,0.92f}}},
    {"dynamic_premium", "Dynamic Premium",
     "Create purposeful kinetic energy with controlled contrast, rhythmic cuts and clear focal hierarchy. Motion and transitions must land on meaningful visual or musical beats.",
     {{0.56f,0.72f,0.58f,0.50f,0.70f,0.58f,0.68f,0.82f,0.42f,0.30f,0.52f,0.70f,0.82f,0.72f,0.86f,0.72f,0.74f,0.56f,0.68f,0.78f,0.78f,0.72f,0.62f,0.88f}}},
    {"editorial_luxury", "Editorial Luxury",
     "Favor refined composition, restrained color, legible typography, deliberate whitespace and consistent visual language. Every title and transition should feel intentional.",
     {{0.54f,0.66f,0.34f,0.54f,0.84f,0.34f,0.86f,0.78f,0.78f,0.14f,0.34f,0.78f,0.28f,0.22f,0.52f,0.42f,0.44f,0.18f,0.88f,0.96f,0.90f,0.50f,0.30f,0.96f}}},
    {"documentary_warm", "Documentary Warm",
     "Preserve natural motion and temporal continuity, use warm but believable color, and let meaningful moments breathe. Avoid over-processing and unnecessary cuts.",
     {{0.58f,0.48f,0.48f,0.76f,0.66f,0.54f,0.66f,0.76f,0.56f,0.24f,0.40f,0.86f,0.40f,0.20f,0.58f,0.60f,0.52f,0.16f,0.94f,0.70f,0.86f,0.52f,0.42f,0.88f}}}
};

inline float clamp01(float x) noexcept { return std::max(0.0f, std::min(1.0f, x)); }
inline float sigmoid(float x) noexcept {
    x = std::max(-20.0f, std::min(20.0f, x));
    return 1.0f / (1.0f + std::exp(-x));
}
inline std::string lower_ascii(std::string s) {
    for (char &c : s) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
    return s;
}
inline bool contains_any(const std::string &s, std::initializer_list<const char*> terms) {
    for (const char *t : terms) if (s.find(t) != std::string::npos) return true;
    return false;
}

class ArtisticIntelligence {
public:
    using Intent = std::array<float, kIntentDims>;
    using Features = std::array<float, kFeatureDims>;
private:
    Intent intent_{};
    std::string intent_text_;
    Candidate candidates_[kCandidateSlots]{};
    // Pairwise Bradley-Terry parameters: global feature preference + intent-conditioned preference.
    std::array<float, kFeatureDims> global_pref_{};
    std::array<float, kIntentDims * kFeatureDims> conditional_pref_{};
    u64 preference_updates_ = 0;
    u32 selected_strategy_ = 0;

    void refresh_candidate_scores() noexcept {
        for (Candidate &c : candidates_) if (c.used) c.score = score(c.f);
    }
    static float target_distance_score(float x, float target) noexcept {
        return clamp01(1.0f - std::fabs(x - target) * 1.65f);
    }
    float prior_score(const Features &f) const noexcept {
        // Broad, explainable priors. Human preference updates can move the critic away from these defaults.
        static constexpr float baseTarget[kFeatureDims] = {
            .54f,.56f,.48f,.50f,.68f,.48f,.70f,.74f,.52f,.28f,.42f,.72f,.45f,.34f,.52f,.48f,.46f,.28f,.78f,.78f,.78f,.48f,.42f,.88f};
        static constexpr float baseWeight[kFeatureDims] = {
            .35f,.55f,.35f,.18f,.42f,.16f,.75f,1.00f,.32f,.70f,.20f,.65f,.24f,.42f,.36f,.18f,.22f,.32f,.82f,.55f,.68f,.18f,.28f,.76f};
        float weighted = 0.0f, total = 0.0f;
        for (u32 i = 0; i < kFeatureDims; ++i) {
            float target = baseTarget[i], weight = baseWeight[i];
            auto mix_target = [&](u32 intent, float t, float w) {
                const float a = intent_[intent];
                target = target * (1.0f - a) + t * a;
                weight += w * a;
            };
            switch (i) {
            case 1: mix_target(10,.78f,.35f); mix_target(3,.68f,.18f); break;
            case 2: mix_target(11,.78f,.45f); mix_target(3,.38f,.20f); mix_target(1,.34f,.15f); break;
            case 3: mix_target(8,.78f,.35f); mix_target(9,.22f,.35f); break;
            case 4: mix_target(3,.84f,.40f); mix_target(0,.78f,.25f); break;
            case 6: mix_target(12,.82f,.35f); mix_target(1,.82f,.25f); break;
            case 7: mix_target(13,.88f,.40f); mix_target(0,.82f,.20f); break;
            case 8: mix_target(1,.86f,.55f); mix_target(3,.78f,.18f); break;
            case 9: mix_target(1,.10f,.75f); mix_target(3,.14f,.55f); break;
            case 11: mix_target(14,.90f,.45f); mix_target(0,.86f,.28f); break;
            case 12: mix_target(2,.82f,.50f); mix_target(6,.84f,.40f); mix_target(7,.22f,.30f); break;
            case 13: mix_target(2,.72f,.55f); mix_target(7,.18f,.35f); mix_target(0,.22f,.25f); break;
            case 14: mix_target(15,.90f,.65f); mix_target(2,.82f,.35f); break;
            case 16: mix_target(2,.76f,.30f); break;
            case 17: mix_target(1,.10f,.60f); mix_target(3,.16f,.40f); break;
            case 18: mix_target(0,.88f,.45f); mix_target(7,.92f,.45f); break;
            case 19: mix_target(12,.96f,.65f); break;
            case 20: mix_target(0,.88f,.35f); mix_target(3,.92f,.25f); break;
            case 21: mix_target(4,.76f,.45f); mix_target(2,.76f,.40f); break;
            case 22: mix_target(1,.18f,.55f); mix_target(3,.28f,.38f); break;
            case 23: mix_target(3,.96f,.45f); mix_target(0,.92f,.30f); break;
            default: break;
            }
            const float q = target_distance_score(f[i], target);
            weighted += q * weight;
            total += weight;
        }
        return total > 0.0f ? weighted / total : 0.5f;
    }
    float learned_logit(const Features &f) const noexcept {
        float z = 0.0f;
        for (u32 k = 0; k < kFeatureDims; ++k) {
            const float x = f[k] - 0.5f;
            z += global_pref_[k] * x;
            for (u32 j = 0; j < kIntentDims; ++j)
                z += intent_[j] * conditional_pref_[j * kFeatureDims + k] * x;
        }
        return z;
    }
    static u64 checksum(const std::uint8_t *p, std::size_t n) noexcept {
        u64 h = 1469598103934665603ULL;
        for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ULL;
        return h;
    }
public:
    ArtisticIntelligence() = default;
    const Intent &intent() const noexcept { return intent_; }
    const std::string &intent_text() const noexcept { return intent_text_; }
    u64 preference_updates() const noexcept { return preference_updates_; }
    u32 selected_strategy() const noexcept { return selected_strategy_; }

    void set_intent(const std::string &text) {
        intent_text_ = text;
        const std::string s = lower_ascii(text);
        intent_.fill(0.0f);
        if (contains_any(s,{"cinematic","filmic","điện ảnh","dien anh","cinema"})) intent_[0] = 1.0f;
        if (contains_any(s,{"minimal","minimalist","tối giản","toi gian","ít chi tiết","it chi tiet"})) intent_[1] = 1.0f;
        if (contains_any(s,{"dynamic","năng động","nang dong","nhanh","high energy"})) intent_[2] = 1.0f;
        if (contains_any(s,{"luxury","luxurious","premium","cao cấp","cao cap","sang trọng","sang trong","elegant","thanh lịch","thanh lich"})) intent_[3] = 1.0f;
        if (contains_any(s,{"playful","vui tươi","vui tuoi","whimsical"})) intent_[4] = 1.0f;
        if (contains_any(s,{"melancholic","melancholy","buồn","buon","trầm","tram"})) intent_[5] = 1.0f;
        if (contains_any(s,{"energetic","sôi động","soi dong","bùng nổ","bung no"})) intent_[6] = 1.0f;
        if (contains_any(s,{"calm","serene","nhẹ nhàng","nhe nhang","yên bình","yen binh"})) intent_[7] = 1.0f;
        if (contains_any(s,{"warm","ấm áp","am ap","ấm"})) intent_[8] = 1.0f;
        if (contains_any(s,{"cool","lạnh","lanh","blue tone"})) intent_[9] = 1.0f;
        if (contains_any(s,{"high contrast","tương phản mạnh","tuong phan manh","tương phản","tuong phan"})) intent_[10] = 1.0f;
        if (contains_any(s,{"saturated","vibrant","rực rỡ","ruc ro","sống động","song dong"})) intent_[11] = 1.0f;
        if (contains_any(s,{"typography","editorial","chữ","tieu de","tiêu đề","text-led"})) intent_[12] = 1.0f;
        if (contains_any(s,{"close-up","closeup","cận cảnh","can canh","macro"})) intent_[13] = 1.0f;
        if (contains_any(s,{"smooth","mượt","muot","fluid motion","slow motion"})) intent_[14] = 1.0f;
        if (contains_any(s,{"rhythmic","beat sync","đồng bộ nhạc","dong bo nhac","nhịp nhạc","nhip nhac"})) intent_[15] = 1.0f;
        clear_candidates();
    }
    bool set_intent_vector(const float *v, u32 count) noexcept {
        if (!v || count != kIntentDims) return false;
        for (u32 i = 0; i < count; ++i) if (!std::isfinite(v[i]) || v[i] < 0.0f || v[i] > 1.0f) return false;
        std::copy(v, v + count, intent_.begin());
        intent_text_ = "explicit-intent-vector";
        clear_candidates();
        return true;
    }
    bool set_candidate(u32 slot, u32 id, const float *features, u32 count) noexcept {
        if (slot >= kCandidateSlots || !features || count != kFeatureDims) return false;
        Candidate c; c.id = id;
        for (u32 i = 0; i < kFeatureDims; ++i) {
            if (!std::isfinite(features[i]) || features[i] < -0.001f || features[i] > 1.001f) return false;
            c.f[i] = clamp01(features[i]);
        }
        c.used = true;
        c.score = score(c.f);
        candidates_[slot] = c;
        return true;
    }
    float score(const Features &f) const noexcept {
        const float prior = prior_score(f);
        // Bounded preference correction; prevents early feedback from dominating all hard aesthetic criteria.
        return clamp01(prior + 0.22f * std::tanh(learned_logit(f) * 0.65f));
    }
    void clear_candidates() noexcept {
        for (Candidate &c : candidates_) c = Candidate{};
    }
    void reset_preferences() noexcept {
        global_pref_.fill(0.0f);
        conditional_pref_.fill(0.0f);
        preference_updates_ = 0;
        refresh_candidate_scores();
    }
    const Candidate *candidate(u32 slot) const noexcept {
        return slot < kCandidateSlots && candidates_[slot].used ? &candidates_[slot] : nullptr;
    }
    u32 rank(u32 rankIndex) const noexcept {
        u32 order[kCandidateSlots], n = 0;
        for (u32 i = 0; i < kCandidateSlots; ++i) if (candidates_[i].used) order[n++] = i;
        for (u32 i = 1; i < n; ++i) {
            const u32 v = order[i]; u32 j = i;
            while (j && candidates_[order[j - 1]].score < candidates_[v].score) { order[j] = order[j - 1]; --j; }
            order[j] = v;
        }
        return rankIndex < n ? order[rankIndex] : 0xFFFFFFFFu;
    }
    bool learn_preference(u32 chosenSlot, u32 rejectedSlot, float learningRate = 0.035f) noexcept {
        if (chosenSlot >= kCandidateSlots || rejectedSlot >= kCandidateSlots || chosenSlot == rejectedSlot ||
            !candidates_[chosenSlot].used || !candidates_[rejectedSlot].used ||
            !std::isfinite(learningRate) || learningRate <= 0.0f || learningRate > 0.2f) return false;
        const Candidate &a = candidates_[chosenSlot], &b = candidates_[rejectedSlot];
        const float delta = score(a.f) - score(b.f);
        const float p = sigmoid(delta * 5.0f);
        const float scale = std::min(1.0f, (1.0f - p) * 2.5f);
        for (u32 k = 0; k < kFeatureDims; ++k) {
            const float dx = (a.f[k] - b.f[k]);
            global_pref_[k] = std::max(-3.0f, std::min(3.0f, global_pref_[k] + learningRate * (scale * dx - 0.001f * global_pref_[k])));
            for (u32 j = 0; j < kIntentDims; ++j) {
                const u32 ix = j * kFeatureDims + k;
                const float g = scale * intent_[j] * dx - 0.001f * conditional_pref_[ix];
                conditional_pref_[ix] = std::max(-3.0f, std::min(3.0f, conditional_pref_[ix] + learningRate * g));
            }
        }
        ++preference_updates_;
        refresh_candidate_scores();
        return true;
    }
    void select_strategy(u32 id) noexcept { if (id < kStrategyCount) selected_strategy_ = id; }
    std::string strategy_json(u32 id) const {
        if (id >= kStrategyCount) return "{}";
        const Strategy &s = kStrategies[id];
        std::string out = "{\"id\":\"" + std::string(s.id) + "\",\"name\":\"" + s.name + "\",\"brief\":\"" + s.brief + "\",\"targetFeatures\":{";
        for (u32 i = 0; i < kFeatureDims; ++i) {
            if (i) out += ',';
            out += "\"" + std::string(kFeatureNames[i]) + "\":" + std::to_string(s.target[i]);
        }
        out += "}}";
        return out;
    }
    std::string plan_guidance() const {
        std::string out = "ARTISTIC INTENT: " + (intent_text_.empty() ? std::string("not specified") : intent_text_) + "\nSTYLE STRATEGY: " + kStrategies[selected_strategy_].name + "\nSTYLE PRINCIPLE: " + kStrategies[selected_strategy_].brief + "\n";
        out += "EXECUTION RULES: preserve the requested subject and story; treat these as aesthetic goals, not hard facts; do not invent source footage or audio; use measurable preview evidence; prefer the least destructive edit that achieves the intent.\n";
        out += "EVALUATION FEATURES (normalized 0..1): brightness, contrast, saturation, warmth, hue_coherence, color_entropy, composition_balance, subject_saliency, negative_space, clutter, edge_density, motion_smoothness, motion_energy, cut_rate, beat_sync, audio_dynamics, shot_scale_variance, transition_density, temporal_continuity, text_legibility, focus_consistency, novelty, visual_complexity, exposure_stability.\n";
        return out;
    }
    std::string score_explanation(u32 slot) const {
        const Candidate *c = candidate(slot); if (!c) return "{}";
        std::string out = "{\"id\":" + std::to_string(c->id) + ",\"score\":" + std::to_string(c->score) + ",\"preferenceUpdates\":" + std::to_string(preference_updates_) + ",\"features\":{";
        for (u32 i = 0; i < kFeatureDims; ++i) {
            if (i) out += ',';
            out += "\"" + std::string(kFeatureNames[i]) + "\":" + std::to_string(c->f[i]);
        }
        out += "}}";
        return out;
    }
    bool serialize(std::vector<std::uint8_t> &out) const {
        out.clear();
        const u32 magic = kStateMagic, version = kStateVersion, dims = kFeatureDims, intents = kIntentDims;
        auto append = [&](const void *p, std::size_t n) { const auto *b = static_cast<const std::uint8_t*>(p); out.insert(out.end(), b, b + n); };
        append(&magic,sizeof(magic)); append(&version,sizeof(version)); append(&dims,sizeof(dims)); append(&intents,sizeof(intents));
        append(&preference_updates_,sizeof(preference_updates_));
        append(global_pref_.data(), global_pref_.size()*sizeof(float));
        append(conditional_pref_.data(), conditional_pref_.size()*sizeof(float));
        const u64 sum = checksum(out.data(), out.size()); append(&sum,sizeof(sum));
        return true;
    }
    bool deserialize(const std::uint8_t *data, std::size_t size) noexcept {
        const std::size_t expected = 4*sizeof(u32)+sizeof(u64)+(kFeatureDims+kIntentDims*kFeatureDims)*sizeof(float)+sizeof(u64);
        if (!data || size != expected) return false;
        u64 saved = 0; std::memcpy(&saved,data+size-sizeof(u64),sizeof(u64));
        if (checksum(data,size-sizeof(u64)) != saved) return false;
        std::size_t p=0; u32 magic=0,version=0,dims=0,intents=0; u64 updates=0;
        auto read=[&](void *dst,std::size_t n){std::memcpy(dst,data+p,n);p+=n;};
        read(&magic,4); read(&version,4); read(&dims,4); read(&intents,4); read(&updates,8);
        if (magic!=kStateMagic || version!=kStateVersion || dims!=kFeatureDims || intents!=kIntentDims) return false;
        std::array<float,kFeatureDims> gw{};
        std::array<float,kIntentDims*kFeatureDims> cw{};
        read(gw.data(),gw.size()*sizeof(float)); read(cw.data(),cw.size()*sizeof(float));
        for (float x:gw) if (!std::isfinite(x)||x < -3.01f||x > 3.01f) return false;
        for (float x:cw) if (!std::isfinite(x)||x < -3.01f||x > 3.01f) return false;
        global_pref_=gw; conditional_pref_=cw; preference_updates_=updates;
        clear_candidates();
        return true;
    }
    static constexpr std::size_t serialized_size() noexcept {
        return 4*sizeof(u32)+sizeof(u64)+(kFeatureDims+kIntentDims*kFeatureDims)*sizeof(float)+sizeof(u64);
    }
};
} // namespace bitart
