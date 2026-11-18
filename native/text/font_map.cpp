// ============================================================================
// native/text/font_map.cpp — Font Variant Mapping Table Implementation
// ============================================================================
#include "font_map.h"
#include "core/hash_primitives.h"
#include "core/string_parse.h"
#include "timeline/project/json_reader.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace stockcut {
namespace font {

namespace {
inline bool is_space_or_punct(char c) noexcept {
    return is_ascii_space(c) || c == '-' || c == '_' || c == '"' || c == '\'';
}

inline u32 hash_font_str(const char* str, usize len) noexcept {
    return (str && len > 0) ? fnv1a_32_bytes(str, len) : 0u;
}
} // namespace

u32 normalize_font_family_name(const char* in, char* out, u32 maxOut) noexcept {
    if (!in || !out || maxOut == 0) return 0;
    u32 inLen = static_cast<u32>(std::strlen(in));
    const char* start = in;
    if (inLen > 0 && (start[0] == '/' || start[0] == '\\')) {
        ++start;
        --inLen;
    }
    if (inLen >= 11 && cstr_equal_icase_n(start, "asset/font/", 11)) {
        start += 11;
        inLen -= 11;
    } else if (inLen >= 5 && cstr_equal_icase_n(start, "font/", 5)) {
        start += 5;
        inLen -= 5;
    }
    u32 effLen = inLen;
    for (u32 i = 0; i < effLen; ++i) {
        if (start[i] == '/' || start[i] == '\\') {
            effLen = i;
            break;
        }
    }
    if (effLen > 4 && start[effLen - 4] == '.') {
        effLen -= 4;
    } else if (effLen > 6 && start[effLen - 6] == '.') {
        effLen -= 6;
    }
    u32 outIdx = 0;
    for (u32 i = 0; i < effLen && outIdx + 1 < maxOut; ++i) {
        const char c = start[i];
        if (is_space_or_punct(c)) continue;
        out[outIdx++] = ascii_lower(c);
    }
    out[outIdx] = '\0';
    if (outIdx > 16 && (cstr_equal(out + outIdx - 16, "variablefontwght") || cstr_equal(out + outIdx - 16, "variablefontwdth"))) {
        outIdx -= 16;
        out[outIdx] = '\0';
    }
    if (outIdx == 0 || cstr_equal(out, "sansserif") || cstr_equal(out, "default") || cstr_equal(out, "systemui")) {
        out[0] = '\0';
        return 0;
    }
    return outIdx;
}

u32 canonical_font_filename(const char* in, char* out, u32 maxOut) noexcept {
    if (!in || !out || maxOut == 0) return 0;
    const u32 inLen = static_cast<u32>(std::strlen(in));
    const char* p = in;
    for (u32 i = 0; i < inLen; ++i) {
        if (in[i] == '/' || in[i] == '\\') {
            p = in + i + 1;
        }
    }
    u32 outIdx = 0;
    while (*p && outIdx + 1 < maxOut) {
        const char c = *p++;
        if (c == ' ' || c == '"' || c == '\'') continue;
        out[outIdx++] = ascii_lower(c);
    }
    out[outIdx] = '\0';
    if (outIdx >= 4) {
        const char* ext4 = out + (outIdx - 4);
        if (cstr_equal(ext4, ".ttf") || cstr_equal(ext4, ".otf")) {
            return outIdx;
        }
    }
    if (outIdx >= 6 && cstr_equal(out + (outIdx - 6), ".woff2")) {
        return outIdx;
    }
    if (outIdx + 4 < maxOut) {
        std::memcpy(out + outIdx, ".ttf", 4);
        outIdx += 4;
        out[outIdx] = '\0';
    }
    return outIdx;
}

FontMapRegistry& FontMapRegistry::instance() noexcept {
    static FontMapRegistry s_instance;
    return s_instance;
}

FontMapRegistry::FontMapRegistry() noexcept {
    reset();
    ensureBuffers();
    loadManifestFile(DEFAULT_FONT_MANIFEST_PATH);
}

FontMapRegistry::~FontMapRegistry() noexcept {
    if (fontArena_) {
        std::free(fontArena_);
        fontArena_ = nullptr;
    }
    if (uploadBuffer_) {
        std::free(uploadBuffer_);
        uploadBuffer_ = nullptr;
    }
}

void FontMapRegistry::ensureBuffers() noexcept {
    if (!fontArena_) {
        fontArena_ = static_cast<u8*>(std::malloc(MAX_FONT_ARENA_BYTES));
        if (fontArena_) std::memset(fontArena_, 0, MAX_FONT_ARENA_BYTES);
    }
    if (!uploadBuffer_) {
        uploadBuffer_ = static_cast<u8*>(std::malloc(MAX_FONT_UPLOAD_BUFFER));
        if (uploadBuffer_) std::memset(uploadBuffer_, 0, MAX_FONT_UPLOAD_BUFFER);
    }
}

void FontMapRegistry::reset() noexcept {
    familyCount_     = 0;
    fileCount_       = 0;
    loadedFileCount_ = 0;
    arenaOffset_     = 0;
    families_.clear();
    files_.clear();
}

void FontMapRegistry::updateDefaultFileIds(FontFamily& fam) noexcept {
    u32 bestNormalDist = INVALID_U32;
    u16 bestNormalId   = 0;
    u32 bestItalicDist = INVALID_U32;
    u16 bestItalicId   = 0;
    for (u32 i = 0; i < fam.variantCount; ++i) {
        const FontVariant& v = fam.variants[i];
        if (v.fontFileId == 0) continue;
        const u32 dist = (v.maxWeight > TARGET_REGULAR_WEIGHT)
            ? (v.maxWeight - TARGET_REGULAR_WEIGHT)
            : (TARGET_REGULAR_WEIGHT - v.maxWeight);
        if (v.isItalic == 0) {
            if (dist < bestNormalDist) {
                bestNormalDist = dist;
                bestNormalId   = v.fontFileId;
            }
        } else {
            if (dist < bestItalicDist) {
                bestItalicDist = dist;
                bestItalicId   = v.fontFileId;
            }
        }
    }
    fam.defaultNormalFileId = bestNormalId;
    fam.defaultItalicFileId = bestItalicId;
    if (fam.defaultNormalFileId == 0 && fam.defaultItalicFileId != 0) {
        fam.defaultNormalFileId = fam.defaultItalicFileId;
    } else if (fam.defaultItalicFileId == 0 && fam.defaultNormalFileId != 0) {
        fam.defaultItalicFileId = fam.defaultNormalFileId;
    }

    // Pre-bake O(1) Nearest-Weight Slot LUT [style: 0..1][weightSlot: 0..8 (100..900)]
    for (u32 style = 0; style < 2u; ++style) {
        for (u32 wSlot = 0; wSlot < 9u; ++wSlot) {
            const u32 targetW = (wSlot + 1u) * 100u;
            const FontVariant* bestMatch = nullptr;
            u32 bestDist = INVALID_U32;
            for (u32 v = 0; v < fam.variantCount; ++v) {
                const FontVariant& var = fam.variants[v];
                if (var.isItalic == style) {
                    const u32 dist = (var.maxWeight > targetW)
                        ? (var.maxWeight - targetW)
                        : (targetW - var.maxWeight);
                    if (dist < bestDist) {
                        bestDist  = dist;
                        bestMatch = &var;
                    }
                }
            }
            if (!bestMatch) {
                for (u32 v = 0; v < fam.variantCount; ++v) {
                    const FontVariant& var = fam.variants[v];
                    const u32 dist = (var.maxWeight > targetW)
                        ? (var.maxWeight - targetW)
                        : (targetW - var.maxWeight);
                    if (dist < bestDist) {
                        bestDist  = dist;
                        bestMatch = &var;
                    }
                }
            }
            fam.weightLut[style][wSlot] = bestMatch ? bestMatch->fontFileId : ((style != 0) ? fam.defaultItalicFileId : fam.defaultNormalFileId);
            fam.matchedWeightLut[style][wSlot] = bestMatch ? bestMatch->maxWeight : DEFAULT_FONT_WEIGHT;
        }
    }
}

void FontMapRegistry::sortVariants(FontFamily& family) noexcept {
    if (family.variantCount > 1) {
        for (u32 i = 0; i < family.variantCount - 1; ++i) {
            for (u32 j = 0; j < family.variantCount - 1 - i; ++j) {
                const bool swapNeeded =
                    (family.variants[j].isItalic > family.variants[j + 1].isItalic) ||
                    (family.variants[j].isItalic == family.variants[j + 1].isItalic &&
                     family.variants[j].maxWeight > family.variants[j + 1].maxWeight);
                if (swapNeeded) {
                    const FontVariant temp = family.variants[j];
                    family.variants[j] = family.variants[j + 1];
                    family.variants[j + 1] = temp;
                }
            }
        }
    }
    updateDefaultFileIds(family);
}

void FontMapRegistry::linkVariantsToFiles() noexcept {
    for (u32 f = 0; f < familyCount_; ++f) {
        FontFamily& fam = families_[f];
        for (u32 v = 0; v < fam.variantCount; ++v) {
            FontVariant& var = fam.variants[v];
            if (var.fontFileId != 0) continue;
            for (u32 i = 0; i < fileCount_; ++i) {
                if (files_[i].pathHash == var.pathHash) {
                    var.fontFileId = files_[i].fileId;
                    break;
                }
            }
        }
        updateDefaultFileIds(fam);
    }
}

namespace {
bool readStringField(
    const timeline::project::JsonReader& json,
    u32 objectStart,
    const char* fieldName,
    char* output,
    u32 capacity) noexcept {
    u32 valStart = 0, valEnd = 0;
    if (!json.field(objectStart, fieldName, valStart, valEnd)) return false;
    return json.readStringValue(valStart, valEnd, output, capacity);
}

bool readBoolField(
    const timeline::project::JsonReader& json,
    u32 objectStart,
    const char* fieldName,
    bool& result) noexcept {
    u32 vStart = 0, vEnd = 0;
    if (!json.field(objectStart, fieldName, vStart, vEnd)) return false;
    MultiArray<char, 16> buf{};
    u32 used = 0;
    if (json.copySpan(vStart, vEnd, buf.data(), 16, used)) {
        if (cstr_equal(buf.data(), "true") || cstr_equal(buf.data(), "1")) {
            result = true;
            return true;
        }
        if (cstr_equal(buf.data(), "false") || cstr_equal(buf.data(), "0")) {
            result = false;
            return true;
        }
    }
    return false;
}
} // namespace

void FontMapRegistry::parseManifestVariantsArray(
    const void* readerPtr,
    u32 varsStart,
    u32 varsEnd,
    FontFamily& fam
) noexcept {
    const auto& reader = *static_cast<const timeline::project::JsonReader*>(readerPtr);
    if (!reader.isArray(varsStart, varsEnd)) return;
    u32 vpos = varsStart + 1;
    u32 varItemStart = 0, varItemEnd = 0;
    while (vpos < varsEnd && fam.variantCount < MAX_FONT_VARIANTS_PER_FAMILY &&
           reader.nextArrayValue(vpos, varsEnd, varItemStart, varItemEnd)) {
        if (!reader.isObject(varItemStart, varItemEnd)) continue;
        double weightVal = static_cast<double>(DEFAULT_FONT_WEIGHT);
        reader.numberField(varItemStart, "weight", weightVal);

        MultiArray<char, FONT_PATH_CAP> rawFile{};
        if (!readStringField(reader, varItemStart, "file", rawFile.data(), FONT_PATH_CAP) || !rawFile[0]) continue;

        MultiArray<char, FONT_PATH_CAP> filePath{};
        if (cstr_equal_icase_n(rawFile.data(), "Asset/font/", 11) || rawFile[0] == '/') {
            copy_cstr_bounded(filePath.data(), FONT_PATH_CAP, rawFile.data());
        } else {
            std::snprintf(filePath.data(), FONT_PATH_CAP, "Asset/font/%s/%s", fam.dir.data(), rawFile.data());
        }

        bool italicVal = false;
        readBoolField(reader, varItemStart, "italic", italicVal);

        MultiArray<char, FONT_PATH_CAP> canonName{};
        const u32 canonLen = canonical_font_filename(filePath.data(), canonName.data(), FONT_PATH_CAP);
        const u32 pHash = hash_font_str(canonName.data(), canonLen);
        u16 fileId = 0;
        for (u32 fi = 0; fi < fileCount_; ++fi) {
            if (files_[fi].pathHash == pHash) {
                fileId = files_[fi].fileId;
                break;
            }
        }
        if (fileId == 0 && fileCount_ < MAX_FONT_FILES) {
            fileId = static_cast<u16>(fileCount_ + 1);
            FontFileRecord& rec = files_[fileCount_++];
            rec.fileId = fileId;
            copy_cstr_bounded(rec.path.data(), FONT_PATH_CAP, filePath.data());
            rec.pathHash  = pHash;
            rec.byteCount = 0;
            rec.fontKey   = fileId;
            rec.buffer    = nullptr;
        }
        FontVariant& v = fam.variants[fam.variantCount++];
        v.maxWeight = static_cast<u16>(clamp(weightVal, 10.0, 1000.0));
        v.isItalic  = italicVal ? 1u : 0u;
        v.reserved  = 0;
        copy_cstr_bounded(v.filePath.data(), FONT_PATH_CAP, filePath.data());
        v.pathHash   = pHash;
        v.fontFileId = fileId;
    }
}

bool FontMapRegistry::loadManifestJson(const char* json, u32 length) noexcept {
    if (!json || length == 0) return false;
    ensureBuffers();
    timeline::project::JsonReader reader(json, length);
    familyCount_ = 0;

    if (reader.isArray(0, length)) {
        u32 pos = 1, itemStart = 0, itemEnd = 0;
        while (pos < length && familyCount_ < MAX_FONT_FAMILIES &&
               reader.nextArrayValue(pos, length, itemStart, itemEnd)) {
            if (!reader.isObject(itemStart, itemEnd)) continue;
            MultiArray<char, FONT_NAME_CAP> rawFamilyName{};
            if (!readStringField(reader, itemStart, "family", rawFamilyName.data(), FONT_NAME_CAP) || !rawFamilyName[0]) continue;
            MultiArray<char, FONT_NAME_CAP> rawDirName{};
            if (!readStringField(reader, itemStart, "dir", rawDirName.data(), FONT_NAME_CAP) || !rawDirName[0]) {
                copy_cstr_bounded(rawDirName.data(), FONT_NAME_CAP, rawFamilyName.data());
            }

            FontFamily& fam = families_[familyCount_];
            fam = {};
            copy_cstr_bounded(fam.displayName.data(), FONT_NAME_CAP, rawFamilyName.data());
            copy_cstr_bounded(fam.dir.data(), FONT_NAME_CAP, rawDirName.data());
            const u32 normLen = normalize_font_family_name(rawFamilyName.data(), fam.name.data(), FONT_NAME_CAP);
            fam.nameHash = hash_font_str(fam.name.data(), normLen);
            MultiArray<char, FONT_NAME_CAP> normDir{};
            const u32 normDirLen = normalize_font_family_name(rawDirName.data(), normDir.data(), FONT_NAME_CAP);
            fam.dirHash = hash_font_str(normDir.data(), normDirLen);

            u32 varsStart = 0, varsEnd = 0;
            if (reader.field(itemStart, "variants", varsStart, varsEnd)) {
                parseManifestVariantsArray(&reader, varsStart, varsEnd, fam);
            }
            sortVariants(fam);
            ++familyCount_;
        }
    }

    linkVariantsToFiles();
    loadAllManifestFonts();
    return familyCount_ > 0;
}

bool FontMapRegistry::readFirstExistingFile(
    const char* const* candidates,
    usize candidateCount,
    u32 maxBytes,
    u32& outReadBytes
) noexcept {
    outReadBytes = 0;
    FILE* f = nullptr;
    for (usize i = 0; i < candidateCount; ++i) {
        const char* p = candidates[i];
        if (!p || !p[0]) continue;
        f = std::fopen(p, "rb");
        if (f) break;
    }
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > static_cast<long>(maxBytes)) {
        std::fclose(f);
        return false;
    }
    ensureBuffers();
    if (!uploadBuffer_) {
        std::fclose(f);
        return false;
    }
    const usize readBytes = std::fread(uploadBuffer_, 1, static_cast<usize>(size), f);
    std::fclose(f);
    outReadBytes = static_cast<u32>(readBytes);
    return outReadBytes > 0;
}

bool FontMapRegistry::loadManifestFile(const char* filePath) noexcept {
    MultiArray<const char*, 4> candidates = {{
        filePath,
        DEFAULT_FONT_MANIFEST_PATH,
        "/Asset/font/fonts.json",
        "../Asset/font/fonts.json"
    }};
    u32 readBytes = 0;
    if (!readFirstExistingFile(candidates.data(), candidates.size(), MAX_FONT_MANIFEST_BYTES, readBytes)) {
        return false;
    }
    return loadManifestJson(reinterpret_cast<const char*>(uploadBuffer_), readBytes);
}

u16 FontMapRegistry::loadFontFileDirect(const char* filePath) noexcept {
    if (!filePath || !filePath[0]) return 0;
    MultiArray<char, FONT_ALT_PATH_CAP> alt1{};
    MultiArray<char, FONT_ALT_PATH_CAP> alt2{};
    if (filePath[0] == '/') {
        std::snprintf(alt1.data(), FONT_ALT_PATH_CAP, "%s", filePath + 1);
    } else {
        std::snprintf(alt1.data(), FONT_ALT_PATH_CAP, "/%s", filePath);
    }
    std::snprintf(alt2.data(), FONT_ALT_PATH_CAP, "../%s", (filePath[0] == '/') ? filePath + 1 : filePath);
    MultiArray<const char*, 3> candidates = {{ filePath, alt1.data(), alt2.data() }};
    u32 readBytes = 0;
    if (!readFirstExistingFile(candidates.data(), candidates.size(), MAX_FONT_UPLOAD_BUFFER, readBytes)) {
        return 0;
    }
    return registerFontFile(filePath, uploadBuffer_, readBytes);
}

u32 FontMapRegistry::loadAllManifestFonts() noexcept {
    u32 count = 0;
    for (u32 i = 0; i < fileCount_; ++i) {
        if (!files_[i].buffer && files_[i].path[0]) {
            if (loadFontFileDirect(files_[i].path.data()) != 0) {
                ++count;
            }
        }
    }
    return count;
}

u16 FontMapRegistry::registerFontFile(const char* path, const u8* data, u32 byteCount) noexcept {
    if (!path || !data || byteCount == 0) return 0;
    ensureBuffers();
    if (!fontArena_) return 0;
    MultiArray<char, FONT_PATH_CAP> canonName{};
    const u32 canonLen = canonical_font_filename(path, canonName.data(), FONT_PATH_CAP);
    const u32 pathHash = hash_font_str(canonName.data(), canonLen);

    auto commitToArena = [&](FontFileRecord& rec) noexcept -> bool {
        if (arenaOffset_ + byteCount > MAX_FONT_ARENA_BYTES) return false;
        u8* targetBuf = fontArena_ + arenaOffset_;
        std::memcpy(targetBuf, data, byteCount);
        arenaOffset_ = (arenaOffset_ + byteCount + FONT_ARENA_ALIGN_MASK) & ~FONT_ARENA_ALIGN_MASK;
        rec.buffer    = targetBuf;
        rec.byteCount = byteCount;
        rec.fontKey   = fnv1a_32_bytes(data, byteCount);
        ++loadedFileCount_;
        linkVariantsToFiles();
        return true;
    };

    for (u32 i = 0; i < fileCount_; ++i) {
        if (files_[i].pathHash == pathHash) {
            if (files_[i].buffer && files_[i].byteCount > 0) {
                return files_[i].fileId;
            }
            return commitToArena(files_[i]) ? files_[i].fileId : 0;
        }
    }
    if (fileCount_ >= MAX_FONT_FILES) return 0;
    const u16 newFileId = static_cast<u16>(fileCount_ + 1);
    FontFileRecord& rec = files_[fileCount_];
    rec.fileId   = newFileId;
    copy_cstr_bounded(rec.path.data(), FONT_PATH_CAP, path);
    rec.pathHash = pathHash;
    if (!commitToArena(rec)) return 0;
    ++fileCount_;
    return newFileId;
}

template <bool IsItalic>
FontVariantMatch FontMapRegistry::lookup(const char* familyName, u16 weight) const noexcept {
    if (familyCount_ == 0) {
        if (fileCount_ > 0) return lookupByFileId(files_[0].fileId);
        return {0, DEFAULT_FONT_WEIGHT, nullptr, 0, 0, ""};
    }
    if (!familyName || !familyName[0]) {
        return lookupByHash<IsItalic>(0, weight);
    }
    MultiArray<char, FONT_NAME_CAP> normName{};
    const u32 normLen = normalize_font_family_name(familyName, normName.data(), FONT_NAME_CAP);
    const u32 hash = hash_font_str(normName.data(), normLen);
    for (u32 i = 0; i < familyCount_; ++i) {
        if (families_[i].nameHash == hash || families_[i].dirHash == hash) {
            return lookupByHash<IsItalic>(families_[i].nameHash, weight);
        }
    }
    MultiArray<char, FONT_PATH_CAP> canonQuery{};
    const u32 queryLen = canonical_font_filename(familyName, canonQuery.data(), FONT_PATH_CAP);
    const u32 queryHash = hash_font_str(canonQuery.data(), queryLen);
    for (u32 i = 0; i < fileCount_; ++i) {
        if (files_[i].pathHash == queryHash) {
            return lookupByFileId(files_[i].fileId);
        }
    }
    return lookupByHash<IsItalic>(hash, weight);
}

template <bool IsItalic>
FontVariantMatch FontMapRegistry::lookupByHash(u32 familyHash, u16 weight) const noexcept {
    if (familyCount_ == 0) {
        if (fileCount_ > 0) return lookupByFileId(files_[0].fileId);
        return {0, DEFAULT_FONT_WEIGHT, nullptr, 0, 0, ""};
    }
    const FontFamily* matchedFamily = nullptr;
    if (familyHash != 0) {
        for (u32 i = 0; i < familyCount_; ++i) {
            if (families_[i].nameHash == familyHash || families_[i].dirHash == familyHash) {
                matchedFamily = &families_[i];
                break;
            }
        }
    }
    if (!matchedFamily) {
        matchedFamily = &families_[0];
    }

    constexpr u32 kStyleIdx = IsItalic ? 1u : 0u;
    const u32 wSlot = (weight == 0)
        ? 3u
        : (clamp<u32>((static_cast<u32>(weight) + 50u) / 100u, 1u, 9u) - 1u);

    const u16 lutFileId = matchedFamily->weightLut[kStyleIdx][wSlot];
    const u16 matchedW  = matchedFamily->matchedWeightLut[kStyleIdx][wSlot];

    if (lutFileId != 0 && lutFileId <= fileCount_) {
        if (!files_[lutFileId - 1].buffer && files_[lutFileId - 1].path[0]) {
            const_cast<FontMapRegistry*>(this)->loadFontFileDirect(files_[lutFileId - 1].path.data());
        }
        FontVariantMatch m = lookupByFileId(lutFileId);
        m.matchedWeight = matchedW;
        if (m.buffer != nullptr) return m;
    }

    const u16 defId = IsItalic ? matchedFamily->defaultItalicFileId : matchedFamily->defaultNormalFileId;
    if (defId != 0) {
        FontVariantMatch defMatch = lookupByFileId(defId);
        if (defMatch.buffer != nullptr) return defMatch;
    }

    for (u32 i = 0; i < fileCount_; ++i) {
        if (files_[i].buffer != nullptr) {
            return lookupByFileId(files_[i].fileId);
        }
    }

    if (lutFileId != 0) {
        FontVariantMatch m = lookupByFileId(lutFileId);
        m.matchedWeight = matchedW;
        return m;
    }

    if (defId != 0) return lookupByFileId(defId);
    return {0, DEFAULT_FONT_WEIGHT, nullptr, 0, 0, ""};
}

template FontVariantMatch FontMapRegistry::lookup<false>(const char*, u16) const noexcept;
template FontVariantMatch FontMapRegistry::lookup<true>(const char*, u16) const noexcept;
template FontVariantMatch FontMapRegistry::lookupByHash<false>(u32, u16) const noexcept;
template FontVariantMatch FontMapRegistry::lookupByHash<true>(u32, u16) const noexcept;

FontVariantMatch FontMapRegistry::lookupByFileId(u16 fileId) const noexcept {
    if (fileId == 0 || fileId > fileCount_) {
        return {0, DEFAULT_FONT_WEIGHT, nullptr, 0, 0, ""};
    }
    const FontFileRecord& f = files_[fileId - 1];
    if (!f.buffer && f.path[0]) {
        const_cast<FontMapRegistry*>(this)->loadFontFileDirect(f.path.data());
    }
    return {f.fileId, DEFAULT_FONT_WEIGHT, f.buffer, f.byteCount, f.fontKey, f.path.data()};
}

const FontFileRecord* FontMapRegistry::getFileRecord(u16 fileId) const noexcept {
    return (fileId == 0 || fileId > fileCount_) ? nullptr : &files_[fileId - 1];
}

const u8* FontMapRegistry::getFileBuffer(u16 fileId) const noexcept {
    const FontFileRecord* r = getFileRecord(fileId);
    return r ? r->buffer : nullptr;
}

u32 FontMapRegistry::getFileSize(u16 fileId) const noexcept {
    const FontFileRecord* r = getFileRecord(fileId);
    return r ? r->byteCount : 0;
}

u32 FontMapRegistry::getFileKey(u16 fileId) const noexcept {
    const FontFileRecord* r = getFileRecord(fileId);
    return r ? r->fontKey : 0;
}

u8* FontMapRegistry::getUploadBuffer() noexcept {
    ensureBuffers();
    return uploadBuffer_;
}

u32 FontMapRegistry::getUploadCapacity() const noexcept {
    return MAX_FONT_UPLOAD_BUFFER;
}

u16 FontMapRegistry::commitUpload(const char* path, u32 pathLen, u32 byteCount) noexcept {
    if (!path || pathLen == 0 || byteCount == 0 || byteCount > MAX_FONT_UPLOAD_BUFFER) return 0;
    ensureBuffers();
    if (!uploadBuffer_) return 0;
    MultiArray<char, FONT_PATH_CAP> pathBuf{};
    const u32 len = min(pathLen, static_cast<u32>(FONT_PATH_CAP - 1));
    std::memcpy(pathBuf.data(), path, len);
    pathBuf[len] = '\0';
    return registerFontFile(pathBuf.data(), uploadBuffer_, byteCount);
}

} // namespace font
} // namespace stockcut
