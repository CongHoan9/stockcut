// ============================================================================
// CoreEngine - Config Options & OptionsMap Implementation
// Stockfish ucioption.cpp Heritage
// ============================================================================
#include "config.h"

namespace stockcut {

// ----------------------------------------------------------------------------
// ConfigOption
// ----------------------------------------------------------------------------
ConfigOption::ConfigOption(const char* defaultVal, OnChange onChange, void* userData) noexcept
    : type_(OptionType::String), onChange_(onChange), userData_(userData) {
    if (defaultVal) {
        copy_cstr_bounded(defaultValue_.data(), CONFIG_OPTION_VAL_CAP, defaultVal);
        copy_cstr_bounded(currentValue_.data(), CONFIG_OPTION_VAL_CAP, defaultVal);
    }
}

ConfigOption::ConfigOption(double defaultVal, double minVal, double maxVal, OnChange onChange, void* userData) noexcept
    : doubleVal_(defaultVal), min_(minVal), max_(maxVal), type_(OptionType::Spin), onChange_(onChange), userData_(userData) {
    std::snprintf(defaultValue_.data(), CONFIG_OPTION_VAL_CAP, "%g", defaultVal);
    std::snprintf(currentValue_.data(), CONFIG_OPTION_VAL_CAP, "%g", defaultVal);
}

const char* ConfigOption::type_str() const noexcept {
    switch (type_) {
        case OptionType::String: return "string";
        case OptionType::Spin:   return "spin";
        case OptionType::Check:  return "check";
        case OptionType::Combo:  return "combo";
        default:                 return "string";
    }
}

bool ConfigOption::is_default() const noexcept {
    if (type_ == OptionType::Spin) {
        const double def = std::strtod(defaultValue_.data(), nullptr);
        return std::abs(doubleVal_ - def) < CONFIG_FLOAT_EPSILON;
    }
    if (type_ == OptionType::Check) {
        return boolVal_ == cstr_equal(defaultValue_.data(), "true");
    }
    return cstr_equal(currentValue_.data(), defaultValue_.data());
}

void ConfigOption::reset_to_default() noexcept {
    copy_cstr_bounded(currentValue_.data(), CONFIG_OPTION_VAL_CAP, defaultValue_.data());
    if (type_ == OptionType::Spin) {
        doubleVal_ = std::strtod(defaultValue_.data(), nullptr);
    } else if (type_ == OptionType::Check) {
        boolVal_ = cstr_equal(defaultValue_.data(), "true");
    }
    if (onChange_) onChange_(*this, userData_);
}

ConfigOption& ConfigOption::operator=(const char* val) noexcept {
    if (!val) return *this;
    if (!cstr_equal(currentValue_.data(), val)) {
        copy_cstr_bounded(currentValue_.data(), CONFIG_OPTION_VAL_CAP, val);
        if (type_ == OptionType::Spin) {
            double v = std::strtod(val, nullptr);
            if (min_ < max_) {
                v = clamp(v, min_, max_);
            }
            doubleVal_ = v;
            std::snprintf(currentValue_.data(), CONFIG_OPTION_VAL_CAP, "%g", v);
        } else if (type_ == OptionType::Check) {
            boolVal_ = (cstr_equal(val, "true") || cstr_equal(val, "1"));
        }
        if (onChange_) onChange_(*this, userData_);
    }
    return *this;
}

ConfigOption& ConfigOption::operator=(double val) noexcept {
    if (min_ < max_) {
        val = clamp(val, min_, max_);
    }
    if (std::abs(doubleVal_ - val) > CONFIG_FLOAT_EPSILON) {
        doubleVal_ = val;
        std::snprintf(currentValue_.data(), CONFIG_OPTION_VAL_CAP, "%g", val);
        if (onChange_) onChange_(*this, userData_);
    }
    return *this;
}

ConfigOption& ConfigOption::operator=(i32 val) noexcept {
    return operator=(static_cast<double>(val));
}

// ----------------------------------------------------------------------------
// ConfigOptionsMap
// ----------------------------------------------------------------------------
i32 ConfigOptionsMap::find_index(const char* name) const noexcept {
    if (!name || !*name) return -1;
    for (u32 i = 0; i < count_; ++i) {
        if (cstr_iequal(names_[i].data(), name)) return static_cast<i32>(i);
    }
    return -1;
}

void ConfigOptionsMap::init_defaults() noexcept {
    count_ = 0;
    add("aspectRatio", ConfigOption("16:9"));
    add("previewResolution", ConfigOption("1080"));
    add("viewport.x", ConfigOption(0.0, CONFIG_VIEWPORT_OFFSET_MIN, CONFIG_VIEWPORT_OFFSET_MAX));
    add("viewport.y", ConfigOption(0.0, CONFIG_VIEWPORT_OFFSET_MIN, CONFIG_VIEWPORT_OFFSET_MAX));
    add("viewport.zoom", ConfigOption(CONFIG_VIEWPORT_ZOOM_DEF, CONFIG_VIEWPORT_ZOOM_MIN, CONFIG_VIEWPORT_ZOOM_MAX));
}

bool ConfigOptionsMap::add(const char* name, const ConfigOption& opt) noexcept {
    if (!name || !*name) return false;
    ConfigOption* existing = find(name);
    if (existing) {
        *existing = opt;
        return true;
    }
    if (count_ >= MAX_OPTIONS) return false;

    copy_cstr_bounded(names_[count_].data(), CONFIG_OPTION_KEY_CAP, name);
    options_[count_] = opt;
    ++count_;
    return true;
}

ConfigOption* ConfigOptionsMap::find(const char* name) noexcept {
    const i32 idx = find_index(name);
    return (idx >= 0) ? &options_[static_cast<u32>(idx)] : nullptr;
}

const ConfigOption* ConfigOptionsMap::find(const char* name) const noexcept {
    const i32 idx = find_index(name);
    return (idx >= 0) ? &options_[static_cast<u32>(idx)] : nullptr;
}

ConfigOption& ConfigOptionsMap::operator[](const char* name) noexcept {
    ConfigOption* opt = find(name);
    if (opt) return *opt;
    add(name, ConfigOption(""));
    return options_[count_ - 1];
}

bool ConfigOptionsMap::set(const char* name, const char* value) noexcept {
    ConfigOption* opt = find(name);
    if (opt) {
        *opt = value;
        return true;
    }
    return add(name, ConfigOption(value));
}

bool ConfigOptionsMap::set(const char* name, double value) noexcept {
    ConfigOption* opt = find(name);
    if (opt) {
        *opt = value;
        return true;
    }
    return add(name, ConfigOption(value, CONFIG_DEFAULT_SPIN_MIN, CONFIG_DEFAULT_SPIN_MAX));
}

// ----------------------------------------------------------------------------
// JSON Serialization (1-space indentation strictly matching config.json)
// ----------------------------------------------------------------------------
u32 ConfigOptionsMap::to_json(char* outBuf, u32 maxLen) const noexcept {
    if (!outBuf || maxLen < 32) return 0;

    const ConfigOption* optRatio = find("aspectRatio");
    const ConfigOption* optRes   = find("previewResolution");
    const ConfigOption* optVx    = find("viewport.x");
    const ConfigOption* optVy    = find("viewport.y");
    const ConfigOption* optVz    = find("viewport.zoom");

    const char* ratio = optRatio ? optRatio->string_val() : "16:9";
    const char* res   = optRes ? optRes->string_val() : "1080";
    const i32 vx      = optVx ? optVx->int_val() : 0;
    const i32 vy      = optVy ? optVy->int_val() : 0;
    const i32 vz      = optVz ? optVz->int_val() : static_cast<i32>(CONFIG_VIEWPORT_ZOOM_DEF);

    const int written = std::snprintf(outBuf, maxLen,
        "{\n"
        " \"aspectRatio\": \"%s\",\n"
        " \"previewResolution\": \"%s\",\n"
        " \"viewport\": {\n"
        "  \"x\": %d,\n"
        "  \"y\": %d,\n"
        "  \"zoom\": %d\n"
        " }",
        ratio, res, vx, vy, vz
    );

    if (written < 0 || static_cast<u32>(written) >= maxLen) {
        return 0;
    }

    u32 curLen = static_cast<u32>(written);

    for (u32 i = 0; i < count_; ++i) {
        const char* nm = names_[i].data();
        if (cstr_iequal(nm, "aspectRatio") ||
            cstr_iequal(nm, "previewResolution") ||
            cstr_iequal(nm, "viewport.x") ||
            cstr_iequal(nm, "viewport.y") ||
            cstr_iequal(nm, "viewport.zoom") ||
            cstr_iequal(nm, "lyricSize")) {
            continue;
        }

        const ConfigOption& opt = options_[i];
        MultiArray<char, CONFIG_JSON_LINE_CAP> lineBuf;
        int lLen = 0;

        if (opt.type() == OptionType::Spin) {
            lLen = std::snprintf(lineBuf.data(), CONFIG_JSON_LINE_CAP, ",\n \"%s\": %g", nm, opt.double_val());
        } else if (opt.type() == OptionType::Check) {
            lLen = std::snprintf(lineBuf.data(), CONFIG_JSON_LINE_CAP, ",\n \"%s\": %s", nm, opt.bool_val() ? "true" : "false");
        } else {
            lLen = std::snprintf(lineBuf.data(), CONFIG_JSON_LINE_CAP, ",\n \"%s\": \"%s\"", nm, opt.string_val());
        }

        if (lLen > 0 && curLen + static_cast<u32>(lLen) + 4 < maxLen) {
            std::memcpy(outBuf + curLen, lineBuf.data(), static_cast<usize>(lLen));
            curLen += static_cast<u32>(lLen);
        }
    }

    if (curLen + 2 < maxLen) {
        outBuf[curLen++] = '\n';
        outBuf[curLen++] = '}';
        outBuf[curLen] = '\0';
    }

    return curLen;
}

// ----------------------------------------------------------------------------
// Minimal, robust JSON parser for config.json (Zero dependencies)
// ----------------------------------------------------------------------------
namespace {
    inline const char* skip_ws(const char* p, const char* end) noexcept {
        while (p < end && is_ascii_space(*p)) ++p;
        return p;
    }

    inline const char* parse_string(const char* p, const char* end, char* out, usize maxLen) noexcept {
        if (p >= end || *p != '"') return nullptr;
        ++p;
        usize idx = 0;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
            }
            if (idx + 1 < maxLen) {
                out[idx++] = *p;
            }
            ++p;
        }
        out[idx] = '\0';
        return (p < end && *p == '"') ? (p + 1) : nullptr;
    }

    inline const char* parse_json_scalar(const char* p, const char* end, char* out, usize maxLen) noexcept {
        if (p >= end) return nullptr;
        if (*p == '"') {
            return parse_string(p, end, out, maxLen);
        }
        const char* vStart = p;
        while (p < end && *p != ',' && *p != '}' && !is_ascii_space(*p)) ++p;
        usize vLen = static_cast<usize>(p - vStart);
        if (vLen >= maxLen) vLen = maxLen - 1;
        std::memcpy(out, vStart, vLen);
        out[vLen] = '\0';
        return p;
    }
} // namespace

bool ConfigOptionsMap::load_json(const char* jsonStr, u32 len) noexcept {
    if (!jsonStr || len == 0) return false;
    const char* p = jsonStr;
    const char* end = jsonStr + len;

    p = skip_ws(p, end);
    if (p >= end || *p != '{') return false;
    ++p;

    MultiArray<char, CONFIG_OPTION_KEY_CAP> key;
    MultiArray<char, CONFIG_JSON_VAL_CAP> val;

    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end || *p == '}') break;
        if (*p == ',') { ++p; continue; }

        p = parse_string(p, end, key.data(), CONFIG_OPTION_KEY_CAP);
        if (!p) break;

        p = skip_ws(p, end);
        if (p >= end || *p != ':') break;
        ++p;
        p = skip_ws(p, end);

        if (cstr_iequal(key.data(), "lyricSize")) {
            p = parse_json_scalar(p, end, val.data(), CONFIG_JSON_VAL_CAP);
            if (!p) break;
            continue;
        }

        if (cstr_iequal(key.data(), "viewport") && p < end && *p == '{') {
            ++p;
            MultiArray<char, CONFIG_OPTION_KEY_CAP> subKey;
            MultiArray<char, CONFIG_OPTION_VAL_CAP> fullKey;
            while (p < end) {
                p = skip_ws(p, end);
                if (p >= end || *p == '}') { if (p < end) ++p; break; }
                if (*p == ',') { ++p; continue; }

                p = parse_string(p, end, subKey.data(), CONFIG_OPTION_KEY_CAP);
                if (!p) break;

                p = skip_ws(p, end);
                if (p >= end || *p != ':') break;
                ++p;
                p = skip_ws(p, end);

                p = parse_json_scalar(p, end, val.data(), CONFIG_JSON_VAL_CAP);
                if (!p) break;

                std::snprintf(fullKey.data(), CONFIG_OPTION_VAL_CAP, "viewport.%s", subKey.data());
                set(fullKey.data(), val.data());
            }
            continue;
        }

        p = parse_json_scalar(p, end, val.data(), CONFIG_JSON_VAL_CAP);
        if (!p) break;
        set(key.data(), val.data());
    }

    return true;
}

} // namespace stockcut
