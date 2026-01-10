#pragma once

#include <string>
#include <optional>
#include <utility>

namespace utils {

/**
 * Result<T> - A structured error handling type for better error propagation.
 *
 * Usage:
 *   Result<> for void operations (success/fail only)
 *   Result<int> for operations returning an int on success
 *
 * Example:
 *   Result<> load_file(const std::string& path) {
 *       if (!fs::exists(path)) {
 *           return Result<>::fail("File not found: " + path);
 *       }
 *       // ... do work ...
 *       return Result<>::ok();
 *   }
 *
 *   auto result = load_file(path);
 *   if (!result) {
 *       LOG_ERROR("Load failed: {}", result.error());
 *       return;
 *   }
 */
template<typename T = void>
struct Result {
    bool success;
    std::string error_message;
    std::optional<T> value;

    // Factory methods
    static Result ok() {
        return {true, "", std::nullopt};
    }

    static Result ok(T val) {
        return {true, "", std::move(val)};
    }

    static Result fail(const std::string& msg) {
        return {false, msg, std::nullopt};
    }

    // Boolean conversion for if (result) checks
    explicit operator bool() const { return success; }

    // Access error message
    const std::string& error() const { return error_message; }

    // Access value (only valid if success)
    const T& get() const { return *value; }
    T& get() { return *value; }

    // Get value or default
    T value_or(T default_val) const {
        return value.value_or(std::move(default_val));
    }
};

// Specialization for void (no return value)
template<>
struct Result<void> {
    bool success;
    std::string error_message;

    static Result ok() {
        return {true, ""};
    }

    static Result fail(const std::string& msg) {
        return {false, msg};
    }

    explicit operator bool() const { return success; }

    const std::string& error() const { return error_message; }
};

// Type alias for common case
using VoidResult = Result<void>;

} // namespace utils
