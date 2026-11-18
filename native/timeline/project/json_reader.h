// ============================================================================
// [ĐÁNH GIÁ ĐIỂM NGHẼN HỆ THỐNG]:
// 1. Duyệt cây phân cấp đệ quy trong Hot Path 60 FPS:
//    - Duyệt qua lại các node timeline làm phát sinh cache miss và chi phí con trỏ.
// 2. Kế thừa template lặp lại ở mỗi khung hình:
//    - Hỏi quan hệ cha-con liên tục làm giảm hiệu suất xử lý khung hình.
//
// [HƯỚNG KHẮC PHỤC TRIỆT ĐỂ — KHÔNG VÁ]:
// 1. Kế thừa 1 lần duy nhất ở Cold Path & Bộ nhớ chia sẻ:
//    - Con trỏ trực tiếp vào bộ nhớ phẳng của cha, đọc O(1) không duyệt cây.
// 2. Bitboard Layer Masking & Ngăn xếp lịch sử StateInfo:
//    - Lọc trạng thái layer bằng bitmask 64-bit và lưu vết vi sai kiểu Stockfish.
// ============================================================================
#pragma once
// Bottleneck: each field query rescans the JSON span.
// Optimization path: cache tokens/spans once, then specialize common value readers.
// Runtime branches remain for JSON syntax and malformed-input validation.
#include "types.h"
namespace stockcut::timeline::project {
constexpr u32 JSON_MAX_NESTING_DEPTH = 64;
struct JsonValueSpan {
    u32 start = 0;
    u32 end = 0;
    bool found = false;
};
class JsonReader {
public:
    // Creates a non-owning reader over one caller-owned JSON byte span.
    JsonReader(const char* data, u32 size);
    // Finds a named member and returns its original source span.
    bool field(u32 objectStart, const char* name, u32& valueStart, u32& valueEnd) const;
    // Advances an array cursor and returns the next value's source span.
    bool nextArrayValue(u32& pos, u32 arrayEnd, u32& valueStart, u32& valueEnd) const;
    // Advances an object cursor and returns the quoted key span and value span.
    bool nextObjectField(
        u32& pos, u32 objectEnd,
        u32& keyStart, u32& keyEnd,
        u32& valueStart, u32& valueEnd
    ) const;
    // Tests whether a span contains a JSON array.
    bool isArray(u32 start, u32 end) const;
    // Tests whether a span contains a JSON object.
    bool isObject(u32 start, u32 end) const;
    // Validates that the complete source span is one JSON object.
    bool isObjectDocument() const;
    // Compares the decoded contents of a bounded JSON string span.
    bool stringValueEquals(u32 start, u32 end, const char* expected) const;
    // Decodes a bounded JSON string span into caller-provided storage.
    bool readStringValue(u32 start, u32 end, char* output, u32 capacity) const;
    // Copies a bounded source span verbatim into caller-provided storage.
    bool copySpan(u32 start, u32 end, char* output, u32 capacity, u32& used) const;
    // Compares a named string field without dynamic allocation.
    bool stringFieldEquals(u32 objectStart, const char* fieldName, const char* expected) const;
    // Parses a named numeric-coercible member.
    bool numberField(u32 objectStart, const char* fieldName, double& result) const;
    // Parses a numeric-coercible value from its source span.
    bool valueNumber(u32 start, u32 end, double& result) const;
    // Detects whether a value is a percent-suffixed string.
    bool valueIsPercent(u32 start, u32 end) const;
    // Converts numeric or percent-formatted time into milliseconds.
    bool timeValue(u32 start, u32 end, double& result) const;
private:
    const char* data_;
    u32 size_;
    // Classifies legal delimiters following a scalar value.
    static bool isValueDelimiter(char ch);
    // Advances a source cursor past JSON whitespace.
    void whitespace(u32& pos) const;
    // Finds and validates the content/end of a quoted JSON string.
    bool stringBounds(u32 start, u32& contentStart, u32& end) const;
    // Compares a JSON string's decoded contents to an ASCII field name.
    bool stringEquals(u32 contentStart, u32 end, const char* expected) const;
    // Decodes a JSON string into caller-provided fixed storage.
    bool readString(u32 start, u32 end, char* out, u32 capacity) const;
    // Validates and skips a nested JSON value.
    bool skipValue(u32 start, u32& end, u32 depth) const;
    // Validates an exact JSON literal token.
    bool literal(u32 start, const char* expected, u32& end) const;
    // Validates the grammar of one JSON number.
    bool jsonNumber(u32 start, u32& end) const;
    // Applies Number-compatible parsing to a JSON scalar.
    bool number(u32 start, u32 end, double& result) const;
    // Parses a decimal percentage into a numeric value.
    static bool decimalPercent(const char* text, double& result);
};
} // namespace stockcut::timeline::project
