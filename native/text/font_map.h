// ============================================================================
// native/text/font_map.h — Font Variant Mapping Table
// ============================================================================
#pragma once
#include "types.h"
#include "data_structures.h"

namespace stockcut {
namespace font {

constexpr u32   MAX_FONT_ARENA_BYTES         = 16u * 1024u * 1024u;
constexpr u32   MAX_FONT_FAMILIES            = 32u;
constexpr u32   MAX_FONT_VARIANTS_PER_FAMILY = 16u;
constexpr u32   MAX_FONT_FILES               = 64u;
constexpr u32   MAX_FONT_UPLOAD_BUFFER       = 8u * 1024u * 1024u;
constexpr u32   MAX_FONT_MANIFEST_BYTES      = 2u * 1024u * 1024u;
constexpr usize FONT_NAME_CAP                = 64;
constexpr usize FONT_PATH_CAP                = 128;
constexpr usize FONT_ALT_PATH_CAP            = 256;
constexpr u16   DEFAULT_FONT_WEIGHT          = 400u;
constexpr u32   TARGET_REGULAR_WEIGHT        = 450u;
constexpr u32   FONT_ARENA_ALIGN_MASK        = 15u;

struct FontVariant {
    u16 maxWeight;
    u8  isItalic;
    u8  reserved;
    u16 fontFileId;
    u32 pathHash;
    MultiArray<char, FONT_PATH_CAP> filePath;
};

struct FontFamily {
    MultiArray<char, FONT_NAME_CAP> name;
    MultiArray<char, FONT_NAME_CAP> displayName;
    MultiArray<char, FONT_NAME_CAP> dir;
    u32 nameHash;
    u32 dirHash;
    u16 variantCount;
    u16 defaultNormalFileId;
    u16 defaultItalicFileId;
    MultiArray<u16, 2, 9> weightLut;
    MultiArray<u16, 2, 9> matchedWeightLut;
    MultiArray<FontVariant, MAX_FONT_VARIANTS_PER_FAMILY> variants;
};

struct FontFileRecord {
    u16 fileId;
    MultiArray<char, FONT_PATH_CAP> path;
    u32 pathHash;
    u32 byteCount;
    u32 fontKey;
    const u8* buffer;
};

struct FontVariantMatch {
    u16 fileId;
    u16 matchedWeight;
    const u8* buffer;
    u32 byteCount;
    u32 fontKey;
    const char* filePath;
};

class FontMapRegistry {
public:
    static FontMapRegistry& instance() noexcept;

    bool loadManifestJson(const char* json, u32 length) noexcept;
    bool loadManifestFile(const char* filePath = DEFAULT_FONT_MANIFEST_PATH) noexcept;
    u16  loadFontFileDirect(const char* filePath) noexcept;
    u32  loadAllManifestFonts() noexcept;
    u16  registerFontFile(const char* path, const u8* data, u32 byteCount) noexcept;

    template <bool IsItalic = false>
    FontVariantMatch lookup(const char* familyName, u16 weight) const noexcept;

    template <bool IsItalic = false>
    FontVariantMatch lookupByHash(u32 familyHash, u16 weight) const noexcept;

    FontVariantMatch lookupByFileId(u16 fileId) const noexcept;

    template <bool IsItalic = false>
    u16 resolveFontFileId(const char* familyName, u16 weight) const noexcept {
        return lookup<IsItalic>(familyName, weight).fileId;
    }

    template <bool IsItalic = false>
    u16 resolveFontFileIdByHash(u32 familyHash, u16 weight) const noexcept {
        return lookupByHash<IsItalic>(familyHash, weight).fileId;
    }

    const FontFileRecord* getFileRecord(u16 fileId) const noexcept;
    const u8* getFileBuffer(u16 fileId) const noexcept;
    u32 getFileSize(u16 fileId) const noexcept;
    u32 getFileKey(u16 fileId) const noexcept;

    u8* getUploadBuffer() noexcept;
    u32 getUploadCapacity() const noexcept;
    u16 commitUpload(const char* path, u32 pathLen, u32 byteCount) noexcept;

    u32 familyCount() const noexcept { return familyCount_; }
    u32 fileCount() const noexcept { return fileCount_; }
    u32 loadedFileCount() const noexcept { return loadedFileCount_; }
    void reset() noexcept;

private:
    FontMapRegistry() noexcept;
    ~FontMapRegistry() noexcept;

    u8* fontArena_ = nullptr;
    u8* uploadBuffer_ = nullptr;
    u32 arenaOffset_ = 0;

    MultiArray<FontFamily, MAX_FONT_FAMILIES> families_; // KHONG CO {} (reset() in ctor)
    u32 familyCount_ = 0;
    MultiArray<FontFileRecord, MAX_FONT_FILES> files_;   // KHONG CO {} (reset() in ctor)
    u32 fileCount_ = 0;
    u32 loadedFileCount_ = 0;

    void ensureBuffers() noexcept;
    void sortVariants(FontFamily& family) noexcept;
    void updateDefaultFileIds(FontFamily& family) noexcept;
    void linkVariantsToFiles() noexcept;
    void parseManifestVariantsArray(
        const void* readerPtr,
        u32 varsStart,
        u32 varsEnd,
        FontFamily& fam
    ) noexcept;
    bool readFirstExistingFile(
        const char* const* candidates,
        usize candidateCount,
        u32 maxBytes,
        u32& outReadBytes
    ) noexcept;
};

u32 normalize_font_family_name(const char* in, char* out, u32 maxOut) noexcept;
u32 canonical_font_filename(const char* in, char* out, u32 maxOut) noexcept;

} // namespace font
} // namespace stockcut
