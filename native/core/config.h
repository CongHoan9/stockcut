// ============================================================================
// CoreEngine - Config Options & OptionsMap (Stockfish ucioption.h Heritage)
// ============================================================================
#pragma once

#include "types.h"
#include "data_structures.h"
#include "string_parse.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace stockcut {

constexpr usize  CONFIG_OPTION_VAL_CAP       = 96;
constexpr usize  CONFIG_OPTION_KEY_CAP       = 64;
constexpr usize  CONFIG_JSON_LINE_CAP        = 256;
constexpr usize  CONFIG_JSON_VAL_CAP         = 128;
constexpr double CONFIG_FLOAT_EPSILON        = 1.0e-6;
constexpr double CONFIG_DEFAULT_SPIN_MIN     = -100000.0;
constexpr double CONFIG_DEFAULT_SPIN_MAX     = 100000.0;
constexpr double CONFIG_VIEWPORT_OFFSET_MIN  = -10000.0;
constexpr double CONFIG_VIEWPORT_OFFSET_MAX  = 10000.0;
constexpr double CONFIG_VIEWPORT_ZOOM_DEF    = 100.0;
constexpr double CONFIG_VIEWPORT_ZOOM_MIN    = 10.0;
constexpr double CONFIG_VIEWPORT_ZOOM_MAX    = 500.0;

enum class OptionType : u8 {
    String = 0,
    Spin   = 1,
    Check  = 2,
    Combo  = 3
};

class ConfigOptionsMap;

class ConfigOption {
public:
    using OnChange = void (*)(const ConfigOption&, void* userData);

    ConfigOption() noexcept = default;

    // String option
    ConfigOption(const char* defaultVal, OnChange onChange = nullptr, void* userData = nullptr) noexcept;

    // Spin / Numeric option (int / double)
    ConfigOption(double defaultVal, double minVal, double maxVal, OnChange onChange = nullptr, void* userData = nullptr) noexcept;

    // Check / Boolean option (Compile-Time Specialized)
    template <bool DefaultVal>
    static ConfigOption make_check(OnChange onChange = nullptr, void* userData = nullptr) noexcept {
        ConfigOption opt{};
        opt.boolVal_  = DefaultVal;
        opt.type_     = OptionType::Check;
        opt.onChange_ = onChange;
        opt.userData_ = userData;
        constexpr const char* kLiteral = DefaultVal ? "true" : "false";
        copy_cstr_bounded(opt.defaultValue_.data(), CONFIG_OPTION_VAL_CAP, kLiteral);
        copy_cstr_bounded(opt.currentValue_.data(), CONFIG_OPTION_VAL_CAP, kLiteral);
        return opt;
    }

    // Assignment operators
    ConfigOption& operator=(const char* val) noexcept;
    ConfigOption& operator=(double val) noexcept;
    ConfigOption& operator=(i32 val) noexcept;

    template <bool Val>
    ConfigOption& assign_bool() noexcept {
        if (boolVal_ != Val) {
            boolVal_ = Val;
            copy_cstr_bounded(currentValue_.data(), CONFIG_OPTION_VAL_CAP, Val ? "true" : "false");
            if (onChange_) onChange_(*this, userData_);
        }
        return *this;
    }

    // Implicit conversions (Stockfish style)
    operator const char*() const noexcept { return currentValue_.data(); }
    operator double() const noexcept { return doubleVal_; }
    operator i32() const noexcept { return static_cast<i32>(doubleVal_); }
    operator bool() const noexcept { return boolVal_; }

    // Type query
    OptionType type() const noexcept { return type_; }
    const char* type_str() const noexcept;

    // Value accessors
    const char* string_val() const noexcept { return currentValue_.data(); }
    double double_val() const noexcept { return doubleVal_; }
    i32 int_val() const noexcept { return static_cast<i32>(doubleVal_); }
    bool bool_val() const noexcept { return boolVal_; }
    const char* default_val() const noexcept { return defaultValue_.data(); }
    double min_val() const noexcept { return min_; }
    double max_val() const noexcept { return max_; }

    bool is_default() const noexcept;
    void reset_to_default() noexcept;

    void set_on_change(OnChange cb, void* userData = nullptr) noexcept {
        onChange_ = cb;
        userData_ = userData;
    }

private:
    friend class ConfigOptionsMap;

    MultiArray<char, CONFIG_OPTION_VAL_CAP> defaultValue_{};
    MultiArray<char, CONFIG_OPTION_VAL_CAP> currentValue_{};
    double doubleVal_  = 0.0;
    double min_        = 0.0;
    double max_        = 0.0;
    bool boolVal_      = false;
    OptionType type_   = OptionType::String;
    OnChange onChange_ = nullptr;
    void* userData_    = nullptr;
};

class ConfigOptionsMap {
public:
    static constexpr u32 MAX_OPTIONS = 64;

    ConfigOptionsMap() noexcept {
        init_defaults();
    }

    void init_defaults() noexcept;

    bool add(const char* name, const ConfigOption& opt) noexcept;

    ConfigOption* find(const char* name) noexcept;
    const ConfigOption* find(const char* name) const noexcept;
    ConfigOption& operator[](const char* name) noexcept;

    bool set(const char* name, const char* value) noexcept;
    bool set(const char* name, double value) noexcept;

    template <bool Val>
    bool set_bool(const char* name) noexcept {
        ConfigOption* opt = find(name);
        if (opt) {
            opt->assign_bool<Val>();
            return true;
        }
        return add(name, ConfigOption::make_check<Val>());
    }

    u32 count() const noexcept { return count_; }
    const char* name_at(u32 idx) const noexcept {
        return (idx < count_) ? names_[idx].data() : "";
    }
    const ConfigOption* option_at(u32 idx) const noexcept {
        return (idx < count_) ? &options_[idx] : nullptr;
    }

    u32 to_json(char* outBuf, u32 maxLen) const noexcept;
    bool load_json(const char* jsonStr, u32 len) noexcept;

    u32 snapshot(char* outBuf, u32 maxLen) const noexcept {
        return to_json(outBuf, maxLen);
    }
    bool restore(const char* snapJson, u32 len) noexcept {
        init_defaults();
        return load_json(snapJson, len);
    }

private:
    i32 find_index(const char* name) const noexcept;

    MultiArray<char, MAX_OPTIONS, CONFIG_OPTION_KEY_CAP> names_{};
    MultiArray<ConfigOption, MAX_OPTIONS> options_{};
    u32 count_ = 0;
};

} // namespace stockcut
