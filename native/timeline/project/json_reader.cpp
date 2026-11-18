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
// Bottleneck: JSON fields are found by rescanning each object; nested objects are
// recursively validated. Optimization path: tokenize once into spans and dispatch
// type-specific field decoding through compile-time readers after schema stabilizes.
#include "json_reader.h"
#include "data_structures.h"
#include "string_parse.h"
namespace stockcut::timeline::project
{
    namespace
    {
        constexpr u32 JSON_SMALL_STR_CAP       = 32u;
        constexpr u32 JSON_SCALAR_STR_CAP      = 128u;
        constexpr u8  JSON_ASCII_CONTROL_LIMIT = 0x20u;
        constexpr u32 JSON_ASCII_MAX_CODE      = 0x7Fu;
        constexpr u8  JSON_NON_ASCII_MIN       = 0x80u;
        constexpr u32 JSON_UNICODE_HEX_DIGITS  = 4u;
        constexpr u32 JSON_MAX_EXPONENT        = 400u;
        constexpr double JSON_PERCENT_DIVISOR  = 100.0;

        template <bool StrictAscii>
        bool decodeJsonChar(const char *data, u32 &pos, u32 contentLimit, char &outCh) noexcept
        {
            char ch = data[pos++];
            if (ch != '\\')
            {
                outCh = ch;
                return true;
            }
            if (pos >= contentLimit)
                return false;
            const char escaped = data[pos++];
            switch (escaped)
            {
            case '"':
            case '\\':
            case '/':
                outCh = escaped;
                return true;
            case 'b':
                outCh = '\b';
                return true;
            case 'f':
                outCh = '\f';
                return true;
            case 'n':
                outCh = '\n';
                return true;
            case 'r':
                outCh = '\r';
                return true;
            case 't':
                outCh = '\t';
                return true;
            case 'u':
            {
                if (pos + JSON_UNICODE_HEX_DIGITS > contentLimit)
                    return false;
                u32 code = 0;
                for (u32 i = 0; i < JSON_UNICODE_HEX_DIGITS; ++i)
                {
                    const i32 nib = hex_nibble(data[pos++]);
                    if (nib < 0)
                        return false;
                    code = (code << 4) | static_cast<u32>(nib);
                }
                if constexpr (StrictAscii)
                {
                    if (code > JSON_ASCII_MAX_CODE)
                        return false;
                    outCh = static_cast<char>(code);
                }
                else
                {
                    outCh = (code <= JSON_ASCII_MAX_CODE) ? static_cast<char>(code) : '?';
                }
                return true;
            }
            default:
                return false;
            }
        }

        bool extractPercentSlice(const char *raw, u32 &outFirst, u32 &outNumericEnd) noexcept
        {
            u32 first = 0;
            while (is_ascii_space(raw[first]))
                ++first;
            u32 last = first;
            while (raw[last])
                ++last;
            while (last > first && is_ascii_space(raw[last - 1]))
                --last;
            if (last <= first || raw[last - 1] != '%')
                return false;
            u32 numericEnd = last - 1;
            while (numericEnd > first &&
                   (is_ascii_space(raw[numericEnd - 1]) ||
                    static_cast<u8>(raw[numericEnd - 1]) >= JSON_NON_ASCII_MIN))
            {
                --numericEnd;
            }
            outFirst = first;
            outNumericEnd = numericEnd;
            return true;
        }
    } // namespace

    // Creates a non-owning reader over one caller-owned JSON byte span.
    JsonReader::JsonReader(const char *data, u32 size) : data_(data), size_(size) {}
    // Finds an object member while validating the syntax of each preceding value.
    bool JsonReader::field(u32 objectStart, const char *name, u32 &valueStart,
                           u32 &valueEnd) const
    {
        u32 pos = objectStart;
        whitespace(pos);
        if (pos >= size_ || data_[pos++] != '{')
            return false;
        whitespace(pos);
        bool found = false;
        while (pos < size_ && data_[pos] != '}')
        {
            u32 keyStart = 0, keyEnd = 0;
            if (!stringBounds(pos, keyStart, keyEnd))
                return false;
            pos = keyEnd;
            whitespace(pos);
            if (pos >= size_ || data_[pos++] != ':')
                return false;
            whitespace(pos);
            const u32 currentStart = pos;
            u32 currentEnd = 0;
            if (!skipValue(pos, currentEnd, 0))
                return false;
            if (stringEquals(keyStart, keyEnd, name))
            {
                valueStart = currentStart;
                valueEnd = currentEnd;
                found = true;
            }
            pos = currentEnd;
            whitespace(pos);
            if (pos < size_ && data_[pos] == ',')
            {
                ++pos;
                whitespace(pos);
            }
            else if (pos >= size_ || data_[pos] != '}')
            {
                return false;
            }
        }
        return pos < size_ && data_[pos] == '}' && found;
    }
    // Advances an array cursor to the next validated value span without copying it.
    bool JsonReader::nextArrayValue(u32 &pos, u32 arrayEnd, u32 &valueStart,
                                    u32 &valueEnd) const
    {
        whitespace(pos);
        if (pos >= arrayEnd || data_[pos] == ']')
            return false;
        valueStart = pos;
        if (!skipValue(pos, valueEnd, 0) || valueEnd > arrayEnd)
            return false;
        pos = valueEnd;
        whitespace(pos);
        if (pos < arrayEnd && data_[pos] == ',')
            ++pos;
        return true;
    }
    // Advances an object cursor and returns one key/value span without copying either.
    bool JsonReader::nextObjectField(
        u32 &pos, u32 objectEnd,
        u32 &keyStart, u32 &keyEnd,
        u32 &valueStart, u32 &valueEnd) const
    {
        whitespace(pos);
        if (pos < objectEnd && data_[pos] == '}')
            return false;
        const u32 quotedKeyStart = pos;
        if (!stringBounds(pos, keyStart, keyEnd))
            return false;
        keyStart = quotedKeyStart;
        pos = keyEnd;
        whitespace(pos);
        if (pos >= objectEnd || data_[pos++] != ':')
            return false;
        whitespace(pos);
        valueStart = pos;
        if (!skipValue(pos, valueEnd, 0) || valueEnd > objectEnd)
            return false;
        pos = valueEnd;
        whitespace(pos);
        if (pos < objectEnd && data_[pos] == ',')
            ++pos;
        return true;
    }
    // Tests whether a bounded JSON value span begins with an array token.
    bool JsonReader::isArray(u32 start, u32 end) const
    {
        whitespace(start);
        return start < end && data_[start] == '[';
    }
    // Tests whether a bounded JSON value span begins with an object token.
    bool JsonReader::isObject(u32 start, u32 end) const
    {
        whitespace(start);
        return start < end && data_[start] == '{';
    }
    // Validates that the entire input span contains exactly one JSON object.
    bool JsonReader::isObjectDocument() const
    {
        u32 start = 0, end = 0;
        whitespace(start);
        if (start >= size_ || data_[start] != '{' || !skipValue(start, end, 0))
            return false;
        whitespace(end);
        return end == size_;
    }
    // Compares a JSON string span after decoding its escapes.
    bool JsonReader::stringValueEquals(u32 start, u32 end, const char *expected) const
    {
        u32 contentStart = 0;
        u32 stringEnd = 0;
        return stringBounds(start, contentStart, stringEnd) &&
               stringEnd == end && stringEquals(contentStart, stringEnd, expected);
    }
    // Decodes a JSON string span into caller-provided storage.
    bool JsonReader::readStringValue(
        u32 start, u32 end, char *output, u32 capacity) const
    {
        return readString(start, end, output, capacity);
    }
    // Copies a source slice without interpreting it.
    bool JsonReader::copySpan(
        u32 start, u32 end, char *output, u32 capacity, u32 &used) const
    {
        if (start > end || end > size_ || used > capacity ||
            end - start > capacity - used)
            return false;
        for (u32 i = start; i < end; ++i)
            output[used++] = data_[i];
        return true;
    }
    // Reads and compares a string-valued member without heap allocation.
    bool JsonReader::stringFieldEquals(u32 objectStart, const char *fieldName,
                                       const char *expected) const
    {
        u32 start = 0, end = 0;
        MultiArray<char, JSON_SMALL_STR_CAP> value;
        return field(objectStart, fieldName, start, end) &&
               readString(start, end, value.data(), JSON_SMALL_STR_CAP) &&
               cstr_equal(value.data(), expected);
    }
    // Parses a numeric-coercible member using the project's Number-compatible rules.
    bool JsonReader::numberField(u32 objectStart, const char *fieldName, double &result) const
    {
        u32 start = 0, end = 0;
        return field(objectStart, fieldName, start, end) && number(start, end, result);
    }
    // Parses a numeric-coercible value directly from its original JSON span.
    bool JsonReader::valueNumber(u32 start, u32 end, double &result) const
    {
        return number(start, end, result);
    }
    // Detects a percent-suffixed string time while respecting surrounding whitespace.
    bool JsonReader::valueIsPercent(u32 start, u32 end) const
    {
        MultiArray<char, JSON_SCALAR_STR_CAP> value;
        u32 first = 0, percentEnd = 0;
        if (!readString(start, end, value.data(), JSON_SCALAR_STR_CAP) ||
            !extractPercentSlice(value.data(), first, percentEnd))
            return false;
        u32 pos = first;
        if (pos < percentEnd && value[pos] == '-')
            ++pos;
        while (pos < percentEnd && value[pos] >= '0' && value[pos] <= '9')
            ++pos;
        if (pos < percentEnd && value[pos] == '.')
        {
            ++pos;
            const u32 fractionStart = pos;
            while (pos < percentEnd && value[pos] >= '0' && value[pos] <= '9')
                ++pos;
            return pos == percentEnd && pos > fractionStart;
        }
        return pos == percentEnd && pos > first &&
               value[pos - 1] >= '0' && value[pos - 1] <= '9';
    }
    // Converts a numeric or percentage string to milliseconds using the unknown-total fallback.
    bool JsonReader::timeValue(u32 start, u32 end, double &result) const
    {
        if (!valueIsPercent(start, end))
            return number(start, end, result);
        MultiArray<char, JSON_SCALAR_STR_CAP> text;
        u32 first = 0, numericEnd = 0;
        if (!readString(start, end, text.data(), JSON_SCALAR_STR_CAP) ||
            !extractPercentSlice(text.data(), first, numericEnd))
            return false;
        text[numericEnd] = '\0';
        double percent = 0;
        if (!decimalPercent(text.data() + first, percent))
            return false;
        result = percent * (FALLBACK_DURATION_1H_MS / JSON_PERCENT_DIVISOR);
        return true;
    }
    // Classifies separators that may legally terminate a JSON scalar.
    bool JsonReader::isValueDelimiter(char ch)
    {
        return is_ascii_space(ch) || ch == ',' || ch == ']' || ch == '}';
    }
    // Moves a source cursor past JSON whitespace without changing the input buffer.
    void JsonReader::whitespace(u32 &pos) const
    {
        while (pos < size_ && is_ascii_space(data_[pos]))
            ++pos;
    }
    // Locates a quoted string's content/end and rejects invalid escapes or raw controls.
    bool JsonReader::stringBounds(u32 start, u32 &contentStart, u32 &end) const
    {
        if (start >= size_ || data_[start] != '"')
            return false;
        contentStart = start + 1;
        u32 pos = contentStart;
        while (pos < size_)
        {
            if (data_[pos] == '\\')
            {
                if (pos + 1 >= size_)
                    return false;
                const char escaped = data_[pos + 1];
                if (escaped == 'u')
                {
                    if (pos + 2 + JSON_UNICODE_HEX_DIGITS > size_)
                        return false;
                    for (u32 digit = pos + 2; digit < pos + 2 + JSON_UNICODE_HEX_DIGITS; ++digit)
                    {
                        if (hex_nibble(data_[digit]) < 0)
                            return false;
                    }
                    pos += 2 + JSON_UNICODE_HEX_DIGITS;
                }
                else if (escaped == '"' || escaped == '\\' || escaped == '/' ||
                         escaped == 'b' || escaped == 'f' || escaped == 'n' ||
                         escaped == 'r' || escaped == 't')
                {
                    pos += 2;
                }
                else
                {
                    return false;
                }
            }
            else if (static_cast<u8>(data_[pos]) < JSON_ASCII_CONTROL_LIMIT)
            {
                return false;
            }
            else if (data_[pos++] == '"')
            {
                end = pos;
                return true;
            }
        }
        return false;
    }
    // Compares a quoted object key to an ASCII field name without materializing a string.
    bool JsonReader::stringEquals(u32 contentStart, u32 end, const char *expected) const
    {
        u32 pos = contentStart;
        u32 out = 0;
        const u32 contentLimit = end - 1;
        while (pos < contentLimit)
        {
            char ch = '\0';
            if (!decodeJsonChar<true>(data_, pos, contentLimit, ch))
                return false;
            if (expected[out] == '\0' || expected[out++] != ch)
                return false;
        }
        return expected[out] == '\0';
    }
    // Decodes one bounded JSON string into a caller-provided fixed-capacity scratch buffer.
    bool JsonReader::readString(u32 start, u32 end, char *out, u32 capacity) const
    {
        if (start >= end || data_[start] != '"' || capacity == 0)
            return false;
        u32 pos = start + 1, used = 0;
        const u32 contentLimit = end - 1;
        while (pos < contentLimit)
        {
            const char ch = data_[pos++];
            if (ch != '\\')
            {
                if (used + 1 >= capacity)
                    return false;
                out[used++] = ch;
                continue;
            }
            if (pos >= contentLimit)
                return false;
            const char escaped = data_[pos++];
            switch (escaped)
            {
            case '"':
            case '\\':
            case '/':
                if (used + 1 >= capacity) return false;
                out[used++] = escaped;
                break;
            case 'b':
                if (used + 1 >= capacity) return false;
                out[used++] = '\b';
                break;
            case 'f':
                if (used + 1 >= capacity) return false;
                out[used++] = '\f';
                break;
            case 'n':
                if (used + 1 >= capacity) return false;
                out[used++] = '\n';
                break;
            case 'r':
                if (used + 1 >= capacity) return false;
                out[used++] = '\r';
                break;
            case 't':
                if (used + 1 >= capacity) return false;
                out[used++] = '\t';
                break;
            case 'u':
            {
                if (pos + JSON_UNICODE_HEX_DIGITS > contentLimit)
                    return false;
                u32 code = 0;
                for (u32 i = 0; i < JSON_UNICODE_HEX_DIGITS; ++i)
                {
                    const i32 nib = hex_nibble(data_[pos++]);
                    if (nib < 0) return false;
                    code = (code << 4) | static_cast<u32>(nib);
                }
                // Check for surrogate pairs: high surrogate 0xD800..0xDBFF followed by \uDC00..\uDFFF
                if (code >= 0xD800u && code <= 0xDBFFu && pos + 6 <= contentLimit && data_[pos] == '\\' && data_[pos + 1] == 'u')
                {
                    pos += 2;
                    u32 low = 0;
                    for (u32 i = 0; i < JSON_UNICODE_HEX_DIGITS; ++i)
                    {
                        const i32 nib = hex_nibble(data_[pos++]);
                        if (nib < 0) return false;
                        low = (low << 4) | static_cast<u32>(nib);
                    }
                    if (low >= 0xDC00u && low <= 0xDFFFu)
                    {
                        code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
                    }
                }
                char utf8Bytes[4];
                const u32 written = utf8_encode_cp(code, utf8Bytes);
                if (written == 0 || used + written >= capacity)
                    return false;
                for (u32 b = 0; b < written; ++b)
                    out[used++] = utf8Bytes[b];
                break;
            }
            default:
                return false;
            }
        }
        out[used] = '\0';
        return true;
    }
    // Validates and skips one recursive JSON value, returning its end offset.
    bool JsonReader::skipValue(u32 start, u32 &end, u32 depth) const
    {
        if (depth > JSON_MAX_NESTING_DEPTH || start >= size_)
            return false;
        u32 pos = start;
        whitespace(pos);
        if (pos >= size_)
            return false;
        const char first = data_[pos];
        if (first == '"')
        {
            u32 contentStart = 0;
            return stringBounds(pos, contentStart, end);
        }
        if (first == '{')
        {
            ++pos;
            whitespace(pos);
            if (pos < size_ && data_[pos] == '}')
            {
                end = pos + 1;
                return true;
            }
            while (pos < size_)
            {
                u32 keyStart = 0, keyEnd = 0;
                if (!stringBounds(pos, keyStart, keyEnd))
                    return false;
                pos = keyEnd;
                whitespace(pos);
                if (pos >= size_ || data_[pos++] != ':')
                    return false;
                whitespace(pos);
                u32 valueEnd = 0;
                if (!skipValue(pos, valueEnd, depth + 1))
                    return false;
                pos = valueEnd;
                whitespace(pos);
                if (pos < size_ && data_[pos] == ',')
                {
                    ++pos;
                    whitespace(pos);
                    if (pos >= size_ || data_[pos] == '}')
                        return false;
                    continue;
                }
                if (pos < size_ && data_[pos] == '}')
                {
                    end = pos + 1;
                    return true;
                }
                return false;
            }
            return false;
        }
        if (first == '[')
        {
            ++pos;
            whitespace(pos);
            if (pos < size_ && data_[pos] == ']')
            {
                end = pos + 1;
                return true;
            }
            while (pos < size_)
            {
                u32 valueEnd = 0;
                if (!skipValue(pos, valueEnd, depth + 1))
                    return false;
                pos = valueEnd;
                whitespace(pos);
                if (pos < size_ && data_[pos] == ',')
                {
                    ++pos;
                    whitespace(pos);
                    if (pos >= size_ || data_[pos] == ']')
                        return false;
                    continue;
                }
                if (pos < size_ && data_[pos] == ']')
                {
                    end = pos + 1;
                    return true;
                }
                return false;
            }
            return false;
        }
        if (first == 't')
            return literal(pos, "true", end);
        if (first == 'f')
            return literal(pos, "false", end);
        if (first == 'n')
            return literal(pos, "null", end);
        if (first == '-' || (first >= '0' && first <= '9'))
            return jsonNumber(pos, end);
        return false;
    }
    // Checks an exact JSON literal token and returns its end offset.
    bool JsonReader::literal(u32 start, const char *expected, u32 &end) const
    {
        u32 pos = start;
        for (u32 i = 0; expected[i]; ++i)
        {
            if (pos >= size_ || data_[pos++] != expected[i])
                return false;
        }
        if (pos < size_ && !isValueDelimiter(data_[pos]))
            return false;
        end = pos;
        return true;
    }
    // Validates JSON number grammar, including fraction and exponent forms.
    bool JsonReader::jsonNumber(u32 start, u32 &end) const
    {
        u32 pos = start;
        if (data_[pos] == '-')
            ++pos;
        if (pos >= size_)
            return false;
        if (data_[pos] == '0')
        {
            ++pos;
            if (pos < size_ && data_[pos] >= '0' && data_[pos] <= '9')
                return false;
        }
        else if (data_[pos] >= '1' && data_[pos] <= '9')
        {
            do
            {
                ++pos;
            } while (pos < size_ && data_[pos] >= '0' && data_[pos] <= '9');
        }
        else
        {
            return false;
        }
        if (pos < size_ && data_[pos] == '.')
        {
            ++pos;
            if (pos >= size_ || data_[pos] < '0' || data_[pos] > '9')
                return false;
            do
            {
                ++pos;
            } while (pos < size_ && data_[pos] >= '0' && data_[pos] <= '9');
        }
        if (pos < size_ && (data_[pos] == 'e' || data_[pos] == 'E'))
        {
            ++pos;
            if (pos < size_ && (data_[pos] == '+' || data_[pos] == '-'))
                ++pos;
            if (pos >= size_ || data_[pos] < '0' || data_[pos] > '9')
                return false;
            do
            {
                ++pos;
            } while (pos < size_ && data_[pos] >= '0' && data_[pos] <= '9');
        }
        if (pos < size_ && !isValueDelimiter(data_[pos]))
            return false;
        end = pos;
        return true;
    }
    // Applies JavaScript-like Number coercion to a JSON scalar span using fixed local storage.
    bool JsonReader::number(u32 start, u32 end, double &result) const
    {
        MultiArray<char, JSON_SCALAR_STR_CAP> text;
        const bool isString = start < end && data_[start] == '"';
        if (isString)
        {
            if (!readString(start, end, text.data(), JSON_SCALAR_STR_CAP))
                return false;
        }
        else if (end - start < JSON_SCALAR_STR_CAP)
        {
            const u32 length = end - start;
            for (u32 i = 0; i < length; ++i)
                text[i] = data_[start + i];
            text[length] = '\0';
        }
        else
        {
            return false;
        }
        u32 textLength = 0;
        while (text[textLength])
            ++textLength;
        while (textLength && is_ascii_space(text[textLength - 1]))
            text[--textLength] = '\0';
        u32 pos = 0;
        while (is_ascii_space(text[pos]))
            ++pos;
        if (!text[pos])
        {
            result = 0;
            return true;
        }
        if (isString && text[pos] == '0' &&
            (text[pos + 1] == 'x' || text[pos + 1] == 'X' ||
             text[pos + 1] == 'b' || text[pos + 1] == 'B' ||
             text[pos + 1] == 'o' || text[pos + 1] == 'O'))
        {
            const char prefix = text[pos + 1];
            const u32 base = (prefix == 'x' || prefix == 'X')   ? 16u
                             : (prefix == 'b' || prefix == 'B') ? 2u
                                                                : 8u;
            pos += 2;
            double value = 0;
            bool hasDigit = false;
            while (text[pos])
            {
                const i32 nib = hex_nibble(text[pos++]);
                if (nib < 0 || static_cast<u32>(nib) >= base)
                    return false;
                hasDigit = true;
                value = value * base + static_cast<u32>(nib);
            }
            if (!hasDigit)
                return false;
            result = value;
            return result == result && result <= DBL_MAX_VAL;
        }
        bool negative = false;
        if (text[pos] == '+' || text[pos] == '-')
            negative = text[pos++] == '-';
        double value = 0;
        bool hasDigit = false;
        while (text[pos] >= '0' && text[pos] <= '9')
        {
            hasDigit = true;
            value = value * 10 + (text[pos++] - '0');
        }
        if (text[pos] == '.')
        {
            ++pos;
            double factor = 0.1;
            while (text[pos] >= '0' && text[pos] <= '9')
            {
                hasDigit = true;
                value += (text[pos++] - '0') * factor;
                factor *= 0.1;
            }
        }
        if (!hasDigit)
        {
            const char *tail = text.data() + pos;
            if (!isString && !negative && cstr_equal(tail, "true"))
            {
                result = 1;
                return true;
            }
            if (!isString && !negative && (cstr_equal(tail, "false") || cstr_equal(tail, "null")))
            {
                result = 0;
                return true;
            }
            return false;
        }
        if (text[pos] == 'e' || text[pos] == 'E')
        {
            ++pos;
            bool exponentNegative = false;
            if (text[pos] == '+' || text[pos] == '-')
                exponentNegative = text[pos++] == '-';
            u32 exponent = 0;
            bool exponentHasDigit = false;
            while (text[pos] >= '0' && text[pos] <= '9')
            {
                exponentHasDigit = true;
                if (exponent < JSON_MAX_EXPONENT)
                    exponent = exponent * 10 + static_cast<u32>(text[pos] - '0');
                ++pos;
            }
            if (!exponentHasDigit)
                return false;
            double scale = 1;
            for (u32 i = 0; i < exponent && i < JSON_MAX_EXPONENT; ++i)
                scale *= 10;
            value = exponentNegative ? value / scale : value * scale;
        }
        while (is_ascii_space(text[pos]))
            ++pos;
        if (text[pos] != '\0')
            return false;
        result = negative ? -value : value;
        return result == result && result <= DBL_MAX_VAL && result >= -DBL_MAX_VAL;
    }
    // Parses the numeric portion of a percentage time without allocating or copying.
    bool JsonReader::decimalPercent(const char *text, double &result)
    {
        u32 pos = 0;
        bool negative = false;
        if (text[pos] == '+' || text[pos] == '-')
            negative = text[pos++] == '-';
        double value = 0;
        bool hasDigit = false;
        while (text[pos] >= '0' && text[pos] <= '9')
        {
            hasDigit = true;
            value = value * 10 + (text[pos++] - '0');
        }
        if (text[pos] == '.')
        {
            ++pos;
            double factor = 0.1;
            while (text[pos] >= '0' && text[pos] <= '9')
            {
                hasDigit = true;
                value += (text[pos++] - '0') * factor;
                factor *= 0.1;
            }
        }
        if (!hasDigit || text[pos] != '\0')
            return false;
        result = negative ? -value : value;
        return true;
    }
} // namespace stockcut::timeline::project