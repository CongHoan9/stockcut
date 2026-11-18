// bit_llm_core.cpp - Bit-LLM v20 "Trained Hierarchical Output Head"  (ĐẠI TU: mô hình thực sự HỌC biểu diễn, mượn kiến trúc Stockfish)
//
// So với v14 (bộ phân loại tam phân trên đặc trưng BĂM NGẪU NHIÊN, cập nhật kiểu perceptron):
//  1. EMBEDDING HỌC ĐƯỢC (HalfKA của Stockfish): mỗi đặc trưng thưa (từ, bigram, từ-bỏ-dấu, token đã sinh, token gần nhất, từ khoá x token trước...)
//     là một hàng i16[L1] trong bảng NF x L1. Tầng đầu = cộng các hàng đang bật (EmbeddingBag). Hàng được HỌC bằng gradient.
//  2. ACCUMULATOR GIA TĂNG (nnue_accumulator): phía câu hỏi tính 1 lần; mỗi bước giải mã chỉ TRỪ các hàng của ngữ cảnh cũ,
//     CỘNG hàng ngữ cảnh mới + 1 hàng "đã sinh". Số học nguyên i32 => gia tăng == tính lại từ đầu (bit-chính-xác, có kiểm thử).
//  3. THÁP NNUE: pairwise clipped product (transform_perspective) -> u8 -> fc1 (i8, ReLU kẹp) -> fc2 -> h u8[LH].
//     LM-head i8 (LH byte/token thay cho ~6 KB/token ternary): logit = dot(W[tok], h)/(127*64) + b[tok].
//  4. TRAINER v20: QAT + hierarchical full-softmax ở mọi độ sâu được giám sát, router CE + fine conditional CE, BPTT.
//     Không còn unigram negative sampling; mỗi target train router bằng phân phối đầy đủ các cluster.
    //  5. Hierarchical Output Head v20: trained router -> cluster probability,
//     fine head -> token probability conditional on cluster. Exact branch-and-bound top-K.
// 10. ADAPTIVE NEURAL REASONING v20: learned halting + memory-policy heads, configurable recurrent budget, deep supervision + BPTT.
//  6. Correction/feedback history (hist_update có "trọng lực"), học tại chỗ từ 👍/👎 mà không cần train lại.
//  7. Option registry có kiểu (setoption), ngân sách nút (nodes) cho think-mode, lệnh explain, kiểm thử bench trong infer_runner.
    //  8. .bin v20: header 128 B, architecture hash, CRC32 covers fine head + RouterW/RouterB.
//  9. Đặc trưng bỏ dấu tiếng Việt (alias): "khong biet" dùng chung hàng embedding với "không biết".
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <algorithm>
#include <array>
#include <vector>
#include <type_traits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <ctime>
#include "context_memory.hpp"
#include "video_agent_runtime.hpp"
#include "artistic_intelligence.hpp"
#include "bit_llm_scale.hpp"
#if defined(__AVX2__) || defined(__AVX512BW__)
#include <immintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif
#ifdef _OPENMP
#include <omp.h>
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define WASM_EXPORT extern "C" EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT extern "C"
#endif

#define MODEL_MAGIC 0x4D4C3130u
#define MODEL_VERSION 22

#if defined(__GNUC__) || defined(__clang__)
#define FORCE_INLINE __attribute__((always_inline)) inline
#else
#define FORCE_INLINE inline
#endif

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

#ifndef FEAT_BITS
#define FEAT_BITS 15 // số hàng embedding = 2^FEAT_BITS (15 -> 32768 hàng x 256 x 2 B = 16 MB)
#endif
#ifndef TRAIN_VOCAB_CAP
#define TRAIN_VOCAB_CAP 16384
#endif
#ifndef BIT_LLM_MAX_GEN_TOKENS
#define BIT_LLM_MAX_GEN_TOKENS 512
#endif
#ifndef BIT_LLM_MAX_BEAM
#define BIT_LLM_MAX_BEAM 16
#endif
#ifndef BIT_LLM_MAX_Q
#define BIT_LLM_MAX_Q 256
#endif
#ifndef BIT_LLM_MAX_A
#define BIT_LLM_MAX_A 512
#endif
#ifndef BIT_LLM_TOKEN_PICKER_CAP
#define BIT_LLM_TOKEN_PICKER_CAP 1024
#endif

namespace nnuecut
{
#ifdef BIT_LLM_TRAIN
    constexpr bool kTrain = true;
#else
    constexpr bool kTrain = false;
#endif

    // ------------------------------------------------------------ KIẾN TRÚC (đổi hằng nào => đổi ARCH_HASH => file cũ bị từ chối rõ ràng)
    constexpr u32 L1 = 256, HALF = L1 / 2, H1 = 128, LH = 128, TOWER_BUCKETS = 4;
    // Hierarchical vocabulary: each router output owns a contiguous block of 128 tokens.
    // The router predicts P(cluster); the fine head predicts P(token | cluster).
    constexpr u32 ROUTER_CLUSTER_SHIFT = 7;
    constexpr u32 ROUTER_CLUSTER_SIZE = 1u << ROUTER_CLUSTER_SHIFT;
    constexpr u32 ROUTER_TRAIN_CAP = (TRAIN_VOCAB_CAP + ROUTER_CLUSTER_SIZE - 1) / ROUTER_CLUSTER_SIZE;
    constexpr u32 ROUTER_MAX_CLUSTERS = ((1u << 20) + ROUTER_CLUSTER_SIZE - 1) / ROUTER_CLUSTER_SIZE;
    FORCE_INLINE u32 router_cluster_count(u32 vocab) noexcept
    {
        return (vocab + ROUTER_CLUSTER_SIZE - 1) >> ROUTER_CLUSTER_SHIFT;
    }
    FORCE_INLINE u32 router_cluster_of(u32 token) noexcept { return token >> ROUTER_CLUSTER_SHIFT; }
    // Adaptive computation: the network predicts when its latent state is ready.
    // The maximum is a safety/latency budget, not a fixed execution depth.
    constexpr u32 REASON_MIN_STEPS = 2;
#ifndef BIT_LLM_REASON_MAX_STEPS
#define BIT_LLM_REASON_MAX_STEPS 16
#endif
#ifndef BIT_LLM_MEMORY_READ_BUDGET
#define BIT_LLM_MEMORY_READ_BUDGET 8
#endif
#ifndef BIT_LLM_MEMORY_TOP_K
#define BIT_LLM_MEMORY_TOP_K 3
#endif
    constexpr u32 REASON_MAX_STEPS = BIT_LLM_REASON_MAX_STEPS;
    constexpr u32 REASON_STEPS = REASON_MAX_STEPS; // BPTT unroll ceiling; runtime policy may stop earlier
    constexpr u32 MAX_MEMORY_READS_PER_BEAM = BIT_LLM_MEMORY_READ_BUDGET;
    constexpr u32 MEMORY_READ_TOP_K = BIT_LLM_MEMORY_TOP_K;
    static_assert(MEMORY_READ_TOP_K >= 1 && MEMORY_READ_TOP_K <= 16, "memory top-k must be 1..16");
    static_assert(REASON_MIN_STEPS >= 1 && REASON_MAX_STEPS >= REASON_MIN_STEPS && REASON_MAX_STEPS <= 32, "adaptive reasoning budget must be 2..32");
    static_assert(MAX_MEMORY_READS_PER_BEAM >= 1 && MAX_MEMORY_READS_PER_BEAM <= 32, "memory read budget must be 1..32");
    static u32 g_reasonBudget = REASON_MAX_STEPS;
    // v22 orchestration services; host APIs expose telemetry and bounded planning.
    static bitscale::ResourceGovernor g_resourceGovernor;
    static bitscale::LongContextBank g_longContext;
    static bitscale::ReasoningGraph g_reasonGraph;
    static bitscale::CreativeCritic g_creativeCritic;
    static bitscale::CurriculumScheduler g_curriculum;

    static u32 g_reasonLastSteps = 0, g_reasonStepSum = 0, g_reasonCallCount = 0;
    static float g_lastHaltProbability = 0.0f;
    static float g_lastMemoryProbability = 0.0f;
    static u64 g_outputTokenScores = 0;
    static u64 g_outputHeadMacs = 0; // fine-head MACs
    static u64 g_outputRouterMacs = 0;
    static u64 g_outputClustersVisited = 0;
    constexpr u32 NF = 1u << FEAT_BITS;
    constexpr u32 QE = 512; // thang embedding: 1.0 thật = 512 đơn vị i16
    constexpr u32 WS = 64;  // thang trọng số i8: 1.0 thật = 64
    constexpr u32 KW_COUNT = 3;
    constexpr u32 MAX_VOCAB = 1u << 20, MAX_TOKEN_LEN = 32, IO_BUFFER_SIZE = 8192, MAX_GEN_TOKENS = BIT_LLM_MAX_GEN_TOKENS, MAX_BEAM = BIT_LLM_MAX_BEAM;
    constexpr u32 MAX_Q = BIT_LLM_MAX_Q, MAX_A = BIT_LLM_MAX_A, MAX_PAIRS = 32768, DATA_BUFFER_SIZE = 8u << 20, CHUNK_BUFFER_SIZE = 1u << 20;
    constexpr u32 EOS = 0, BOS = 0xFFFFFFFEu, UNK = 0xFFFFFFFFu;
    constexpr u32 MAX_STEP_FEATS = 12, MAX_QF = MAX_Q * 8 + 8;
    static_assert(MAX_QF >= MAX_Q * 6 + 1, "QFeats index buffer is too small for tokenizer features");
    static_assert(MAX_GEN_TOKENS >= 192 && MAX_GEN_TOKENS <= 512, "generation capacity must be 192..512");
    static_assert(MAX_BEAM >= 1 && MAX_BEAM <= 16, "beam capacity must be 1..16");
    static_assert(MAX_Q >= 96 && MAX_Q <= 256 && MAX_A >= 192 && MAX_A <= 512, "context capacities are outside supported bounds");
    static_assert(BIT_LLM_TOKEN_PICKER_CAP >= 4 * MAX_BEAM, "token picker capacity must cover beam fan-out");
    constexpr u32 ARCH_HASH = ((((L1 * 31u + H1) * 31u + LH) * 31u + QE) * 31u + WS) * 31u + FEAT_BITS * 131u + KW_COUNT * 7u + TOWER_BUCKETS * 19u + REASON_MAX_STEPS * 17u + MAX_MEMORY_READS_PER_BEAM * 3u + MEMORY_READ_TOP_K * 11u + ROUTER_CLUSTER_SIZE * 31u + MAX_GEN_TOKENS * 13u + MAX_BEAM * 17u + MAX_Q * 19u + MAX_A * 23u + 0x75u; // v20 trained hierarchical router + conditional fine head

    // ------------------------------------------------------------ tiện ích bit/hash
    FORCE_INLINE u64 splitmix64(u64 &st) noexcept
    {
        u64 z = (st += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    FORCE_INLINE u64 mix64(u64 x) noexcept
    {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }
    FORCE_INLINE i32 clampi(i32 v, i32 lo, i32 hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

    // ---- UTF-8: hạ chữ thường + bỏ dấu tiếng Việt
    FORCE_INLINE u32 lower_cp(u32 c) noexcept
    {
        if (c < 0x80)
            return (c >= 'A' && c <= 'Z') ? c + 32 : c;
        if (c >= 0xC0 && c <= 0xDE)
            return c == 0xD7 ? c : c + 32;
        if (c >= 0x100 && c <= 0x137)
            return (c == 0x130 || c == 0x131) ? c : (c | 1u);
        if (c >= 0x139 && c <= 0x148)
            return (c & 1u) ? c + 1 : c;
        if (c >= 0x14A && c <= 0x177)
            return c | 1u;
        if (c == 0x178)
            return 0xFF;
        if (c >= 0x179 && c <= 0x17E)
            return (c & 1u) ? c + 1 : c;
        if (c == 0x1A0 || c == 0x1AF)
            return c + 1;
        if (c >= 0x1E00 && c <= 0x1EFF && !(c >= 0x1E96 && c <= 0x1E9F))
            return c | 1u;
        return c;
    }
    // code point chữ thường -> chữ cái gốc (chỉ xử lý tiếng Việt / Latin thường gặp); không đổi thì trả lại chính nó
    inline u32 strip_cp(u32 c) noexcept
    {
        switch (c)
        {
        case 0xE0:
        case 0xE1:
        case 0xE2:
        case 0xE3:
        case 0x103:
            return 'a';
        case 0xE8:
        case 0xE9:
        case 0xEA:
            return 'e';
        case 0xEC:
        case 0xED:
        case 0x129:
            return 'i';
        case 0xF2:
        case 0xF3:
        case 0xF4:
        case 0xF5:
        case 0x1A1:
            return 'o';
        case 0xF9:
        case 0xFA:
        case 0x169:
        case 0x1B0:
            return 'u';
        case 0xFD:
            return 'y';
        case 0x111:
            return 'd';
        default:
            break;
        }
        if (c >= 0x1EA1 && c <= 0x1EB7)
            return 'a';
        if (c >= 0x1EB9 && c <= 0x1EC7)
            return 'e';
        if (c >= 0x1EC9 && c <= 0x1ECB)
            return 'i';
        if (c >= 0x1ECD && c <= 0x1EE3)
            return 'o';
        if (c >= 0x1EE5 && c <= 0x1EF1)
            return 'u';
        if (c >= 0x1EF3 && c <= 0x1EF9)
            return 'y';
        if ((c >= 0x0300 && c <= 0x0303) || c == 0x0306 || c == 0x0309 || c == 0x031B || c == 0x0323)
            return 0;
        return c;
    }
    inline u32 decode_utf8(const char *s, u32 len, u32 &i) noexcept
    {
        const u8 c = (u8)s[i];
        if (c < 0x80)
        {
            ++i;
            return c;
        }
        const u32 n = c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : (c >= 0xC0 ? 2 : 1));
        bool ok = n > 1 && i + n <= len;
        for (u32 j = 1; ok && j < n; ++j)
            ok = ((u8)s[i + j] & 0xC0) == 0x80;
        if (!ok)
        {
            ++i;
            return 0x110000u + c; // byte không hợp lệ: giữ nguyên
        }
        u32 cp = n == 2 ? (c & 0x1Fu) : (n == 3 ? (c & 0x0Fu) : (c & 0x07u));
        for (u32 j = 1; j < n; ++j)
            cp = (cp << 6) | ((u8)s[i + j] & 0x3Fu);
        const u32 minCp = n == 2 ? 0x80u : (n == 3 ? 0x800u : 0x10000u);
        if (cp < minCp || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
        {
            ++i;
            return 0x110000u + c;
        }
        i += n;
        return cp;
    }
    inline u32 encode_utf8(u32 cp, char *o) noexcept
    {
        if (cp >= 0x110000u)
        {
            o[0] = (char)(cp - 0x110000u);
            return 1;
        }
        if (cp < 0x80)
        {
            o[0] = (char)cp;
            return 1;
        }
        if (cp < 0x800)
        {
            o[0] = (char)(0xC0 | (cp >> 6));
            o[1] = (char)(0x80 | (cp & 0x3F));
            return 2;
        }
        if (cp < 0x10000)
        {
            o[0] = (char)(0xE0 | (cp >> 12));
            o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            o[2] = (char)(0x80 | (cp & 0x3F));
            return 3;
        }
        o[0] = (char)(0xF0 | (cp >> 18));
        o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    // Một lượt qua từ: băm bản chữ thường (h), băm bản bỏ dấu (h2), ghi bản chữ thường vào low (>= 4*len+1 byte). Trả độ dài low.
    inline u32 word_hashes(const char *s, u32 len, u64 &h, u64 &h2, char *low) noexcept
    {
        h = 0xcbf29ce484222325ULL;
        h2 = 0x84222325cbf29ce4ULL;
        u32 i = 0, o = 0;
        char tmp[4];
        while (i < len)
        {
            const u32 lc = lower_cp(decode_utf8(s, len, i));
            const u32 n = encode_utf8(lc, tmp);
            for (u32 k = 0; k < n; ++k)
            {
                low[o++] = tmp[k];
                h = (h ^ (u8)tmp[k]) * 0x100000001b3ULL;
            }
            const u32 stripped = strip_cp(lc);
            if (stripped)
            {
                const u32 m = encode_utf8(stripped, tmp);
                for (u32 k = 0; k < m; ++k)
                    h2 = (h2 ^ (u8)tmp[k]) * 0x100000001b3ULL;
            }
        }
        low[o] = 0;
        return o;
    }
    FORCE_INLINE bool is_sep(char c) noexcept
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == '.' || c == '?' || c == '!' || c == ';' || c == ':' || c == '"' || c == '\'' ||
               c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '-' || c == '/' || c == '\\' || c == '|' || c == '=' || c == '<' || c == '>';
    }

    inline u32 crc32_update(u32 crc, const void *data, size_t n) noexcept
    {
        // Function-local static initialization is thread-safe since C++11.
        // The old bool/init pair could race if native callers checked models concurrently.
        static const std::array<u32, 256> tab = []() noexcept
        {
            std::array<u32, 256> t{};
            for (u32 i = 0; i < 256; ++i)
            {
                u32 c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();
        const u8 *p = (const u8 *)data;
        crc = ~crc;
        for (size_t i = 0; i < n; ++i)
            crc = tab[(crc ^ p[i]) & 255] ^ (crc >> 8);
        return ~crc;
    }

    // ------------------------------------------------------------ HEADER / BỐ CỤC FILE
    struct alignas(64) ModelHeader
    {
        u32 magic, version, vocabCount, epochs, numDocs, l1, featBits, lh;
        u32 arenaBytes, descBytes, archHash, netCrc, h1, half, formatFlags, formatReserved;
        float temp; // nhiệt độ hiệu chuẩn tau: z = logit / tau
        u32 qe, ws, kwCount, reserved[12];
    };
    static_assert(sizeof(ModelHeader) == 128, "ModelHeader phai 128 byte");
    constexpr u32 HEADER_SIZE = (u32)sizeof(ModelHeader);

    enum class Section : u8
    {
        Header,
        Hash,
        Df,
        Len,
        Arena,
        Desc,
        Emb,
        Tower,
        HeadW,
        HeadB,
        RouterW,
        RouterB,
        Count
    };
    constexpr u32 NSEC = (u32)Section::Count;

    struct TowerQ
    {
        i16 ebias[L1];
        i8 w1[H1 * L1];
        i32 b1[H1];
        i8 wr[H1 * H1];
        i32 br[H1];
        i8 whalt[H1];
        i32 bhalt;
        i8 wmem[H1];
        i32 bmem;
        i8 w2[TOWER_BUCKETS * LH * H1];
        i32 b2[TOWER_BUCKETS * LH];
    };

    alignas(64) static ModelHeader g_model{};
    alignas(64) static TowerQ g_tw{};
    static i16 *g_emb = nullptr; // NF*L1
    static i8 *g_hw = nullptr;   // vocab*LH, fine conditional head
    static float *g_hb = nullptr; // vocab, fine conditional bias
    static i8 *g_routerW = nullptr; // ceil(vocab/128)*LH, trained coarse router
    static float *g_routerB = nullptr;
    static u32 g_routerCap = 0;
    static i16 *g_fb = nullptr; // compatibility view: tổng phản hồi theo token, không dùng để chấm điểm
    constexpr u32 FB_CLUSTERS = 1024, FB_WAYS = 4;
    struct FeedbackEntry
    {
        u64 context;
        u32 token;
        i16 score;
        u8 age, used;
    };
    struct alignas(64) FeedbackCluster
    {
        FeedbackEntry e[FB_WAYS];
    };
    static_assert(sizeof(FeedbackEntry) == 16 && sizeof(FeedbackCluster) == 64, "feedback entries must fit one cache line");
    static FeedbackCluster g_feedback[FB_CLUSTERS]{};
    static u8 g_feedbackGen = 1;
    static u64 g_feedbackEpoch = 1, g_feedbackCacheContext = 0, g_feedbackCacheEpoch = 0;
    static std::vector<i16> g_feedbackQueryScore;
    FORCE_INLINE u32 feedback_cluster_index(u64 context, u32 token) noexcept
    {
        const u64 h = mix64(context ^ ((u64)token * 0x9E3779B97F4A7C15ULL));
        return (u32)(h >> (64 - 10));
    }
    inline FeedbackEntry *feedback_find(u64 context, u32 token, bool create) noexcept
    {
        FeedbackCluster &c = g_feedback[feedback_cluster_index(context, token)];
        FeedbackEntry *empty = nullptr, *victim = nullptr;
        i32 victimValue = -1;
        for (u32 i = 0; i < FB_WAYS; ++i)
        {
            FeedbackEntry &e = c.e[i];
            if (e.used && e.context == context && e.token == token)
                return &e;
            if (!e.used && !empty)
                empty = &e;
            if (e.used)
            {
                const u32 age = (u8)(g_feedbackGen - e.age);
                const i32 value = (i32)std::min<u32>(age, 255) * 16384 + (8192 - std::abs((i32)e.score));
                if (value > victimValue)
                {
                    victimValue = value;
                    victim = &e;
                }
            }
        }
        if (!create)
            return nullptr;
        FeedbackEntry *e = empty ? empty : victim;
        if (!e)
            return nullptr;
        *e = {context, token, 0, g_feedbackGen, 1};
        return e;
    }
    inline i16 feedback_lookup(u64 context, u32 token) noexcept
    {
        FeedbackEntry *e = feedback_find(context, token, false);
        return e ? e->score : 0;
    }
    inline void feedback_prepare_query(u64 context, u32 vocabCount)
    {
        if (g_feedbackCacheContext == context && g_feedbackCacheEpoch == g_feedbackEpoch && g_feedbackQueryScore.size() == vocabCount)
            return;
        g_feedbackQueryScore.assign(vocabCount, 0);
        for (const FeedbackCluster &c : g_feedback)
            for (const FeedbackEntry &e : c.e)
                if (e.used && e.context == context && e.token < vocabCount)
                    g_feedbackQueryScore[e.token] = e.score;
        g_feedbackCacheContext = context;
        g_feedbackCacheEpoch = g_feedbackEpoch;
    }
    static u32 g_netCap = 0;
    static char g_desc[1024];
    static u32 g_descLen = 0;

    // ---- từ vựng động
    static u64 *g_hash = nullptr;
    static u16 *g_df = nullptr;
    static u8 *g_len = nullptr;
    static u32 *g_off = nullptr;
    static char *g_arena = nullptr;
    static u32 g_arenaUsed = 0, g_arenaCap = 0, g_metaCap = 0;
    static u32 *g_htId = nullptr;
    static u8 *g_htPsl = nullptr;
    static u32 g_htCap = 0;

    inline bool grow_buf(void **p, size_t bytes, size_t oldBytes) noexcept
    {
        void *n = std::realloc(*p, bytes);
        if (!n)
            return false;
        if (bytes > oldBytes)
            std::memset((u8 *)n + oldBytes, 0, bytes - oldBytes);
        *p = n;
        return true;
    }
    inline bool meta_reserve(u32 cap) noexcept
    {
        if (cap <= g_metaCap)
            return true;
        if (!grow_buf((void **)&g_hash, (size_t)cap * 8, (size_t)g_metaCap * 8) || !grow_buf((void **)&g_df, (size_t)cap * 2, (size_t)g_metaCap * 2) ||
            !grow_buf((void **)&g_len, (size_t)cap, (size_t)g_metaCap) || !grow_buf((void **)&g_off, (size_t)cap * 4, (size_t)g_metaCap * 4) ||
            !grow_buf((void **)&g_fb, (size_t)cap * 2, (size_t)g_metaCap * 2))
            return false;
        g_metaCap = cap;
        return true;
    }
    inline bool arena_reserve(u32 bytes) noexcept
    {
        if (bytes <= g_arenaCap)
            return true;
        u32 n = g_arenaCap ? g_arenaCap : 256;
        while (n < bytes)
            n *= 2;
        if (!grow_buf((void **)&g_arena, n, g_arenaCap))
            return false;
        g_arenaCap = n;
        return true;
    }
    // Mạng: bảng embedding (luôn đủ), head theo số token tối đa
    inline bool net_alloc(u32 cap) noexcept
    {
        if (!g_emb)
            g_emb = (i16 *)std::calloc((size_t)NF * L1, 2);
        if (!g_emb)
            return false;
        std::free(g_hw);
        std::free(g_hb);
        std::free(g_routerW);
        std::free(g_routerB);
        const u32 routerCap = std::max<u32>(1, router_cluster_count(cap));
        g_hw = (i8 *)std::calloc((size_t)(cap ? cap : 1) * LH, 1);
        g_hb = (float *)std::calloc((size_t)(cap ? cap : 1), 4);
        g_routerW = (i8 *)std::calloc((size_t)routerCap * LH, 1);
        g_routerB = (float *)std::calloc((size_t)routerCap, sizeof(float));
        g_routerCap = (g_routerW && g_routerB) ? routerCap : 0;
        g_netCap = (g_hw && g_hb && g_routerW && g_routerB) ? cap : 0;
        return g_netCap != 0 || cap == 0;
    }

    // ---- bảng băm Robin Hood (id 0 = <eos> không nằm trong bảng)
    FORCE_INLINE u32 ht_ideal(u64 h) noexcept { return (u32)(h >> 17) & (g_htCap - 1); }
    FORCE_INLINE u32 find_token(u64 h) noexcept
    {
        if (!g_htId)
            return 0;
        const u32 mask = g_htCap - 1;
        u32 idx = ht_ideal(h), psl = 0;
        while (true)
        {
            const u32 id = g_htId[idx];
            if (id == 0)
                return 0;
            if (g_hash[id] == h)
                return id;
            if ((u32)g_htPsl[idx] < psl)
                return 0;
            idx = (idx + 1) & mask;
            if (++psl > 255)
                return 0;
        }
    }
    inline void ht_insert(u32 id) noexcept
    {
        const u32 mask = g_htCap - 1;
        u32 idx = ht_ideal(g_hash[id]), psl = 0, cur = id;
        while (true)
        {
            if (g_htId[idx] == 0)
            {
                g_htId[idx] = cur;
                g_htPsl[idx] = (u8)(psl > 255 ? 255 : psl);
                return;
            }
            if ((u32)g_htPsl[idx] < psl)
            {
                const u32 oi = g_htId[idx], op = g_htPsl[idx];
                g_htId[idx] = cur;
                g_htPsl[idx] = (u8)(psl > 255 ? 255 : psl);
                cur = oi;
                psl = op;
            }
            idx = (idx + 1) & mask;
            ++psl;
        }
    }
    inline bool rebuild_ht(u32 minCap = 0) noexcept
    {
        u32 want = 1024;
        const u32 need = (g_model.vocabCount > minCap ? g_model.vocabCount : minCap) * 2 + 2;
        while (want < need)
            want <<= 1;
        std::free(g_htId);
        std::free(g_htPsl);
        g_htId = (u32 *)std::calloc(want, 4);
        g_htPsl = (u8 *)std::calloc(want, 1);
        if (!g_htId || !g_htPsl)
        {
            g_htCap = 0;
            return false;
        }
        g_htCap = want;
        for (u32 id = 1; id < g_model.vocabCount; ++id)
            ht_insert(id);
        return true;
    }
    inline void rebuild_offsets() noexcept
    {
        u32 o = 0;
        for (u32 i = 0; i < g_model.vocabCount; ++i)
        {
            g_off[i] = o;
            o += g_len[i];
        }
        g_arenaUsed = o;
    }

    // ------------------------------------------------------------ TOKENIZER
    struct TokInfo
    {
        u64 h, h2;  // băm chữ thường / băm bỏ dấu
        u64 sub[3]; // byte n-gram 2/3/4 của từ đã chuẩn hoá, dùng cho subword
        u32 id;     // 0 hoặc UNK = chưa biết
    };
    static u32 g_overflow = 0;
#ifdef BIT_LLM_TRAIN
    static void init_head_row(u32 id) noexcept;
#endif
    inline u32 register_token(const char *low, u32 len, u64 h) noexcept
    {
        if (g_model.vocabCount >= MAX_VOCAB || g_model.vocabCount >= g_netCap || g_model.vocabCount >= g_metaCap || !arena_reserve(g_arenaUsed + len))
        {
            ++g_overflow;
            return UNK;
        }
        if (((u64)g_model.vocabCount + 1) * 2 > g_htCap && !rebuild_ht(g_model.vocabCount + 1))
        {
            ++g_overflow;
            return UNK;
        }
        const u32 id = g_model.vocabCount++;
        g_off[id] = g_arenaUsed;
        g_len[id] = (u8)len;
        std::memcpy(g_arena + g_arenaUsed, low, len);
        g_arenaUsed += len;
        g_hash[id] = h;
        g_df[id] = 0;
        g_fb[id] = 0;
        if (id)
            ht_insert(id);
#ifdef BIT_LLM_TRAIN
        init_head_row(id);
#endif
        return id;
    }
    inline u32 tokenize(const char *s, u32 len, TokInfo *out, u32 maxN, bool add) noexcept
    {
        u32 n = 0, pos = 0;
        char low[4 * MAX_TOKEN_LEN + 8];
        while (pos < len && n < maxN)
        {
            while (pos < len && (u8)s[pos] <= 0x20) ++pos;
            if (pos >= len) break;
            const u32 st = pos;
            // Keep punctuation as explicit one-character tokens. The v18-compatible tokenizer introduced
            // can therefore learn and emit JSON/DSL delimiters rather than losing them.
            if (is_sep(s[pos])) ++pos;
            else while (pos < len && (u8)s[pos] > 0x20 && !is_sep(s[pos])) ++pos;
            u32 wl = pos - st;
            if (wl > MAX_TOKEN_LEN - 1)
            {
                wl = MAX_TOKEN_LEN - 1;
                while (wl > 0 && ((u8)s[st + wl] & 0xC0) == 0x80) --wl;
            }
            if (!wl) continue;
            TokInfo t{};
            const u32 ll = word_hashes(s + st, wl, t.h, t.h2, low);
            for (u32 ng = 2; ng <= 4; ++ng)
            {
                u64 sh = 0x535542574F524400ULL ^ ng;
                if (ll < ng) sh = mix64(sh ^ t.h);
                else for (u32 k = 0; k + ng <= ll; ++k)
                {
                    u64 g = 0xcbf29ce484222325ULL;
                    for (u32 j = 0; j < ng; ++j) g = (g ^ (u8)low[k + j]) * 0x100000001b3ULL;
                    sh = mix64(sh ^ g);
                }
                t.sub[ng - 2] = sh;
            }
            t.id = find_token(t.h);
            if (t.id == 0 && add) t.id = register_token(low, ll, t.h);
            out[n++] = t;
        }
        return n;
    }

    // ------------------------------------------------------------ ĐẶC TRƯNG THƯA -> CHỈ SỐ HÀNG EMBEDDING
    FORCE_INLINE u32 fidx(u32 g, u64 a, u64 b = 0) noexcept
    {
        return (u32)(mix64(a * 0x9E3779B97F4A7C15ULL ^ mix64(b + (u64)g * 0xD6E8FEB86659FD93ULL)) >> (64 - FEAT_BITS));
    }
    // Nhóm: 1 từ | 2 từ bỏ dấu | 3 bigram(bỏ dấu) | 4 từ lạ | 5 độ dài | 6 token đã sinh | 7..12 ngữ cảnh | 13 prefix | 14 subword
    struct QFeats
    {
        u32 idx[MAX_QF];
        u32 n;
        u64 kw[KW_COUNT];
        u32 known, total;
        u64 key;
    };
    inline void build_qfeats(const TokInfo *t, u32 n, QFeats &q) noexcept
    {
        q.n = 0;
        q.known = q.total = 0;
        u64 kwH[KW_COUNT] = {};
        u32 kwDf[KW_COUNT] = {};
        u64 prev2 = 0, key = 0xBEEF;
        for (u32 i = 0; i < n; ++i)
        {
            const bool known = t[i].id != 0 && t[i].id != UNK;
            ++q.total;
            if (known)
            {
                ++q.known;
                q.idx[q.n++] = fidx(1, t[i].h);
            }
            else
                q.idx[q.n++] = fidx(4, 0);
            for (u32 k = 0; k < 3; ++k)
                q.idx[q.n++] = fidx(14, t[i].sub[k]);
            q.idx[q.n++] = fidx(2, t[i].h2);
            if (prev2)
                q.idx[q.n++] = fidx(3, prev2, t[i].h2);
            prev2 = t[i].h2;
            key = mix64(key ^ t[i].h2);
            if (known) // từ khoá = từ hiếm nhất (df nhỏ), hoà thì hash nhỏ hơn
            {
                const u32 df = g_df[t[i].id] ? g_df[t[i].id] : 1;
                bool dup = false;
                for (u32 k = 0; k < KW_COUNT; ++k)
                    dup = dup || kwH[k] == t[i].h;
                if (!dup)
                {
                    u32 pos = KW_COUNT;
                    for (u32 k = 0; k < KW_COUNT && pos == KW_COUNT; ++k)
                        if (kwH[k] == 0 || df < kwDf[k] || (df == kwDf[k] && t[i].h < kwH[k]))
                            pos = k;
                    if (pos < KW_COUNT)
                    {
                        for (u32 j = KW_COUNT - 1; j > pos; --j)
                        {
                            kwH[j] = kwH[j - 1];
                            kwDf[j] = kwDf[j - 1];
                        }
                        kwH[pos] = t[i].h;
                        kwDf[pos] = df;
                    }
                }
            }
        }
        q.idx[q.n++] = fidx(5, n < 15 ? n : 15);
        for (u32 k = 0; k < KW_COUNT; ++k)
            q.kw[k] = kwH[k];
        q.key = mix64(key);
    }
    // Đặc trưng phụ thuộc ngữ cảnh bước (y1,y2,y3,t): các hàng này bị TRỪ rồi CỘNG khi tiến một bước
    FORCE_INLINE u32 step_feats(const QFeats &q, u32 y1, u32 y2, u32 y3, u32 t, u64 prefix, u32 *o) noexcept
    {
        u32 n = 0;
        o[n++] = fidx(7, y1);
        o[n++] = fidx(8, y2);
        o[n++] = fidx(9, y3);
        o[n++] = fidx(10, t < 31 ? t : 31);
        o[n++] = fidx(12, y1, y2);
        o[n++] = fidx(13, q.key, prefix);
        for (u32 k = 0; k < KW_COUNT; ++k)
            if (q.kw[k])
                o[n++] = fidx(11, q.kw[k], y1);
        return n;
    }
    FORCE_INLINE u32 sofar_feat(u32 tok) noexcept { return fidx(6, tok); }
    FORCE_INLINE u64 prefix_seed(const QFeats &q) noexcept { return mix64(q.key ^ 0x5052454649585631ULL); }
    FORCE_INLINE u64 prefix_next(u64 prefix, u32 tok) noexcept { return mix64(prefix ^ ((u64)tok + 0x9E3779B97F4A7C15ULL)); }

    // ------------------------------------------------------------ SUY LUẬN SỐ NGUYÊN: ACCUMULATOR + THÁP + HEAD
    FORCE_INLINE void row_add(i32 *RESTRICT_ACC, const i16 *row) noexcept
    {
#if defined(__wasm_simd128__)
        // Stockfish NNUE accumulators use SIMD widening loads for the same
        // int16-weight -> int32-accumulator operation. Keep scalar tail/fallback
        // so this remains correct if L1 changes or SIMD is disabled.
        u32 j = 0;
        for (; j + 8 <= L1; j += 8)
        {
            const v128_t w = wasm_v128_load(row + j);
            const v128_t lo = wasm_i32x4_extend_low_i16x8(w);
            const v128_t hi = wasm_i32x4_extend_high_i16x8(w);
            wasm_v128_store(RESTRICT_ACC + j, wasm_i32x4_add(wasm_v128_load(RESTRICT_ACC + j), lo));
            wasm_v128_store(RESTRICT_ACC + j + 4, wasm_i32x4_add(wasm_v128_load(RESTRICT_ACC + j + 4), hi));
        }
        for (; j < L1; ++j)
            RESTRICT_ACC[j] += row[j];
#elif defined(__AVX2__)
        u32 j = 0;
        for (; j + 8 <= L1; j += 8)
        {
            const __m128i v16 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(row + j));
            const __m256i v32 = _mm256_cvtepi16_epi32(v16);
            const __m256i a32 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(RESTRICT_ACC + j));
            _mm256_storeu_si256(reinterpret_cast<__m256i *>(RESTRICT_ACC + j), _mm256_add_epi32(a32, v32));
        }
        for (; j < L1; ++j) RESTRICT_ACC[j] += row[j];
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
        u32 j = 0;
        for (; j + 8 <= L1; j += 8)
        {
            const int16x8_t v = vld1q_s16(row + j);
            vst1q_s32(RESTRICT_ACC + j, vaddq_s32(vld1q_s32(RESTRICT_ACC + j), vmovl_s16(vget_low_s16(v))));
            vst1q_s32(RESTRICT_ACC + j + 4, vaddq_s32(vld1q_s32(RESTRICT_ACC + j + 4), vmovl_s16(vget_high_s16(v))));
        }
        for (; j < L1; ++j) RESTRICT_ACC[j] += row[j];
#else
        for (u32 j = 0; j < L1; ++j)
            RESTRICT_ACC[j] += row[j];
#endif
    }
    FORCE_INLINE void row_sub(i32 *RESTRICT_ACC, const i16 *row) noexcept
    {
#if defined(__wasm_simd128__)
        u32 j = 0;
        for (; j + 8 <= L1; j += 8)
        {
            const v128_t w = wasm_v128_load(row + j);
            const v128_t lo = wasm_i32x4_extend_low_i16x8(w);
            const v128_t hi = wasm_i32x4_extend_high_i16x8(w);
            wasm_v128_store(RESTRICT_ACC + j, wasm_i32x4_sub(wasm_v128_load(RESTRICT_ACC + j), lo));
            wasm_v128_store(RESTRICT_ACC + j + 4, wasm_i32x4_sub(wasm_v128_load(RESTRICT_ACC + j + 4), hi));
        }
        for (; j < L1; ++j)
            RESTRICT_ACC[j] -= row[j];
#elif defined(__AVX2__)
        u32 j = 0;
        for (; j + 8 <= L1; j += 8)
        {
            const __m128i v16 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(row + j));
            const __m256i v32 = _mm256_cvtepi16_epi32(v16);
            const __m256i a32 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(RESTRICT_ACC + j));
            _mm256_storeu_si256(reinterpret_cast<__m256i *>(RESTRICT_ACC + j), _mm256_sub_epi32(a32, v32));
        }
        for (; j < L1; ++j) RESTRICT_ACC[j] -= row[j];
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
        u32 j = 0;
        for (; j + 8 <= L1; j += 8)
        {
            const int16x8_t v = vld1q_s16(row + j);
            vst1q_s32(RESTRICT_ACC + j, vsubq_s32(vld1q_s32(RESTRICT_ACC + j), vmovl_s16(vget_low_s16(v))));
            vst1q_s32(RESTRICT_ACC + j + 4, vsubq_s32(vld1q_s32(RESTRICT_ACC + j + 4), vmovl_s16(vget_high_s16(v))));
        }
        for (; j < L1; ++j) RESTRICT_ACC[j] -= row[j];
#else
        for (u32 j = 0; j < L1; ++j)
            RESTRICT_ACC[j] -= row[j];
#endif
    }
    // Activations in the NNUE tower are clipped to [0,127], so signed-byte
    // widening multiply is exact for both operands. This mirrors Stockfish's
    // narrow-weight / wide-accumulator dot products and is used by every head
    // score as well as both dense tower layers.
    // Compile-time kernel policy: callers can force the scalar reference path without a
    // runtime ISA branch. The default dispatch is selected entirely by compiler target flags.
    template <u32 N, bool AllowVector = true>
    FORCE_INLINE i32 dot_i8_u8(const i8 *w, const u8 *a) noexcept
    {
#if defined(__AVX512BW__)
        if constexpr (AllowVector)
        {
            const __m512i ones = _mm512_set1_epi16(1);
            __m512i acc = _mm512_setzero_si512();
            u32 j = 0;
            for (; j + 64 <= N; j += 64)
            {
                const __m512i vw = _mm512_loadu_si512(reinterpret_cast<const void *>(w + j));
                const __m512i va = _mm512_loadu_si512(reinterpret_cast<const void *>(a + j));
                const __m512i pair = _mm512_maddubs_epi16(va, vw);
                acc = _mm512_add_epi32(acc, _mm512_madd_epi16(pair, ones));
            }
            alignas(64) i32 lanes[16];
            _mm512_store_si512(reinterpret_cast<void *>(lanes), acc);
            i32 total = 0;
            for (u32 k = 0; k < 16; ++k) total += lanes[k];
            for (; j < N; ++j) total += (i32)w[j] * a[j];
            return total;
        }
        else
        {
            i32 total = 0; for (u32 j = 0; j < N; ++j) total += (i32)w[j] * a[j]; return total;
        }
#elif defined(__AVX2__)
        if constexpr (AllowVector)
        {
            const __m256i ones = _mm256_set1_epi16(1);
            __m256i acc = _mm256_setzero_si256();
            u32 j = 0;
            for (; j + 32 <= N; j += 32)
            {
                const __m256i vw = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(w + j));
                const __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(a + j));
                const __m256i pair = _mm256_maddubs_epi16(va, vw);
                acc = _mm256_add_epi32(acc, _mm256_madd_epi16(pair, ones));
            }
            alignas(32) i32 lanes[8];
            _mm256_store_si256(reinterpret_cast<__m256i *>(lanes), acc);
            i32 total = 0;
            for (u32 k = 0; k < 8; ++k) total += lanes[k];
            for (; j < N; ++j) total += (i32)w[j] * a[j];
            return total;
        }
        else
        {
            i32 total = 0; for (u32 j = 0; j < N; ++j) total += (i32)w[j] * a[j]; return total;
        }
#elif defined(__wasm_simd128__)
        if constexpr (AllowVector)
        {
            v128_t sum0 = wasm_i32x4_splat(0), sum1 = wasm_i32x4_splat(0);
            u32 j = 0;
            for (; j + 16 <= N; j += 16)
            {
                const v128_t vw = wasm_v128_load(w + j), va = wasm_v128_load(a + j);
                const v128_t p0 = wasm_i16x8_extmul_low_i8x16(vw, va);
                const v128_t p1 = wasm_i16x8_extmul_high_i8x16(vw, va);
                const v128_t pair = wasm_i16x8_add(p0, p1);
                sum0 = wasm_i32x4_add(sum0, wasm_i32x4_extend_low_i16x8(pair));
                sum1 = wasm_i32x4_add(sum1, wasm_i32x4_extend_high_i16x8(pair));
            }
            alignas(16) i32 lanes0[4], lanes1[4];
            wasm_v128_store(lanes0, sum0); wasm_v128_store(lanes1, sum1);
            i32 total = lanes0[0]+lanes0[1]+lanes0[2]+lanes0[3]+lanes1[0]+lanes1[1]+lanes1[2]+lanes1[3];
            for (; j < N; ++j) total += (i32)w[j] * a[j];
            return total;
        }
        else
        {
            i32 total = 0; for (u32 j = 0; j < N; ++j) total += (i32)w[j] * a[j]; return total;
        }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
        if constexpr (AllowVector)
        {
            int32x4_t acc = vdupq_n_s32(0);
            u32 j = 0;
            for (; j + 16 <= N; j += 16)
            {
                const int8x16_t vw = vld1q_s8(w + j);
                const uint8x16_t vu = vld1q_u8(a + j);
                const int8x16_t va = vreinterpretq_s8_u8(vu); // activations are bounded to 0..127
                const int16x8_t p0 = vmull_s8(vget_low_s8(vw), vget_low_s8(va));
                const int16x8_t p1 = vmull_s8(vget_high_s8(vw), vget_high_s8(va));
                acc = vaddq_s32(acc, vpaddlq_s16(p0));
                acc = vaddq_s32(acc, vpaddlq_s16(p1));
            }
            i32 lanes[4]; vst1q_s32(lanes, acc);
            i32 total = lanes[0]+lanes[1]+lanes[2]+lanes[3];
            for (; j < N; ++j) total += (i32)w[j] * a[j];
            return total;
        }
        else
        {
            i32 total = 0; for (u32 j = 0; j < N; ++j) total += (i32)w[j] * a[j]; return total;
        }
#else
        (void)AllowVector;
        i32 total = 0; for (u32 j = 0; j < N; ++j) total += (i32)w[j] * a[j]; return total;
#endif
    }
    template <int Sign>
    inline void acc_apply(i32 *acc, const u32 *idx, u32 n) noexcept
    {
        static_assert(Sign == 1 || Sign == -1, "accumulator updates are add or subtract");
        for (u32 i = 0; i < n; ++i)
        {
            const i16 *r = g_emb + (size_t)idx[i] * L1;
            if constexpr (Sign > 0)
                row_add(acc, r);
            else
                row_sub(acc, r);
        }
    }
    inline void acc_init(const QFeats &q, i32 *acc) noexcept
    {
        for (u32 j = 0; j < L1; ++j)
            acc[j] = g_tw.ebias[j];
        acc_apply<1>(acc, q.idx, q.n);
    }
    inline void acc_start(const QFeats &q, i32 *accQ, i32 *accS, u64 &prefix) noexcept
    {
        acc_init(q, accQ);
        std::memset(accS, 0, sizeof(i32) * L1);
        prefix = prefix_seed(q);
        u32 f[MAX_STEP_FEATS];
        acc_apply<1>(accS, f, step_feats(q, BOS, BOS, BOS, 0, prefix, f));
    }
    // Tiến một bước (gia tăng): trừ ngữ cảnh cũ, cộng ngữ cảnh mới + token vừa sinh
    inline void acc_advance(i32 *accS, const QFeats &q, u32 y1, u32 y2, u32 y3, u32 len, u64 &prefix, u32 tok) noexcept
    {
        u32 a[MAX_STEP_FEATS], b[MAX_STEP_FEATS];
        acc_apply<-1>(accS, a, step_feats(q, y1, y2, y3, len, prefix, a));
        const u64 nextPrefix = prefix_next(prefix, tok);
        acc_apply<1>(accS, b, step_feats(q, tok, y1, y2, len + 1, nextPrefix, b));
        const u32 sf = sofar_feat(tok);
        acc_apply<1>(accS, &sf, 1);
        prefix = nextPrefix;
    }
    // Adaptive neural deliberation. A learned halting neuron reads the current
    // latent state after each recurrent transition. It may stop early; the budget
    // only caps pathological/very uncertain trajectories.
    FORCE_INLINE float halt_probability(const u8 *state) noexcept
    {
        const i32 z = g_tw.bhalt + dot_i8_u8<H1>(g_tw.whalt, state);
        const float x = std::max(-12.0f, std::min(12.0f, (float)z * (1.0f / (127.0f * WS))));
        return 1.0f / (1.0f + std::exp(-x));
    }
    FORCE_INLINE float memory_read_probability(const u8 *state) noexcept
    {
        const i32 z = g_tw.bmem + dot_i8_u8<H1>(g_tw.wmem, state);
        const float x = std::max(-12.0f, std::min(12.0f, (float)z * (1.0f / (127.0f * WS))));
        return 1.0f / (1.0f + std::exp(-x));
    }
    FORCE_INLINE u32 tower_bucket(u32 pos) noexcept { return pos < 1 ? 0 : (pos < 3 ? 1 : (pos < 9 ? 2 : 3)); }
    inline void tower_reasoned_adaptive(const i32 *accQ, const i32 *accS, u8 *h8, u32 pos) noexcept
    {
        u8 p8[L1], state[H1], next[H1];
        for (u32 j = 0; j < L1; ++j)
        {
            const i32 ca = clampi(accQ[j], 0, (i32)QE), cb = clampi(accS[j], 0, (i32)QE);
            p8[j] = (u8)((ca * cb * 127 + (1 << 17)) >> 18);
        }
        for (u32 i = 0; i < H1; ++i)
        {
            const i32 z = g_tw.b1[i] + dot_i8_u8<L1>(g_tw.w1 + i * L1, p8);
            state[i] = (u8)clampi((z + 32) >> 6, 0, 127);
        }
        const u32 budget = std::max<u32>(REASON_MIN_STEPS, std::min<u32>(REASON_MAX_STEPS, g_reasonBudget));
        u32 steps = 0;
        float hp = 0.0f;
        for (u32 step = 0; step < budget; ++step)
        {
            for (u32 i = 0; i < H1; ++i)
            {
                const i32 branch = g_tw.br[i] + dot_i8_u8<H1>(g_tw.wr + i * H1, state);
                const u8 candidate = (u8)clampi((i32)state[i] + ((branch + 32) >> 6), 0, 127);
                next[i] = (u8)((3u * (u32)state[i] + (u32)candidate + 2u) >> 2);
            }
            std::memcpy(state, next, sizeof(state));
            ++steps;
            hp = halt_probability(state);
            // A two-transition floor prevents the controller from learning a
            // trivial one-pass shortcut. Beyond that, stop is model-predicted.
            if (steps >= REASON_MIN_STEPS && hp >= 0.5f)
                break;
        }
        g_reasonLastSteps = steps;
        g_reasonStepSum += steps;
        ++g_reasonCallCount;
        g_lastHaltProbability = hp;
        g_lastMemoryProbability = memory_read_probability(state);
        const u32 bucket = tower_bucket(pos);
        const i8 *w2 = g_tw.w2 + (size_t)bucket * LH * H1;
        const i32 *b2 = g_tw.b2 + (size_t)bucket * LH;
        for (u32 i = 0; i < LH; ++i)
        {
            const i32 z = b2[i] + dot_i8_u8<H1>(w2 + i * H1, state);
            h8[i] = (u8)clampi((z + 32) >> 6, 0, 127);
        }
    }
    FORCE_INLINE void tower(const i32 *accQ, const i32 *accS, u8 *h8, u32 pos) noexcept
    {
        tower_reasoned_adaptive(accQ, accS, h8, pos);
    }
    FORCE_INLINE float head_dot(const u8 *h8, u32 t) noexcept
    {
        const i8 *w = g_hw + (size_t)t * LH;
        const i32 s = dot_i8_u8<LH>(w, h8);
        return (float)s * (1.0f / (127.0f * WS)) + g_hb[t];
    }
    FORCE_INLINE float router_head_dot(const u8 *h8, u32 cluster) noexcept
    {
        const i8 *w = g_routerW + (size_t)cluster * LH;
        const i32 s = dot_i8_u8<LH>(w, h8);
        return (float)s * (1.0f / (127.0f * WS)) + g_routerB[cluster];
    }
    template <bool Parallel = true>
    inline void head_all(const u8 *h8, u32 n, float *out) noexcept
    {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) if (Parallel && n > 4096)
#endif
        for (i64 t = 0; t < (i64)n; ++t)
            out[t] = head_dot(h8, (u32)t);
    }

    // ------------------------------------------------------------ CONTEXT TT    // ------------------------------------------------------------ CONTEXT TT (4 entries with full 64-bit key)
    struct TTEntry
    {
        u64 key, packed;
    };
    struct alignas(64) TTCluster
    {
        TTEntry e[4];
    };
    constexpr u32 TT_CL = 1u << 12;
    static_assert(sizeof(TTEntry) == 16 && sizeof(TTCluster) == 64, "TT cluster must fit one cache line");
    static_assert(MAX_VOCAB <= (1u << 20), "packed TT tokens require 20-bit token ids");
    static TTCluster g_ttab[TT_CL];
    static u8 g_ttgen = 1;
    FORCE_INLINE u32 tt_count(const TTEntry &e) noexcept { return (u32)((e.packed >> 60) & 3u); }
    FORCE_INLINE u32 tt_token(const TTEntry &e, u32 i) noexcept { return (u32)((e.packed >> (20 * i)) & 0xFFFFFu); }
    FORCE_INLINE u32 tt_age(const TTEntry &e) noexcept { return ((u32)g_ttgen - (u32)(e.packed >> 62)) & 3u; }
    FORCE_INLINE u64 tt_pack(const u32 *toks, u32 n) noexcept
    {
        u64 p = (u64)(n & 3u) << 60;
        for (u32 i = 0; i < n; ++i)
            p |= ((u64)toks[i] & 0xFFFFFu) << (20 * i);
        return p | ((u64)(g_ttgen & 3u) << 62);
    }
    FORCE_INLINE u64 ctx_key(u64 qkey, u32 y1, u32 y2, u32 y3, u32 t) noexcept
    {
        return mix64(qkey) ^ mix64(y1 * 4ull + 1) ^ mix64(y2 * 4ull + 2) ^ mix64(y3 * 4ull + 3) ^ mix64(0xC0FFEEull + t);
    }
    FORCE_INLINE TTCluster &tt_cluster(u64 key) noexcept { return g_ttab[(u32)(key >> 52)]; }
    inline TTEntry *tt_probe(u64 key) noexcept
    {
        TTCluster &c = tt_cluster(key);
        for (u32 i = 0; i < 4; ++i)
            if (c.e[i].key == key && tt_count(c.e[i]))
            {
                c.e[i].packed = (c.e[i].packed & ((1ULL << 62) - 1)) | ((u64)(g_ttgen & 3u) << 62);
                return &c.e[i];
            }
        return nullptr;
    }
    inline void tt_store(u64 key, const u32 *toks, u32 n) noexcept
    {
        n = std::min<u32>(n, 3);
        if (n == 0)
            return;
        TTCluster &c = tt_cluster(key);
        TTEntry *r = nullptr;
        for (u32 i = 0; i < 4 && !r; ++i)
            if (c.e[i].key == key && tt_count(c.e[i]))
                r = &c.e[i];
        for (u32 i = 0; i < 4 && !r; ++i)
            if (tt_count(c.e[i]) == 0)
                r = &c.e[i];
        if (!r)
        {
            i32 worst = -1;
            for (u32 i = 0; i < 4; ++i)
            {
                const i32 value = (i32)tt_age(c.e[i]) * 4 + (3 - (i32)tt_count(c.e[i]));
                if (value > worst)
                {
                    worst = value;
                    r = &c.e[i];
                }
            }
        }
        r->key = key;
        r->packed = tt_pack(toks, n);
    }

    // ------------------------------------------------------------ OPTION REGISTRY (kiểu UCI)
    static i32 o_Beam = 8, o_Creativity = 100, o_MinLogProb = 200, o_Fast = 0, o_RepPenalty = 150, o_Nodes = 0, o_MinCand = 24;
    struct OptDef
    {
        const char *name;
        i32 *val;
        i32 mn, mx;
    };
    // FastPicker/MinCand were superseded by the trained hierarchical router.
    // Keep internal symbols for legacy code paths, but do not expose inert options.
    static const OptDef g_opts[] = {{"Beam", &o_Beam, 1, (i32)MAX_BEAM}, {"Creativity", &o_Creativity, 0, 300}, {"MinLogProb", &o_MinLogProb, 0, 1000}, {"RepPenalty", &o_RepPenalty, 0, 1000}, {"Nodes", &o_Nodes, 0, 100000000}};
    constexpr u32 N_OPTS = sizeof(g_opts) / sizeof(g_opts[0]);

    // ------------------------------------------------------------ GỌI TRẠNG THÁI TOÀN CỤC KHÁC
    static char g_in[IO_BUFFER_SIZE], g_out[IO_BUFFER_SIZE];
    static bitart::ArtisticIntelligence g_artistic;
    alignas(64) static float g_artisticCandidateInput[bitart::kCandidateSlots][bitart::kFeatureDims]{};
    alignas(64) static float g_artisticIntentInput[bitart::kIntentDims]{};
    alignas(64) static u8 g_artisticStateOut[4096]{};
    alignas(64) static u8 g_artisticStateIn[4096]{};
    alignas(64) static u8 g_chunk_buffer[CHUNK_BUFFER_SIZE];
    // Persistent, non-parametric context store. It returns evidence snippets only;
    // no memory item is ever copied to the answer buffer or returned as an answer.
    static bitctx::ContextMemory g_contextMemory;
    static bitagent::VideoAgentPlan g_agentPlan;
    static u32 g_agentActiveStepId = 0;
    static std::vector<u8> g_contextSnapshot, g_contextIncoming;
    static bool g_contextSnapshotReady = false;
#if !defined(__EMSCRIPTEN__)
    static bool g_contextDiskInitialized = false;
    static void context_load_disk_once()
    {
        if (g_contextDiskInitialized) return;
        if (!g_contextMemory.load_file("context_memory.ctx")) {
            std::ifstream probe("context_memory.ctx", std::ios::binary);
            if (probe.good()) {
                probe.close(); std::remove("context_memory.corrupt.ctx");
                std::rename("context_memory.ctx", "context_memory.corrupt.ctx");
                g_contextMemory.clear();
            }
        }
        g_contextDiskInitialized = true;
    }
#endif
    struct TraceStep
    {
        i32 token, score, margin;
    };
    static TraceStep g_trace[MAX_GEN_TOKENS];
    static u32 g_traceCount = 0, g_answerSource = 0, g_lastNovelty = 0;
    static u64 g_nodesTotal = 0;
    static u32 g_memoryReadCount = 0;
    static u32 g_lastInputLen = 0;
    static i32 g_stat[8], g_dsStat[8];
    static u32 g_lastTok[MAX_GEN_TOKENS], g_lastN = 0;
    static u64 g_lastFeedbackCtx = 0;
    static u32 g_pairs = 0;

    FORCE_INLINE float calT() noexcept { return g_model.temp >= 0.05f ? g_model.temp : 1.0f; }

    struct Layout
    {
        u64 off[NSEC + 1], bytes[NSEC], total;
        FORCE_INLINE u32 section_at(u64 pos) const noexcept
        {
            u32 i = 0;
            while (i < NSEC - 1 && pos >= off[i + 1])
                ++i;
            return i;
        }
    };
    static Layout g_lay{};
    static u64 g_loaded = 0, g_loadTarget = 0;
    static bool g_headerOk = false;
    static u32 g_headerError = 0;

    inline void compute_layout(u32 n, u32 arena, Layout &L) noexcept
    {
        const u32 rc = router_cluster_count(n);
        const u64 sz[NSEC] = {HEADER_SIZE, (u64)n * 8, (u64)n * 2, (u64)n, (u64)arena, (u64)g_model.descBytes,
                              (u64)NF * L1 * 2, sizeof(TowerQ), (u64)n * LH, (u64)n * 4,
                              (u64)rc * LH, (u64)rc * 4};
        u64 o = 0;
        for (u32 i = 0; i < NSEC; ++i)
        {
            L.off[i] = o;
            L.bytes[i] = sz[i];
            o += (sz[i] + 63) & ~63ULL;
        }
        L.off[NSEC] = o;
        L.total = L.off[NSEC - 1] + L.bytes[NSEC - 1];
    }
    inline bool model_header_valid() noexcept
    {
        g_headerError = 0;

        if (g_model.magic != MODEL_MAGIC)
            return (g_headerError = 1), false;

        if (g_model.version != MODEL_VERSION)
            return (g_headerError = 2), false;

        if (g_model.archHash != ARCH_HASH)
            return (g_headerError = 3), false;

        if (g_model.vocabCount == 0 ||
            g_model.vocabCount > MAX_VOCAB)
            return (g_headerError = 4), false;

        if (g_model.l1 != L1 ||
            g_model.featBits != FEAT_BITS ||
            g_model.lh != LH ||
            g_model.h1 != H1 ||
            g_model.half != HALF ||
            g_model.qe != QE ||
            g_model.ws != WS ||
            g_model.kwCount != KW_COUNT)
            return (g_headerError = 5), false;

        if (g_model.arenaBytes > (u64)g_model.vocabCount * MAX_TOKEN_LEN ||
            g_model.descBytes > sizeof(g_desc) ||
            g_model.numDocs > (1u << 26))
            return (g_headerError = 6), false;

        if (g_model.formatFlags != 0 ||
            g_model.formatReserved != 0)
            return (g_headerError = 7), false;

        if (!std::isfinite(g_model.temp) ||
            g_model.temp < 0.05f ||
            g_model.temp > 100.0f)
            return (g_headerError = 8), false;

        return true;
    }
    inline u8 *section_ptr(u32 i) noexcept
    {
        switch ((Section)i)
        {
        case Section::Header:
            return (u8 *)&g_model;
        case Section::Hash:
            return (u8 *)g_hash;
        case Section::Df:
            return (u8 *)g_df;
        case Section::Len:
            return (u8 *)g_len;
        case Section::Arena:
            return (u8 *)g_arena;
        case Section::Desc:
            return (u8 *)g_desc;
        case Section::Emb:
            return (u8 *)g_emb;
        case Section::Tower:
            return (u8 *)&g_tw;
        case Section::HeadW:
            return (u8 *)g_hw;
        case Section::HeadB:
            return (u8 *)g_hb;
        case Section::RouterW:
            return (u8 *)g_routerW;
        case Section::RouterB:
            return (u8 *)g_routerB;
        default:
            return nullptr;
        }
    }
    inline u32 net_crc() noexcept
    {
        const u32 N = g_model.vocabCount;
        u32 c = crc32_update(0, g_emb, (size_t)NF * L1 * 2);
        c = crc32_update(c, &g_tw, sizeof(g_tw));
        c = crc32_update(c, g_hw, (size_t)N * LH);
        c = crc32_update(c, g_hb, (size_t)N * 4);
        const u32 rc = router_cluster_count(N);
        c = crc32_update(c, g_routerW, (size_t)rc * LH);
        return crc32_update(c, g_routerB, (size_t)rc * sizeof(float));
    }

    // ---- scratch / TokenPicker
    static u32 *g_cid = nullptr;       // bounded candidate ids only, never vocabulary-sized
    static float *g_clg = nullptr;     // retained only for diagnostics/training full scans
    static u32 g_scratchCap = 0;
    constexpr u32 TOKEN_PICKER_CAP = BIT_LLM_TOKEN_PICKER_CAP;
    inline bool ensure_scratch() noexcept
    {
        // Hierarchical inference never materializes a vocabulary-sized logit array.
        const u32 need = TOKEN_PICKER_CAP + 8;
        if (g_cid && g_clg && need <= g_scratchCap)
            return true;
        u32 *newIds = g_cid ? g_cid : (u32 *)std::malloc((size_t)(TOKEN_PICKER_CAP + 8) * sizeof(u32));
        float *newLogits = (float *)std::malloc((size_t)need * sizeof(float));
        if (!newIds || !newLogits)
        {
            if (!g_cid) std::free(newIds);
            std::free(newLogits);
            return false;
        }
        std::free(g_clg);
        g_cid = newIds;
        g_clg = newLogits;
        g_scratchCap = need;
        return true;
    }
    static u32 g_stageWin[4];                  // thống kê: TT candidate hoặc full vocabulary scan

    inline bool rebuild_runtime() noexcept
    {
        rebuild_offsets();
        if (g_arenaUsed != g_model.arenaBytes || !rebuild_ht())
            return false;
        for (u32 id = 1; id < g_model.vocabCount; ++id)
            if (find_token(g_hash[id]) != id)
                return false;
        return ensure_scratch();
    }

    inline void free_meta() noexcept
    {
        std::free(g_hash);
        std::free(g_df);
        std::free(g_len);
        std::free(g_off);
        std::free(g_arena);
        std::free(g_fb);
        std::free(g_htId);
        std::free(g_htPsl);
        g_hash = nullptr;
        g_df = nullptr;
        g_len = nullptr;
        g_off = nullptr;
        g_arena = nullptr;
        g_fb = nullptr;
        g_htId = nullptr;
        g_htPsl = nullptr;
        g_arenaUsed = g_arenaCap = g_metaCap = g_htCap = 0;
    }
    inline void reset_all() noexcept
    {
        g_agentPlan.clear();
        g_headerError = 0;
        g_agentActiveStepId = 0;
        free_meta();
        std::memset(g_feedback, 0, sizeof(g_feedback));
        g_feedbackGen = 1;
        ++g_feedbackEpoch;
        g_feedbackCacheContext = g_feedbackCacheEpoch = 0;
        g_feedbackQueryScore.clear();
        g_lastFeedbackCtx = 0;
        std::memset(&g_model, 0, sizeof(g_model));
        std::memset(&g_lay, 0, sizeof(g_lay));
        std::memset(&g_tw, 0, sizeof(g_tw));
        g_loaded = g_loadTarget = 0;
        g_headerOk = false;
        g_descLen = 0;
    }
} // namespace nnuecut
using namespace nnuecut;

// v20 hierarchical-output model: trained router + cluster-conditional fine head; format is versioned by ARCH_HASH.

// =====================================================================================================================================
// TRAINER (chỉ khi -DBIT_LLM_TRAIN): float master + QAT + hierarchical full-softmax + Adam thưa
// =====================================================================================================================================
#ifdef BIT_LLM_TRAIN
namespace nnuecut
{
    struct Dense
    {
        float *w = nullptr, *m = nullptr, *v = nullptr, *g = nullptr;
        u32 n = 0;
        void release() noexcept
        {
            std::free(w);
            std::free(m);
            std::free(v);
            std::free(g);
            w = m = v = g = nullptr;
            n = 0;
        }
        bool alloc(u32 k) noexcept
        {
            release();
            if (!k)
                return true;
            float *nw = (float *)std::calloc(k, sizeof(float));
            float *nm = (float *)std::calloc(k, sizeof(float));
            float *nv = (float *)std::calloc(k, sizeof(float));
            float *ng = (float *)std::calloc(k, sizeof(float));
            if (!nw || !nm || !nv || !ng)
            {
                std::free(nw);
                std::free(nm);
                std::free(nv);
                std::free(ng);
                return false;
            }
            w = nw;
            m = nm;
            v = nv;
            g = ng;
            n = k;
            return true;
        }
        bool ready() const noexcept { return n != 0 && w && m && v && g; }
    };
    static float *T_E = nullptr, *T_Em = nullptr, *T_Ev = nullptr; // NF*L1
    static Dense D_eb, D_w1, D_b1, D_wr, D_br, D_whalt, D_bhalt, D_wmem, D_bmem, D_w2, D_b2;
    static Dense D_routerW, D_routerB;
    static float *T_hw = nullptr, *T_hwm = nullptr, *T_hwv = nullptr, *T_hb = nullptr, *T_hbm = nullptr, *T_hbv = nullptr;
#ifdef BIT_LLM_DIAGNOSTICS
    static u64 g_diagMaskActive[3], g_diagMaskTotal[3], g_diagDaccZero, g_diagDaccTotal, g_diagLogQCount;
    static double g_diagDaccAbs, g_diagLogQShift;
#endif
    static u64 T_step = 0, g_rng = 0x1234567ULL;
    static float T_beta1Pow = 1.0f, T_beta2Pow = 1.0f;
    static float Q_eb[L1], Q_w1[H1 * L1], Q_b1[H1], Q_wr[H1 * H1], Q_br[H1], Q_whalt[H1], Q_bhalt, Q_wmem[H1], Q_bmem;
    static float Q_w2[TOWER_BUCKETS * LH * H1], Q_b2[TOWER_BUCKETS * LH];
    // siêu tham số huấn luyện
    static float h_lr = 3e-3f, h_lrDecayTo = 0.05f;
    // Hệ số lr riêng cho từng nhóm tham số (nhân với --lr). Lớp dày được Adam cập nhật hàng chục nghìn lần/epoch nên cần lr nhỏ hơn nhiều,
    // nếu không fc1/fc2 bão hoà (chết) và mô hình kẹt ở loss ~ entropy ngẫu nhiên. Có thể ghi đè bằng biến môi trường K_DENSE/K_EMB/K_HEAD.
    static float envf(const char *n, float d)
    {
        const char *v = std::getenv(n);
        return v ? (float)std::atof(v) : d;
    }
    static float k_dense = envf("K_DENSE", 0.03f), k_emb = envf("K_EMB", 0.3f), k_head = envf("K_HEAD", 0.3f);
    static u32 h_neg = 24, h_hnEvery = 16, h_totalEpochs = 100, h_sched = 1;
    // Legacy UI fields retained for ABI compatibility. Hierarchical full-softmax
    // does not sample negatives and does not use a logQ correction.
    static float h_logq = 1.f;
    static u32 g_holdPm = 0, g_holdCount = 0, g_augPct = 20, g_trainEpochs = 0;
    static char g_data[DATA_BUFFER_SIZE + 1];
    static TokInfo *g_qTok = nullptr;
    static u8 g_qLen[MAX_PAIRS];
    static u32 (*g_aTok)[MAX_A] = nullptr;
    static u8 g_aLen[MAX_PAIRS];
    static u8 g_isHold[MAX_PAIRS];
    static u32 g_replayPairs = 0, g_newPairs = 0;
    static float *g_bestE = nullptr; // snapshot
    static std::vector<float> g_bestDense, g_bestHead;
    static bool g_haveBest = false;
    static u32 g_bestVocab = 0;

    FORCE_INLINE float fqE(float x) noexcept
    {
        float r = std::floor(x * QE + 0.5f);
        r = r > 32767.f ? 32767.f : (r < -32768.f ? -32768.f : r);
        return r * (1.f / QE);
    }
    FORCE_INLINE void apply_embedding_features(float *acc, const u32 *ids,
                                                u32 count, float sign) noexcept
    {
        for (u32 f = 0; f < count; ++f)
        {
            const float *row = T_E + (size_t)ids[f] * L1;
            for (u32 j = 0; j < L1; ++j) acc[j] += sign * fqE(row[j]);
        }
    }

    FORCE_INLINE float fqW(float x) noexcept
    {
        float r = std::floor(x * WS + 0.5f);
        r = r > 127.f ? 127.f : (r < -127.f ? -127.f : r);
        return r * (1.f / WS);
    }
    FORCE_INLINE float fqB(float x) noexcept { return std::floor(x * (127.f * WS) + 0.5f) * (1.f / (127.f * WS)); }
    FORCE_INLINE float fqA(float x) noexcept { return std::floor(x * 127.f + 0.5f) * (1.f / 127.f); }
    // C++17-compatible leaky clip derivative: exact mask inside [0,1], small
    // gradient outside so saturated activations can recover.
    FORCE_INLINE float clipMask(float z) noexcept { return (z > 0.f && z < 1.f) ? 1.f : 0.01f; }
    FORCE_INLINE float rnd01() noexcept { return (float)((splitmix64(g_rng) >> 40) * (1.0 / 16777216.0)); }

    static void init_head_row(u32 id) noexcept
    {
        if (!T_hw || id >= TRAIN_VOCAB_CAP)
            return;
        u64 s = mix64(0xBADC0DE ^ id);
        for (u32 j = 0; j < LH; ++j)
        {
            T_hw[(size_t)id * LH + j] = ((float)(splitmix64(s) >> 40) * (1.f / 16777216.f) - 0.5f) * 0.1f;
            T_hwm[(size_t)id * LH + j] = T_hwv[(size_t)id * LH + j] = 0;
        }
        T_hb[id] = T_hbm[id] = T_hbv[id] = 0;
    }
    static bool train_masters_ready() noexcept
    {
        return T_E && T_Em && T_Ev && T_hw && T_hwm && T_hwv && T_hb && T_hbm && T_hbv &&
               D_eb.ready() && D_w1.ready() && D_b1.ready() && D_wr.ready() && D_br.ready() && D_whalt.ready() && D_bhalt.ready() && D_wmem.ready() && D_bmem.ready() && D_w2.ready() && D_b2.ready() && D_routerW.ready() && D_routerB.ready();
    }
    static void release_train_masters() noexcept
    {
        std::free(T_E);
        std::free(T_Em);
        std::free(T_Ev);
        T_E = T_Em = T_Ev = nullptr;
        std::free(T_hw);
        std::free(T_hwm);
        std::free(T_hwv);
        std::free(T_hb);
        std::free(T_hbm);
        std::free(T_hbv);
        T_hw = T_hwm = T_hwv = T_hb = T_hbm = T_hbv = nullptr;
        D_eb.release();
        D_w1.release();
        D_b1.release();
        D_wr.release();
        D_br.release();
        D_whalt.release();
        D_bhalt.release();
        D_wmem.release();
        D_bmem.release();
        D_w2.release();
        D_b2.release();
        D_routerW.release();
        D_routerB.release();
    }
    static bool init_masters() noexcept
    {
        release_train_masters();
        T_E = (float *)std::calloc((size_t)NF * L1, sizeof(float));
        T_Em = (float *)std::calloc((size_t)NF * L1, sizeof(float));
        T_Ev = (float *)std::calloc((size_t)NF * L1, sizeof(float));
        const bool denseOK = D_eb.alloc(L1) && D_w1.alloc(H1 * L1) && D_b1.alloc(H1) &&
                             D_wr.alloc(H1 * H1) && D_br.alloc(H1) && D_whalt.alloc(H1) && D_bhalt.alloc(1) && D_wmem.alloc(H1) && D_bmem.alloc(1) && D_w2.alloc(TOWER_BUCKETS * LH * H1) && D_b2.alloc(TOWER_BUCKETS * LH) &&
                             D_routerW.alloc(ROUTER_TRAIN_CAP * LH) && D_routerB.alloc(ROUTER_TRAIN_CAP);
        const size_t hn = (size_t)TRAIN_VOCAB_CAP * LH;
        T_hw = (float *)std::calloc(hn, sizeof(float));
        T_hwm = (float *)std::calloc(hn, sizeof(float));
        T_hwv = (float *)std::calloc(hn, sizeof(float));
        T_hb = (float *)std::calloc(TRAIN_VOCAB_CAP, sizeof(float));
        T_hbm = (float *)std::calloc(TRAIN_VOCAB_CAP, sizeof(float));
        T_hbv = (float *)std::calloc(TRAIN_VOCAB_CAP, sizeof(float));
        if (!denseOK || !train_masters_ready())
        {
            release_train_masters();
            return false;
        }

        u64 s = 0x5EEDF00DULL;
        for (size_t i = 0; i < (size_t)NF * L1; ++i)
            T_E[i] = ((float)(splitmix64(s) >> 40) * (1.f / 16777216.f) - 0.5f) * 0.1f;
        for (u32 j = 0; j < L1; ++j)
            D_eb.w[j] = 0.5f;
        const float s1 = std::sqrt(3.f / L1), s2 = std::sqrt(3.f / H1), sr = std::sqrt(1.f / H1);
        for (u32 j = 0; j < H1 * L1; ++j)
            D_w1.w[j] = ((float)(splitmix64(s) >> 40) * (1.f / 16777216.f) * 2 - 1) * s1;
        for (u32 j = 0; j < H1 * H1; ++j)
            D_wr.w[j] = ((float)(splitmix64(s) >> 40) * (1.f / 16777216.f) * 2 - 1) * sr;
        for (u32 j = 0; j < TOWER_BUCKETS * LH * H1; ++j)
            D_w2.w[j] = ((float)(splitmix64(s) >> 40) * (1.f / 16777216.f) * 2 - 1) * s2;
        for (u32 j = 0; j < ROUTER_TRAIN_CAP * LH; ++j)
            D_routerW.w[j] = ((float)(splitmix64(s) >> 40) * (1.f / 16777216.f) * 2 - 1) * sr;
        std::memset(D_routerB.w, 0, ROUTER_TRAIN_CAP * sizeof(float));
        for (u32 j = 0; j < H1; ++j)
            D_b1.w[j] = 0.3f;
        std::memset(D_br.w, 0, H1 * sizeof(float));
        std::memset(D_whalt.w, 0, H1 * sizeof(float));
        D_bhalt.w[0] = -1.25f; // learn to stop only when extra compute is no longer useful
        std::memset(D_wmem.w, 0, H1 * sizeof(float));
        D_bmem.w[0] = 0.0f; // neutral prior; training learns when evidence is needed
        for (u32 j = 0; j < TOWER_BUCKETS * LH; ++j)
            D_b2.w[j] = 0.3f;
        T_step = 0;
        T_beta1Pow = T_beta2Pow = 1.0f;
        return true;
    }
    static void refresh_quant() noexcept
    {
        if (!train_masters_ready())
            return;
        for (u32 j = 0; j < L1; ++j)
            Q_eb[j] = fqE(D_eb.w[j]);
        for (u32 j = 0; j < H1 * L1; ++j)
            Q_w1[j] = fqW(D_w1.w[j]);
        for (u32 j = 0; j < H1; ++j)
            Q_b1[j] = fqB(D_b1.w[j]);
        for (u32 j = 0; j < H1 * H1; ++j)
            Q_wr[j] = fqW(D_wr.w[j]);
        for (u32 j = 0; j < H1; ++j)
            Q_br[j] = fqB(D_br.w[j]);
        for (u32 j = 0; j < H1; ++j)
            Q_whalt[j] = fqW(D_whalt.w[j]);
        Q_bhalt = fqB(D_bhalt.w[0]);
        for (u32 j = 0; j < H1; ++j)
            Q_wmem[j] = fqW(D_wmem.w[j]);
        Q_bmem = fqB(D_bmem.w[0]);
        for (u32 j = 0; j < TOWER_BUCKETS * LH * H1; ++j)
            Q_w2[j] = fqW(D_w2.w[j]);
        for (u32 j = 0; j < TOWER_BUCKETS * LH; ++j)
            Q_b2[j] = fqB(D_b2.w[j]);
    }
    // float master -> mạng số nguyên dùng cho suy luận/eval
    static void quantize_masters() noexcept
    {
        if (!train_masters_ready() || !g_emb || !g_hw || !g_hb || !g_routerW || !g_routerB ||
            g_routerCap < router_cluster_count(g_model.vocabCount) || g_model.vocabCount > TRAIN_VOCAB_CAP)
            return;
        for (size_t i = 0; i < (size_t)NF * L1; ++i)
            g_emb[i] = (i16)clampi((i32)std::floor(T_E[i] * QE + 0.5f), -32768, 32767);
        for (u32 j = 0; j < L1; ++j)
            g_tw.ebias[j] = (i16)clampi((i32)std::floor(D_eb.w[j] * QE + 0.5f), -32768, 32767);
        for (u32 j = 0; j < H1 * L1; ++j)
            g_tw.w1[j] = (i8)clampi((i32)std::floor(D_w1.w[j] * WS + 0.5f), -127, 127);
        for (u32 j = 0; j < H1; ++j)
            g_tw.b1[j] = (i32)std::floor(D_b1.w[j] * (127.f * WS) + 0.5f);
        for (u32 j = 0; j < H1 * H1; ++j)
            g_tw.wr[j] = (i8)clampi((i32)std::floor(D_wr.w[j] * WS + 0.5f), -127, 127);
        for (u32 j = 0; j < H1; ++j)
            g_tw.br[j] = (i32)std::floor(D_br.w[j] * (127.f * WS) + 0.5f);
        for (u32 j = 0; j < H1; ++j)
            g_tw.whalt[j] = (i8)clampi((i32)std::floor(D_whalt.w[j] * WS + 0.5f), -127, 127);
        g_tw.bhalt = (i32)std::floor(D_bhalt.w[0] * (127.f * WS) + 0.5f);
        for (u32 j = 0; j < H1; ++j)
            g_tw.wmem[j] = (i8)clampi((i32)std::floor(D_wmem.w[j] * WS + 0.5f), -127, 127);
        g_tw.bmem = (i32)std::floor(D_bmem.w[0] * (127.f * WS) + 0.5f);
        for (u32 j = 0; j < TOWER_BUCKETS * LH * H1; ++j)
            g_tw.w2[j] = (i8)clampi((i32)std::floor(D_w2.w[j] * WS + 0.5f), -127, 127);
        for (u32 j = 0; j < TOWER_BUCKETS * LH; ++j)
            g_tw.b2[j] = (i32)std::floor(D_b2.w[j] * (127.f * WS) + 0.5f);
        const u32 N = g_model.vocabCount;
        for (size_t i = 0; i < (size_t)N * LH; ++i)
            g_hw[i] = (i8)clampi((i32)std::floor(T_hw[i] * WS + 0.5f), -127, 127);
        for (u32 t = 0; t < N; ++t)
            g_hb[t] = T_hb[t];
        const u32 RC = router_cluster_count(N);
        for (u32 i = 0; i < RC * LH; ++i)
            g_routerW[i] = (i8)clampi((i32)std::floor(D_routerW.w[i] * WS + 0.5f), -127, 127);
        for (u32 c = 0; c < RC; ++c)
            g_routerB[c] = D_routerB.w[c];
    }
    // mạng số nguyên (.bin đã nạp) -> float master (học tiếp)
    static void dequantize_to_masters() noexcept
    {
        if (!T_E && !init_masters())
            return;
        if (!train_masters_ready() || !g_emb || !g_hw || !g_hb || !g_routerW || !g_routerB ||
            g_routerCap < router_cluster_count(g_model.vocabCount) || g_model.vocabCount > TRAIN_VOCAB_CAP)
            return;
        for (size_t i = 0; i < (size_t)NF * L1; ++i)
        {
            T_E[i] = (float)g_emb[i] / QE;
            T_Em[i] = T_Ev[i] = 0;
        }
        for (u32 j = 0; j < L1; ++j)
            D_eb.w[j] = (float)g_tw.ebias[j] / QE;
        for (u32 j = 0; j < H1 * L1; ++j)
            D_w1.w[j] = (float)g_tw.w1[j] / WS;
        for (u32 j = 0; j < H1; ++j)
            D_b1.w[j] = (float)g_tw.b1[j] / (127.f * WS);
        for (u32 j = 0; j < H1 * H1; ++j)
            D_wr.w[j] = (float)g_tw.wr[j] / WS;
        for (u32 j = 0; j < H1; ++j)
            D_br.w[j] = (float)g_tw.br[j] / (127.f * WS);
        for (u32 j = 0; j < H1; ++j)
            D_whalt.w[j] = (float)g_tw.whalt[j] / WS;
        D_bhalt.w[0] = (float)g_tw.bhalt / (127.f * WS);
        for (u32 j = 0; j < H1; ++j)
            D_wmem.w[j] = (float)g_tw.wmem[j] / WS;
        D_bmem.w[0] = (float)g_tw.bmem / (127.f * WS);
        for (u32 j = 0; j < TOWER_BUCKETS * LH * H1; ++j)
            D_w2.w[j] = (float)g_tw.w2[j] / WS;
        for (u32 j = 0; j < TOWER_BUCKETS * LH; ++j)
            D_b2.w[j] = (float)g_tw.b2[j] / (127.f * WS);
        const u32 N = g_model.vocabCount;
        for (size_t i = 0; i < (size_t)N * LH; ++i)
        {
            T_hw[i] = (float)g_hw[i] / WS;
            T_hwm[i] = T_hwv[i] = 0;
        }
        for (u32 t = 0; t < N; ++t)
        {
            T_hb[t] = g_hb[t];
            T_hbm[t] = T_hbv[t] = 0;
        }
        const u32 RC = router_cluster_count(N);
        for (u32 i = 0; i < RC * LH; ++i)
        {
            D_routerW.w[i] = (float)g_routerW[i] / WS;
            D_routerW.m[i] = D_routerW.v[i] = D_routerW.g[i] = 0.0f;
        }
        for (u32 c = 0; c < RC; ++c)
        {
            D_routerB.w[c] = g_routerB[c];
            D_routerB.m[c] = D_routerB.v[c] = D_routerB.g[c] = 0.0f;
        }
        T_step = 0;
        T_beta1Pow = T_beta2Pow = 1.0f;
    }
    FORCE_INLINE void adam1(float &w, float &m, float &v, float g, float lr, float c1, float c2) noexcept
    {
        m = 0.9f * m + 0.1f * g;
        v = 0.999f * v + 0.001f * g * g;
        w -= lr * (m * c1) / (std::sqrt(v * c2) + 1e-8f);
    }
    static void dense_flush(Dense &d, float lr, float c1, float c2) noexcept
    {
        for (u32 i = 0; i < d.n; ++i)
        {
            adam1(d.w[i], d.m[i], d.v[i], d.g[i], lr, c1, c2);
            d.g[i] = 0;
        }
    }
    // One training position. The recurrent latent trajectory is supervised at
    // every depth and differentiated end-to-end (shared-weight truncated BPTT).
    template <bool FullScan>
    static float train_step(const float *accQ, const float *accSInput, u32 y, u32 pos,
                            float lr, float c1, float c2, float *dQ, float *daccSOut,
                            bool &scanOk) noexcept
    {
        const u32 N = g_model.vocabCount;
        if (N == 0 || y >= N || router_cluster_count(N) > ROUTER_TRAIN_CAP)
        {
            if (dQ) std::memset(dQ, 0, sizeof(float) * L1);
            if (daccSOut) std::memset(daccSOut, 0, sizeof(float) * L1);
            scanOk = false;
            return 0.0f;
        }
        constexpr u32 R = REASON_STEPS;
        constexpr float kGate = 0.25f;
        u32 supervisedDepths[5]{};
        u32 supervisedCount = 0;
        const u32 requestedDepths[5] = {0u, 2u, 4u, 8u, R};
        for (u32 d : requestedDepths)
        {
            if (d > R) continue;
            u32 at = 0;
            while (at < supervisedCount && supervisedDepths[at] < d) ++at;
            if (at < supervisedCount && supervisedDepths[at] == d) continue;
            for (u32 k = supervisedCount; k > at; --k) supervisedDepths[k] = supervisedDepths[k - 1];
            supervisedDepths[at] = d;
            ++supervisedCount;
        }
        const float lossScale = 1.0f / (float)std::max<u32>(1, supervisedCount);
        auto isSupervised = [&](u32 d) noexcept
        {
            for (u32 i = 0; i < supervisedCount; ++i) if (supervisedDepths[i] == d) return true;
            return false;
        };
        const float *accS = accSInput;
        float p[L1], z1[H1], a1[H1];
        float reasonState[R + 1][H1] = {};
        float reasonPre[R][H1] = {};
        float z2[R + 1][LH] = {};
        float dState[R + 1][H1] = {};
        float depthLoss[R + 1];
        std::fill(depthLoss, depthLoss + R + 1, 1.0e6f);

        float ca[L1], cb[L1];
        for (u32 j = 0; j < L1; ++j)
        {
            const float a = accQ[j], b = accS[j];
            ca[j] = a < 0.f ? 0.f : (a > 1.f ? 1.f : a);
            cb[j] = b < 0.f ? 0.f : (b > 1.f ? 1.f : b);
            p[j] = fqA(ca[j] * cb[j]);
        }
        for (u32 i = 0; i < H1; ++i)
        {
            float z = Q_b1[i];
            const float *w = Q_w1 + i * L1;
            for (u32 j = 0; j < L1; ++j) z += w[j] * p[j];
            z1[i] = z;
            a1[i] = fqA(z < 0.f ? 0.f : (z > 1.f ? 1.f : z));
            reasonState[0][i] = a1[i];
        }

        // Recurrent neural reasoning: a shared learned transition repeatedly
        // refines a latent state. The convex residual gate prevents divergence.
        for (u32 step = 0; step < R; ++step)
        {
            for (u32 i = 0; i < H1; ++i)
            {
                float branch = Q_br[i];
                const float *w = Q_wr + i * H1;
                for (u32 j = 0; j < H1; ++j) branch += w[j] * reasonState[step][j];
                const float pre = reasonState[step][i] + branch;
                reasonPre[step][i] = pre;
                const float candidate = fqA(pre < 0.f ? 0.f : (pre > 1.f ? 1.f : pre));
                reasonState[step + 1][i] = fqA((1.f - kGate) * reasonState[step][i] + kGate * candidate);
            }
        }
#ifdef BIT_LLM_DIAGNOSTICS
        for (u32 j = 0; j < L1; ++j)
        {
            g_diagMaskActive[0] += (u64)(accQ[j] > 0.f && accQ[j] < 1.f) + (u64)(accS[j] > 0.f && accS[j] < 1.f);
            g_diagMaskTotal[0] += 2;
        }
        for (u32 i = 0; i < H1; ++i)
        {
            g_diagMaskActive[1] += (u64)(z1[i] > 0.f && z1[i] < 1.f);
            ++g_diagMaskTotal[1];
        }
#endif
        // Joint hierarchical maximum-likelihood objective:
        //   P(y|h) = P(router_cluster(y)|h) * P(y|router_cluster(y),h)
        // The router receives a full softmax over clusters; the fine head receives
        // an exact softmax over all tokens in the target cluster (128 max).
        const u32 RC = router_cluster_count(N);
        const u32 targetCluster = router_cluster_of(y);
        const u32 clusterStart = targetCluster * ROUTER_CLUSTER_SIZE;
        if (targetCluster >= RC || clusterStart >= N)
        {
            if (dQ) std::memset(dQ, 0, sizeof(float) * L1);
            if (daccSOut) std::memset(daccSOut, 0, sizeof(float) * L1);
            scanOk = false;
            return 0.0f;
        }
        const u32 fineCount = std::min<u32>(ROUTER_CLUSTER_SIZE, N - clusterStart);
        const u32 targetSlot = y - clusterStart;
        if (targetSlot >= fineCount)
        {
            if (dQ) std::memset(dQ, 0, sizeof(float) * L1);
            if (daccSOut) std::memset(daccSOut, 0, sizeof(float) * L1);
            scanOk = false;
            return 0.0f;
        }
        float routerScore[ROUTER_TRAIN_CAP]{};
        static thread_local float fineGradW[ROUTER_CLUSTER_SIZE][LH];
        static thread_local float fineGradB[ROUTER_CLUSTER_SIZE];
        std::memset(fineGradW, 0, sizeof(fineGradW));
        std::memset(fineGradB, 0, sizeof(fineGradB));
        const u32 bucket = tower_bucket(pos);
        const u32 w2off = bucket * LH * H1, b2off = bucket * LH;
        float lossTotal = 0.f;

        for (u32 depth = 0; depth <= R; ++depth)
        {
            if (!isSupervised(depth)) continue;
            float h[LH], dh[LH] = {};
            for (u32 i = 0; i < LH; ++i)
            {
                float z = Q_b2[b2off + i];
                const float *w = Q_w2 + w2off + i * H1;
                for (u32 j = 0; j < H1; ++j) z += w[j] * reasonState[depth][j];
                z2[depth][i] = z;
                h[i] = fqA(z < 0.f ? 0.f : (z > 1.f ? 1.f : z));
            }
#ifdef BIT_LLM_DIAGNOSTICS
            if (depth == R)
                for (u32 i = 0; i < LH; ++i)
                {
                    g_diagMaskActive[2] += (u64)(z2[depth][i] > 0.f && z2[depth][i] < 1.f);
                    ++g_diagMaskTotal[2];
                }
#endif
            // Coarse router: full cluster softmax, not sampled. This teaches an
            // explicit probability for every cluster and gives inference a valid bound.
            float rmax = -1.0e30f;
            for (u32 c = 0; c < RC; ++c)
            {
                const float *rw = D_routerW.w + (size_t)c * LH;
                float score = D_routerB.w[c];
                for (u32 j = 0; j < LH; ++j) score += fqW(rw[j]) * h[j];
                routerScore[c] = score;
                rmax = std::max(rmax, score);
            }
            float rsum = 0.0f;
            for (u32 c = 0; c < RC; ++c) rsum += std::exp(routerScore[c] - rmax);
            const float routerLogZ = rmax + std::log(std::max(rsum, 1.0e-30f));
            const float routerLoss = routerLogZ - routerScore[targetCluster];
            for (u32 c = 0; c < RC; ++c)
            {
                const float prob = std::exp(routerScore[c] - routerLogZ);
                const float gi = (prob - (c == targetCluster ? 1.0f : 0.0f)) * lossScale;
                D_routerB.g[c] += gi;
                float *gw = D_routerW.g + (size_t)c * LH;
                const float *w = D_routerW.w + (size_t)c * LH;
                for (u32 j = 0; j < LH; ++j)
                {
                    const float qw = fqW(w[j]);
                    gw[j] += gi * h[j];
                    dh[j] += gi * qw;
                }
            }

            // Fine conditional distribution over every member of the target cluster.
            float fineScore[ROUTER_CLUSTER_SIZE];
            float fmax = -1.0e30f;
            for (u32 slot = 0; slot < fineCount; ++slot)
            {
                const u32 token = clusterStart + slot;
                const float *w = T_hw + (size_t)token * LH;
                float score = T_hb[token];
                for (u32 j = 0; j < LH; ++j) score += fqW(w[j]) * h[j];
                fineScore[slot] = score;
                fmax = std::max(fmax, score);
            }
            float fsum = 0.0f;
            for (u32 slot = 0; slot < fineCount; ++slot) fsum += std::exp(fineScore[slot] - fmax);
            const float fineLogZ = fmax + std::log(std::max(fsum, 1.0e-30f));
            const float fineLoss = fineLogZ - fineScore[targetSlot];
            const float positionLoss = routerLoss + fineLoss;
            depthLoss[depth] = positionLoss;
            lossTotal += positionLoss * lossScale;

            for (u32 slot = 0; slot < fineCount; ++slot)
            {
                const u32 token = clusterStart + slot;
                const float prob = std::exp(fineScore[slot] - fineLogZ);
                const float gi = (prob - (slot == targetSlot ? 1.0f : 0.0f)) * lossScale;
                fineGradB[slot] += gi;
                const float *w = T_hw + (size_t)token * LH;
                for (u32 j = 0; j < LH; ++j)
                {
                    const float qw = fqW(w[j]);
                    fineGradW[slot][j] += gi * h[j];
                    dh[j] += gi * qw;
                }
            }

            // Periodic exact hierarchical top-1 / hard-rival diagnostic. This
            // scans all clusters only on the configured diagnostic positions.
            if constexpr (FullScan)
            {
                if (depth == R)
                {
                    u32 best = UNK;
                    float bestScore = -1.0e30f;
                    for (u32 c = 0; c < RC; ++c)
                    {
                        const u32 begin = c * ROUTER_CLUSTER_SIZE;
                        const u32 count = std::min<u32>(ROUTER_CLUSTER_SIZE, N - begin);
                        float local[ROUTER_CLUSTER_SIZE];
                        float lm = -1.0e30f;
                        for (u32 slot = 0; slot < count; ++slot)
                        {
                            const u32 token = begin + slot;
                            const float *w = T_hw + (size_t)token * LH;
                            float value = T_hb[token];
                            for (u32 j = 0; j < LH; ++j) value += fqW(w[j]) * h[j];
                            local[slot] = value;
                            lm = std::max(lm, value);
                        }
                        float lsum = 0.0f;
                        for (u32 slot = 0; slot < count; ++slot) lsum += std::exp(local[slot] - lm);
                        const float lz = lm + std::log(std::max(lsum, 1.0e-30f));
                        const float parentLogP = routerScore[c] - routerLogZ;
                        for (u32 slot = 0; slot < count; ++slot)
                        {
                            const u32 token = begin + slot;
                            const float joint = parentLogP + local[slot] - lz;
                            if (joint > bestScore) { bestScore = joint; best = token; }
                        }
                    }
                    scanOk = best == y;
                }
            }

            // QAT backward through the position-specific readout.
            for (u32 i = 0; i < LH; ++i)
            {
                const float dz = dh[i] * clipMask(z2[depth][i]);
                D_b2.g[b2off + i] += dz;
                float *gw = D_w2.g + w2off + i * H1;
                const float *w = Q_w2 + w2off + i * H1;
                for (u32 j = 0; j < H1; ++j)
                {
                    gw[j] += dz * reasonState[depth][j];
                    dState[depth][j] += dz * w[j];
                }
            }
        }

        // One fine-head Adam update per target-cluster row after all supervised depths.
        for (u32 slot = 0; slot < fineCount; ++slot)
        {
            const u32 token = clusterStart + slot;
            float *row = T_hw + (size_t)token * LH;
            float *m = T_hwm + (size_t)token * LH;
            float *v = T_hwv + (size_t)token * LH;
            for (u32 j = 0; j < LH; ++j)
            {
                adam1(row[j], m[j], v[j], fineGradW[slot][j], lr * k_head, c1, c2);
                row[j] = std::max(-1.99f, std::min(1.99f, row[j]));
            }
            adam1(T_hb[token], T_hbm[token], T_hbv[token], fineGradB[slot], lr * k_head, c1, c2);
        }

        // ACT-style learned halting controller. Its training target comes from
        // the measured marginal gain of the next recurrent pass: continue when
        // the next depth reduces token loss materially, otherwise learn to stop.
        // This is a learned decision, not a fixed inference depth.
        float haltGradW[H1] = {};
        float haltGradB = 0.0f;
        constexpr float kHaltLossWeight = 0.05f;
        constexpr float kHaltGainMargin = 0.01f;
        u32 haltTargets = 0;
        for (u32 i = 0; i + 1 < supervisedCount; ++i)
            if (supervisedDepths[i] >= REASON_MIN_STEPS && supervisedDepths[i] < R) ++haltTargets;
        const float haltDenom = (float)std::max<u32>(1, haltTargets);
        for (u32 i = 0; i + 1 < supervisedCount; ++i)
        {
            const u32 depth = supervisedDepths[i], nextDepth = supervisedDepths[i + 1];
            if (depth < REASON_MIN_STEPS || depth >= R || nextDepth > R || nextDepth == depth) continue;
            const float gain = depthLoss[depth] - depthLoss[nextDepth];
            const float targetStop = std::max(0.0f, std::min(1.0f, 0.5f + (kHaltGainMargin - gain) * 10.0f));
            float logit = Q_bhalt;
            for (u32 j = 0; j < H1; ++j) logit += Q_whalt[j] * reasonState[depth][j];
            logit = std::max(-12.0f, std::min(12.0f, logit));
            const float probStop = 1.0f / (1.0f + std::exp(-logit));
            const float bce = -(targetStop * std::log(std::max(probStop, 1e-6f)) +
                                (1.0f - targetStop) * std::log(std::max(1.0f - probStop, 1e-6f)));
            lossTotal += kHaltLossWeight * bce / haltDenom;
            const float dlogit = kHaltLossWeight * (probStop - targetStop) / haltDenom;
            haltGradB += dlogit;
            for (u32 j = 0; j < H1; ++j)
            {
                haltGradW[j] += dlogit * reasonState[depth][j];
                dState[depth][j] += dlogit * Q_whalt[j];
            }
        }
        for (u32 j = 0; j < H1; ++j)
        {
            adam1(D_whalt.w[j], D_whalt.m[j], D_whalt.v[j], haltGradW[j], lr * k_dense, c1, c2);
            D_whalt.w[j] = std::max(-1.99f, std::min(1.99f, D_whalt.w[j]));
        }
        adam1(D_bhalt.w[0], D_bhalt.m[0], D_bhalt.v[0], haltGradB, lr * k_dense, c1, c2);
        D_bhalt.w[0] = std::max(-8.0f, std::min(8.0f, D_bhalt.w[0]));

        // A separate learned evidence controller predicts whether external context
        // is likely to help. Teacher signal is the unresolved token loss after the
        // model's own recurrent refinement: if latent passes have not reduced loss,
        // ask for evidence; if they have, keep reasoning internally.
        float memGradW[H1] = {};
        float memGradB = 0.0f;
        constexpr float kMemoryLossWeight = 0.04f;
        constexpr float kMemoryGainMargin = 0.04f;
        constexpr float kMemoryGainScale = 8.0f;
        u32 memoryTargets = 0;
        for (u32 i = 0; i < supervisedCount; ++i)
            if (supervisedDepths[i] >= REASON_MIN_STEPS && supervisedDepths[i] <= R) ++memoryTargets;
        const float memoryDenom = (float)std::max<u32>(1, memoryTargets);
        for (u32 i = 0; i < supervisedCount; ++i)
        {
            const u32 depth = supervisedDepths[i];
            if (depth < REASON_MIN_STEPS || depth > R) continue;
            const float gain = depthLoss[0] - depthLoss[depth];
            const float targetRead = std::max(0.0f, std::min(1.0f, 0.5f + (kMemoryGainMargin - gain) * kMemoryGainScale));
            float logit = Q_bmem;
            for (u32 j = 0; j < H1; ++j) logit += Q_wmem[j] * reasonState[depth][j];
            logit = std::max(-12.0f, std::min(12.0f, logit));
            const float probRead = 1.0f / (1.0f + std::exp(-logit));
            const float bce = -(targetRead * std::log(std::max(probRead, 1e-6f)) +
                                (1.0f - targetRead) * std::log(std::max(1.0f - probRead, 1e-6f)));
            lossTotal += kMemoryLossWeight * bce / memoryDenom;
            const float dlogit = kMemoryLossWeight * (probRead - targetRead) / memoryDenom;
            memGradB += dlogit;
            for (u32 j = 0; j < H1; ++j)
            {
                memGradW[j] += dlogit * reasonState[depth][j];
                dState[depth][j] += dlogit * Q_wmem[j];
            }
        }
        for (u32 j = 0; j < H1; ++j)
        {
            adam1(D_wmem.w[j], D_wmem.m[j], D_wmem.v[j], memGradW[j], lr * k_dense, c1, c2);
            D_wmem.w[j] = std::max(-1.99f, std::min(1.99f, D_wmem.w[j]));
        }
        adam1(D_bmem.w[0], D_bmem.m[0], D_bmem.v[0], memGradB, lr * k_dense, c1, c2);
        D_bmem.w[0] = std::max(-8.0f, std::min(8.0f, D_bmem.w[0]));

        // BPTT through the shared recurrent cell. Every intermediate state has
        // its own LM loss, so gradients combine direct deep supervision and future steps.
        for (u32 step = R; step > 0; --step)
        {
            for (u32 i = 0; i < H1; ++i)
            {
                const float mask = clipMask(reasonPre[step - 1][i]);
                const float incoming = dState[step][i];
                const float dr = incoming * kGate * mask;
                dState[step - 1][i] += incoming * ((1.f - kGate) + kGate * mask);
                D_br.g[i] += dr;
                float *gw = D_wr.g + i * H1;
                const float *w = Q_wr + i * H1;
                for (u32 j = 0; j < H1; ++j)
                {
                    gw[j] += dr * reasonState[step - 1][j];
                    dState[step - 1][j] += dr * w[j];
                }
            }
        }

        float da1[H1], dz1[H1], dp[L1] = {};
        for (u32 i = 0; i < H1; ++i)
        {
            da1[i] = dState[0][i];
            dz1[i] = da1[i] * clipMask(z1[i]);
            D_b1.g[i] += dz1[i];
            float *gw = D_w1.g + i * L1;
            const float *w = Q_w1 + i * L1;
            for (u32 j = 0; j < L1; ++j)
            {
                gw[j] += dz1[i] * p[j];
                dp[j] += dz1[i] * w[j];
            }
        }
        float daccQ[L1], daccS[L1];
        for (u32 j = 0; j < L1; ++j)
        {
            daccQ[j] = dp[j] * cb[j] * clipMask(accQ[j]);
            daccS[j] = dp[j] * ca[j] * clipMask(accS[j]);
        }
#ifdef BIT_LLM_DIAGNOSTICS
        for (u32 j = 0; j < L1; ++j)
        {
            g_diagDaccZero += (daccQ[j] == 0.f) + (daccS[j] == 0.f);
            g_diagDaccTotal += 2;
            g_diagDaccAbs += std::abs((double)daccQ[j]) + std::abs((double)daccS[j]);
        }
#endif
        for (u32 j = 0; j < L1; ++j)
        {
            D_eb.g[j] += daccQ[j];
            dQ[j] += daccQ[j];
        }
        if (daccSOut) std::memcpy(daccSOut, daccS, sizeof(daccS));
        return lossTotal;
    }
} // namespace nnuecut
#endif // BIT_LLM_TRAIN

// =====================================================================================================================================
// API cơ bản + suy luận (beam, think) + nạp/xuất
// =====================================================================================================================================
namespace nnuecut
{
    inline void init_runtime_state() noexcept
    {
        std::memset(g_ttab, 0, sizeof(g_ttab));
        g_ttgen = 1;
        g_nodesTotal = 0;
        std::memset(g_stat, 0, sizeof(g_stat));
        std::memset(g_dsStat, 0, sizeof(g_dsStat));
        std::memset(g_stageWin, 0, sizeof(g_stageWin));
    }
} // namespace nnuecut

WASM_EXPORT void wasm_bit_llm_init()
{
    reset_all();
    g_model.magic = MODEL_MAGIC;
    g_model.version = MODEL_VERSION;
    g_model.l1 = L1;
    g_model.featBits = FEAT_BITS;
    g_model.lh = LH;
    g_model.h1 = H1;
    g_model.half = HALF;
    g_model.qe = QE;
    g_model.ws = WS;
    g_model.kwCount = KW_COUNT;
    g_model.archHash = ARCH_HASH;
    g_model.temp = 1.0f;
    const u32 cap = kTrain ? (u32)TRAIN_VOCAB_CAP : 1u;
    net_alloc(cap);
    meta_reserve(cap);
    arena_reserve(256);
    rebuild_ht();
    init_runtime_state();
    g_outputTokenScores = g_outputHeadMacs = g_outputRouterMacs = g_outputClustersVisited = 0;
    g_overflow = 0;
#ifdef BIT_LLM_TRAIN
    g_haveBest = false;
    g_bestVocab = 0;
    if (init_masters())
        refresh_quant();
    g_pairs = g_replayPairs = g_newPairs = g_holdCount = 0;
    if (!g_aTok)
        g_aTok = (u32(*)[MAX_A])std::calloc((size_t)MAX_PAIRS * MAX_A, 4);
    if (!g_qTok)
        g_qTok = (TokInfo *)std::calloc((size_t)MAX_PAIRS * MAX_Q, sizeof(TokInfo));
#endif
    {
        const char e[] = "<eos>";
        u64 h, h2;
        char low[32];
        const u32 l = word_hashes(e, 5, h, h2, low);
        register_token(low, l, h);
    }
    g_pairs = 0;
    ensure_scratch();
}

WASM_EXPORT char *wasm_bit_llm_get_input_ptr() { return g_in; }
WASM_EXPORT const char *wasm_bit_llm_get_output_ptr() { return g_out; }

// Context Memory ABI. Search only supplies text evidence to the neural accumulator;
// there is deliberately no retrieval-answer path in wasm_bit_llm_infer().
WASM_EXPORT u32 wasm_bit_llm_context_add(u32 textLen, u32 kind, u32 importance, u32 timestampLo, u32 timestampHi)
{
    if (textLen == 0 || textLen >= IO_BUFFER_SIZE || kind > 255 || importance > 255) return 0;
    g_in[textLen] = 0;
    const u64 ts = ((u64)timestampHi << 32) | (u64)timestampLo;
#if !defined(__EMSCRIPTEN__)
    nnuecut::context_load_disk_once();
    if (!nnuecut::g_contextMemory.add_and_append("context_memory.ctx", g_in, textLen, (u8)kind, (u8)importance, ts)) return 0;
#else
    if (!nnuecut::g_contextMemory.add(g_in, textLen, (u8)kind, (u8)importance, ts)) return 0;
#endif
    nnuecut::g_contextSnapshotReady = false;
    return (u32)nnuecut::g_contextMemory.size();
}
WASM_EXPORT u32 wasm_bit_llm_context_search(u32 queryLen, u32 topK, u32 timestampLo, u32 timestampHi)
{
    if (queryLen >= IO_BUFFER_SIZE || topK == 0) return 0;
    g_in[queryLen] = 0;
    const u64 now = ((u64)timestampHi << 32) | (u64)timestampLo;
    return (u32)nnuecut::g_contextMemory.search_at(g_in, queryLen, std::min<u32>(64, topK), now).size();
}
WASM_EXPORT void wasm_bit_llm_context_clear()
{
    nnuecut::g_contextMemory.clear(); nnuecut::g_contextSnapshot.clear(); nnuecut::g_contextIncoming.clear();
    nnuecut::g_contextSnapshotReady = false;
#if !defined(__EMSCRIPTEN__)
    nnuecut::g_contextDiskInitialized = true;
    (void)nnuecut::g_contextMemory.save_file("context_memory.ctx");
#endif
}
WASM_EXPORT void wasm_bit_llm_context_set_scope(u32 sessionLo, u32 sessionHi, u32 projectLo, u32 projectHi)
{
    const u64 session = ((u64)sessionHi << 32) | sessionLo;
    const u64 project = ((u64)projectHi << 32) | projectLo;
    nnuecut::g_contextMemory.set_scope(session, project);
}
WASM_EXPORT u32 wasm_bit_llm_context_count() { return (u32)nnuecut::g_contextMemory.size(); }
WASM_EXPORT u32 wasm_bit_llm_context_indexed_terms() { return (u32)nnuecut::g_contextMemory.indexed_terms(); }
WASM_EXPORT double wasm_bit_llm_context_ram_bytes() { return (double)nnuecut::g_contextMemory.memory_bytes(); }
WASM_EXPORT u32 wasm_bit_llm_context_hit_id_lo(u32 i)
{
    const bitctx::Hit *h = nnuecut::g_contextMemory.hit(i);
    const bitctx::Entry *e = h ? nnuecut::g_contextMemory.entry(h->entry_index) : nullptr;
    return e ? (u32)e->id : 0;
}
WASM_EXPORT u32 wasm_bit_llm_context_hit_id_hi(u32 i)
{
    const bitctx::Hit *h = nnuecut::g_contextMemory.hit(i);
    const bitctx::Entry *e = h ? nnuecut::g_contextMemory.entry(h->entry_index) : nullptr;
    return e ? (u32)(e->id >> 32) : 0;
}
WASM_EXPORT u32 wasm_bit_llm_context_hit_score(u32 i)
{
    const bitctx::Hit *h = nnuecut::g_contextMemory.hit(i); return h ? h->score_milli : 0;
}
WASM_EXPORT u32 wasm_bit_llm_context_hit_text_len(u32 i)
{
    const bitctx::Hit *h = nnuecut::g_contextMemory.hit(i);
    const bitctx::Entry *e = h ? nnuecut::g_contextMemory.entry(h->entry_index) : nullptr;
    return e ? (u32)e->text.size() : 0;
}
WASM_EXPORT const char *wasm_bit_llm_context_hit_text_ptr(u32 i)
{
    const bitctx::Hit *h = nnuecut::g_contextMemory.hit(i);
    const bitctx::Entry *e = h ? nnuecut::g_contextMemory.entry(h->entry_index) : nullptr;
    return e ? e->text.c_str() : "";
}
WASM_EXPORT u32 wasm_bit_llm_context_hit_kind(u32 i)
{
    const bitctx::Hit *h = nnuecut::g_contextMemory.hit(i);
    const bitctx::Entry *e = h ? nnuecut::g_contextMemory.entry(h->entry_index) : nullptr;
    return e ? e->kind : 0;
}
WASM_EXPORT u32 wasm_bit_llm_context_export_begin()
{
    if (!nnuecut::g_contextMemory.serialize(nnuecut::g_contextSnapshot) || nnuecut::g_contextSnapshot.size() > 0xFFFFFFFFull) {
        nnuecut::g_contextSnapshot.clear(); nnuecut::g_contextSnapshotReady = false; return 0;
    }
    nnuecut::g_contextSnapshotReady = true; return (u32)nnuecut::g_contextSnapshot.size();
}
WASM_EXPORT u32 wasm_bit_llm_context_export_read(u32 offset, u32 size)
{
    auto &v = nnuecut::g_contextSnapshot;
    if (!nnuecut::g_contextSnapshotReady || size == 0 || size > CHUNK_BUFFER_SIZE || offset >= v.size()) return 0;
    if ((u64)offset + size > v.size()) size = (u32)(v.size() - offset);
    std::memcpy(g_chunk_buffer, v.data() + offset, size); return size;
}
WASM_EXPORT u32 wasm_bit_llm_context_import_begin(u32 totalSize)
{
    if (totalSize < 16 || totalSize > (512u * 1024u * 1024u)) return 0;
    try { nnuecut::g_contextIncoming.assign(totalSize, 0); }
    catch (...) { nnuecut::g_contextIncoming.clear(); return 0; }
    return 1;
}
WASM_EXPORT u32 wasm_bit_llm_context_import_chunk(u32 offset, u32 size)
{
    auto &v = nnuecut::g_contextIncoming;
    if (size == 0 || size > CHUNK_BUFFER_SIZE || offset > v.size() || (u64)offset + size > v.size()) return 0;
    std::memcpy(v.data() + offset, g_chunk_buffer, size); return size;
}
WASM_EXPORT u32 wasm_bit_llm_context_import_finalize()
{
    auto &v = nnuecut::g_contextIncoming;
    if (v.empty() || !nnuecut::g_contextMemory.deserialize(v.data(), v.size())) { v.clear(); return 0; }
    v.clear(); nnuecut::g_contextSnapshot.clear(); nnuecut::g_contextSnapshotReady = false;
#if !defined(__EMSCRIPTEN__)
    nnuecut::g_contextDiskInitialized = true;
    if (!nnuecut::g_contextMemory.save_file("context_memory.ctx")) return 0;
#endif
    return (u32)nnuecut::g_contextMemory.size();
}
#if !defined(__EMSCRIPTEN__)
WASM_EXPORT u32 wasm_bit_llm_context_load_disk() { nnuecut::context_load_disk_once(); return (u32)nnuecut::g_contextMemory.size(); }
WASM_EXPORT u32 wasm_bit_llm_context_save_disk() { return nnuecut::g_contextMemory.save_file("context_memory.ctx") ? 1u : 0u; }
#endif

// =====================================================================
// VIDEO AGENT PLAN ABI
// The host executes actions through its editor adapter and reports a verified
// editor-state read-back. The core never treats a generated string as execution.
// =====================================================================
WASM_EXPORT u32 wasm_bit_llm_agent_plan_set_action_catalog(u32 catalogLen)
{
    if (!catalogLen || catalogLen >= IO_BUFFER_SIZE) return 0;
    g_in[catalogLen] = 0;
    const std::string raw(g_in, catalogLen);
    std::vector<std::string> actions;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        const std::size_t comma = raw.find(',', pos);
        const std::string item = raw.substr(pos, comma == std::string::npos ? comma : comma - pos);
        if (item.empty()) return 0;
        actions.push_back(item);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return nnuecut::g_agentPlan.set_action_catalog(actions) ? 1u : 0u;
}
WASM_EXPORT u32 wasm_bit_llm_agent_plan_begin(u32 taskLen)
{
    if (taskLen == 0 || taskLen >= IO_BUFFER_SIZE) return 0;
    g_in[taskLen] = 0;
    nnuecut::g_agentActiveStepId = 0;
    const std::string task(g_in, taskLen);
    nnuecut::g_artistic.set_intent(task); // infer creative intent from the actual brief
    return nnuecut::g_agentPlan.begin(task) ? 1u : 0u;
}
WASM_EXPORT u32 wasm_bit_llm_agent_plan_build_prompt()
{
    std::string prompt = nnuecut::g_agentPlan.plan_prompt();
    if (prompt.empty()) return 0;
    const std::string artistic = nnuecut::g_artistic.plan_guidance();
    const std::size_t room = IO_BUFFER_SIZE - 1 - prompt.size();
    if (room > 24) {
        prompt += "\n\n";
        prompt.append(artistic.data(), std::min<std::size_t>(artistic.size(), room - 2));
    }
    if (prompt.size() >= IO_BUFFER_SIZE) prompt.resize(IO_BUFFER_SIZE - 1);
    std::memcpy(g_in, prompt.data(), prompt.size()); g_in[prompt.size()] = 0;
    return (u32)prompt.size();
}
WASM_EXPORT u32 wasm_bit_llm_agent_plan_parse_output(u32 outputLen)
{
    if (outputLen >= IO_BUFFER_SIZE) outputLen = IO_BUFFER_SIZE - 1;
    g_out[outputLen] = 0;
    return nnuecut::g_agentPlan.parse_plan(std::string(g_out, outputLen)) ? nnuecut::g_agentPlan.size() : 0u;
}
WASM_EXPORT u32 wasm_bit_llm_agent_plan_add_step(u32 id, u32 maxRetries, u32 dataLen)
{
    if (!dataLen || dataLen >= IO_BUFFER_SIZE) return 0;
    g_in[dataLen] = 0;
    const std::string packed(g_in, dataLen);
    const std::size_t a = packed.find('\n');
    const std::size_t b = a == std::string::npos ? a : packed.find('\n', a + 1);
    const std::size_t c = b == std::string::npos ? b : packed.find('\n', b + 1);
    if (a == std::string::npos || b == std::string::npos || c == std::string::npos) return 0;
    const std::string action = packed.substr(0, a);
    const std::string payload = packed.substr(a + 1, b - a - 1);
    const std::string post = packed.substr(b + 1, c - b - 1);
    const std::string depText = packed.substr(c + 1);
    std::vector<u32> deps;
    if (!depText.empty() && depText != "-") {
        std::size_t pos = 0;
        while (pos < depText.size()) {
            const std::size_t comma = depText.find(',', pos);
            const std::string part = depText.substr(pos, comma == std::string::npos ? comma : comma - pos);
            char *end = nullptr; const unsigned long v = std::strtoul(part.c_str(), &end, 10);
            if (part.empty() || !end || *end || v == 0 || v > 0xFFFFFFFFul || deps.size() >= bitagent::kMaxDependencies) return 0;
            deps.push_back((u32)v);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    return nnuecut::g_agentPlan.add_step(id, action, payload, post, deps, maxRetries) ? 1u : 0u;
}
WASM_EXPORT u32 wasm_bit_llm_agent_plan_revise_step(u32 id, u32 dataLen)
{
    if (!dataLen || dataLen >= IO_BUFFER_SIZE) return 0;
    g_in[dataLen] = 0;
    const std::string packed(g_in, dataLen);
    const std::size_t a = packed.find('\n');
    const std::size_t b = a == std::string::npos ? a : packed.find('\n', a + 1);
    if (a == std::string::npos || b == std::string::npos) return 0;
    const std::string action = packed.substr(0, a);
    const std::string payload = packed.substr(a + 1, b - a - 1);
    const std::string post = packed.substr(b + 1);
    return nnuecut::g_agentPlan.revise(id, action, payload, post) ? 1u : 0u;
}
WASM_EXPORT u32 wasm_bit_llm_agent_plan_claim_next()
{
    const bitagent::PlanStep *step = nnuecut::g_agentPlan.claim_next();
    nnuecut::g_agentActiveStepId = step ? step->id : 0;
    return nnuecut::g_agentActiveStepId;
}
WASM_EXPORT const char *wasm_bit_llm_agent_plan_action_ptr()
{
    const auto *s = nnuecut::g_agentPlan.step(nnuecut::g_agentActiveStepId); return s ? s->action.c_str() : "";
}
WASM_EXPORT const char *wasm_bit_llm_agent_plan_payload_ptr()
{
    const auto *s = nnuecut::g_agentPlan.step(nnuecut::g_agentActiveStepId); return s ? s->payload_json.c_str() : "{}";
}
WASM_EXPORT const char *wasm_bit_llm_agent_plan_postcondition_ptr()
{
    const auto *s = nnuecut::g_agentPlan.step(nnuecut::g_agentActiveStepId); return s ? s->postcondition.c_str() : "";
}
WASM_EXPORT u32 wasm_bit_llm_agent_plan_active_revision()
{
    const auto *s = nnuecut::g_agentPlan.step(nnuecut::g_agentActiveStepId); return s ? s->revision : 0;
}
WASM_EXPORT u32 wasm_bit_llm_agent_plan_active_attempt()
{
    const auto *s = nnuecut::g_agentPlan.step(nnuecut::g_agentActiveStepId); return s ? s->attempts : 0;
}
WASM_EXPORT u32 wasm_bit_llm_agent_plan_report(u32 stepId, u32 actionSucceeded, u32 verified, u32 resultLen)
{
    if (resultLen >= IO_BUFFER_SIZE) return 0;
    g_in[resultLen] = 0;
    const std::string evidence(g_in, resultLen);
    if (!nnuecut::g_agentPlan.report(stepId, actionSucceeded != 0, verified != 0, evidence)) return 0;
    if (actionSucceeded && verified && !evidence.empty()) {
        char record[4096]; // ContextMemory records are capped at 4095 bytes.
        const int n = std::snprintf(record, sizeof(record), "Verified video action: step=%u; result=%s", stepId, evidence.c_str());
        if (n > 0) {
#if !defined(__EMSCRIPTEN__)
            nnuecut::context_load_disk_once();
            (void)nnuecut::g_contextMemory.add_and_append("context_memory.ctx", record,
                std::min<std::size_t>((std::size_t)n, sizeof(record)-1), 4, 190, (u64)std::time(nullptr));
            nnuecut::g_contextSnapshotReady = false;
#else
            (void)nnuecut::g_contextMemory.add(record, std::min<std::size_t>((std::size_t)n, sizeof(record)-1),
                4, 190, (u64)std::time(nullptr));
            nnuecut::g_contextSnapshotReady = false;
#endif
        }
    }
    nnuecut::g_agentActiveStepId = 0;
    return 1;
}
WASM_EXPORT u32 wasm_bit_llm_agent_plan_build_repair_prompt(u32 stepId)
{
    const std::string prompt = nnuecut::g_agentPlan.repair_prompt(stepId);
    if (prompt.empty() || prompt.size() >= IO_BUFFER_SIZE) return 0;
    std::memcpy(g_in, prompt.data(), prompt.size()); g_in[prompt.size()] = 0;
    return (u32)prompt.size();
}
WASM_EXPORT u32 wasm_bit_llm_agent_plan_parse_repair(u32 outputLen)
{
    if (outputLen >= IO_BUFFER_SIZE) outputLen = IO_BUFFER_SIZE - 1;
    g_out[outputLen] = 0;
    return nnuecut::g_agentPlan.parse_repair(std::string(g_out, outputLen)) ? 1u : 0u;
}
WASM_EXPORT u32 wasm_bit_llm_agent_plan_status(u32 stepId)
{
    const auto *s = nnuecut::g_agentPlan.step(stepId);
    return s ? static_cast<u32>(s->status) : 0xFFFFFFFFu;
}
WASM_EXPORT u32 wasm_bit_llm_agent_plan_step_count() { return nnuecut::g_agentPlan.size(); }
WASM_EXPORT u32 wasm_bit_llm_agent_plan_ready_count() { return nnuecut::g_agentPlan.ready_count(); }
WASM_EXPORT u32 wasm_bit_llm_agent_plan_complete() { return nnuecut::g_agentPlan.complete() ? 1u : 0u; }
WASM_EXPORT u32 wasm_bit_llm_agent_plan_terminal_failure() { return nnuecut::g_agentPlan.terminal_failure() ? 1u : 0u; }
WASM_EXPORT const char *wasm_bit_llm_agent_plan_error_ptr() { return nnuecut::g_agentPlan.last_error().c_str(); }

// =====================================================================
// ARTISTIC INTELLIGENCE v21 ABI
// Host supplies preview measurements; the critic ranks candidates and learns from A/B choices.
// =====================================================================
WASM_EXPORT u32 wasm_bit_llm_artistic_version() { return 21u; }
WASM_EXPORT u32 wasm_bit_llm_artistic_intent_dims() { return bitart::kIntentDims; }
WASM_EXPORT u32 wasm_bit_llm_artistic_feature_dims() { return bitart::kFeatureDims; }
WASM_EXPORT u32 wasm_bit_llm_artistic_candidate_capacity() { return bitart::kCandidateSlots; }
WASM_EXPORT u32 wasm_bit_llm_artistic_strategy_count() { return bitart::kStrategyCount; }
WASM_EXPORT u32 wasm_bit_llm_artistic_set_intent_text(u32 len)
{
    if (!len || len >= IO_BUFFER_SIZE) return 0;
    g_in[len] = 0;
    nnuecut::g_artistic.set_intent(std::string(g_in, len));
    return 1;
}
WASM_EXPORT float *wasm_bit_llm_artistic_get_intent_ptr()
{
    return const_cast<float *>(nnuecut::g_artistic.intent().data());
}
WASM_EXPORT float *wasm_bit_llm_artistic_intent_buffer_ptr()
{
    return nnuecut::g_artisticIntentInput;
}
WASM_EXPORT u32 wasm_bit_llm_artistic_set_intent_vector()
{
    return nnuecut::g_artistic.set_intent_vector(nnuecut::g_artisticIntentInput, bitart::kIntentDims) ? 1u : 0u;
}
WASM_EXPORT float *wasm_bit_llm_artistic_candidate_buffer_ptr(u32 slot)
{
    return slot < bitart::kCandidateSlots ? nnuecut::g_artisticCandidateInput[slot] : nullptr;
}
WASM_EXPORT u32 wasm_bit_llm_artistic_clear_candidates()
{
    nnuecut::g_artistic.clear_candidates();
    std::memset(nnuecut::g_artisticCandidateInput, 0, sizeof(nnuecut::g_artisticCandidateInput));
    return 1u;
}
WASM_EXPORT void wasm_bit_llm_artistic_reset_preferences() { nnuecut::g_artistic.reset_preferences(); }
WASM_EXPORT u32 wasm_bit_llm_artistic_candidate_submit(u32 slot, u32 candidateId)
{
    if (slot >= bitart::kCandidateSlots) return 0;
    return nnuecut::g_artistic.set_candidate(slot, candidateId,
        nnuecut::g_artisticCandidateInput[slot], bitart::kFeatureDims) ? 1u : 0u;
}
WASM_EXPORT float wasm_bit_llm_artistic_candidate_score(u32 slot)
{
    const auto *c = nnuecut::g_artistic.candidate(slot);
    return c ? c->score : -1.0f;
}
WASM_EXPORT u32 wasm_bit_llm_artistic_rank(u32 rankIndex)
{
    return nnuecut::g_artistic.rank(rankIndex);
}
WASM_EXPORT u32 wasm_bit_llm_artistic_preference(u32 chosenSlot, u32 rejectedSlot, float learningRate)
{
    return nnuecut::g_artistic.learn_preference(chosenSlot, rejectedSlot, learningRate) ? 1u : 0u;
}
WASM_EXPORT u32 wasm_bit_llm_artistic_preference_updates()
{
    return (u32)std::min<u64>(nnuecut::g_artistic.preference_updates(), 0xFFFFFFFFu);
}
WASM_EXPORT void wasm_bit_llm_artistic_select_strategy(u32 id) { nnuecut::g_artistic.select_strategy(id); }
WASM_EXPORT u32 wasm_bit_llm_artistic_get_strategy_json(u32 id)
{
    const std::string json = nnuecut::g_artistic.strategy_json(id);
    if (json.empty() || json.size() >= IO_BUFFER_SIZE) return 0;
    std::memcpy(g_out, json.data(), json.size()); g_out[json.size()] = 0;
    return (u32)json.size();
}
WASM_EXPORT u32 wasm_bit_llm_artistic_get_score_json(u32 slot)
{
    const std::string json = nnuecut::g_artistic.score_explanation(slot);
    if (json.empty() || json.size() >= IO_BUFFER_SIZE) return 0;
    std::memcpy(g_out, json.data(), json.size()); g_out[json.size()] = 0;
    return (u32)json.size();
}
WASM_EXPORT u32 wasm_bit_llm_artistic_state_size()
{
    return (u32)bitart::ArtisticIntelligence::serialized_size();
}
WASM_EXPORT u8 *wasm_bit_llm_artistic_state_out_ptr() { return nnuecut::g_artisticStateOut; }
WASM_EXPORT u8 *wasm_bit_llm_artistic_state_in_ptr() { return nnuecut::g_artisticStateIn; }
WASM_EXPORT u32 wasm_bit_llm_artistic_export_state()
{
    std::vector<u8> bytes;
    if (!nnuecut::g_artistic.serialize(bytes) || bytes.size() > sizeof(nnuecut::g_artisticStateOut)) return 0;
    std::memcpy(nnuecut::g_artisticStateOut, bytes.data(), bytes.size());
    return (u32)bytes.size();
}
WASM_EXPORT u32 wasm_bit_llm_artistic_import_state(u32 len)
{
    if (len != bitart::ArtisticIntelligence::serialized_size() || len > sizeof(nnuecut::g_artisticStateIn)) return 0;
    return nnuecut::g_artistic.deserialize(nnuecut::g_artisticStateIn, len) ? 1u : 0u;
}

WASM_EXPORT u32 wasm_bit_llm_reasoning_engine_version() { return 22u; }
WASM_EXPORT u32 wasm_bit_llm_get_reasoning_steps() { return nnuecut::g_reasonCallCount ? (nnuecut::g_reasonStepSum / nnuecut::g_reasonCallCount) : 0u; }
WASM_EXPORT u32 wasm_bit_llm_get_reasoning_max_steps() { return nnuecut::g_reasonBudget; }
WASM_EXPORT void wasm_bit_llm_set_reasoning_budget(u32 n) { nnuecut::g_reasonBudget = n < nnuecut::REASON_MIN_STEPS ? nnuecut::REASON_MIN_STEPS : (n > nnuecut::REASON_MAX_STEPS ? nnuecut::REASON_MAX_STEPS : n); }
WASM_EXPORT u32 wasm_bit_llm_get_reasoning_last_steps() { return nnuecut::g_reasonLastSteps; }
WASM_EXPORT u32 wasm_bit_llm_get_reasoning_halt_permille() { return (u32)(nnuecut::g_lastHaltProbability * 1000.0f + 0.5f); }
WASM_EXPORT u32 wasm_bit_llm_get_memory_read_probability_permille() { return (u32)(nnuecut::g_lastMemoryProbability * 1000.0f + 0.5f); }
WASM_EXPORT u32 wasm_bit_llm_get_memory_read_budget() { return nnuecut::MAX_MEMORY_READS_PER_BEAM; }
WASM_EXPORT u32 wasm_bit_llm_get_memory_reads() { return nnuecut::g_memoryReadCount; }
// v22 six-pillar scale APIs. Resource observations are supplied by the host.
WASM_EXPORT u32 wasm_bit_llm_scale_hardware_threads() { return nnuecut::g_resourceGovernor.hardware_threads(); }
WASM_EXPORT void wasm_bit_llm_scale_set_thread_cap(u32 n) { nnuecut::g_resourceGovernor.set_thread_cap(n); }
WASM_EXPORT void wasm_bit_llm_scale_observe(double cpuBusy, double ramUsed, double ramTotal) {
    nnuecut::g_resourceGovernor.observe(cpuBusy, ramUsed, ramTotal);
    nnuecut::g_resourceGovernor.apply_threads();
    const auto plan=nnuecut::g_resourceGovernor.plan();
    nnuecut::g_reasonBudget=std::max<u32>(nnuecut::REASON_MIN_STEPS,std::min<u32>(nnuecut::REASON_MAX_STEPS,plan.reason_steps));
}
WASM_EXPORT u32 wasm_bit_llm_scale_get_plan(u32 field) { const auto p=nnuecut::g_resourceGovernor.plan(); return field==0?p.threads:field==1?p.branch_budget:field==2?p.retrieval_budget:field==3?p.reason_steps:static_cast<u32>(p.ram_fraction*1000.0); }
WASM_EXPORT void wasm_bit_llm_long_context_set_memory_limit(u32 bytes) { nnuecut::g_longContext.set_memory_limit(bytes); }
WASM_EXPORT u32 wasm_bit_llm_long_context_add(const char *p,u32 n,u32 chunk,u32 overlap) { return p&&n&&nnuecut::g_longContext.add_document(std::string_view(p,n),chunk,overlap)?static_cast<u32>(nnuecut::g_longContext.size()):0; }
WASM_EXPORT u32 wasm_bit_llm_long_context_search(const char *p,u32 n,u32 topk,u32 rank) { if(!p||!n)return 0; const auto h=nnuecut::g_longContext.search(std::string_view(p,n),topk); if(rank>=h.size())return 0; const auto*c=nnuecut::g_longContext.get(h[rank].index); return c?static_cast<u32>(c->id):0; }
WASM_EXPORT u32 wasm_bit_llm_long_context_set_embedding(u32 chunkId,const float *v,u32 dims) { return v&&nnuecut::g_longContext.set_embedding(chunkId,v,dims)?1u:0u; }
WASM_EXPORT u32 wasm_bit_llm_long_context_search_vector(const char *p,u32 n,const float *v,u32 dims,u32 topk,u32 rank) { if(!p||!n)return 0; const auto h=nnuecut::g_longContext.search(std::string_view(p,n),topk,v,dims,0.65f); if(rank>=h.size())return 0; const auto*c=nnuecut::g_longContext.get(h[rank].index); return c?static_cast<u32>(c->id):0; }
WASM_EXPORT u32 wasm_bit_llm_long_context_get(u32 chunkId) { for(u32 i=0;i<nnuecut::g_longContext.size();++i){const auto*c=nnuecut::g_longContext.get(i);if(c&&c->id==chunkId){const u32 n=static_cast<u32>(std::min<std::size_t>(c->text.size(),IO_BUFFER_SIZE-1));std::memcpy(g_out,c->text.data(),n);g_out[n]=0;return n;}}return 0; }
WASM_EXPORT u32 wasm_bit_llm_reason_graph_add_branch(u32 parent,float score,const float *signature,u32 dims) { return static_cast<u32>(nnuecut::g_reasonGraph.add_branch(parent,score,signature,dims)); }
WASM_EXPORT u32 wasm_bit_llm_reason_graph_select(u32 limit,float diversity,u32 rank) { const auto ids=nnuecut::g_reasonGraph.select(limit,diversity);return rank<ids.size()?static_cast<u32>(ids[rank]):0; }
WASM_EXPORT void wasm_bit_llm_long_context_clear() { nnuecut::g_longContext.clear(); }
WASM_EXPORT u32 wasm_bit_llm_long_context_chunks() { return static_cast<u32>(nnuecut::g_longContext.size()); }
WASM_EXPORT u32 wasm_bit_llm_long_context_bytes() { return static_cast<u32>(std::min<std::size_t>(0xFFFFFFFFu,nnuecut::g_longContext.bytes())); }
WASM_EXPORT void wasm_bit_llm_reason_graph_reset() { nnuecut::g_reasonGraph.clear(); }
WASM_EXPORT u32 wasm_bit_llm_reason_graph_advance() { return nnuecut::g_reasonGraph.advance()?static_cast<u32>(nnuecut::g_reasonGraph.stage()):static_cast<u32>(bitscale::ReasonStage::Done); }
WASM_EXPORT u32 wasm_bit_llm_reason_graph_stage() { return static_cast<u32>(nnuecut::g_reasonGraph.stage()); }
WASM_EXPORT u32 wasm_bit_llm_creative_evaluate(const char *p,u32 pn,const char *intent,u32 in,float coherence,u32 metric) { if(!p||!pn||!intent)return 0; const auto x=nnuecut::g_creativeCritic.evaluate(std::string_view(p,pn),std::string_view(intent,in),coherence); const float v=metric==0?x.total:metric==1?x.novelty:metric==2?x.fit:x.coherence; return static_cast<u32>(v*1000.f+0.5f); }
WASM_EXPORT void wasm_bit_llm_creative_accept(const char *p,u32 n) { if(p&&n)nnuecut::g_creativeCritic.accept(std::string_view(p,n)); }
WASM_EXPORT void wasm_bit_llm_curriculum_configure(u32 n) { nnuecut::g_curriculum.configure(n); }
WASM_EXPORT void wasm_bit_llm_curriculum_set_difficulty(float d) { nnuecut::g_curriculum.set_difficulty(d); }
WASM_EXPORT u32 wasm_bit_llm_curriculum_get_plan(u32 field) { const auto p=nnuecut::g_curriculum.plan(); const float v=field==0?p.sample_weight:field==1?p.reasoning_weight:field==2?p.long_context_weight:p.learning_rate_scale; return static_cast<u32>(std::max(0.f,v)*1000.f+0.5f); }
WASM_EXPORT u32 wasm_bit_llm_get_reasoning_active() { return 1u; }
WASM_EXPORT const char *wasm_bit_llm_get_token_ptr(u32 id) { return id < g_model.vocabCount ? g_arena + g_off[id] : g_arena; }
WASM_EXPORT u32 wasm_bit_llm_get_token_len(u32 id) { return id < g_model.vocabCount ? g_len[id] : 0; }
WASM_EXPORT u32 wasm_bit_llm_get_vocab_count() { return g_model.vocabCount; }
WASM_EXPORT u32 wasm_bit_llm_get_head_words() { return LH; }
WASM_EXPORT const i32 *wasm_bit_llm_get_trace_ptr() { return &g_trace[0].token; }
WASM_EXPORT u32 wasm_bit_llm_get_trace_count() { return g_traceCount; }
WASM_EXPORT i32 wasm_bit_llm_get_stat(u32 i) { return i < 8 ? g_stat[i] : 0; }
WASM_EXPORT i32 wasm_bit_llm_get_ds_stat(u32 i) { return i < 8 ? g_dsStat[i] : 0; }
WASM_EXPORT void wasm_bit_llm_set_beam(u32 b) { o_Beam = b < 1 ? 1 : (b > MAX_BEAM ? (i32)MAX_BEAM : (i32)b); }
WASM_EXPORT u32 wasm_bit_llm_get_beam() { return (u32)o_Beam; }
WASM_EXPORT u8 *wasm_bit_llm_get_model_ptr() { return (u8 *)&g_model; }
WASM_EXPORT u32 wasm_bit_llm_get_header_size() { return HEADER_SIZE; }
WASM_EXPORT u32 wasm_bit_llm_get_header_error() { return g_headerError; }
WASM_EXPORT u32 wasm_bit_llm_get_proj_entry_size() { return LH; }
WASM_EXPORT u32 wasm_bit_llm_get_proj_bytes() { return LH; }
WASM_EXPORT u32 wasm_bit_llm_get_chunk_buffer_size() { return CHUNK_BUFFER_SIZE; }
WASM_EXPORT u8 *wasm_bit_llm_get_chunk_buffer_ptr() { return g_chunk_buffer; }
// Tương thích API cũ (PAGED đã bỏ: head chỉ ~LH byte/token)
WASM_EXPORT void wasm_bit_llm_set_ram_budget_kb(u32) {}
WASM_EXPORT u32 wasm_bit_llm_is_paged() { return 0; }
WASM_EXPORT u32 wasm_bit_llm_get_block_tokens() { return 0; }
WASM_EXPORT u32 wasm_bit_llm_get_max_slots() { return 0; }
WASM_EXPORT double wasm_bit_llm_get_proj_base() { return (double)g_lay.off[(u32)Section::HeadW]; }
WASM_EXPORT double wasm_bit_llm_get_file_size() { return (double)g_lay.total; }
WASM_EXPORT double wasm_bit_llm_get_stream_bytes() { return (double)g_loadTarget; }
WASM_EXPORT u32 wasm_bit_llm_get_block_stat(u32 i) { return i < 4 ? g_stageWin[i] : 0; } // TT candidates / full scan
WASM_EXPORT u32 wasm_bit_llm_block_in_ram(u32) { return 1; }
WASM_EXPORT u32 wasm_bit_llm_get_creativity() { return (u32)o_Creativity; }
WASM_EXPORT void wasm_bit_llm_set_creativity(u32 pct) { o_Creativity = pct > 300 ? 300 : (i32)pct; }
WASM_EXPORT u32 wasm_bit_llm_get_novelty() { return g_lastNovelty; }
WASM_EXPORT u32 wasm_bit_llm_get_temp() { return (u32)(calT() * 100.f); } // x100
WASM_EXPORT void wasm_bit_llm_set_temp(u32 x100)
{
    x100 = x100 < 5 ? 5 : (x100 > 10000 ? 10000 : x100);
    g_model.temp = (float)x100 / 100.f;
}
WASM_EXPORT void wasm_bit_llm_set_sampling(u32, u32) {}
WASM_EXPORT u32 wasm_bit_llm_get_top_p() { return 100; }
WASM_EXPORT void wasm_bit_llm_set_min_lp(u32 x100) { o_MinLogProb = x100 > 1000 ? 1000 : (i32)x100; }
WASM_EXPORT u32 wasm_bit_llm_get_answer_source() { return g_answerSource; } // 2=neural, 3=fallback
WASM_EXPORT u32 wasm_bit_llm_get_nodes() { return (u32)g_nodesTotal; }
WASM_EXPORT void wasm_bit_llm_set_desc(u32 len)
{
    g_descLen = len > sizeof(g_desc) ? (u32)sizeof(g_desc) : len;
    std::memcpy(g_desc, g_in, g_descLen);
}

// UCI-like: g_in = "Name value" -> 1 ok, 0 tên lạ, 2 ngoài khoảng. Thông điệp ghi vào g_out.
WASM_EXPORT u32 wasm_bit_llm_setoption(u32 len)
{
    if (len >= IO_BUFFER_SIZE)
        len = IO_BUFFER_SIZE - 1;
    g_in[len] = 0;
    char name[48] = {};
    long v = 0;
    u32 i = 0, n = 0;
    while (i < len && g_in[i] == ' ')
        ++i;
    while (i < len && g_in[i] != ' ' && n < 47)
        name[n++] = g_in[i++];
    v = std::strtol(g_in + i, nullptr, 10);
    for (u32 k = 0; k < N_OPTS; ++k)
    {
        bool eq = std::strlen(g_opts[k].name) == n;
        for (u32 c = 0; eq && c < n; ++c)
            eq = (name[c] | 32) == (g_opts[k].name[c] | 32);
        if (!eq)
            continue;
        if (v < g_opts[k].mn || v > g_opts[k].mx)
        {
            std::snprintf(g_out, IO_BUFFER_SIZE, "option %s: ngoai khoang [%d,%d]", g_opts[k].name, g_opts[k].mn, g_opts[k].mx);
            return 2;
        }
        *g_opts[k].val = (i32)v;
        std::snprintf(g_out, IO_BUFFER_SIZE, "%s = %ld", g_opts[k].name, v);
        return 1;
    }
    std::snprintf(g_out, IO_BUFFER_SIZE, "option khong ton tai: %s", name);
    return 0;
}
WASM_EXPORT u32 wasm_bit_llm_list_options()
{
    u32 o = 0;
    for (u32 k = 0; k < N_OPTS && o + 80 < IO_BUFFER_SIZE; ++k)
        o += (u32)std::snprintf(g_out + o, IO_BUFFER_SIZE - o, "option name %s type spin default %d min %d max %d\n", g_opts[k].name, *g_opts[k].val, g_opts[k].mn, g_opts[k].mx);
    return o;
}

// Phản hồi 👍/👎 cho đáp án vừa sinh: hist_update có "trọng lực" (như StatsEntry của Stockfish), học tại chỗ, không cần train lại.
inline void feedback_apply(u64 context, const u32 *tokens, u32 count, i32 good) noexcept
{
    const int bonus = good > 0 ? 1200 : -1200;
    ++g_feedbackGen;
    if (++g_feedbackEpoch == 0)
        ++g_feedbackEpoch;
    for (u32 i = 0; i < count; ++i)
    {
        const u32 t = tokens[i];
        if (t == 0 || t >= g_model.vocabCount)
            continue;
        if (FeedbackEntry *e = feedback_find(context, t, true))
        {
            const i32 next = (i32)e->score + bonus - (i32)e->score * std::abs(bonus) / 8192;
            e->score = (i16)clampi(next, -8192, 8192);
            e->age = g_feedbackGen;
        }
        // Preserve the legacy inspection API without applying this global value
        // to logits; inference uses only the context-keyed table above.
        if (g_fb && t < g_metaCap)
        {
            i16 &h = g_fb[t];
            const i32 next = (i32)h + bonus - (i32)h * std::abs(bonus) / 8192;
            h = (i16)clampi(next, -8192, 8192);
        }
    }
}
WASM_EXPORT void wasm_bit_llm_feedback(i32 good)
{
    if (g_answerSource != 2 || g_lastN == 0)
        return;
    feedback_apply(g_lastFeedbackCtx, g_lastTok, g_lastN, good);
}
WASM_EXPORT void wasm_bit_llm_feedback_text(u32 qLen, u32 aLen, i32 good)
{
    if (!g_emb || !g_hw || qLen == 0 || qLen >= IO_BUFFER_SIZE || aLen == 0 || aLen >= IO_BUFFER_SIZE)
        return;
    static TokInfo qTok[MAX_Q], aTok[MAX_A];
    const u32 qn = tokenize(g_in, qLen, qTok, MAX_Q, false);
    const u32 an = tokenize(g_out, aLen, aTok, MAX_A, false);
    if (qn == 0 || an == 0)
        return;
    static QFeats q;
    build_qfeats(qTok, qn, q);
    u32 tokens[MAX_GEN_TOKENS], n = 0;
    for (u32 i = 0; i < an && n < MAX_GEN_TOKENS; ++i)
        if (aTok[i].id != 0 && aTok[i].id != UNK)
            tokens[n++] = aTok[i].id;
    if (n)
        feedback_apply(q.key, tokens, n, good);
}
WASM_EXPORT i16 *wasm_bit_llm_get_fb_ptr() { return g_fb; } // JS có thể lưu/nạp lại bảng này (IndexedDB)

// =====================================================================================================================================
// BEAM SEARCH (cây node) trên ACCUMULATOR GIA TĂNG + TokenPicker
// =====================================================================================================================================
namespace nnuecut
{
    struct QRun
    {
        QFeats q;
    };
    struct BeamNode
    {
        u32 parent, tok;
        i32 score, margin;
    };
    constexpr u32 NO_NODE = 0xFFFFFFFFu;
    struct alignas(64) BeamState
    {
        i32 accQ[L1], accS[L1];
        double lp, lpp;
        u64 prefix;
        u32 node, len, y1, y2, y3;
        u64 memReadIds[MAX_MEMORY_READS_PER_BEAM * MEMORY_READ_TOP_K];
        u64 lastMemoryQueryKey;
        u32 memReadCount, memReads;
        bool done;
    };
    struct BeamCand
    {
        double lp, lpp;
        u32 src, tok, len;
        i32 score, margin;
        bool done, carry;
    };
    struct BeamResult
    {
        double lp;
        u32 len;
        bool done;
        u32 tok[MAX_GEN_TOKENS];
        TraceStep tr[MAX_GEN_TOKENS];
    };

    // Think-session latent cache. It memoizes only deterministic tower outputs;
    // full prefix + evidence IDs + question + model generation form the key.
    constexpr u32 THINK_LATENT_CACHE_SIZE = 2048;
    static_assert((THINK_LATENT_CACHE_SIZE & (THINK_LATENT_CACHE_SIZE - 1)) == 0,
                  "Think latent cache must be power-of-two sized");
    struct alignas(64) ThinkLatentEntry
    {
        u64 key;
        u8 hidden[LH];
        float memoryProbability;
        float haltProbability;
        u32 reasonSteps;
    };
    static ThinkLatentEntry g_thinkLatentCache[THINK_LATENT_CACHE_SIZE]{};
    static u64 g_thinkCacheHits = 0, g_thinkCacheMisses = 0;
    static BeamState g_thinkRoot{};
    static bool g_thinkRootReady = false;

    inline u64 think_latent_key(const QFeats &q, const BeamState &bs) noexcept
    {
        u64 key = mix64(q.key ^ bs.prefix ^ ((u64)bs.len << 32) ^ g_model.epochs ^ ((u64)g_model.archHash << 1));
        for (u32 i = 0; i < bs.memReadCount; ++i)
            key = mix64(key ^ bs.memReadIds[i] ^ ((u64)i * 0x9E3779B97F4A7C15ULL));
        return key ? key : 1;
    }
    inline void tower_for_beam(const QFeats &q, const BeamState &bs, u8 *h8, bool useCache) noexcept
    {
        if (!useCache)
        {
            tower(bs.accQ, bs.accS, h8, bs.len);
            return;
        }
        const u64 key = think_latent_key(q, bs);
        ThinkLatentEntry &entry = g_thinkLatentCache[(u32)key & (THINK_LATENT_CACHE_SIZE - 1)];
        if (entry.key == key)
        {
            std::memcpy(h8, entry.hidden, LH);
            g_lastMemoryProbability = entry.memoryProbability;
            g_lastHaltProbability = entry.haltProbability;
            g_reasonLastSteps = entry.reasonSteps;
            ++g_thinkCacheHits;
            return;
        }
        ++g_thinkCacheMisses;
        tower(bs.accQ, bs.accS, h8, bs.len);
        entry.key = key;
        std::memcpy(entry.hidden, h8, LH);
        entry.memoryProbability = g_lastMemoryProbability;
        entry.haltProbability = g_lastHaltProbability;
        entry.reasonSteps = g_reasonLastSteps;
    }
    static BeamNode g_nodes[MAX_GEN_TOKENS * MAX_BEAM + 8];
    static u32 g_nodeCount = 0;
    static BeamState g_bsA[MAX_BEAM], g_bsB[MAX_BEAM];
    // A Gumbel beam can contribute up to 4*B candidates per live state.
    // Capacity must cover B states, not only the final B*B selection window.
    static BeamCand g_cand[4 * MAX_BEAM * MAX_BEAM];
    static u64 g_trng = 0xABCDEF12345ULL;
    inline void set_search_seed(u32 seed) noexcept
    {
        g_trng = ((u64)seed << 32) | (u64)(seed ^ 0x9E3779B9u);
        if (g_trng == 0)
            g_trng = 0xABCDEF12345ULL;
    }
    constexpr u32 MAXC = TOKEN_PICKER_CAP;
    static_assert(MAXC >= 4 * MAX_BEAM, "candidate shortlist must cover the expanded beam fan-out");
    static u8 g_pickStage[MAXC]; // parallel to g_cid while FastPicker is active

    FORCE_INLINE bool cand_better(const BeamCand &a, const BeamCand &b) noexcept
    {
        if (a.lpp != b.lpp)
            return a.lpp > b.lpp;
        if (a.src != b.src)
            return a.src < b.src;
        return a.tok < b.tok;
    }
    inline double novelty_of(const QFeats &q) noexcept
    {
        const double unk = q.total ? 1.0 - (double)q.known / (double)q.total : 0.0;
        return std::min(1.0, unk);
    }

    // Neural confidence + learned ACT controller decide whether to request more
    // evidence. Context results are encoded as ordinary learned embedding features.
    // The bounded read count is a safety budget; actual reads are conditional.
    inline bool beam_has_memory_id(const BeamState &bs, u64 id) noexcept
    {
        for (u32 i = 0; i < bs.memReadCount; ++i) if (bs.memReadIds[i] == id) return true;
        return false;
    }
    inline void append_query_token(char *dst, u32 &used, const char *src, u32 len) noexcept
    {
        if (used && used + 1 < IO_BUFFER_SIZE) dst[used++] = ' ';
        const u32 take = std::min<u32>(len, IO_BUFFER_SIZE - used - 1);
        if (take) { std::memcpy(dst + used, src, take); used += take; }
        dst[used] = 0;
    }
    inline bool memory_apply_query(const char *query, u32 used, BeamState &bs) noexcept
    {
        if (!query || used == 0 || g_contextMemory.size() == 0 ||
            bs.memReads >= MAX_MEMORY_READS_PER_BEAM ||
            bs.memReadCount >= MAX_MEMORY_READS_PER_BEAM * MEMORY_READ_TOP_K)
            return false;
        ++bs.memReads;
        const u64 now = (u64)std::time(nullptr);
        const auto &hits = g_contextMemory.search_at(query, used, MEMORY_READ_TOP_K, now);
        bool added = false;
        static TokInfo memTok[32];
        for (u32 hi = 0; hi < hits.size() && hi < MEMORY_READ_TOP_K; ++hi)
        {
            const bitctx::Hit *hit = g_contextMemory.hit(hi);
            const bitctx::Entry *entry = hit ? g_contextMemory.entry(hit->entry_index) : nullptr;
            if (!entry || beam_has_memory_id(bs, entry->id) ||
                bs.memReadCount >= MAX_MEMORY_READS_PER_BEAM * MEMORY_READ_TOP_K)
                continue;
            // Limit each evidence record so a long memory note cannot drown out the live request.
            const u32 n = tokenize(entry->text.data(), (u32)entry->text.size(), memTok, 32, false);
            if (!n) continue;
            QFeats memoryFeatures{};
            build_qfeats(memTok, n, memoryFeatures);
            // Inject retrieved evidence into the question accumulator, not the answer buffer.
            acc_apply<1>(bs.accQ, memoryFeatures.idx, memoryFeatures.n);
            bs.memReadIds[bs.memReadCount++] = entry->id;
            ++g_memoryReadCount;
            added = true;
        }
        return added;
    }
    inline bool memory_read_initial(const QFeats &, BeamState &bs) noexcept
    {
#if !defined(__EMSCRIPTEN__)
        context_load_disk_once();
#endif
        // Every inference performs one evidence lookup from the user's actual request.
        // The neural policy is only allowed to request additional hops later.
        return memory_apply_query(g_in, g_lastInputLen, bs);
    }
    inline bool memory_read_for_state(const QFeats &q, BeamState &bs, float readProbability) noexcept
    {
        if (readProbability < 0.5f || bs.memReads >= MAX_MEMORY_READS_PER_BEAM ||
            bs.memReadCount >= MAX_MEMORY_READS_PER_BEAM * MEMORY_READ_TOP_K)
            return false;
        const u64 queryKey = mix64(q.key ^ bs.prefix ^ ((u64)bs.y1 << 32) ^ ((u64)bs.y2 << 1) ^ bs.len);
        if (queryKey == bs.lastMemoryQueryKey) return false;
        bs.lastMemoryQueryKey = queryKey;
        char query[IO_BUFFER_SIZE]{};
        u32 used = 0;
        append_query_token(query, used, g_in, g_lastInputLen);
        const u32 tail[3] = {bs.y3, bs.y2, bs.y1};
        for (u32 tok : tail)
            if (tok != BOS && tok != EOS && tok < g_model.vocabCount)
                append_query_token(query, used, g_arena + g_off[tok], g_len[tok]);
        return memory_apply_query(query, used, bs);
    }

    // Neural TokenPicker: ContextTT -> FULL vocabulary fallback. full=true nghĩa là ứng viên i == token i.
    inline u32 gather_scores(const u8 *h8, u64 ttkey, bool &full) noexcept
    {
        const u32 N = g_model.vocabCount;
        if (o_Fast)
        {
            u32 n = 0;
            auto add = [&](u32 t, u32 st) noexcept
            {
                if (t >= N || n >= MAXC)
                    return; // validate token id before using it
                for (u32 i = 0; i < n; ++i)
                    if (g_cid[i] == t)
                        return;
                g_pickStage[n] = (u8)st;
                g_cid[n++] = t;
            };
            add(EOS, 0);
            if (const TTEntry *e = tt_probe(ttkey))
                for (u32 i = 0, count = tt_count(*e); i < count; ++i)
                    add(tt_token(*e, i), 0);
            if (n >= (u32)o_MinCand)
            {
                full = false;
                for (u32 i = 0; i < n; ++i)
                    g_clg[i] = head_dot(h8, g_cid[i]);
                return n;
            }
        }
        full = true;
        head_all(h8, N, g_clg);
        return N;
    }

    // Fast-picker probe deliberately does not fall back to a full scan. The caller
    // can select the fused streaming path when the shortlist is too small.
    inline u32 gather_fast_scores(const u8 *h8, u64 ttkey, bool &full) noexcept
    {
        const u32 N = g_model.vocabCount;
        u32 n = 0;
        auto add = [&](u32 t, u32 st) noexcept
        {
            if (t >= N || n >= MAXC) return;
            for (u32 i = 0; i < n; ++i) if (g_cid[i] == t) return;
            g_pickStage[n] = (u8)st;
            g_cid[n++] = t;
        };
        add(EOS, 0);
        if (const TTEntry *e = tt_probe(ttkey))
            for (u32 i = 0, count = tt_count(*e); i < count; ++i)
                add(tt_token(*e, i), 0);
        if (n < (u32)o_MinCand)
        {
            full = true;
            return 0;
        }
        full = false;
        for (u32 i = 0; i < n; ++i) g_clg[i] = head_dot(h8, g_cid[i]);
        return n;
    }

    struct StreamToken
    {
        u32 tok = 0;
        float raw = 0.0f;
        double score = -std::numeric_limits<double>::infinity();
    };
    struct StreamTop
    {
        StreamToken item[4 * MAX_BEAM];
        u32 count = 0;
        double logZ = 0.0;
    };
    FORCE_INLINE bool stream_better(const StreamToken &a, const StreamToken &b) noexcept
    {
        return a.score > b.score || (a.score == b.score && a.tok < b.tok);
    }
    FORCE_INLINE bool stream_worse(const StreamToken &a, const StreamToken &b) noexcept
    {
        return stream_better(b, a);
    }
    inline void stream_heap_insert(StreamTop &top, u32 capacity, const StreamToken &v) noexcept
    {
        if (capacity == 0) return;
        if (top.count < capacity)
        {
            u32 i = top.count++;
            top.item[i] = v;
            while (i)
            {
                const u32 parent = (i - 1) >> 1;
                if (!stream_worse(top.item[i], top.item[parent])) break;
                std::swap(top.item[i], top.item[parent]);
                i = parent;
            }
            return;
        }
        if (!stream_better(v, top.item[0])) return;
        top.item[0] = v;
        u32 i = 0;
        for (;;)
        {
            const u32 left = i * 2 + 1;
            if (left >= top.count) break;
            const u32 right = left + 1;
            u32 worst = left;
            if (right < top.count && stream_worse(top.item[right], top.item[left])) worst = right;
            if (!stream_worse(top.item[worst], top.item[i])) break;
            std::swap(top.item[worst], top.item[i]);
            i = worst;
        }
    }

    // Exact full-vocabulary scoring with O(K) scratch instead of materializing N
    // logits and N log-probabilities. Online log-sum-exp preserves the same -60
    // tail cutoff used by the reference implementation.
    struct StreamBlockResult
    {
        double maximum;
        double expSum;
        StreamTop top;
    };
    static std::vector<StreamBlockResult> g_streamBlockScratch;

    // Multi-vector int8 x uint8 dot products. Each output weight vector is
    // loaded once per SIMD chunk and reused for all live beam activation vectors.
    FORCE_INLINE void head_dot_multi_beam(const i8 *w, const u8 *const hidden[MAX_BEAM],
                                          u32 beamCount, i32 out[MAX_BEAM]) noexcept
    {
#if defined(__AVX512VNNI__)
        __m512i acc[MAX_BEAM];
        for (u32 b = 0; b < beamCount; ++b) acc[b] = _mm512_setzero_si512();
        u32 j = 0;
        for (; j + 64 <= LH; j += 64)
        {
            const __m512i vw = _mm512_loadu_si512(reinterpret_cast<const void *>(w + j));
            for (u32 b = 0; b < beamCount; ++b)
            {
                const __m512i va = _mm512_loadu_si512(reinterpret_cast<const void *>(hidden[b] + j));
                acc[b] = _mm512_dpbusd_epi32(acc[b], va, vw);
            }
        }
        for (u32 b = 0; b < beamCount; ++b)
        {
            alignas(64) i32 lanes[16];
            _mm512_store_si512(reinterpret_cast<void *>(lanes), acc[b]);
            i32 sum = 0;
            for (u32 k = 0; k < 16; ++k) sum += lanes[k];
            for (u32 k = j; k < LH; ++k) sum += (i32)w[k] * hidden[b][k];
            out[b] = sum;
        }
#elif defined(__AVX512BW__)
        const __m512i ones = _mm512_set1_epi16(1);
        __m512i acc[MAX_BEAM];
        for (u32 b = 0; b < beamCount; ++b) acc[b] = _mm512_setzero_si512();
        u32 j = 0;
        for (; j + 64 <= LH; j += 64)
        {
            const __m512i vw = _mm512_loadu_si512(reinterpret_cast<const void *>(w + j));
            for (u32 b = 0; b < beamCount; ++b)
            {
                const __m512i va = _mm512_loadu_si512(reinterpret_cast<const void *>(hidden[b] + j));
                const __m512i pair = _mm512_maddubs_epi16(va, vw);
                acc[b] = _mm512_add_epi32(acc[b], _mm512_madd_epi16(pair, ones));
            }
        }
        for (u32 b = 0; b < beamCount; ++b)
        {
            alignas(64) i32 lanes[16];
            _mm512_store_si512(reinterpret_cast<void *>(lanes), acc[b]);
            i32 sum = 0;
            for (u32 k = 0; k < 16; ++k) sum += lanes[k];
            for (u32 k = j; k < LH; ++k) sum += (i32)w[k] * hidden[b][k];
            out[b] = sum;
        }
#elif defined(__AVX2__)
        const __m256i ones = _mm256_set1_epi16(1);
        __m256i acc[MAX_BEAM];
        for (u32 b = 0; b < beamCount; ++b) acc[b] = _mm256_setzero_si256();
        u32 j = 0;
        for (; j + 32 <= LH; j += 32)
        {
            const __m256i vw = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(w + j));
            for (u32 b = 0; b < beamCount; ++b)
            {
                const __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(hidden[b] + j));
                const __m256i pair = _mm256_maddubs_epi16(va, vw);
                acc[b] = _mm256_add_epi32(acc[b], _mm256_madd_epi16(pair, ones));
            }
        }
        for (u32 b = 0; b < beamCount; ++b)
        {
            alignas(32) i32 lanes[8];
            _mm256_store_si256(reinterpret_cast<__m256i *>(lanes), acc[b]);
            i32 sum = 0;
            for (u32 k = 0; k < 8; ++k) sum += lanes[k];
            for (u32 k = j; k < LH; ++k) sum += (i32)w[k] * hidden[b][k];
            out[b] = sum;
        }
#else
        for (u32 b = 0; b < beamCount; ++b) out[b] = dot_i8_u8<LH>(w, hidden[b]);
#endif
    }

    inline void stream_full_scores(const u8 *h8, const BeamState &bs, double invT,
                                   double repetitionPenalty, u32 keepK, StreamTop &out) noexcept;

    // Batch the output projection across live beams. The token-row weights are
    // loaded once and reused against every beam hidden state. OpenMP partitions
    // independent vocabulary tiles; each tile has private LSE/top-K accumulators.
    inline void stream_full_scores_batch(const BeamState *const states[MAX_BEAM],
                                         const u8 *const hidden[MAX_BEAM], u32 beamCount,
                                         double invT, double repetitionPenalty, u32 keepK,
                                         StreamTop out[MAX_BEAM]) noexcept
    {
        const u32 N = g_model.vocabCount;
        keepK = std::max<u32>(1, std::min<u32>(keepK, 4 * MAX_BEAM));
        if (!beamCount || !N) return;
        // The first decode step often has a single live beam. Use the simpler
        // SIMD streaming kernel instead of paying tile/scratch/OpenMP overhead.
        if (beamCount == 1)
        {
            stream_full_scores(hidden[0], *states[0], invT, repetitionPenalty, keepK, out[0]);
            return;
        }
        constexpr u32 BLOCK_TOKENS = 1024;
        const u32 blocks = (N + BLOCK_TOKENS - 1) / BLOCK_TOKENS;
        try
        {
            g_streamBlockScratch.resize((size_t)blocks * beamCount);
        }
        catch (...)
        {
            // Preserve correctness under memory pressure; fall back to the
            // lower-scratch per-beam streaming path rather than terminating.
            for (u32 b = 0; b < beamCount; ++b)
                stream_full_scores(hidden[b], *states[b], invT, repetitionPenalty, keepK, out[b]);
            return;
        }
        u32 history[MAX_BEAM][MAX_GEN_TOKENS];
        u32 historyCount[MAX_BEAM]{};
        for (u32 b = 0; b < beamCount; ++b)
        {
            for (u32 nd = states[b]->node; nd != NO_NODE && historyCount[b] < MAX_GEN_TOKENS; nd = g_nodes[nd].parent)
                if (g_nodes[nd].tok != EOS) history[b][historyCount[b]++] = g_nodes[nd].tok;
            std::sort(history[b], history[b] + historyCount[b]);
            historyCount[b] = (u32)(std::unique(history[b], history[b] + historyCount[b]) - history[b]);
        }
#ifdef _OPENMP
#pragma omp parallel for schedule(static) if (blocks > 4 && N > 4096)
#endif
        for (i64 block = 0; block < (i64)blocks; ++block)
        {
            const u32 begin = (u32)block * BLOCK_TOKENS;
            const u32 end = std::min<u32>(N, begin + BLOCK_TOKENS);
            for (u32 b = 0; b < beamCount; ++b)
            {
                StreamBlockResult &r = g_streamBlockScratch[(size_t)block * beamCount + b];
                r.maximum = -std::numeric_limits<double>::infinity();
                r.expSum = 0.0;
                r.top.count = 0;
                r.top.logZ = 0.0;
            }
            for (u32 tok = begin; tok < end; ++tok)
            {
                const i8 *w = g_hw + (size_t)tok * LH;
                i32 dots[MAX_BEAM]{};
                head_dot_multi_beam(w, hidden, beamCount, dots);
                const double feedback = (double)g_feedbackQueryScore[tok] * (1.0 / 2048.0);
                for (u32 b = 0; b < beamCount; ++b)
                {
                    const BeamState &bs = *states[b];
                    const float raw = (float)dots[b] * (1.0f / (127.0f * WS)) + g_hb[tok];
                    double score = ((double)raw + feedback) * invT;
                    if (bs.len == 0 && tok == EOS) score -= 50.0;
                    if (historyCount[b] && std::binary_search(history[b], history[b] + historyCount[b], tok))
                        score -= repetitionPenalty * invT;
                    if (bs.len && bs.y1 != BOS && bs.y1 != EOS && tok == bs.y1)
                        score -= 12.0 * invT;
                    StreamBlockResult &r = g_streamBlockScratch[(size_t)block * beamCount + b];
                    if (score > r.maximum)
                    {
                        const double delta = r.maximum - score;
                        r.expSum = delta > -745.0 ? r.expSum * std::exp(delta) + 1.0 : 1.0;
                        r.maximum = score;
                    }
                    else
                    {
                        const double delta = score - r.maximum;
                        if (delta > -745.0) r.expSum += std::exp(delta);
                    }
                    stream_heap_insert(r.top, keepK, StreamToken{tok, raw, score});
                }
            }
        }
        for (u32 b = 0; b < beamCount; ++b)
        {
            double maximum = -std::numeric_limits<double>::infinity();
            for (u32 block = 0; block < blocks; ++block)
                maximum = std::max(maximum, g_streamBlockScratch[(size_t)block * beamCount + b].maximum);
            double expSum = 0.0;
            out[b].count = 0;
            for (u32 block = 0; block < blocks; ++block)
            {
                const StreamBlockResult &r = g_streamBlockScratch[(size_t)block * beamCount + b];
                if (r.expSum > 0.0 && r.maximum - maximum > -745.0)
                    expSum += std::exp(r.maximum - maximum) * r.expSum;
                for (u32 i = 0; i < r.top.count; ++i)
                    stream_heap_insert(out[b], keepK, r.top.item[i]);
            }
            out[b].logZ = maximum + std::log(expSum);
            std::sort(out[b].item, out[b].item + out[b].count, stream_better);
        }
        g_outputTokenScores += (u64)N * beamCount;
        g_outputHeadMacs += (u64)N * beamCount * LH;
    }

    inline void stream_full_scores(const u8 *h8, const BeamState &bs, double invT,
                                   double repetitionPenalty, u32 keepK, StreamTop &out) noexcept
    {
        out.count = 0;
        out.logZ = 0.0;
        const u32 N = g_model.vocabCount;
        keepK = std::max<u32>(1, std::min<u32>(keepK, 4 * MAX_BEAM));
        u32 history[MAX_GEN_TOKENS], historyCount = 0;
        for (u32 nd = bs.node; nd != NO_NODE && historyCount < MAX_GEN_TOKENS; nd = g_nodes[nd].parent)
            if (g_nodes[nd].tok != EOS) history[historyCount++] = g_nodes[nd].tok;
        std::sort(history, history + historyCount);
        historyCount = (u32)(std::unique(history, history + historyCount) - history);

        double maximum = -std::numeric_limits<double>::infinity();
        double expSum = 0.0;
        for (u32 tok = 0; tok < N; ++tok)
        {
            const float raw = head_dot(h8, tok);
            double score = ((double)raw + (double)g_feedbackQueryScore[tok] * (1.0 / 2048.0)) * invT;
            if (bs.len == 0 && tok == EOS) score -= 50.0;
            if (historyCount && std::binary_search(history, history + historyCount, tok))
                score -= repetitionPenalty * invT;
            if (bs.len && bs.y1 != BOS && bs.y1 != EOS && tok == bs.y1)
                score -= 12.0 * invT;

            if (score > maximum)
            {
                const double delta = maximum - score;
                expSum = delta > -60.0 ? expSum * std::exp(delta) + 1.0 : 1.0;
                maximum = score;
            }
            else
            {
                const double delta = score - maximum;
                if (delta > -60.0) expSum += std::exp(delta);
            }
            stream_heap_insert(out, keepK, StreamToken{tok, raw, score});
        }
        if (N == 0 || !(expSum > 0.0) || !std::isfinite(maximum))
        {
            out.count = 0;
            out.logZ = 0.0;
            return;
        }
        out.logZ = maximum + std::log(expSum);
        std::sort(out.item, out.item + out.count, stream_better);
        g_outputTokenScores += N;
        g_outputHeadMacs += (u64)N * LH;
    }

    struct RouterRank
    {
        u32 cluster;
        double logProbability;
        float raw;
    };

    // The trained factorization is a normalized hierarchical distribution:
    //   P(token) = P(cluster) * P(token | cluster).
    // Since log P(token | cluster) <= 0, log P(cluster) is a rigorous upper
    // bound for every unvisited token in that cluster. We can therefore stop
    // once the next router bound is below the current global top-K threshold.
    inline void stream_hierarchical_scores(const u8 *h8, const BeamState &bs,
                                           double invT, double repetitionPenalty,
                                           u32 keepK, StreamTop &out,
                                           bool includeFeedback = true) noexcept
    {
        out.count = 0;
        out.logZ = 0.0; // exact: sum_c P(c) * sum_t P(t|c) == 1
        const u32 N = g_model.vocabCount;
        const u32 C = router_cluster_count(N);
        keepK = std::max<u32>(1, std::min<u32>(keepK, 4 * MAX_BEAM));
        if (!N || !C || C > ROUTER_MAX_CLUSTERS || g_routerCap < C) return;

        static thread_local std::vector<RouterRank> rankScratch;
        rankScratch.resize(C);
        RouterRank *rank = rankScratch.data();
        double routerMax = -std::numeric_limits<double>::infinity();
        for (u32 c = 0; c < C; ++c)
        {
            const float raw = router_head_dot(h8, c);
            const double z = (double)raw * invT;
            rank[c] = {c, z, raw};
            routerMax = std::max(routerMax, z);
        }
        double routerSum = 0.0;
        for (u32 c = 0; c < C; ++c)
            routerSum += std::exp(rank[c].logProbability - routerMax);
        const double routerLogZ = routerMax + std::log(std::max(routerSum, 1.0e-300));
        for (u32 c = 0; c < C; ++c)
            rank[c].logProbability -= routerLogZ;
        // A max-heap yields the next cluster bound without sorting all C clusters.
        const auto lowerPriority = [](const RouterRank &a, const RouterRank &b) noexcept
        {
            if (a.logProbability != b.logProbability) return a.logProbability < b.logProbability;
            return a.cluster > b.cluster;
        };
        std::make_heap(rank, rank + C, lowerPriority);

        u32 history[MAX_GEN_TOKENS], historyCount = 0;
        if (repetitionPenalty > 0.0 || bs.len)
        {
            for (u32 nd = bs.node; nd != NO_NODE && historyCount < MAX_GEN_TOKENS; nd = g_nodes[nd].parent)
                if (g_nodes[nd].tok != EOS) history[historyCount++] = g_nodes[nd].tok;
            std::sort(history, history + historyCount);
            historyCount = (u32)(std::unique(history, history + historyCount) - history);
        }

        u32 visitedClusters = 0;
        u32 scoredTokens = 0;
        for (u32 ri = 0; ri < C; ++ri)
        {
            std::pop_heap(rank, rank + (C - ri), lowerPriority);
            const RouterRank rr = rank[C - ri - 1];
            // The next cluster's log P(cluster) bounds every child token.
            // Strict '<' preserves deterministic token-id tie breaking at equality.
            if (out.count == keepK && rr.logProbability < out.item[0].score)
                break;

            const u32 begin = rr.cluster * ROUTER_CLUSTER_SIZE;
            const u32 count = std::min<u32>(ROUTER_CLUSTER_SIZE, N - begin);
            double localScore[ROUTER_CLUSTER_SIZE];
            float fineRaw[ROUTER_CLUSTER_SIZE];
            double localMax = -std::numeric_limits<double>::infinity();
            for (u32 slot = 0; slot < count; ++slot)
            {
                const u32 tok = begin + slot;
                const float raw = head_dot(h8, tok);
                fineRaw[slot] = raw;
                double score = ((double)raw + (includeFeedback && tok < g_feedbackQueryScore.size()
                                    ? (double)g_feedbackQueryScore[tok] * (1.0 / 2048.0) : 0.0)) * invT;
                if (bs.len == 0 && tok == EOS) score -= 50.0 * invT;
                if (historyCount && std::binary_search(history, history + historyCount, tok))
                    score -= repetitionPenalty * invT;
                if (bs.len && bs.y1 != BOS && bs.y1 != EOS && tok == bs.y1)
                    score -= 12.0 * invT;
                localScore[slot] = score;
                localMax = std::max(localMax, score);
            }
            double localSum = 0.0;
            for (u32 slot = 0; slot < count; ++slot)
                localSum += std::exp(localScore[slot] - localMax);
            const double localLogZ = localMax + std::log(std::max(localSum, 1.0e-300));
            for (u32 slot = 0; slot < count; ++slot)
            {
                const u32 tok = begin + slot;
                const double childLogP = localScore[slot] - localLogZ;
                const double jointLogP = rr.logProbability + childLogP;
                const float combinedRaw = rr.raw + fineRaw[slot];
                stream_heap_insert(out, keepK, StreamToken{tok, combinedRaw, jointLogP});
            }
            ++visitedClusters;
            scoredTokens += count;
        }
        std::sort(out.item, out.item + out.count, stream_better);
        g_outputTokenScores += scoredTokens;
        g_outputHeadMacs += (u64)scoredTokens * LH;
        g_outputRouterMacs += (u64)C * LH;
        g_outputClustersVisited += visitedClusters;
    }

    inline double hierarchical_target_logprob(const u8 *h8, u32 target, double invT = 1.0) noexcept
    {
        const u32 N = g_model.vocabCount;
        if (target >= N || !N) return -std::numeric_limits<double>::infinity();
        const u32 C = router_cluster_count(N);
        const u32 targetCluster = router_cluster_of(target);
        double rmax = -std::numeric_limits<double>::infinity();
        static thread_local std::vector<double> routerScoreScratch;
        routerScoreScratch.resize(C);
        double *routerScore = routerScoreScratch.data();
        for (u32 c = 0; c < C; ++c)
        {
            routerScore[c] = (double)router_head_dot(h8, c) * invT;
            rmax = std::max(rmax, routerScore[c]);
        }
        double rsum = 0.0;
        for (u32 c = 0; c < C; ++c) rsum += std::exp(routerScore[c] - rmax);
        const double routerLogZ = rmax + std::log(std::max(rsum, 1.0e-300));

        const u32 begin = targetCluster * ROUTER_CLUSTER_SIZE;
        const u32 count = std::min<u32>(ROUTER_CLUSTER_SIZE, N - begin);
        double fineScore[ROUTER_CLUSTER_SIZE];
        double fmax = -std::numeric_limits<double>::infinity();
        for (u32 slot = 0; slot < count; ++slot)
        {
            fineScore[slot] = (double)head_dot(h8, begin + slot) * invT;
            fmax = std::max(fmax, fineScore[slot]);
        }
        double fsum = 0.0;
        for (u32 slot = 0; slot < count; ++slot) fsum += std::exp(fineScore[slot] - fmax);
        const double fineLogZ = fmax + std::log(std::max(fsum, 1.0e-300));
        return (routerScore[targetCluster] - routerLogZ) +
               (fineScore[target - begin] - fineLogZ);
    }

    inline void stream_hierarchical_scores_batch(const BeamState *const states[MAX_BEAM],
                                                  const u8 *const hidden[MAX_BEAM], u32 beamCount,
                                                  double invT, double repetitionPenalty, u32 keepK,
                                                  StreamTop out[MAX_BEAM]) noexcept
    {
        // Different beams may visit different router clusters. Keep per-beam
        // decoder scratch private; a later micro-batch kernel can share router
        // weight loads without introducing cross-beam state races.
        for (u32 b = 0; b < beamCount; ++b)
            stream_hierarchical_scores(hidden[b], *states[b], invT,
                                       repetitionPenalty, keepK, out[b], true);
    }

    inline void run_beam(const QFeats &q, u32 B, BeamResult &result, double gumbel, double tScale,
                         const BeamState *initialRoot = nullptr, bool useThinkCache = false) noexcept
    {
        if (!ensure_scratch())
        {
            std::memset(&result, 0, sizeof(result));
            return;
        }
        B = B < 1 ? 1 : (B > MAX_BEAM ? MAX_BEAM : B);
        const double invT = 1.0 / ((double)calT() * tScale);
        const double rep = (double)o_RepPenalty / 100.0;
        const u32 N = g_model.vocabCount;
        feedback_prepare_query(q.key, N);
        BeamState *cur = g_bsA, *nxt = g_bsB;
        u32 nb = 1;
        g_nodeCount = 0;
        if (initialRoot)
            cur[0] = *initialRoot;
        else
        {
            std::memset(&cur[0], 0, sizeof(BeamState));
            acc_start(q, cur[0].accQ, cur[0].accS, cur[0].prefix);
            memory_read_initial(q, cur[0]);
            cur[0].node = NO_NODE;
            cur[0].y1 = cur[0].y2 = cur[0].y3 = BOS;
        }
        cur[0].lp = cur[0].lpp = 0.0;
        cur[0].node = NO_NODE;
        cur[0].len = 0;
        cur[0].done = false;
        float *const lg = g_clg;
        double z[MAXC]; // fast shortlist only; full-vocabulary path streams directly
        for (u32 step = 0; step < MAX_GEN_TOKENS; ++step)
        {
            u32 nc = 0;
            bool any = false;
            struct BeamPick { u32 tok; float raw; double logp; double noise; };
            struct PreparedBeam
            {
                u32 bi;
                u64 ttKey;
                bool full;
                u32 count;
                u8 hidden[LH];
                BeamPick picks[4 * MAX_BEAM];
                StreamTop streamed;
            };
            PreparedBeam prepared[MAX_BEAM]{};
            u32 preparedCount = 0;
            for (u32 bi = 0; bi < nb; ++bi)
            {
                BeamState &bs = cur[bi];
                if (bs.done)
                {
                    g_cand[nc++] = {bs.lp, bs.lpp, bi, EOS, bs.len, 0, 0, true, true};
                    continue;
                }
                any = true;
                ++g_nodesTotal;
                u8 h8[LH];
                tower_for_beam(q, bs, h8, useThinkCache);
                const float stateReadProbability = g_lastMemoryProbability;
                if (memory_read_for_state(q, bs, stateReadProbability))
                    tower_for_beam(q, bs, h8, useThinkCache);
                const u64 tk = ctx_key(q.key, bs.y1, bs.y2, bs.y3, bs.len);
                // Hierarchical routing supersedes the legacy TT-only FastPicker.
                // Always consume the trained router distribution for candidate scores.
                bool full = true;
                u32 n = 0;
                (void)tk;
                (void)o_Fast;
                PreparedBeam &p = prepared[preparedCount++];
                p.bi = bi;
                p.ttKey = tk;
                p.full = full;
                p.count = 0;
                if (full)
                {
                    std::memcpy(p.hidden, h8, LH);
                    continue;
                }

                // Shortlist mode is bounded by MAXC. Copy its candidates now because
                // the next beam reuses the global gather scratch.
                for (u32 i = 0; i < n; ++i)
                {
                    const u32 tok = g_cid[i];
                    z[i] = ((double)lg[i] + (double)g_feedbackQueryScore[tok] * (1.0 / 2048.0)) * invT;
                    if (bs.len == 0 && tok == EOS) z[i] -= 50.0;
                }
                u32 pickerWinner = 0;
                for (u32 i = 1; i < n; ++i) if (z[i] > z[pickerWinner]) pickerWinner = i;
                const u32 pickerStage = (u32)g_pickStage[pickerWinner];
                if (pickerStage < 4) ++g_stageWin[pickerStage];
                {
                    u32 hist[MAX_GEN_TOKENS], hn = 0;
                    for (u32 nd = bs.node; nd != NO_NODE; nd = g_nodes[nd].parent)
                        if (g_nodes[nd].tok != EOS) hist[hn++] = g_nodes[nd].tok;
                    std::sort(hist, hist + hn);
                    hn = (u32)(std::unique(hist, hist + hn) - hist);
                    for (u32 j = 0; j < hn; ++j)
                        for (u32 i = 0; i < n; ++i) if (g_cid[i] == hist[j]) { z[i] -= rep * invT; break; }
                    if (bs.len && bs.y1 != BOS && bs.y1 != EOS)
                        for (u32 i = 0; i < n; ++i) if (g_cid[i] == bs.y1) { z[i] -= 12.0 * invT; break; }
                }
                double mx = z[0];
                for (u32 i = 1; i < n; ++i) mx = z[i] > mx ? z[i] : mx;
                double Z = 0.0;
                for (u32 i = 0; i < n; ++i) { const double d = z[i] - mx; if (d > -60.0) Z += std::exp(d); }
                const double logZ = mx + std::log(Z);
                const u32 MK = gumbel > 0 ? std::min<u32>(n, 4 * B) : std::min<u32>(n, B);
                u32 top[4 * MAX_BEAM], count = 0;
                for (u32 i = 0; i < n; ++i)
                {
                    u32 at = count;
                    if (count < MK) ++count;
                    else if (z[i] <= z[top[count - 1]]) continue;
                    else at = count - 1;
                    while (at > 0 && z[top[at - 1]] < z[i]) { top[at] = top[at - 1]; --at; }
                    top[at] = i;
                }
                p.count = count;
                for (u32 k = 0; k < count; ++k)
                {
                    const u32 i = top[k];
                    p.picks[k] = {g_cid[i], lg[i], z[i] - logZ, 0.0};
                }
            }

            // Score all full-vocabulary beams together. This reuses each output
            // weight row across every live beam and parallelizes independent tiles.
            const BeamState *batchStates[MAX_BEAM]{};
            const u8 *batchHidden[MAX_BEAM]{};
            u32 batchPreparedIndex[MAX_BEAM]{};
            u32 batchCount = 0;
            for (u32 pi = 0; pi < preparedCount; ++pi)
                if (prepared[pi].full)
                {
                    batchStates[batchCount] = &cur[prepared[pi].bi];
                    batchHidden[batchCount] = prepared[pi].hidden;
                    batchPreparedIndex[batchCount] = pi;
                    ++batchCount;
                }
            if (batchCount)
            {
                StreamTop batchTop[MAX_BEAM]{};
                const u32 want = gumbel > 0 ? 4 * B : B;
                stream_hierarchical_scores_batch(batchStates, batchHidden, batchCount, invT, rep, want, batchTop);
                for (u32 b = 0; b < batchCount; ++b)
                {
                    PreparedBeam &p = prepared[batchPreparedIndex[b]];
                    p.streamed = batchTop[b];
                    p.count = batchTop[b].count;
                    for (u32 k = 0; k < p.count; ++k)
                    {
                        p.picks[k] = {batchTop[b].item[k].tok, batchTop[b].item[k].raw,
                                      batchTop[b].item[k].score - batchTop[b].logZ, 0.0};
                    }
                    ++g_stageWin[3];
                }
            }

            // Preserve deterministic RNG order: apply Gumbel and emit candidates
            // in original beam order only after all batched scores are ready.
            for (u32 pi = 0; pi < preparedCount; ++pi)
            {
                PreparedBeam &p = prepared[pi];
                u32 tn = p.count;
                if (!tn) continue;
                u32 top3[3];
                const u32 k3 = std::min<u32>(3, tn);
                for (u32 k = 0; k < k3; ++k) top3[k] = p.picks[k].tok;
                tt_store(p.ttKey, top3, k3);
                if (gumbel > 0)
                {
                    for (u32 k = 0; k < tn; ++k)
                    {
                        const double u = ((double)(splitmix64(g_trng) >> 11) + 0.5) * (1.0 / 9007199254740992.0);
                        p.picks[k].noise = gumbel * -std::log(-std::log(u));
                    }
                    std::sort(p.picks, p.picks + tn, [](const BeamPick &a, const BeamPick &b) noexcept
                    { return a.logp + a.noise > b.logp + b.noise; });
                    tn = std::min<u32>(tn, B);
                }
                const BeamState &bs = cur[p.bi];
                for (u32 k = 0; k < tn; ++k)
                {
                    const u32 tok = p.picks[k].tok, len = bs.len + 1;
                    const i32 sc = (i32)(p.picks[k].raw * 256.f);
                    const i32 margin = (k + 1 < tn) ? (i32)((p.picks[k].raw - p.picks[k + 1].raw) * 256.f) : sc;
                    g_cand[nc++] = {bs.lp + p.picks[k].logp,
                                    bs.lpp + p.picks[k].logp + p.picks[k].noise,
                                    p.bi, tok, len, sc, margin,
                                    (tok == EOS) || (len >= MAX_GEN_TOKENS), false};
                }
            }
            if (!any)
                break;
            const u32 keep = nc < B ? nc : B;
            std::partial_sort(g_cand, g_cand + keep, g_cand + nc, cand_better);
            for (u32 i = 0; i < keep; ++i)
            {
                const BeamCand &c = g_cand[i];
                const BeamState &s = cur[c.src];
                BeamState &d = nxt[i];
                if (c.carry)
                {
                    d.lp = s.lp;
                    d.lpp = s.lpp;
                    d.node = s.node;
                    d.len = s.len;
                    d.y1 = s.y1;
                    d.y2 = s.y2;
                    d.y3 = s.y3;
                    d.prefix = s.prefix;
                    std::memcpy(d.memReadIds, s.memReadIds, sizeof(d.memReadIds));
                    d.lastMemoryQueryKey = s.lastMemoryQueryKey;
                    d.memReadCount = s.memReadCount;
                    d.memReads = s.memReads;
                    d.done = true;
                }
                else
                {
                    std::memcpy(d.accQ, s.accQ, sizeof(d.accQ));
                    std::memcpy(d.accS, s.accS, sizeof(d.accS));
                    g_nodes[g_nodeCount] = {s.node, c.tok, c.score, c.margin};
                    d.node = g_nodeCount++;
                    d.lp = c.lp;
                    d.lpp = c.lpp;
                    d.len = c.len;
                    d.y3 = s.y2;
                    d.y2 = s.y1;
                    d.y1 = c.tok;
                    d.done = c.done;
                    d.prefix = s.prefix;
                    std::memcpy(d.memReadIds, s.memReadIds, sizeof(d.memReadIds));
                    d.lastMemoryQueryKey = s.lastMemoryQueryKey;
                    d.memReadCount = s.memReadCount;
                    d.memReads = s.memReads;
                    if (!c.done)
                        acc_advance(d.accS, q, s.y1, s.y2, s.y3, s.len, d.prefix, c.tok); // chỉ B nhánh sống sót mới tốn cập nhật gia tăng
                }
            }
            std::swap(cur, nxt);
            nb = keep;
            bool allDone = true;
            for (u32 i = 0; i < nb; ++i)
                allDone = allDone && cur[i].done;
            if (allDone)
                break;
        }
        auto ends_eos = [&](const BeamState &b) noexcept
        { return b.done && b.len > 0 && g_nodes[b.node].tok == EOS; };
        u32 best = 0;
        for (u32 i = 1; i < nb; ++i)
        {
            const bool bi = ends_eos(cur[i]), bb = ends_eos(cur[best]);
            if ((bi && !bb) || (bi == bb && cur[i].lp > cur[best].lp))
                best = i;
        }
        const BeamState &b = cur[best];
        result.lp = b.lp;
        result.len = b.len;
        result.done = b.done;
        u32 nd = b.node;
        for (u32 i = b.len; i-- > 0;)
        {
            const BeamNode &n = g_nodes[nd];
            result.tok[i] = n.tok;
            result.tr[i] = {(i32)n.tok, n.score, n.margin};
            nd = n.parent;
        }
    }

    inline u32 write_output(const BeamResult &b) noexcept
    {
        g_traceCount = b.len;
        for (u32 i = 0; i < b.len; ++i)
            g_trace[i] = b.tr[i];
        g_lastN = 0;
        u32 out = 0;
        bool previousAttachesRight = false;
        bool singleQuoteOpen = false, doubleQuoteOpen = false;
        u32 backslashRun = 0;
        for (u32 i = 0; i < b.len; ++i)
        {
            const u32 tok = b.tok[i];
            if (tok == EOS || tok >= g_model.vocabCount) break;
            const u32 wl = g_len[tok];
            const char *word = g_arena + g_off[tok];
            const bool onePunct = wl == 1 && is_sep(word[0]) && (u8)word[0] > 0x20;
            bool attachLeft = false, attachRight = false;
            if (onePunct)
            {
                const char c = word[0];
                switch (c)
                {
                    case '{': case '[': case '(': attachRight = true; break;
                    case '}': case ']': case ')': case ',': case ';': case '?': case '!': case '%': attachLeft = true; break;
                    case ':': attachLeft = true; attachRight = true; break;
                    case '.':
                        attachLeft = true;
                        // Keep decimal numbers and common media-file extensions contiguous.
                        if (i + 1 < b.len && b.tok[i + 1] < g_model.vocabCount)
                        {
                            const u32 pn = b.tok[i + 1], pl = g_len[pn];
                            const char *next = g_arena + g_off[pn];
                            const bool nextDigit = pl == 1 && next[0] >= '0' && next[0] <= '9';
                            const bool ext = (pl == 3 && (!std::memcmp(next,"png",3) || !std::memcmp(next,"jpg",3) || !std::memcmp(next,"mp4",3) || !std::memcmp(next,"wav",3) || !std::memcmp(next,"mov",3) || !std::memcmp(next,"mp3",3) || !std::memcmp(next,"gif",3))) ||
                                             (pl == 4 && (!std::memcmp(next,"jpeg",4) || !std::memcmp(next,"webp",4) || !std::memcmp(next,"json",4) || !std::memcmp(next,"webm",4) || !std::memcmp(next,"avif",4) || !std::memcmp(next,"svgz",4)));
                            if (nextDigit || ext) attachRight = true;
                        }
                        break;
                    case '"':
                        if (singleQuoteOpen || (backslashRun & 1u)) { attachLeft = true; attachRight = true; }
                        else { attachLeft = doubleQuoteOpen; attachRight = !doubleQuoteOpen; doubleQuoteOpen = !doubleQuoteOpen; }
                        break;
                    case '\'':
                        if (doubleQuoteOpen || (backslashRun & 1u)) { attachLeft = true; attachRight = true; }
                        else { attachLeft = singleQuoteOpen; attachRight = !singleQuoteOpen; singleQuoteOpen = !singleQuoteOpen; }
                        break;
                    case '-': case '/': case '\\': case '=': case '|': attachLeft = true; attachRight = true; break;
                    default: break;
                }
            }
            const bool needSpace = out > 0 && !attachLeft && !previousAttachesRight;
            if (out + (needSpace ? 1u : 0u) + wl + 1u >= IO_BUFFER_SIZE) break;
            if (needSpace) g_out[out++] = ' ';
            std::memcpy(&g_out[out], word, wl);
            out += wl;
            g_lastTok[g_lastN++] = tok;
            previousAttachesRight = attachRight;
            if (wl == 1 && word[0] == '\\') ++backslashRun;
            else backslashRun = 0;
        }
        g_out[out] = '\0';
        return out;
    }
    inline bool can_infer() noexcept { return g_model.vocabCount > 1 && g_emb && g_hw && g_hb && g_routerW && g_routerB && g_routerCap >= router_cluster_count(g_model.vocabCount); }

    static const char FALLBACK_TEXT[] = "Mô hình chưa thể tạo câu trả lời. Hãy kiểm tra và nạp lại mô hình.";
    inline u32 write_fallback() noexcept
    {
        g_traceCount = 0;
        g_lastN = 0;
        const u32 n = (u32)(sizeof(FALLBACK_TEXT) - 1);
        std::memcpy(g_out, FALLBACK_TEXT, n + 1);
        return n;
    }
    inline bool degenerate(const u64 *hs, u32 n) noexcept
    {
        u32 run = 1, uniq = 0;
        for (u32 i = 0; i < n; ++i)
        {
            if (i && hs[i] == hs[i - 1] && ++run >= 3)
                return true;
            if (i && hs[i] != hs[i - 1])
                run = 1;
            bool seen = false;
            for (u32 j = 0; j < i && !seen; ++j)
                seen = hs[j] == hs[i];
            uniq += !seen;
        }
        return n >= 8 && uniq * 2 < n;
    }
    inline bool answer_ok(const BeamResult &b, u32 outLen) noexcept
    {
        if (outLen == 0 || b.len == 0)
            return false;
        u64 hs[MAX_GEN_TOKENS];
        u32 n = 0;
        for (u32 i = 0; i < b.len; ++i)
        {
            const u32 t = b.tok[i];
            if (t == EOS || t >= g_model.vocabCount)
                break;
            hs[n++] = g_hash[t];
        }
        if (n == 0 || degenerate(hs, n))
            return false;
        if (o_MinLogProb < 1000 && b.lp / (double)b.len < -(double)o_MinLogProb / 100.0)
            return false;
        return true;
    }
} // namespace nnuecut

WASM_EXPORT void wasm_bit_llm_set_search_seed(u32 seed) { nnuecut::set_search_seed(seed); }

WASM_EXPORT u32 wasm_bit_llm_infer(u32 inputLen)
{
    if (inputLen >= IO_BUFFER_SIZE)
        inputLen = IO_BUFFER_SIZE - 1;
    g_in[inputLen] = '\0';
    nnuecut::g_lastInputLen = inputLen;
    nnuecut::g_reasonStepSum = nnuecut::g_reasonCallCount = nnuecut::g_memoryReadCount = 0;
    nnuecut::g_answerSource = 0;
    g_ttgen = (u8)((g_ttgen + 1) & 3u);
    g_lastFeedbackCtx = 0;
    static TokInfo ti[MAX_Q];
    const u32 n = tokenize(g_in, inputLen, ti, MAX_Q, false);
    if (n == 0)
    {
        g_traceCount = 0;
        g_out[0] = '\0';
        return 0;
    }
    static QFeats q;
    build_qfeats(ti, n, q);
    g_lastFeedbackCtx = q.key;
    if (!can_infer())
    {
        g_answerSource = 3;
        return write_fallback();
    }
    static BeamResult b;
    const double nov = novelty_of(q), cre = (double)o_Creativity / 100.0;
    g_lastNovelty = (u32)(nov * 1000.0 + 0.5);
    run_beam(q, (u32)o_Beam, b, std::min(1.5, 0.6 * cre * nov), 1.0 + 0.5 * cre * nov);
    const u32 out = write_output(b);
    if (out == 0)
    {
        g_answerSource = 3;
        return write_fallback();
    }
    g_answerSource = 2;
    return out;
}

// explain: in nguồn trả lời + từng bước greedy với top-5 token (logit) và tầng TokenPicker. Ghi vào g_out.
WASM_EXPORT u32 wasm_bit_llm_explain(u32 inputLen)
{
    if (inputLen >= IO_BUFFER_SIZE)
    {
        inputLen = IO_BUFFER_SIZE - 1;
    }
    g_in[inputLen] = '\0';
    static TokInfo ti[MAX_Q];
    static QFeats q;
    u32 o = 0;
    auto P = [&](const char *f, auto... a)
    {
        if (o + 200 < IO_BUFFER_SIZE)
        {
            if constexpr (sizeof...(a) == 0)
            {
                o += (u32)std::snprintf(g_out + o, IO_BUFFER_SIZE - o, "%s", f);
            }
            else
            {
                o += (u32)std::snprintf(g_out + o, IO_BUFFER_SIZE - o, f, a...);
            }
        }
    };
    const u32 n = tokenize(g_in, inputLen, ti, MAX_Q, false);
    if (!can_infer() || n == 0)
    {
        P("khong the sinh\n");
        g_out[o] = 0;
        return o;
    }
    build_qfeats(ti, n, q);
    P("tu biet %u/%u, novelty=%.2f, T=%.2f\n", q.known, q.total, novelty_of(q), calT());
    P("adaptive latent reasoning: learned halt policy, budget=%u (min=%u, max=%u)\n", g_reasonBudget, REASON_MIN_STEPS, REASON_MAX_STEPS);
    ensure_scratch();
    BeamState s{};
    acc_start(q, s.accQ, s.accS, s.prefix);
    s.y1 = s.y2 = s.y3 = BOS;
    const u32 N = g_model.vocabCount;
    for (u32 step = 0; step < 24; ++step)
    {
        u8 h8[LH];
        tower(s.accQ, s.accS, h8, s.len);
        StreamTop top{};
        stream_hierarchical_scores(h8, s, 1.0, 0.0, 5, top, false);
        P("t=%u ROUTER:", step);
        for (u32 k = 0; k < top.count; ++k)
        {
            const u32 t = top.item[k].tok;
            P(" [%.*s logP=%.4f P=%.3f%%]", t == EOS ? 5 : (int)g_len[t],
              t == EOS ? "<eos>" : g_arena + g_off[t], top.item[k].score,
              100.0 * std::exp(top.item[k].score));
        }
        P("\n");
        if (!top.count) break;
        const u32 tok = top.item[0].tok;
        if (tok == EOS || tok >= N)
            break;
        acc_advance(s.accS, q, s.y1, s.y2, s.y3, s.len, s.prefix, tok);
        s.y3 = s.y2;
        s.y2 = s.y1;
        s.y1 = tok;
        ++s.len;
    }
    g_out[o] = 0;
    return o;
}

// ===================== THINK MODE (Gumbel-top-k, bỏ phiếu); dừng theo khoảng tin cậy Wilson HOẶC theo ngân sách NÚT (tất định) =====================
namespace nnuecut
{
    constexpr u32 THINK_POOL = 64;
    struct ThinkCand
    {
        BeamResult rep;
        u32 votes;
        double meanSum;
        u64 key;
    };
    static ThinkCand g_pool[THINK_POOL];
    static u32 g_poolN = 0, g_views = 0, g_tn = 0, g_leaderVotes = 0, g_thinkMode = 0;
    static TokInfo g_tti[MAX_Q];
    static QFeats g_tq;
    static u64 g_thinkNodes0 = 0;
    static u64 beam_key(const BeamResult &b) noexcept
    {
        u64 k = 0x1234567ULL;
        for (u32 i = 0; i < b.len; ++i)
            k = mix64(k ^ b.tok[i]);
        return k;
    }
    static double wilson(double k, double n, double sign) noexcept
    {
        const double z = 1.959964, p = k / n, z2 = z * z;
        return (p + z2 / (2 * n) + sign * z * std::sqrt(p * (1 - p) / n + z2 / (4 * n * n))) / (1 + z2 / n);
    }
} // namespace nnuecut

WASM_EXPORT u32 wasm_bit_llm_think_begin(u32 inputLen)
{
    if (inputLen >= IO_BUFFER_SIZE)
        inputLen = IO_BUFFER_SIZE - 1;
    g_in[inputLen] = '\0';
    nnuecut::g_lastInputLen = inputLen;
    nnuecut::g_reasonStepSum = nnuecut::g_reasonCallCount = nnuecut::g_memoryReadCount = 0;
    g_poolN = g_views = g_leaderVotes = 0;
    g_thinkMode = 0;
    g_answerSource = 0;
    g_thinkNodes0 = g_nodesTotal;
    g_ttgen = (u8)((g_ttgen + 1) & 3u);
    g_lastFeedbackCtx = 0;
    g_tn = tokenize(g_in, inputLen, g_tti, MAX_Q, false);
    for (auto &entry : g_thinkLatentCache) entry = ThinkLatentEntry{};
    g_thinkCacheHits = g_thinkCacheMisses = 0;
    g_thinkRootReady = false;
    if (g_tn > 0)
    {
        build_qfeats(g_tti, g_tn, g_tq);
        g_lastFeedbackCtx = g_tq.key;
        if (!can_infer())
        {
            g_thinkMode = 3;
            g_answerSource = 3;
        }
        else
        {
            std::memset(&g_thinkRoot, 0, sizeof(g_thinkRoot));
            acc_start(g_tq, g_thinkRoot.accQ, g_thinkRoot.accS, g_thinkRoot.prefix);
            memory_read_initial(g_tq, g_thinkRoot); // single retrieval per Think session
            g_thinkRoot.node = NO_NODE;
            g_thinkRoot.y1 = g_thinkRoot.y2 = g_thinkRoot.y3 = BOS;
            g_thinkRoot.lp = g_thinkRoot.lpp = 0.0;
            g_thinkRootReady = true;
        }
    }
    return g_tn;
}
WASM_EXPORT u32 wasm_bit_llm_think_step()
{
    if (g_tn == 0)
        return 0;
    if (g_thinkMode != 0)
    {
        g_views = 1;
        g_leaderVotes = 1;
        return 1;
    }
    const u32 v = g_views++;
    static BeamResult b;
    const double nov = novelty_of(g_tq), cre = (double)o_Creativity / 100.0;
    g_lastNovelty = (u32)(nov * 1000.0 + 0.5);
    const double gs = std::min(1.5, (0.4 + 0.6 * nov) * (0.5 + 0.5 * cre));
    run_beam(g_tq, (u32)o_Beam, b, v == 0 ? 0.0 : gs,
             1.0 + 0.5 * cre * nov, g_thinkRootReady ? &g_thinkRoot : nullptr, true);
    const u64 key = beam_key(b);
    const double mean = b.len ? b.lp / (double)b.len : 0.0;
    bool merged = false;
    for (u32 i = 0; i < g_poolN && !merged; ++i)
        if (g_pool[i].key == key)
        {
            ++g_pool[i].votes;
            g_pool[i].meanSum += mean;
            if (b.lp > g_pool[i].rep.lp)
                g_pool[i].rep = b;
            merged = true;
        }
    if (!merged && g_poolN < THINK_POOL)
    {
        ThinkCand &c = g_pool[g_poolN++];
        c.rep = b;
        c.votes = 1;
        c.meanSum = mean;
        c.key = key;
    }
    g_leaderVotes = 0;
    for (u32 i = 0; i < g_poolN; ++i)
        if (g_pool[i].votes > g_leaderVotes)
            g_leaderVotes = g_pool[i].votes;
    return g_views;
}
WASM_EXPORT u32 wasm_bit_llm_think_leader_votes() { return g_leaderVotes; }
WASM_EXPORT u32 wasm_bit_llm_think_views() { return g_views; }
WASM_EXPORT u32 wasm_bit_llm_think_cache_hits() { return (u32)g_thinkCacheHits; }
WASM_EXPORT u32 wasm_bit_llm_think_cache_misses() { return (u32)g_thinkCacheMisses; }
WASM_EXPORT u32 wasm_bit_llm_get_output_tokens_scanned() { return (u32)g_outputTokenScores; }
WASM_EXPORT u32 wasm_bit_llm_get_output_head_macs_low() { return (u32)g_outputHeadMacs; }
WASM_EXPORT u32 wasm_bit_llm_get_output_head_macs_high() { return (u32)(g_outputHeadMacs >> 32); }
WASM_EXPORT u32 wasm_bit_llm_get_output_router_macs_low() { return (u32)g_outputRouterMacs; }
WASM_EXPORT u32 wasm_bit_llm_get_output_router_macs_high() { return (u32)(g_outputRouterMacs >> 32); }
WASM_EXPORT u32 wasm_bit_llm_get_output_clusters_visited() { return (u32)g_outputClustersVisited; }
WASM_EXPORT u32 wasm_bit_llm_get_router_cluster_count() { return router_cluster_count(g_model.vocabCount); }
WASM_EXPORT void wasm_bit_llm_reset_perf_counters() { g_outputTokenScores = g_outputHeadMacs = g_outputRouterMacs = g_outputClustersVisited = 0; }
WASM_EXPORT u32 wasm_bit_llm_think_nodes() { return (u32)(g_nodesTotal - g_thinkNodes0); }
WASM_EXPORT u32 wasm_bit_llm_think_confident()
{
    if (g_thinkMode != 0)
        return g_views >= 1 ? 1 : 0;
    if (o_Nodes > 0 && g_nodesTotal - g_thinkNodes0 >= (u64)o_Nodes)
        return 1; // ngân sách theo số nút: tái lập được giữa mọi máy
    if (g_views < 4 || g_poolN == 0)
        return 0;
    u32 v1 = 0, v2 = 0;
    for (u32 i = 0; i < g_poolN; ++i)
    {
        const u32 v = g_pool[i].votes;
        if (v > v1)
        {
            v2 = v1;
            v1 = v;
        }
        else if (v > v2)
            v2 = v;
    }
    return wilson(v1, g_views, -1.0) > wilson(v2, g_views, +1.0) ? 1 : 0;
}
WASM_EXPORT u32 wasm_bit_llm_think_finish()
{
    if (g_thinkMode == 3 || !g_poolN)
    {
        g_answerSource = 3;
        return write_fallback();
    }
    u32 best = 0;
    for (u32 i = 1; i < g_poolN; ++i)
    {
        const double a = g_pool[i].meanSum / (double)g_pool[i].votes, c = g_pool[best].meanSum / (double)g_pool[best].votes;
        if (g_pool[i].votes > g_pool[best].votes || (g_pool[i].votes == g_pool[best].votes && a > c))
            best = i;
    }
    g_stat[7] = (i32)g_pool[best].votes;
    const u32 out = write_output(g_pool[best].rep);
    if (out == 0)
    {
        g_answerSource = 3;
        return write_fallback();
    }
    g_answerSource = 2;
    return out;
}


// ===================== KIỂM THỬ NỘI BỘ: gia tăng == tính lại từ đầu; neural SIMD =====================
// Trả 1 nếu accumulator gia tăng khớp bit-chính-xác với tính lại từ đầu trên chuỗi token cho trước (g_in = câu hỏi).
WASM_EXPORT u32 wasm_bit_llm_selftest_acc(u32 inputLen)
{
    if (inputLen >= IO_BUFFER_SIZE)
        inputLen = IO_BUFFER_SIZE - 1;
    g_in[inputLen] = 0;
    static TokInfo ti[MAX_Q];
    static QFeats q;
    const u32 n = tokenize(g_in, inputLen, ti, MAX_Q, false);
    build_qfeats(ti, n, q);
    i32 incQ[L1], incS[L1], refQ[L1], refS[L1];
    u64 prefix;
    acc_start(q, incQ, incS, prefix);
    u32 y1 = BOS, y2 = BOS, y3 = BOS, len = 0;
    u32 sofar[16], ns = 0;
    const u32 N = g_model.vocabCount ? g_model.vocabCount : 2;
    u64 r = 77;
    for (u32 step = 0; step < 12; ++step)
    {
        const u32 tok = 1 + (u32)(splitmix64(r) % (N > 1 ? N - 1 : 1));
        acc_advance(incS, q, y1, y2, y3, len, prefix, tok);
        sofar[ns++] = tok;
        y3 = y2;
        y2 = y1;
        y1 = tok;
        ++len;
        acc_init(q, refQ);
        std::memset(refS, 0, sizeof(refS));
        u32 f[MAX_STEP_FEATS];
        acc_apply<1>(refS, f, step_feats(q, y1, y2, y3, len, prefix, f));
        for (u32 k = 0; k < ns; ++k)
        {
            const u32 sf = sofar_feat(sofar[k]);
            acc_apply<1>(refS, &sf, 1);
        }
        if (std::memcmp(incQ, refQ, sizeof(incQ)) != 0 || std::memcmp(incS, refS, sizeof(incS)) != 0)
            return 0;
    }
    return 1;
}
WASM_EXPORT u32 wasm_bit_llm_selftest_simd()
{
    i8 w[L1];
    u8 a[L1];
    i16 row[L1];
    i32 acc[L1], refAcc[L1];
    for (u32 i = 0; i < L1; ++i)
    {
        w[i] = (i8)((i32)((i * 73u + 19u) % 255u) - 127);
        a[i] = (u8)((i * 29u + 7u) % 128u);
        row[i] = (i16)((i32)((i * 251u + 11u) % 65536u) - 32768);
        acc[i] = refAcc[i] = (i32)i * 31 - 2000;
    }
    row_add(acc, row);
    for (u32 i = 0; i < L1; ++i)
        refAcc[i] += row[i];
    if (std::memcmp(acc, refAcc, sizeof(acc)) != 0)
        return 0;
    row_sub(acc, row);
    for (u32 i = 0; i < L1; ++i)
        refAcc[i] -= row[i];
    if (std::memcmp(acc, refAcc, sizeof(acc)) != 0)
        return 0;
    i32 refH1 = 0, refHalf = 0, refLH = 0, refL1 = 0;
    for (u32 i = 0; i < L1; ++i)
    {
        const i32 p = (i32)w[i] * a[i];
        refL1 += p;
        if (i < H1)
            refH1 += p;
        if (i < HALF)
            refHalf += p;
        if (i < LH)
            refLH += p;
    }
    if (dot_i8_u8<H1>(w, a) != refH1 || dot_i8_u8<HALF>(w, a) != refHalf ||
        dot_i8_u8<LH>(w, a) != refLH || dot_i8_u8<L1>(w, a) != refL1 ||
        dot_i8_u8<H1, false>(w, a) != dot_i8_u8<H1>(w, a) ||
        dot_i8_u8<HALF, false>(w, a) != dot_i8_u8<HALF>(w, a) ||
        dot_i8_u8<LH, false>(w, a) != dot_i8_u8<LH>(w, a) ||
        dot_i8_u8<L1, false>(w, a) != dot_i8_u8<L1>(w, a))
        return 0;
    return 1;
}
WASM_EXPORT u32 wasm_bit_llm_selftest_text()
{
    u64 h1, h2, h3, h4;
    char a[32], b[32];
    const char precomposed[] = "\xC3\xA1"; // á
    const char decomposed[] = "a\xCC\x81"; // a + combining acute
    word_hashes(precomposed, sizeof(precomposed) - 1, h1, h2, a);
    word_hashes(decomposed, sizeof(decomposed) - 1, h3, h4, b);
    if (h2 != h4 || h1 == h3)
        return 0;
    const char malformed[] = "\xC0\xAF"; // overlong UTF-8 must remain two bytes
    if (word_hashes(malformed, sizeof(malformed) - 1, h1, h2, a) != 2)
        return 0;
    return 1;
}
// Benchmark hook: callers measure wall time externally and compare checksums
// across builds. Work is bounded to keep accidental UI calls responsive.
WASM_EXPORT u32 wasm_bit_llm_bench_output(u32 rows, u32 rounds)
{
    rows = rows < 1 ? 1 : (rows > 65536 ? 65536 : rows);
    rounds = rounds < 1 ? 1 : (rounds > 512 ? 512 : rounds);
    const u64 maxDots = 400000;
    if ((u64)rows * rounds > maxDots)
        rounds = (u32)std::max<u64>(1, maxDots / rows);
    std::vector<i8> weights((size_t)rows * LH);
    u8 h[LH];
    for (u32 j = 0; j < LH; ++j)
        h[j] = (u8)((j * 31u + 5u) & 127u);
    for (size_t i = 0; i < weights.size(); ++i)
        weights[i] = (i8)((i32)((i * 17u + 3u) % 255u) - 127);
    u32 checksum = 0;
    for (u32 r = 0; r < rounds; ++r)
        for (u32 t = 0; t < rows; ++t)
            checksum += (u32)dot_i8_u8<LH>(weights.data() + (size_t)t * LH, h);
    return checksum;
}

// ===================== NẠP / XUẤT MÔ HÌNH (streaming, FLAT) =====================
WASM_EXPORT u32 wasm_bit_llm_begin_load()
{
    reset_all();
    return HEADER_SIZE;
}
WASM_EXPORT u32 wasm_bit_llm_load_chunk_direct(u32 offset, u32 size)
{
    if (size == 0 || size > CHUNK_BUFFER_SIZE || (u64)offset != g_loaded)
        return 0;
    const u8 *src = g_chunk_buffer;
    u32 left = size;
    if (g_loaded < HEADER_SIZE)
    {
        const u32 n = (u32)((HEADER_SIZE - g_loaded) < left ? (HEADER_SIZE - g_loaded) : left);
        std::memcpy((u8 *)&g_model + g_loaded, src, n);
        g_loaded += n;
        src += n;
        left -= n;
        if (g_loaded == HEADER_SIZE)
        {
            if (!model_header_valid())
                return 0; // header sai cấu trúc, sai kiến trúc hoặc chứa giá trị ngoài giới hạn
            const u32 N = g_model.vocabCount;
            compute_layout(N, g_model.arenaBytes, g_lay);
            const u32 cap = kTrain ? (N > (u32)TRAIN_VOCAB_CAP ? N : (u32)TRAIN_VOCAB_CAP) : N;
            if (kTrain && N > (u32)TRAIN_VOCAB_CAP)
                return 0;
            if (!meta_reserve(cap) || !arena_reserve(g_model.arenaBytes + 1) || !net_alloc(cap))
                return 0;
            g_loadTarget = g_lay.total;
            g_headerOk = true;
        }
    }
    while (left && g_loaded < g_loadTarget)
    {
        if (!g_headerOk)
            return 0;
        const u32 i = g_lay.section_at(g_loaded);
        const u64 rel = g_loaded - g_lay.off[i];
        u64 n = g_lay.off[i + 1] - g_loaded;
        if (n > left)
            n = left;
        if (n > g_loadTarget - g_loaded)
            n = g_loadTarget - g_loaded;
        if (rel < g_lay.bytes[i])
        {
            u64 c = g_lay.bytes[i] - rel;
            if (c > n)
                c = n;
            u8 *d = section_ptr(i);
            if (!d)
                return 0;
            std::memcpy(d + rel, src, (size_t)c);
        }
        g_loaded += n;
        src += n;
        left -= (u32)n;
    }
    return left == 0 ? 1 : 0;
}
WASM_EXPORT u32 wasm_bit_llm_validate_model()
{
    if (!model_header_valid() || !g_emb || !g_hw || !g_hb || !g_routerW || !g_routerB || !g_hash || !g_df || !g_len || !g_arena ||
        g_metaCap < g_model.vocabCount || g_netCap < g_model.vocabCount || g_routerCap < router_cluster_count(g_model.vocabCount))
    {
        wasm_bit_llm_init();
        return 0;
    }
    // The network CRC intentionally covers only inference weights. Validate the
    // token metadata separately before rebuilding indexes that trust offsets,
    // lengths, and hashes from the model file.
    u64 arenaBytes = 0;
    char low[MAX_TOKEN_LEN * 4 + 1];
    for (u32 id = 0; id < g_model.vocabCount; ++id)
    {
        const u32 len = g_len[id];
        if (len > MAX_TOKEN_LEN || arenaBytes + len > g_model.arenaBytes || g_df[id] > g_model.numDocs ||
            (id == 0 && (len != 5 || std::memcmp(g_arena + arenaBytes, "<eos>", 5) != 0)))
        {
            wasm_bit_llm_init();
            return 0;
        }
        u64 h, h2;
        const u32 lowLen = word_hashes(g_arena + arenaBytes, len, h, h2, low);
        if (lowLen != len || h != g_hash[id])
        {
            wasm_bit_llm_init();
            return 0;
        }
        arenaBytes += len;
    }
    if (arenaBytes != g_model.arenaBytes || !rebuild_runtime())
    {
        wasm_bit_llm_init();
        return 0;
    }
    g_pairs = 0;
    return 1;
}
WASM_EXPORT u32 wasm_bit_llm_finalize_load()
{
    if (!g_headerOk || g_loaded != g_loadTarget)
    {
        wasm_bit_llm_init();
        return 0;
    }
    g_arenaUsed = g_model.arenaBytes;
    g_descLen = g_model.descBytes;
    if (net_crc() != g_model.netCrc)
    {
        wasm_bit_llm_init();
        return 0;
    } // CRC32 vùng mạng: bắt tệp hỏng/cắt cụt
    init_runtime_state();
    const u32 ok = wasm_bit_llm_validate_model();
#ifdef BIT_LLM_TRAIN
    if (ok)
    {
        dequantize_to_masters();
        refresh_quant();
    }
#endif
    return ok;
}
WASM_EXPORT u32 wasm_bit_llm_export_begin()
{
    if (!g_emb || !g_hw || !g_hb ||
        !g_routerW || !g_routerB ||
        !g_hash || !g_df || !g_len || !g_arena)
        return 0;

#ifdef BIT_LLM_TRAIN
    quantize_masters();
#endif

    const u32 N = g_model.vocabCount;
    g_model.magic = MODEL_MAGIC;
    g_model.version = MODEL_VERSION;

    g_model.l1 = L1;
    g_model.featBits = FEAT_BITS;
    g_model.lh = LH;
    g_model.h1 = H1;
    g_model.half = HALF;
    g_model.qe = QE;
    g_model.ws = WS;
    g_model.kwCount = KW_COUNT;

    g_model.formatFlags = 0;
    g_model.formatReserved = 0;
    if (N == 0 || N > MAX_VOCAB)
        return 0;

    // Ghi lại đầy đủ thông tin kiến trúc vào header.
    g_model.magic = MODEL_MAGIC;
    g_model.version = MODEL_VERSION;
    g_model.vocabCount = N;

    g_model.l1 = L1;
    g_model.featBits = FEAT_BITS;
    g_model.lh = LH;
    g_model.h1 = H1;
    g_model.half = HALF;
    g_model.qe = QE;
    g_model.ws = WS;
    g_model.kwCount = KW_COUNT;

    g_model.arenaBytes = g_arenaUsed;
    g_model.descBytes = g_descLen;

    g_model.archHash = ARCH_HASH;
    g_model.netCrc = net_crc();

    g_model.formatFlags = 0;
    g_model.formatReserved = 0;

    compute_layout(N, g_model.arenaBytes, g_lay);

    if (g_lay.total == 0 ||
        g_lay.total > 0xFFFFFFFFULL)
        return 0;

    return (u32)g_lay.total;
}
WASM_EXPORT u32 wasm_bit_llm_export_read(u32 offset, u32 size)
{
    if (size > CHUNK_BUFFER_SIZE)
        size = CHUNK_BUFFER_SIZE;
    if ((u64)offset >= g_lay.total)
        return 0;
    if ((u64)offset + size > g_lay.total)
        size = (u32)(g_lay.total - offset);
    std::memset(g_chunk_buffer, 0, size);
    u64 pos = offset;
    const u64 end = (u64)offset + size;
    while (pos < end)
    {
        const u32 i = g_lay.section_at(pos);
        const u64 rel = pos - g_lay.off[i];
        u64 n = g_lay.off[i + 1] - pos;
        if (n > end - pos)
            n = end - pos;
        if (rel < g_lay.bytes[i])
        {
            u64 c = g_lay.bytes[i] - rel;
            if (c > n)
                c = n;
            const u8 *s = section_ptr(i);
            if (s)
                std::memcpy(g_chunk_buffer + (pos - offset), s + rel, (size_t)c);
        }
        pos += n;
    }
    return size;
}
WASM_EXPORT u32 wasm_bit_llm_get_model_size() { return (u32)g_lay.total; }
WASM_EXPORT u32 wasm_bit_llm_get_ram_bytes()
{
    u64 b = HEADER_SIZE + (u64)g_metaCap * (8 + 2 + 1 + 4 + 2) + g_arenaCap + (u64)g_htCap * 5 + (u64)NF * L1 * 2 + sizeof(TowerQ) + (u64)g_netCap * (LH + 4) + (u64)g_routerCap * (LH + 4);
    b += CHUNK_BUFFER_SIZE + (u64)g_scratchCap * 16 + sizeof(BeamState) * (MAX_BEAM * 2) + sizeof(TTCluster) * TT_CL;
    return (u32)b;
}
// Mật độ trọng số head khác 0 (‰)
WASM_EXPORT u32 wasm_bit_llm_weight_density()
{
    if (!g_hw || !g_model.vocabCount)
        return 0;
    u64 nz = 0;
    for (size_t i = 0; i < (size_t)g_model.vocabCount * LH; ++i)
        nz += g_hw[i] != 0;
    return (u32)(nz * 1000ULL / ((u64)g_model.vocabCount * LH));
}

// =====================================================================================================================================
// TRAINING API
// =====================================================================================================================================
#ifdef BIT_LLM_TRAIN
namespace nnuecut
{
    static bool g_continual = false;
    // tăng cường: thay một phần từ bằng từ lạ, bỏ một phần (xác định theo rng của epoch)
    static u32 make_view(u32 pi, bool aug, TokInfo *out) noexcept
    {
        const u32 n = g_qLen[pi];
        const TokInfo *src = g_qTok + (size_t)pi * MAX_Q;
        if (!aug || g_augPct == 0 || n < 2)
        {
            std::memcpy(out, src, n * sizeof(TokInfo));
            return n;
        }
        u32 m = 0;
        for (u32 k = 0; k < n; ++k)
        {
            const float r = rnd01() * 100.f;
            if (r < (float)g_augPct)
            {
                out[m] = src[k];
                out[m].id = UNK;
                out[m].h = mix64(splitmix64(g_rng)) | 1;
                if (rnd01() < 0.5f)
                    out[m].h2 = out[m].h ^ 0x5555;
                for (u32 j = 0; j < 3; ++j)
                    out[m].sub[j] = mix64(out[m].h ^ ((u64)j * 0x9E3779B97F4A7C15ULL));
                ++m;
            }
            else if (r < (float)g_augPct * 1.5f && k + 1 < n)
                continue; // bỏ từ
            else
                out[m++] = src[k];
        }
        if (m == 0)
        {
            out[0] = src[0];
            m = 1;
        }
        return m;
    }
    FORCE_INLINE bool hold_of_question(const TokInfo *t, u32 qn) noexcept
    {
        if (g_holdPm == 0)
            return false;
        u64 ph = 0x48014D0B5ULL;
        for (u32 k = 0; k < qn; ++k)
            ph = mix64(ph ^ t[k].h2);
        return (u32)((ph >> 17) % 1000u) < g_holdPm;
    }
    static u32 load_dataset_impl(u32 len, bool reset, bool replay) noexcept
    {
        if (!g_qTok || !g_aTok || !g_hash || !g_df || !g_len || !g_arena || !g_hw || !g_hb)
            return 0;
        if (len > DATA_BUFFER_SIZE)
            len = DATA_BUFFER_SIZE;
        g_data[len] = '\0';
        if (reset)
        {
            g_pairs = g_replayPairs = g_newPairs = g_holdCount = 0;
                std::memset(g_df, 0, (size_t)g_metaCap * sizeof(u16));
        }
        g_overflow = 0;
        u32 skipped = 0, filtered = 0, ansTokens = 0, added = 0, p = 0;
        static TokInfo qt[MAX_Q], at[MAX_A];
        while (p < len)
        {
            u32 ls = p;
            while (p < len && g_data[p] != '\n')
                ++p;
            u32 le = p;
            if (p < len)
                ++p;
            while (le > ls && g_data[le - 1] == '\r')
                --le;
            if (le == ls || g_data[ls] == '#')
                continue;
            u32 bar = ls;
            while (bar < le && g_data[bar] != '|' && g_data[bar] != '\t')
                ++bar;
            if (bar >= le || g_pairs >= MAX_PAIRS)
            {
                ++skipped;
                continue;
            }
            const u32 ov0 = g_overflow;
            const u32 qn = tokenize(g_data + ls, bar - ls, qt, MAX_Q, true);
            const u32 an = tokenize(g_data + bar + 1, le - bar - 1, at, MAX_A - 1, true);
            bool bad = qn == 0 || an == 0 || g_overflow != ov0;
            for (u32 k = 0; k < an && !bad; ++k)
                bad = at[k].id == UNK;
            for (u32 k = 0; k < qn && !bad; ++k)
                bad = qt[k].id == UNK;
            if (bad)
            {
                ++skipped;
                continue;
            }
            u64 ah[MAX_A], qh[MAX_Q];
            for (u32 k = 0; k < an; ++k)
                ah[k] = at[k].h;
            for (u32 k = 0; k < qn; ++k)
                qh[k] = qt[k].h;
            if (degenerate(qh, qn) || degenerate(ah, an))
            {
                ++filtered;
                continue;
            }
            std::memcpy(g_qTok + (size_t)g_pairs * MAX_Q, qt, qn * sizeof(TokInfo));
            g_qLen[g_pairs] = (u8)qn;
            for (u32 k = 0; k < an; ++k)
                g_aTok[g_pairs][k] = at[k].id;
            g_aTok[g_pairs][an] = EOS;
            g_aLen[g_pairs] = (u8)(an + 1);
            const bool hold = !replay && hold_of_question(qt, qn);
            g_isHold[g_pairs] = hold ? 1 : 0;
            g_holdCount += hold ? 1u : 0u;
            if (!replay)
                for (u32 k = 0; k < qn; ++k)
                {
                    bool seen = false;
                    for (u32 j = 0; j < k && !seen; ++j)
                        seen = qt[j].id == qt[k].id;
                    if (!seen && qt[k].id > 0 && g_df[qt[k].id] < 0xFFFFu)
                        ++g_df[qt[k].id];
                }
            ansTokens += an + 1;
            ++g_pairs;
            ++added;
        }
        if (replay)
            g_replayPairs += added;
        else
        {
            g_newPairs += added;
            g_model.numDocs = reset ? added : g_model.numDocs + added;
        }
        g_dsStat[0] = (i32)g_pairs;
        g_dsStat[1] = (i32)skipped;
        g_dsStat[2] = (i32)g_model.vocabCount;
        g_dsStat[3] = (i32)ansTokens;
        g_dsStat[4] = (i32)g_overflow;
        g_dsStat[5] = (i32)filtered;
        g_dsStat[6] = (i32)g_replayPairs;
        g_dsStat[7] = (i32)g_newPairs;
        return g_pairs;
    }

    // top-1 (‰) teacher forcing bằng ĐƯỜNG SUY LUẬN SỐ NGUYÊN (accumulator gia tăng + tháp u8 + head i8). view: 0 nguyên bản, 1 có từ lạ.
    static double g_lastEvalNll = std::numeric_limits<double>::infinity();
    static u32 eval_pairs(u32 view, i32 holdFilter, u32 maxPairs = 0xFFFFFFFFu, double *meanNll = nullptr) noexcept
    {
        if (!train_masters_ready() || g_pairs == 0 || !ensure_scratch())
            return 0;
        quantize_masters();
        static QFeats q;
        static TokInfo tv[MAX_Q];
        u64 steps = 0, ok = 0;
        double nllSum = 0.0;
        u32 used = 0;
        u64 save = g_rng;
        g_rng = 0xE7A1ULL;
        for (u32 pi = 0; pi < g_pairs && used < maxPairs; ++pi)
        {
            if (holdFilter >= 0 && (i32)(g_isHold[pi] != 0) != holdFilter)
                continue;
            ++used;
            const u32 qn = view ? make_view(pi, true, tv) : (std::memcpy(tv, g_qTok + (size_t)pi * MAX_Q, g_qLen[pi] * sizeof(TokInfo)), (u32)g_qLen[pi]);
            build_qfeats(tv, qn, q);
            i32 acc[L1];
            u64 prefix;
            i32 accS[L1];
            acc_start(q, acc, accS, prefix);
            u32 y1 = BOS, y2 = BOS, y3 = BOS;
            for (u32 t = 0; t < g_aLen[pi]; ++t)
            {
                const u32 y = g_aTok[pi][t];
                u8 h8[LH];
                tower(acc, accS, h8, t);
                if (meanNll)
                    nllSum -= hierarchical_target_logprob(h8, y, 1.0);
                BeamState neutral{};
                neutral.node = NO_NODE;
                neutral.len = 1; // evaluation has no first-token EOS suppression
                neutral.y1 = neutral.y2 = neutral.y3 = BOS;
                StreamTop top{};
                stream_hierarchical_scores(h8, neutral, 1.0, 0.0, 1, top, false);
                const u32 best = top.count ? top.item[0].tok : UNK;
                ok += best == y;
                ++steps;
                acc_advance(accS, q, y1, y2, y3, t, prefix, y);
                y3 = y2;
                y2 = y1;
                y1 = y;
            }
        }
        g_rng = save;
        if (meanNll)
            *meanNll = steps ? nllSum / (double)steps : std::numeric_limits<double>::infinity();
        return steps ? (u32)(ok * 1000 / steps) : 0;
    }
    static void free_best() noexcept
    {
        std::free(g_bestE);
        g_bestE = nullptr;
        g_bestDense.clear();
        g_bestHead.clear();
        g_haveBest = false;
        g_bestVocab = 0;
    }
} // namespace nnuecut

WASM_EXPORT char *wasm_bit_llm_get_data_ptr() { return g_data; }
WASM_EXPORT u32 wasm_bit_llm_get_data_size() { return DATA_BUFFER_SIZE; }
WASM_EXPORT u32 wasm_bit_llm_load_dataset(u32 len) { return load_dataset_impl(len, true, false); }
WASM_EXPORT u32 wasm_bit_llm_load_dataset_inc(u32 len, u32 replay) { return load_dataset_impl(len, false, replay != 0); }
WASM_EXPORT u32 wasm_bit_llm_begin_session(u32 continual, u32)
{
    free_best();
    g_continual = continual != 0;
    g_trainEpochs = 0;
    g_pairs = g_replayPairs = g_newPairs = g_holdCount = 0;
    return 1;
}
WASM_EXPORT void wasm_bit_llm_end_session()
{
    free_best();
    g_continual = false;
}
// (fast, fullEvery->hnEvery, cacheMB(bỏ), marginMode(bỏ), schedMode: 1 = lr giảm tuyến tính theo tổng epoch)
WASM_EXPORT void wasm_bit_llm_set_train_opts(u32, u32 fullEvery, u32, u32, u32 schedMode)
{
    h_hnEvery = fullEvery < 1 ? 1 : fullEvery;
    h_sched = schedMode;
}
// hyper: 0 lr(x1e-5) | 1 legacy neg (ignored by full-softmax) | 2 diagnostic interval | 3 total epochs | 4 legacy logQ (ignored)
WASM_EXPORT u32 wasm_bit_llm_get_hyper(u32 i) { return i == 0 ? (u32)(h_lr * 1e5f) : i == 1 ? h_neg
                                                                                 : i == 2   ? h_hnEvery
                                                                                 : i == 3   ? h_totalEpochs
                                                                                 : i == 4   ? (u32)(h_logq * 100.f + 0.5f)
                                                                                            : 0; }
WASM_EXPORT void wasm_bit_llm_set_hyper(u32 i, u32 v)
{
    if (i == 0)
        h_lr = (float)v * 1e-5f;
    else if (i == 1)
    {
        h_neg = v < 1 ? 1 : (v > 64 ? 64 : v);
    }
    else if (i == 2)
        h_hnEvery = v < 1 ? 1 : v;
    else if (i == 3)
        h_totalEpochs = v < 1 ? 1 : v;
    else if (i == 4)
    {
        h_logq = (float)(v > 200 ? 200 : v) / 100.f;
    }
}
WASM_EXPORT u32 wasm_bit_llm_tune_hyper(u32, u32, u32) { return 0; } // legacy API retained; hierarchical full-softmax has no sampled-negative knob
WASM_EXPORT u32 wasm_bit_llm_eval_range(u32, u32) { return eval_pairs(0, -1); }
WASM_EXPORT u32 wasm_bit_llm_eval_holdout(u32 view) { return eval_pairs(view > 1 ? 1 : view, 1, 0xFFFFFFFFu, &g_lastEvalNll); }
WASM_EXPORT double wasm_bit_llm_get_eval_nll() { return g_lastEvalNll; }
WASM_EXPORT u32 wasm_bit_llm_get_holdout_count() { return g_holdCount; }
WASM_EXPORT void wasm_bit_llm_set_holdout(u32 permille) { g_holdPm = permille > 500 ? 500 : permille; }
WASM_EXPORT void wasm_bit_llm_set_aug(u32 pct) { g_augPct = pct > 40 ? 40 : pct; }
WASM_EXPORT u32 wasm_bit_llm_is_holdout_line(u32 len)
{
    if (g_holdPm == 0)
        return 0;
    if (len >= IO_BUFFER_SIZE)
        len = IO_BUFFER_SIZE - 1;
    u32 bar = 0;
    while (bar < len && g_in[bar] != '|' && g_in[bar] != '\t')
        ++bar;
    static TokInfo t[MAX_Q];
    const u32 qn = tokenize(g_in, bar, t, MAX_Q, false);
    return (qn > 0 && hold_of_question(t, qn)) ? 1u : 0u;
}
WASM_EXPORT u32 wasm_bit_llm_snapshot_best()
{
    if (!train_masters_ready() || g_model.vocabCount == 0 || g_model.vocabCount > TRAIN_VOCAB_CAP)
        return 0;
    const size_t en = (size_t)NF * L1;
    if (!g_bestE)
        g_bestE = (float *)std::malloc(en * 4);
    if (!g_bestE)
        return 0;
    std::memcpy(g_bestE, T_E, en * 4);
    g_bestDense.clear();
    for (Dense *d : {&D_eb, &D_w1, &D_b1, &D_wr, &D_br, &D_whalt, &D_bhalt, &D_wmem, &D_bmem, &D_w2, &D_b2, &D_routerW, &D_routerB})
        g_bestDense.insert(g_bestDense.end(), d->w, d->w + d->n);
    g_bestHead.assign(T_hw, T_hw + (size_t)g_model.vocabCount * LH);
    g_bestHead.insert(g_bestHead.end(), T_hb, T_hb + g_model.vocabCount);
    g_bestVocab = g_model.vocabCount;
    g_haveBest = true;
    return 1;
}
WASM_EXPORT u32 wasm_bit_llm_restore_best()
{
    // A snapshot includes vocabulary-indexed head rows. Restoring it into a model
    // with a different vocabulary size would partially copy unrelated rows.
    if (!g_haveBest || !g_bestE || !train_masters_ready() || g_model.vocabCount != g_bestVocab)
        return 0;
    const size_t denseCount = (size_t)D_eb.n + D_w1.n + D_b1.n + D_wr.n + D_br.n + D_whalt.n + D_bhalt.n + D_wmem.n + D_bmem.n + D_w2.n + D_b2.n + D_routerW.n + D_routerB.n;
    if (g_bestDense.size() != denseCount || g_bestHead.size() != (size_t)g_bestVocab * (LH + 1))
        return 0;
    std::memcpy(T_E, g_bestE, (size_t)NF * L1 * sizeof(float));
    size_t o = 0;
    for (Dense *d : {&D_eb, &D_w1, &D_b1, &D_wr, &D_br, &D_whalt, &D_bhalt, &D_wmem, &D_bmem, &D_w2, &D_b2, &D_routerW, &D_routerB})
    {
        std::memcpy(d->w, g_bestDense.data() + o, (size_t)d->n * sizeof(float));
        o += d->n;
    }
    const size_t weightCount = (size_t)g_bestVocab * LH;
    std::memcpy(T_hw, g_bestHead.data(), weightCount * sizeof(float));
    std::memcpy(T_hb, g_bestHead.data() + weightCount, (size_t)g_bestVocab * sizeof(float));
    // Refresh both float-side QAT caches and the actual integer inference model.
    refresh_quant();
    quantize_masters();
    return 1;
}

// Một epoch: router full-softmax + fine conditional full-softmax, tăng cường câu hỏi mỗi lần.
WASM_EXPORT u32 wasm_bit_llm_train_epoch(u32 seed)
{
    if (!train_masters_ready() || g_pairs == 0 || !ensure_scratch())
        return 0;
    u32 totalSteps = 0;
    static std::vector<u32> base;
    base.assign(g_pairs + 1, 0);
    for (u32 i = 0; i < g_pairs; ++i)
    {
        base[i] = totalSteps;
        totalSteps += g_isHold[i] ? 0u : (u32)g_aLen[i];
    }
    base[g_pairs] = totalSteps;
    static std::vector<u16> order;
    order.clear();
    for (u32 i = 0; i < g_pairs; ++i)
        if (!g_isHold[i])
            order.push_back((u16)i);
    if (order.empty())
        return 0;
    g_rng = 0xC0FFEEULL ^ ((u64)seed * 0x9e3779b97f4a7c15ULL);
    for (u32 i = (u32)order.size() - 1; i > 0; --i)
        std::swap(order[i], order[splitmix64(g_rng) % (i + 1)]);

    const float frac = h_sched ? std::min(1.f, (float)g_trainEpochs / (float)h_totalEpochs) : 0.f;
    // Curriculum warm-up is active in the actual optimizer path; the host may
    // additionally consume the exposed reasoning/long-context task weights.
    g_curriculum.configure(h_totalEpochs);
    const auto curriculumPlan = g_curriculum.plan();
    const float lr = h_lr * (1.f - (1.f - h_lrDecayTo) * frac) * curriculumPlan.learning_rate_scale;
    refresh_quant();
    static TokInfo tv[MAX_Q];
    static QFeats q;
    double lossSum = 0;
    u64 steps = 0, scanned = 0, scanOk = 0;
    const u32 DB = 4;
    u32 sinceFlush = 0;
#ifdef BIT_LLM_DIAGNOSTICS
    std::memset(g_diagMaskActive, 0, sizeof(g_diagMaskActive));
    std::memset(g_diagMaskTotal, 0, sizeof(g_diagMaskTotal));
    g_diagDaccZero = g_diagDaccTotal = g_diagLogQCount = 0;
    g_diagDaccAbs = g_diagLogQShift = 0.0;
#endif
    struct SparseEmbGrad { u32 id; std::array<float, L1> grad; };
    static std::vector<SparseEmbGrad> sparseRows;
    static std::unordered_map<u32, u32> sparseIndex;
    sparseRows.reserve(4096);
    sparseIndex.reserve(4096);
    for (u32 n = 0; n < order.size(); ++n)
    {
        const u32 pi = order[n];
        sparseRows.clear();
        sparseIndex.clear();
        const u32 qn = make_view(pi, true, tv);
        build_qfeats(tv, qn, q);
        float accQ[L1];
        for (u32 j = 0; j < L1; ++j)
            accQ[j] = Q_eb[j];
        for (u32 f = 0; f < q.n; ++f)
        {
            const float *r = T_E + (size_t)q.idx[f] * L1;
            for (u32 j = 0; j < L1; ++j)
                accQ[j] += fqE(r[j]);
        }
        float dQ[L1] = {};
        float accS[L1] = {};
        u64 prefix = prefix_seed(q);
        u32 stepIds[MAX_STEP_FEATS], stepN = 0;
        u32 stepHistory[MAX_A][MAX_STEP_FEATS]{};
        u8 stepHistoryCount[MAX_A]{};
        float daccHistory[MAX_A][L1]{};
        u32 y1 = BOS, y2 = BOS, y3 = BOS;
        const u32 *ans = g_aTok[pi];
        const u32 answerLen = std::min<u32>(g_aLen[pi], MAX_A);
        stepN = step_feats(q, y1, y2, y3, 0, prefix, stepIds);
        apply_embedding_features(accS, stepIds, stepN, 1.0f);
        u32 stepIdsNext[MAX_STEP_FEATS];
        for (u32 t = 0; t < answerLen; ++t)
        {
            const u32 y = ans[t];
            std::memcpy(stepHistory[t], stepIds, stepN * sizeof(u32));
            stepHistoryCount[t] = (u8)stepN;
            // Hierarchical full-softmax is exact: there is no negative sampling.
            const u32 sidx = base[pi] + t;
            const bool scan = ((sidx + (u32)seed) % h_hnEvery) == 0;
            bool sok = false;
            ++T_step;
            T_beta1Pow = T_step <= 10000 ? T_beta1Pow * 0.9f : 0.0f;
            T_beta2Pow = T_step <= 20000 ? T_beta2Pow * 0.999f : 0.0f;
            const float c1 = 1.f / (1.f - T_beta1Pow), c2 = 1.f / (1.f - T_beta2Pow);
            float daccS[L1]{};
            const float stepLoss = scan ? train_step<true>(accQ, accS, y, t, lr, c1, c2, dQ, daccS, sok)
                                        : train_step<false>(accQ, accS, y, t, lr, c1, c2, dQ, daccS, sok);
            lossSum += stepLoss;
            ++steps;
            // Store the per-position accumulator gradient. Prefix-feature gradients
            // are recovered later with a reverse suffix sum, avoiding a repeated
            // O(t) walk over every old so-far feature at every target position.
            std::memcpy(daccHistory[t], daccS, sizeof(daccS));
            if (scan)
            {
                ++scanned;
                scanOk += sok;
            }
            if (++sinceFlush >= DB)
            {
                sinceFlush = 0;
                {
                    const float ld = lr * k_dense;
                    dense_flush(D_w1, ld, c1, c2);
                    dense_flush(D_b1, ld, c1, c2);
                    dense_flush(D_wr, ld, c1, c2);
                    dense_flush(D_br, ld, c1, c2);
                    dense_flush(D_w2, ld, c1, c2);
                    dense_flush(D_b2, ld, c1, c2);
                    dense_flush(D_routerW, ld, c1, c2);
                    dense_flush(D_routerB, ld, c1, c2);
                    dense_flush(D_eb, ld * (k_emb / k_dense), c1, c2);
                }
                refresh_quant();
            }
            // Incremental next-state update: + so-far token, - old transition rows,
            // + new transition rows. T_E is frozen until the end of this answer.
            if (y != EOS)
            {
                const u32 sf = sofar_feat(y);
                apply_embedding_features(accS, &sf, 1, 1.0f);
            }
            apply_embedding_features(accS, stepIds, stepN, -1.0f);
            prefix = prefix_next(prefix, y);
            y3 = y2;
            y2 = y1;
            y1 = y;
            stepN = step_feats(q, y1, y2, y3, t + 1, prefix, stepIdsNext);
            std::memcpy(stepIds, stepIdsNext, stepN * sizeof(u32));
            apply_embedding_features(accS, stepIds, stepN, 1.0f);
        }
        // Reconstruct the exact feature-gradient incidence without replaying every
        // historical so-far row at every token. Step features receive dAccS[t].
        // A so-far feature introduced by answer[t] receives sum(dAccS[k], k>t).
        auto add_sparse_row = [&](u32 id, const float *grad, float scale) noexcept
        {
            auto it = sparseIndex.find(id);
            u32 rowIndex;
            if (it == sparseIndex.end())
            {
                rowIndex = (u32)sparseRows.size();
                SparseEmbGrad row{};
                row.id = id;
                sparseRows.push_back(std::move(row));
                sparseIndex.emplace(id, rowIndex);
            }
            else rowIndex = it->second;
            auto &dst = sparseRows[rowIndex].grad;
            for (u32 j = 0; j < L1; ++j) dst[j] += grad[j] * scale;
        };
        float suffixGrad[L1]{};
        const float invAnswerLen = 1.0f / (float)std::max<u32>(1, answerLen);
        for (u32 t = answerLen; t-- > 0;)
        {
            // Current transition feature rows are active only at this position.
            u32 ids[MAX_STEP_FEATS];
            const u32 count = stepHistoryCount[t];
            std::memcpy(ids, stepHistory[t], count * sizeof(u32));
            std::sort(ids, ids + count);
            for (u32 i = 0; i < count; )
            {
                u32 e = i + 1;
                while (e < count && ids[e] == ids[i]) ++e;
                add_sparse_row(ids[i], daccHistory[t], (float)(e - i) * invAnswerLen);
                i = e;
            }
            // Token t becomes a persistent so-far feature only for later positions.
            if (ans[t] != EOS)
            {
                bool hasFutureGradient = false;
                for (u32 j = 0; j < L1 && !hasFutureGradient; ++j)
                    hasFutureGradient = suffixGrad[j] != 0.0f;
                if (hasFutureGradient)
                {
                    const u32 sf = sofar_feat(ans[t]);
                    add_sparse_row(sf, suffixGrad, invAnswerLen);
                }
            }
            for (u32 j = 0; j < L1; ++j) suffixGrad[j] += daccHistory[t][j];
        }

        // One sparse Adam update per unique answer feature row. This is sequence-
        // batch training, intentionally different from the old per-token updates.
        const float c1 = 1.f / (1.f - T_beta1Pow), c2 = 1.f / (1.f - T_beta2Pow);
        for (const SparseEmbGrad &rowGrad : sparseRows)
        {
            float *r = T_E + (size_t)rowGrad.id * L1;
            float *rm = T_Em + (size_t)rowGrad.id * L1;
            float *rv = T_Ev + (size_t)rowGrad.id * L1;
            for (u32 j = 0; j < L1; ++j)
            {
                adam1(r[j], rm[j], rv[j], rowGrad.grad[j], lr * k_emb, c1, c2);
                r[j] = std::max(-60.f, std::min(60.f, r[j]));
            }
        }
        // Question embedding: one Adam update for the whole answer.
        u32 questionIds[MAX_QF];
        std::memcpy(questionIds, q.idx, q.n * sizeof(u32));
        std::sort(questionIds, questionIds + q.n);
        for (u32 f = 0; f < q.n; )
        {
            u32 e = f + 1;
            while (e < q.n && questionIds[e] == questionIds[f]) ++e;
            const u32 id = questionIds[f], multiplicity = e - f;
            float *r = T_E + (size_t)id * L1, *rm = T_Em + (size_t)id * L1, *rv = T_Ev + (size_t)id * L1;
            for (u32 j = 0; j < L1; ++j)
            {
                const float grad = dQ[j] * (float)multiplicity / (float)answerLen;
                adam1(r[j], rm[j], rv[j], grad, lr * k_emb, c1, c2);
                r[j] = r[j] > 60.f ? 60.f : (r[j] < -60.f ? -60.f : r[j]);
            }
            f = e;
        }
    }
    // Flush the final partial micro-batch. Without this, the last 1..DB-1
    // token-position gradients (including router CE) would remain pending until
    // the next epoch and would not be reflected in an export/evaluation now.
    if (sinceFlush)
    {
        const float ld = lr * k_dense;
        const float c1 = 1.f / (1.f - T_beta1Pow), c2 = 1.f / (1.f - T_beta2Pow);
        dense_flush(D_w1, ld, c1, c2);
        dense_flush(D_b1, ld, c1, c2);
        dense_flush(D_wr, ld, c1, c2);
        dense_flush(D_br, ld, c1, c2);
        dense_flush(D_w2, ld, c1, c2);
        dense_flush(D_b2, ld, c1, c2);
        dense_flush(D_routerW, ld, c1, c2);
        dense_flush(D_routerB, ld, c1, c2);
        dense_flush(D_eb, ld * (k_emb / k_dense), c1, c2);
    }
    refresh_quant();
    quantize_masters(); // make trained router/fine weights immediately usable by native inference
    ++g_model.epochs;
    ++g_trainEpochs;
    g_stat[0] = (i32)(lossSum / (double)(steps ? steps : 1) * 1000.0);
    g_stat[1] = scanned ? (i32)(scanOk * 1000 / scanned) : 0;
    g_stat[2] = 0;
    g_stat[3] = (i32)order.size();
    g_stat[4] = (i32)steps;
    g_stat[5] = (i32)(lr * 1e6f);
    g_stat[6] = (i32)g_model.epochs;
    return (u32)steps;
}

WASM_EXPORT double wasm_bit_llm_get_train_diag(u32 i)
{
#ifdef BIT_LLM_DIAGNOSTICS
    if (i < 3)
        return g_diagMaskTotal[i] ? (100.0 * (double)g_diagMaskActive[i] / (double)g_diagMaskTotal[i]) : 0.0;
    if (i == 3)
        return g_diagDaccTotal ? (100.0 * (double)g_diagDaccZero / (double)g_diagDaccTotal) : 0.0;
    if (i == 4)
        return g_diagDaccTotal ? g_diagDaccAbs / (double)g_diagDaccTotal : 0.0;
    if (i == 5)
        return g_diagLogQCount ? g_diagLogQShift / (double)g_diagLogQCount : 0.0;
    if (i == 6)
        return (double)g_diagLogQCount;
#else
    (void)i;
#endif
    return 0.0;
}

// Hiệu chuẩn nhiệt độ tau (z = logit/tau) bằng tìm lưới theo NLL trên tập giữ-lại (hoặc tập train nếu không có), dùng đường suy luận số nguyên.
WASM_EXPORT void wasm_bit_llm_calibrate_temperature(u32 seed)
{
    if (!train_masters_ready() || g_pairs == 0 || !ensure_scratch())
        return;
    quantize_masters();
    const bool useHold = g_holdCount >= 8;
    static QFeats q;
    static std::vector<double> nll;
    const u32 G = 28;
    float taus[G];
    for (u32 i = 0; i < G; ++i)
        taus[i] = 0.25f * std::pow(1.18f, (float)i);
    nll.assign(G, 0.0);
    u64 rs = 0xCA11B7A7EULL ^ seed;
    u32 steps = 0;
    const u32 maxSteps = 1500;
    for (u32 pi = 0; pi < g_pairs && steps < maxSteps; ++pi)
    {
        if ((g_isHold[pi] != 0) != useHold)
            continue;
        if (!useHold && (splitmix64(rs) & 3) != 0)
            continue;
        build_qfeats(g_qTok + (size_t)pi * MAX_Q, g_qLen[pi], q);
        i32 accQ[L1], accS[L1];
        u64 prefix;
        acc_start(q, accQ, accS, prefix);
        u32 y1 = BOS, y2 = BOS, y3 = BOS;
        for (u32 t = 0; t < g_aLen[pi] && steps < maxSteps; ++t)
        {
            const u32 y = g_aTok[pi][t];
            u8 h8[LH];
            tower(accQ, accS, h8, t);
            for (i32 gi = 0; gi < (i32)G; ++gi)
                nll[gi] -= hierarchical_target_logprob(h8, y, 1.0 / (double)taus[gi]);
            ++steps;
            acc_advance(accS, q, y1, y2, y3, t, prefix, y);
            y3 = y2;
            y2 = y1;
            y1 = y;
        }
    }
    if (steps == 0)
        return;
    u32 bi = 0;
    for (u32 gi = 1; gi < G; ++gi)
        if (nll[gi] < nll[bi])
            bi = gi;
    g_model.temp = taus[bi];
    g_stat[2] = (i32)(nll[bi] / steps * 1000.0);
}
#else
WASM_EXPORT char *wasm_bit_llm_get_data_ptr() { return nullptr; }
WASM_EXPORT u32 wasm_bit_llm_get_data_size() { return 0; }
WASM_EXPORT u32 wasm_bit_llm_load_dataset(u32) { return 0; }
WASM_EXPORT u32 wasm_bit_llm_train_epoch(u32) { return 0; }
WASM_EXPORT u32 wasm_bit_llm_load_dataset_inc(u32, u32) { return 0; }
WASM_EXPORT u32 wasm_bit_llm_begin_session(u32, u32) { return 0; }
WASM_EXPORT void wasm_bit_llm_end_session() {}
WASM_EXPORT void wasm_bit_llm_set_train_opts(u32, u32, u32, u32, u32) {}
WASM_EXPORT u32 wasm_bit_llm_get_hyper(u32) { return 0; }
WASM_EXPORT void wasm_bit_llm_set_hyper(u32, u32) {}
WASM_EXPORT u32 wasm_bit_llm_tune_hyper(u32, u32, u32) { return 0; }
WASM_EXPORT u32 wasm_bit_llm_eval_range(u32, u32) { return 0; }
WASM_EXPORT u32 wasm_bit_llm_eval_holdout(u32) { return 0; }
WASM_EXPORT double wasm_bit_llm_get_eval_nll() { return 0.0; }
WASM_EXPORT u32 wasm_bit_llm_get_holdout_count() { return 0; }
WASM_EXPORT void wasm_bit_llm_set_holdout(u32) {}
WASM_EXPORT void wasm_bit_llm_set_aug(u32) {}
WASM_EXPORT u32 wasm_bit_llm_is_holdout_line(u32) { return 0; }
WASM_EXPORT u32 wasm_bit_llm_snapshot_best() { return 0; }
WASM_EXPORT u32 wasm_bit_llm_restore_best() { return 0; }
WASM_EXPORT void wasm_bit_llm_calibrate_temperature(u32) {}
WASM_EXPORT double wasm_bit_llm_get_train_diag(u32) { return 0.0; }
#endif
