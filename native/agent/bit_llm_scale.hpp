#pragma once
// Bit-LLM v22 scale systems: long-context storage, reasoning branches,
// creative evaluation, curriculum scheduling and adaptive CPU/RAM policy.
#include "../core/types.h"
#include "../core/hash_primitives.h"
#include "../core/string_parse.h"
#include "../core/data_structures.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef _OPENMP
#include <omp.h>
#endif

namespace bitscale {

using stockcut::u8;
using stockcut::u32;
using stockcut::u64;
using stockcut::usize;
using stockcut::isize;
using stockcut::MultiArray;

constexpr u32 kSignatureDims = 16;

inline u64 hash_token(std::string_view s) noexcept {
    u64 h = stockcut::FNV1A_64_OFFSET;
    for (char c : s) {
        h = stockcut::fnv1a_64_step(h, static_cast<u8>(stockcut::ascii_lower(c)));
    }
    return h;
}

inline std::vector<u64> tokenize_hashes(std::string_view s) {
    std::vector<u64> out;
    usize i = 0;
    while (i < s.size()) {
        while (i < s.size() && static_cast<u8>(s[i]) <= 32u) ++i;
        const auto b = i;
        while (i < s.size() && static_cast<u8>(s[i]) > 32u) ++i;
        if (i > b) out.push_back(hash_token(s.substr(b, i - b)));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

struct ResourcePlan { u32 threads = 1, branch_budget = 2, retrieval_budget = 8, reason_steps = 16; double ram_fraction = 0.25; };
class ResourceGovernor {
    u32 cores_ = stockcut::max(1u, std::thread::hardware_concurrency());
    u32 user_cap_ = 0;
    double cpu_busy_ = 0.0, ram_used_ = 0.0, ram_total_ = 0.0;
    ResourcePlan plan_{};
public:
    u32 hardware_threads() const noexcept { return cores_; }
    void set_thread_cap(u32 n) noexcept { user_cap_ = n; }
    void observe(double cpuBusy01, double ramUsedBytes, double ramTotalBytes) noexcept {
        cpu_busy_ = stockcut::clamp(cpuBusy01, 0.0, 1.0);
        ram_used_ = stockcut::max(0.0, ramUsedBytes);
        ram_total_ = stockcut::max(0.0, ramTotalBytes);
        const u32 cap = user_cap_ ? stockcut::min(user_cap_, cores_) : cores_;
        const double headroom = ram_total_ > 0.0 ? stockcut::max(0.0, (ram_total_ - ram_used_) / ram_total_) : 0.25;
        const double cpuRoom = stockcut::max(0.15, 1.0 - cpu_busy_);
        plan_.threads = stockcut::clamp(static_cast<u32>(std::ceil(cap * cpuRoom)), 1u, cap);
        plan_.ram_fraction = stockcut::clamp(headroom * 0.70, 0.05, 0.70);
        plan_.branch_budget = stockcut::clamp(static_cast<u32>(2.0 + 62.0 * stockcut::min(cpuRoom, headroom)), 2u, 64u);
        plan_.retrieval_budget = stockcut::clamp(static_cast<u32>(4.0 + 60.0 * headroom), 4u, 64u);
        plan_.reason_steps = stockcut::clamp(static_cast<u32>(8.0 + 56.0 * cpuRoom), 8u, 64u);
    }
    ResourcePlan plan() const noexcept { return plan_; }
    void apply_threads() const noexcept {
#ifdef _OPENMP
        omp_set_dynamic(0);
        omp_set_num_threads(static_cast<int>(plan_.threads));
#endif
    }
};

struct ContextChunk { u64 id = 0, parent = 0; u32 ordinal = 0; std::string text; std::vector<u64> terms; std::vector<float> embedding; float importance = 0.5f; };
struct ContextHit { u32 index = 0; float score = 0.0f; };
class LongContextBank {
    std::vector<ContextChunk> chunks_;
    u64 next_id_ = 1;
    usize max_bytes_ = 256ull * 1024ull * 1024ull;
public:
    bool add_document(std::string_view text, usize chunkBytes = 2048, usize overlapBytes = 256, float importance = 0.5f) {
        if (text.empty() || chunkBytes < 128 || chunkBytes > 65536 || overlapBytes >= chunkBytes) return false;
        const u64 parent = next_id_++;
        std::vector<ContextChunk> pending;
        usize pendingBytes = 0;
        usize pos = 0; u32 ordinal = 0;
        while (pos < text.size()) {
            usize end = stockcut::min(text.size(), pos + chunkBytes);
            if (end < text.size()) { const auto cut = text.rfind(' ', end); if (cut > pos + chunkBytes / 2) end = cut; }
            ContextChunk c; c.id = next_id_++; c.parent = parent; c.ordinal = ordinal++;
            c.text.assign(text.substr(pos, end - pos)); c.terms = tokenize_hashes(c.text); c.importance = stockcut::clamp(importance, 0.0f, 1.0f);
            pendingBytes += c.text.capacity() + c.terms.capacity() * sizeof(u64);
            pending.push_back(std::move(c));
            if (end == text.size()) break;
            pos = end > overlapBytes ? end - overlapBytes : end;
        }
        if (pendingBytes > max_bytes_ || bytes() > max_bytes_ - pendingBytes) return false;
        chunks_.insert(chunks_.end(), std::make_move_iterator(pending.begin()), std::make_move_iterator(pending.end()));
        return true;
    }
    void set_memory_limit(usize b) noexcept { max_bytes_ = stockcut::max<usize>(1024u * 1024u, b); }
    usize memory_limit() const noexcept { return max_bytes_; }
    bool set_embedding(u64 id, const float* values, u32 dims) {
        if (!values || !dims || dims > 4096) return false;
        for (auto& c : chunks_) {
            if (c.id != id) continue;
            std::vector<float> v(values, values + dims);
            double norm = 0.0;
            for (float x : v) { if (!std::isfinite(x)) return false; norm += double(x) * x; }
            if (norm < 1e-12) return false;
            const float inv = static_cast<float>(1.0 / std::sqrt(norm));
            for (float& x : v) x *= inv;
            c.embedding.swap(v);
            return true;
        }
        return false;
    }
    std::vector<ContextHit> search(std::string_view query, u32 topk = 8, const float* queryEmbedding = nullptr, u32 dims = 0, float semanticWeight = 0.5f) const {
        const auto q = tokenize_hashes(query); std::vector<ContextHit> hits;
        if (q.empty() || !topk) return hits;
        hits.reserve(chunks_.size());
        for (u32 i = 0; i < chunks_.size(); ++i) {
            const auto& c = chunks_[i]; usize a = 0, b = 0, match = 0;
            while (a < q.size() && b < c.terms.size()) { if (q[a] == c.terms[b]) { ++match; ++a; ++b; } else if (q[a] < c.terms[b]) ++a; else ++b; }
            if (!match) continue;
            const float precision = static_cast<float>(match) / stockcut::max<usize>(1u, c.terms.size());
            const float recall = static_cast<float>(match) / static_cast<float>(q.size());
            const float lexical = (0.65f * recall + 0.35f * precision) * (0.75f + 0.5f * c.importance);
            float semantic = 0.0f;
            if (queryEmbedding && dims && c.embedding.size() == dims) {
                double dot = 0.0, norm = 0.0;
                for (u32 j = 0; j < dims; ++j) {
                    if (!std::isfinite(queryEmbedding[j])) { norm = -1.0; break; }
                    dot += double(queryEmbedding[j]) * c.embedding[j];
                    norm += double(queryEmbedding[j]) * queryEmbedding[j];
                }
                if (norm > 1e-12) semantic = static_cast<float>(stockcut::clamp(dot / std::sqrt(norm), 0.0, 1.0));
            }
            const float sw = stockcut::clamp(semanticWeight, 0.0f, 1.0f);
            const float score = (queryEmbedding && dims && c.embedding.size() == dims) ? ((1.0f - sw) * lexical + sw * semantic) : lexical;
            hits.push_back({i, score});
        }
        const auto n = stockcut::min<usize>(topk, hits.size());
        std::partial_sort(hits.begin(), hits.begin() + static_cast<isize>(n), hits.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
        hits.resize(n); return hits;
    }
    const ContextChunk* get(u32 i) const noexcept { return i < chunks_.size() ? &chunks_[i] : nullptr; }
    usize size() const noexcept { return chunks_.size(); }
    usize bytes() const noexcept { usize n = 0; for (const auto& c : chunks_) n += c.text.capacity() + c.terms.capacity() * sizeof(u64) + c.embedding.capacity() * sizeof(float); return n; }
    void clear() { chunks_.clear(); next_id_ = 1; }
};

enum class ReasonStage : u32 { Understand, Decompose, Hypothesize, Reason, Critique, Verify, Synthesize, Done };
struct ReasonBranch { u64 id = 0; u32 parent = 0; float score = 0.0f; MultiArray<float, kSignatureDims> signature{}; bool alive = true; };
class ReasoningGraph {
    ReasonStage stage_ = ReasonStage::Understand; std::vector<ReasonBranch> branches_; u64 next_ = 1;
public:
    ReasonStage stage() const noexcept { return stage_; }
    bool advance() noexcept { if (stage_ == ReasonStage::Done) return false; stage_ = static_cast<ReasonStage>(static_cast<u32>(stage_) + 1u); return true; }
    u64 add_branch(u32 parent, float score, const float* sig, u32 n = kSignatureDims) {
        if (!sig || n != kSignatureDims || branches_.size() >= 256 || !std::isfinite(score)) return 0;
        ReasonBranch b; b.id = next_++; b.parent = parent; b.score = score;
        for (u32 i = 0; i < kSignatureDims; ++i) { if (!std::isfinite(sig[i])) return 0; b.signature[i] = sig[i]; }
        branches_.push_back(b); return b.id;
    }
    static float distance(const ReasonBranch& a, const ReasonBranch& b) noexcept {
        float d = 0.0f;
        for (u32 i = 0; i < kSignatureDims; ++i) { const float x = a.signature[i] - b.signature[i]; d += x * x; }
        return std::sqrt(d / static_cast<float>(kSignatureDims));
    }
    std::vector<u64> select(u32 limit, float diversity = 0.25f) {
        std::vector<const ReasonBranch*> pool;
        for (const auto& b : branches_) if (b.alive) pool.push_back(&b);
        std::vector<const ReasonBranch*> chosen;
        std::vector<u64> out;
        diversity = stockcut::clamp(diversity, 0.0f, 1.0f);
        limit = stockcut::min<u32>(64u, limit);
        while (!pool.empty() && chosen.size() < limit) {
            usize best = 0; float bestScore = -std::numeric_limits<float>::infinity();
            for (usize i = 0; i < pool.size(); ++i) {
                float similarity = 0.0f;
                for (const auto* c : chosen) similarity = stockcut::max(similarity, stockcut::max(0.0f, 1.0f - distance(*pool[i], *c)));
                const float adjusted = pool[i]->score - diversity * similarity;
                if (adjusted > bestScore) { bestScore = adjusted; best = i; }
            }
            chosen.push_back(pool[best]); out.push_back(pool[best]->id);
            pool.erase(pool.begin() + static_cast<isize>(best));
        }
        for (auto& b : branches_) b.alive = std::find(out.begin(), out.end(), b.id) != out.end();
        return out;
    }
    void clear() { stage_ = ReasonStage::Understand; branches_.clear(); next_ = 1; }
};

struct CreativeScore { float novelty = 0.0f, fit = 0.0f, coherence = 0.0f, total = 0.0f; };
class CreativeCritic {
    std::vector<std::vector<u64>> history_;
public:
    CreativeScore evaluate(std::string_view proposal, std::string_view intent, float coherence = 0.7f) const {
        const auto p = tokenize_hashes(proposal), q = tokenize_hashes(intent); CreativeScore s;
        usize best = 0;
        for (const auto& h : history_) {
            usize a = 0, b = 0, m = 0;
            while (a < p.size() && b < h.size()) { if (p[a] == h[b]) { ++m; ++a; ++b; } else if (p[a] < h[b]) ++a; else ++b; }
            const usize uni = p.size() + h.size() - m;
            best = stockcut::max(best, uni ? static_cast<usize>(1000u * m / uni) : usize(1000u));
        }
        s.novelty = history_.empty() ? 1.0f : 1.0f - static_cast<float>(best) / 1000.0f;
        usize match = 0, a = 0, b = 0;
        while (a < p.size() && b < q.size()) { if (p[a] == q[b]) { ++match; ++a; ++b; } else if (p[a] < q[b]) ++a; else ++b; }
        s.fit = q.empty() ? 0.5f : static_cast<float>(match) / static_cast<float>(q.size());
        s.coherence = stockcut::clamp(coherence, 0.0f, 1.0f);
        s.total = 0.35f * s.novelty + 0.35f * s.fit + 0.30f * s.coherence;
        return s;
    }
    void accept(std::string_view proposal) {
        history_.push_back(tokenize_hashes(proposal));
        if (history_.size() > 4096) history_.erase(history_.begin(), history_.begin() + 1024);
    }
    usize size() const noexcept { return history_.size(); }
};

struct CurriculumPlan { float sample_weight = 1.0f, reasoning_weight = 0.25f, long_context_weight = 0.1f, learning_rate_scale = 1.0f; };
class CurriculumScheduler {
    u64 step_ = 0, total_ = 10000; float difficulty_ = 0.5f;
public:
    void configure(u64 totalSteps) noexcept { total_ = stockcut::max<u64>(1ULL, totalSteps); }
    void set_difficulty(float d) noexcept { difficulty_ = stockcut::clamp(d, 0.0f, 1.0f); }
    CurriculumPlan plan() noexcept {
        const float progress = stockcut::min(1.0f, static_cast<float>(step_) / static_cast<float>(total_));
        CurriculumPlan p;
        p.sample_weight = 0.75f + 0.5f * difficulty_;
        p.reasoning_weight = 0.15f + 0.85f * progress;
        p.long_context_weight = 0.05f + 0.95f * stockcut::max(0.0f, (progress - 0.2f) / 0.8f);
        p.learning_rate_scale = progress < 0.05f ? 0.5f + 10.0f * progress : 1.0f;
        ++step_;
        return p;
    }
    u64 step() const noexcept { return step_; }
};

} // namespace bitscale
