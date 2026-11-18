#pragma once
// Bit-LLM Context Memory v2: scoped, accent-insensitive, recoverable evidence store.
#include "../core/types.h"
#include "../core/hash_primitives.h"
#include "../core/string_parse.h"
#include "../core/data_structures.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bitctx {

using stockcut::u8;
using stockcut::u16;
using stockcut::u32;
using stockcut::u64;
using stockcut::usize;
using stockcut::isize;
using stockcut::MultiArray;

struct Entry {
    u64 id = 0;
    u64 timestamp = 0;
    u64 session_id = 0;
    u64 project_id = 0;
    u8 kind = 0;
    u8 importance = 0;
    std::string text;
    std::vector<u64> terms;
};

struct Hit { u32 entry_index = 0; u32 score_milli = 0; };

class ContextMemory {
    static constexpr u32 kMagic = 0x324D4342u;       // BCM2
    static constexpr u32 kLegacyMagic = 0x374D4342u; // BCM7 (v1)
    static constexpr u32 kVersion = 2;
    static constexpr u32 kRecordMagic = 0x32524342u; // BCR2
    static constexpr usize kHeaderBytes = 16;
    static constexpr usize kRecordHeaderBytes = 52;
    static constexpr usize kMaxEntries = 65536;
    static constexpr usize kMaxText = 4095;
    static constexpr usize kMaxFile = 512ull * 1024ull * 1024ull;
    std::vector<Entry> entries_;
    std::unordered_map<u64, std::vector<u32>> inverted_;
    std::vector<Hit> hits_;
    u64 next_id_ = 1;
    u64 session_scope_ = 0, project_scope_ = 0;
    bool recovered_damage_ = false;

    static bool sep(u8 c) noexcept {
        return c <= 0x20u || c == ',' || c == '.' || c == '?' || c == '!' || c == ';' ||
               c == ':' || c == '"' || c == '\'' || c == '(' || c == ')' || c == '[' ||
               c == ']' || c == '{' || c == '}' || c == '-' || c == '/' || c == '\\';
    }
    static u32 decode_utf8(const char* s, usize n, usize& i) noexcept {
        const u8 c = static_cast<u8>(s[i]);
        if (c < 0x80u) { ++i; return c; }
        const u32 count = c >= 0xF0u ? 4u : (c >= 0xE0u ? 3u : (c >= 0xC2u ? 2u : 1u));
        if (count == 1u || i + count > n) { ++i; return 0x110000u + c; }
        u32 cp = c & (count == 2u ? 0x1Fu : (count == 3u ? 0x0Fu : 0x07u));
        for (u32 j = 1; j < count; ++j) {
            const u8 d = static_cast<u8>(s[i + j]);
            if ((d & 0xC0u) != 0x80u) { ++i; return 0x110000u + c; }
            cp = (cp << 6) | (d & 0x3Fu);
        }
        const u32 minv = count == 2u ? 0x80u : (count == 3u ? 0x800u : 0x10000u);
        if (cp < minv || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            ++i; return 0x110000u + c;
        }
        i += count; return cp;
    }
    static u32 lower_cp(u32 c) noexcept {
        if (c >= 'A' && c <= 'Z') return c + 32u;
        if (c >= 0xC0u && c <= 0xDEu && c != 0xD7u) return c + 32u;
        if (c >= 0x100u && c <= 0x12Fu && (c & 1u) == 0u) return c + 1u;
        if (c >= 0x132u && c <= 0x137u && (c & 1u) == 0u) return c + 1u;
        if (c >= 0x139u && c <= 0x148u && (c & 1u)) return c + 1u;
        if (c >= 0x14Au && c <= 0x177u && (c & 1u) == 0u) return c + 1u;
        if (c == 0x178u) return 0xFFu;
        if (c >= 0x179u && c <= 0x17Eu && (c & 1u)) return c + 1u;
        if (c == 0x1A0u || c == 0x1AFu) return c + 1u;
        if (c >= 0x1E00u && c <= 0x1EFFu && !(c >= 0x1E96u && c <= 0x1E9Fu) && (c & 1u) == 0u) return c + 1u;
        return c;
    }
    static u32 strip_cp(u32 c) noexcept {
        switch (c) {
            case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0x103: return 'a';
            case 0xE8: case 0xE9: case 0xEA: return 'e';
            case 0xEC: case 0xED: case 0x129: return 'i';
            case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0x1A1: return 'o';
            case 0xF9: case 0xFA: case 0x169: case 0x1B0: return 'u';
            case 0xFD: return 'y';
            case 0x111: return 'd';
            default: break;
        }
        if (c >= 0x1EA1u && c <= 0x1EB7u) return 'a';
        if (c >= 0x1EB9u && c <= 0x1EC7u) return 'e';
        if (c >= 0x1EC9u && c <= 0x1ECBu) return 'i';
        if (c >= 0x1ECDu && c <= 0x1EE3u) return 'o';
        if (c >= 0x1EE5u && c <= 0x1EF1u) return 'u';
        if (c >= 0x1EF3u && c <= 0x1EF9u) return 'y';
        if ((c >= 0x0300u && c <= 0x0303u) || c == 0x0306u || c == 0x0309u || c == 0x031Bu || c == 0x0323u) return 0u;
        return c;
    }
    static std::vector<u64> terms_of(const char* s, usize n) {
        std::vector<u64> out;
        usize i = 0;
        while (i < n) {
            while (i < n && sep(static_cast<u8>(s[i]))) ++i;
            if (i >= n) break;
            u64 h = stockcut::FNV1A_64_OFFSET;
            bool any = false;
            while (i < n && !sep(static_cast<u8>(s[i]))) {
                u32 cp = strip_cp(lower_cp(decode_utf8(s, n, i)));
                if (!cp) continue;
                any = true;
                if (cp >= 0x110000u) {
                    h = stockcut::fnv1a_64_step(h, static_cast<u8>(cp - 0x110000u));
                } else if (cp < 0x80u) {
                    h = stockcut::fnv1a_64_step(h, static_cast<u8>(cp));
                } else {
                    MultiArray<char, 4> utf8Buf{};
                    const u32 m = stockcut::utf8_encode_cp(cp, utf8Buf.data());
                    for (u32 j = 0; j < m; ++j) {
                        h = stockcut::fnv1a_64_step(h, static_cast<u8>(utf8Buf[j]));
                    }
                }
            }
            if (any) out.push_back(h);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
    template<class T> static void put(std::vector<u8>& b, T v) {
        for (usize i = 0; i < sizeof(T); ++i) b.push_back(static_cast<u8>((static_cast<u64>(v) >> (8u * i)) & 0xFFu));
    }
    template<class T> static bool get(const u8* b, usize n, usize& p, T& v) {
        if (p > n || sizeof(T) > n - p) return false;
        u64 x = 0; for (usize i = 0; i < sizeof(T); ++i) x |= static_cast<u64>(b[p++]) << (8u * i);
        v = static_cast<T>(x); return true;
    }
    void rebuild_index() {
        inverted_.clear();
        for (u32 i = 0; i < entries_.size(); ++i)
            for (u64 t : entries_[i].terms) inverted_[t].push_back(i);
    }
    bool scope_match(const Entry& e, u64 session, u64 project) const noexcept {
        if (project) {
            if (e.project_id) return e.project_id == project;
            if (e.session_id) return session && e.session_id == session;
            return true;
        }
        if (session) {
            if (e.project_id) return false;
            return !e.session_id || e.session_id == session;
        }
        return !e.project_id && !e.session_id;
    }
    static void append_header(std::vector<u8>& out) {
        std::vector<u8> h; put(h, kMagic); put(h, kVersion); put(h, static_cast<u32>(kHeaderBytes));
        put(h, stockcut::crc32_compute(h.data(), h.size())); out.insert(out.end(), h.begin(), h.end());
    }
    static bool make_record(const Entry& e, std::vector<u8>& out) {
        if (e.text.empty() || e.text.size() > kMaxText) return false;
        std::vector<u8> r; r.reserve(kRecordHeaderBytes + e.text.size());
        put(r, kRecordMagic); put(r, static_cast<u32>(kRecordHeaderBytes + e.text.size()));
        put(r, e.id); put(r, e.timestamp); put(r, e.session_id); put(r, e.project_id);
        put(r, e.kind); put(r, e.importance); put(r, static_cast<u16>(0)); put(r, static_cast<u32>(e.text.size()));
        r.insert(r.end(), e.text.begin(), e.text.end());
        const u32 sum = stockcut::crc32_compute(r.data() + 8, r.size() - 8);
        r.insert(r.begin() + 48, 4, 0);
        for (u32 i = 0; i < 4; ++i) r[48 + i] = static_cast<u8>((sum >> (8u * i)) & 0xFFu);
        out.insert(out.end(), r.begin(), r.end());
        return true;
    }
    bool append_record_file(const char* path, const Entry& e) const {
        std::ifstream in(path, std::ios::binary);
        const bool fresh = !in.good();
        if (in.good()) {
            u32 magic = 0, version = 0;
            in.read(reinterpret_cast<char*>(&magic), 4); in.read(reinterpret_cast<char*>(&version), 4);
            if (!in.good() || magic != kMagic || version != kVersion) return false;
        }
        std::ofstream f(path, std::ios::binary | std::ios::app);
        if (!f) return false;
        if (fresh) {
            std::vector<u8> h; append_header(h);
            f.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
        }
        std::vector<u8> r;
        if (!make_record(e, r)) return false;
        f.write(reinterpret_cast<const char*>(r.data()), static_cast<std::streamsize>(r.size()));
        f.flush(); return static_cast<bool>(f);
    }
    bool add_entry(Entry&& e) {
        if (entries_.size() >= kMaxEntries) return false;
        const u32 idx = static_cast<u32>(entries_.size());
        entries_.push_back(std::move(e));
        for (u64 t : entries_.back().terms) inverted_[t].push_back(idx);
        next_id_ = stockcut::max(next_id_, entries_.back().id + 1);
        hits_.clear(); return true;
    }
    bool deserialize_v1(const u8* data, usize size) {
        recovered_damage_ = false;
        if (size < 24) return false;
        const usize body = size - 8;
        u64 saved = 0; std::memcpy(&saved, data + body, 8);
        if (stockcut::fnv1a_64_bytes(data, body) != saved) return false;
        usize p = 0; u32 magic = 0, ver = 0, count = 0; u64 next = 1;
        if (!get(data, body, p, magic) || !get(data, body, p, ver) || !get(data, body, p, count) || !get(data, body, p, next) || magic != kLegacyMagic || ver != 1 || count > kMaxEntries) return false;
        std::vector<Entry> fresh; fresh.reserve(count); u64 maxid = 0;
        for (u32 i = 0; i < count; ++i) {
            Entry e; u32 len = 0;
            if (!get(data, body, p, e.id) || !get(data, body, p, e.timestamp) || !get(data, body, p, e.kind) || !get(data, body, p, e.importance) || !get(data, body, p, len) || !len || len > kMaxText || p > body || len > body - p) return false;
            e.text.assign(reinterpret_cast<const char*>(data + p), len); p += len; e.terms = terms_of(e.text.data(), e.text.size());
            if (!e.terms.empty()) fresh.push_back(std::move(e));
            if (fresh.size() > kMaxEntries) return false;
            maxid = stockcut::max(maxid, e.id);
        }
        if (p != body) return false;
        entries_.swap(fresh); next_id_ = stockcut::max(next, maxid + 1); rebuild_index(); hits_.clear(); return true;
    }
public:
    void set_scope(u64 sessionId, u64 projectId) noexcept { session_scope_ = sessionId; project_scope_ = projectId; }
    u64 session_scope() const noexcept { return session_scope_; }
    u64 project_scope() const noexcept { return project_scope_; }

    u64 add(const char* text, usize len, u8 kind, u8 importance, u64 timestamp,
            u64 sessionId = 0, u64 projectId = 0) {
        if (!text || len == 0 || len > kMaxText || entries_.size() >= kMaxEntries) return 0;
        Entry e; e.id = next_id_; e.timestamp = timestamp; e.session_id = sessionId ? sessionId : session_scope_;
        e.project_id = projectId ? projectId : project_scope_; e.kind = kind; e.importance = importance;
        e.text.assign(text, len); e.terms = terms_of(text, len);
        if (e.terms.empty() || !add_entry(std::move(e))) return 0;
        return entries_.back().id;
    }
    bool add_and_append(const char* path, const char* text, usize len, u8 kind, u8 importance, u64 timestamp) {
        if (!text || len == 0 || len > kMaxText || entries_.size() >= kMaxEntries) return false;
        Entry e; e.id = next_id_; e.timestamp = timestamp; e.session_id = session_scope_; e.project_id = project_scope_;
        e.kind = kind; e.importance = importance; e.text.assign(text, len); e.terms = terms_of(text, len);
        if (e.terms.empty()) return false;
        if (!append_record_file(path, e)) {
            if (!add_entry(Entry(e)) || !save_file(path)) {
                if (!entries_.empty() && entries_.back().id == e.id) { entries_.pop_back(); rebuild_index(); next_id_ = e.id; }
                return false;
            }
            return true;
        }
        return add_entry(std::move(e));
    }
    const std::vector<Hit>& search_at(const char* query, usize len, u32 topk, u64 now,
                                      u64 sessionId = 0, u64 projectId = 0) {
        hits_.clear();
        if (!query || len == 0 || topk == 0 || entries_.empty()) return hits_;
        const auto terms = terms_of(query, len); if (terms.empty()) return hits_;
        const u64 session = sessionId ? sessionId : session_scope_;
        const u64 project = projectId ? projectId : project_scope_;
        std::unordered_map<u32, double> scores;
        std::unordered_map<u32, u32> matchedTerms;
        const u32 minimumOverlap = static_cast<u32>(stockcut::min<usize>(2u, terms.size()));
        const double nDocs = static_cast<double>(entries_.size());
        double avgLen = 0.0; for (const auto& e : entries_) avgLen += static_cast<double>(e.terms.size());
        avgLen = avgLen / stockcut::max(1.0, nDocs);
        for (u64 t : terms) {
            auto it = inverted_.find(t); if (it == inverted_.end()) continue;
            const double df = static_cast<double>(it->second.size());
            const double idf = std::log1p((nDocs - df + 0.5) / (df + 0.5));
            for (u32 idx : it->second) {
                if (idx >= entries_.size()) continue;
                const Entry& e = entries_[idx]; if (!scope_match(e, session, project)) continue;
                ++matchedTerms[idx];
                const double age = now > e.timestamp && e.timestamp ? static_cast<double>(now - e.timestamp) : 0.0;
                const double recency = 1.0 / (1.0 + age / (30.0 * 86400.0));
                const double importance = 1.0 + static_cast<double>(e.importance) / 255.0;
                const double dl = static_cast<double>(stockcut::max<usize>(1u, e.terms.size()));
                const double norm = 1.0 / (0.5 + 0.5 * dl / stockcut::max(1.0, avgLen));
                scores[idx] += idf * recency * importance * norm;
            }
        }
        hits_.reserve(scores.size());
        for (const auto& kv : scores) {
            const auto mi = matchedTerms.find(kv.first);
            if (mi == matchedTerms.end() || mi->second < minimumOverlap) continue;
            hits_.push_back({kv.first, static_cast<u32>(stockcut::min(static_cast<double>(stockcut::INVALID_U32), kv.second * 1000.0))});
        }
        const auto end = hits_.begin() + static_cast<isize>(stockcut::min<usize>(topk, hits_.size()));
        std::partial_sort(hits_.begin(), end, hits_.end(), [](const Hit& a, const Hit& b) { return a.score_milli > b.score_milli; });
        if (hits_.size() > topk) hits_.resize(topk);
        return hits_;
    }
    const Hit* hit(u32 i) const noexcept { return i < hits_.size() ? &hits_[i] : nullptr; }
    const Entry* entry(u32 i) const noexcept { return i < entries_.size() ? &entries_[i] : nullptr; }
    usize size() const noexcept { return entries_.size(); }
    usize indexed_terms() const noexcept { return inverted_.size(); }
    usize memory_bytes() const noexcept {
        usize n = entries_.capacity() * sizeof(Entry) + hits_.capacity() * sizeof(Hit);
        for (const auto& e : entries_) n += e.text.capacity() + e.terms.capacity() * sizeof(u64);
        for (const auto& p : inverted_) n += sizeof(p) + p.second.capacity() * sizeof(u32);
        return n;
    }
    void clear() { entries_.clear(); inverted_.clear(); hits_.clear(); next_id_ = 1; }
    bool serialize(std::vector<u8>& out) const {
        out.clear(); append_header(out);
        for (const Entry& e : entries_) if (!make_record(e, out)) { out.clear(); return false; }
        return true;
    }
    bool deserialize(const u8* data, usize size) {
        if (!data || size < kHeaderBytes || size > kMaxFile) return false;
        u32 magic = 0, version = 0;
        std::memcpy(&magic, data, 4); std::memcpy(&version, data + 4, 4);
        if (magic == kLegacyMagic && version == 1) return deserialize_v1(data, size);
        usize p = 0; u32 hmagic = 0, ver = 0, hs = 0, hcrc = 0;
        if (!get(data, size, p, hmagic) || !get(data, size, p, ver) || !get(data, size, p, hs) || !get(data, size, p, hcrc) ||
            hmagic != kMagic || ver != kVersion || hs != kHeaderBytes || hcrc != stockcut::crc32_compute(data, 12)) return false;
        std::vector<Entry> fresh; fresh.reserve(256); u64 maxid = 0;
        recovered_damage_ = false;
        while (p < size) {
            const usize start = p;
            if (size - p < kRecordHeaderBytes) { recovered_damage_ = true; break; }
            u32 rm = 0, rs = 0, txtLen = 0, crc = 0; Entry e; u16 reserved = 0;
            if (!get(data, size, p, rm) || !get(data, size, p, rs) || rm != kRecordMagic || rs < kRecordHeaderBytes || rs > kRecordHeaderBytes + kMaxText) { recovered_damage_ = true; break; }
            if (!get(data, size, p, e.id) || !get(data, size, p, e.timestamp) || !get(data, size, p, e.session_id) || !get(data, size, p, e.project_id) ||
                !get(data, size, p, e.kind) || !get(data, size, p, e.importance) || !get(data, size, p, reserved) || !get(data, size, p, txtLen) || !get(data, size, p, crc)) { recovered_damage_ = true; break; }
            if (!txtLen || txtLen > kMaxText || rs != kRecordHeaderBytes + txtLen || p > size || txtLen > size - p) { recovered_damage_ = true; break; }
            const usize recordEnd = start + rs;
            const usize crcStart  = start + 8;
            const usize crcEnd    = start + 48;
            u32 computed = stockcut::crc32_compute(data + crcStart, crcEnd - crcStart);
            computed = stockcut::crc32_update(computed, data + p, recordEnd - p);
            if (computed == crc) {
                e.text.assign(reinterpret_cast<const char*>(data + p), txtLen); e.terms = terms_of(e.text.data(), e.text.size());
                if (!e.terms.empty() && fresh.size() < kMaxEntries) { maxid = stockcut::max(maxid, e.id); fresh.push_back(std::move(e)); }
            } else recovered_damage_ = true;
            p = recordEnd;
        }
        if (fresh.empty() && size > kHeaderBytes) return false;
        entries_.swap(fresh); next_id_ = maxid + 1; rebuild_index(); hits_.clear(); return true;
    }
    bool save_file(const char* path) const {
        if (!path || !*path) return false;
        std::vector<u8> b; if (!serialize(b)) return false;
        const std::string tmp = std::string(path) + ".tmp";
        { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if (!f) return false;
          f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); f.flush(); if (!f) return false; }
        if (std::rename(tmp.c_str(), path) != 0) {
            std::remove(path);
            if (std::rename(tmp.c_str(), path) != 0) { std::remove(tmp.c_str()); return false; }
        }
        return true;
    }
    bool load_file(const char* path) {
        if (!path || !*path) return false;
        std::ifstream f(path, std::ios::binary); if (!f) return false;
        std::vector<u8> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (b.size() > kMaxFile) return false;
        const bool ok = deserialize(b.data(), b.size());
        if (ok && recovered_damage_) {
            (void)save_file(path);
            recovered_damage_ = false;
        }
        return ok;
    }
};

} // namespace bitctx
