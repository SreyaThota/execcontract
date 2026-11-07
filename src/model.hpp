// Plain value types shared by collection, contract handling and evaluation.
#pragma once

#include "sys.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace execcontract {

// ---------------------------------------------------------------- limits

inline constexpr std::size_t kMaxContractBytes = 64 * 1024;
inline constexpr std::size_t kMaxContractFdEntries = 1024;
inline constexpr std::size_t kMaxInventoryFds = 1024;
inline constexpr std::size_t kMaxStatusBytes = 64 * 1024;
inline constexpr std::size_t kMaxCwdBytes = 64 * 1024;
inline constexpr std::size_t kMaxExpectedPathBytes = 4095;
inline constexpr int kMaxJsonDepth = 8;

// ---------------------------------------------------------------- values

// Coarse descriptor type derived from fstat(2) st_mode.
enum class FdType { regular, directory, pipe, socket, char_device, block_device, other };

const char* to_string(FdType type) noexcept;
std::optional<FdType> fd_type_from_string(std::string_view text) noexcept;

// A resource-limit value. `unlimited` corresponds to RLIM_INFINITY; `value`
// is only meaningful when !unlimited. Deliberately not std::optional so that
// "unlimited" can never be confused with "missing".
struct LimitValue {
    bool unlimited = false;
    std::uint64_t value = 0;

    static LimitValue finite(std::uint64_t v) noexcept { return {false, v}; }
    static LimitValue infinity() noexcept { return {true, 0}; }
    friend bool operator==(const LimitValue& a, const LimitValue& b) noexcept {
        return a.unlimited == b.unlimited && (a.unlimited || a.value == b.value);
    }
    friend bool operator!=(const LimitValue& a, const LimitValue& b) noexcept { return !(a == b); }
};

struct Limits {
    LimitValue soft;
    LimitValue hard;
};

// The result of one observation. The collectors only create instances through
// ok() / fail(), which set exactly one member. The members are public, though,
// so the type itself does not enforce that: a default-constructed Observed
// holds neither. Consumers must treat "no value" as unknown regardless of
// `error` (evaluate.cpp fails closed with `observation_missing`).
template <class T>
struct Observed {
    std::optional<T> value;
    std::optional<SysError> error;

    static Observed ok(T v) { return Observed{std::move(v), std::nullopt}; }
    static Observed fail(SysError e) { return Observed{std::nullopt, std::move(e)}; }
};

// ---------------------------------------------------------------- observations

struct FdEntry {
    int fd = -1;
    std::optional<FdType> type;          // set when fstat succeeded
    std::optional<SysError> type_error;  // set when fstat failed
};

struct FdInventory {
    std::vector<FdEntry> entries;  // ascending by fd; may be partial if !complete
    bool complete = false;
    int scanner_fd = -1;            // descriptor of the enumeration stream, -1 if never opened
    std::optional<SysError> error;  // set iff !complete
};

struct Snapshot {
    FdInventory fds;
    Observed<unsigned> umask;
    Observed<Limits> rlimit_nofile;
    Observed<Limits> rlimit_core;
    Observed<std::string> cwd;
};

// ---------------------------------------------------------------- contract

struct FdRule {
    int fd = -1;
    bool required = false;  // false: permitted (may be absent)
    std::optional<FdType> type;
};

struct LimitRule {
    std::optional<LimitValue> soft;
    std::optional<LimitValue> hard;
};

struct Contract {
    std::optional<std::vector<FdRule>> fds;  // sorted ascending by fd
    std::optional<std::string> cwd;
    std::optional<unsigned> umask;
    std::optional<LimitRule> rlimit_nofile;
    std::optional<LimitRule> rlimit_core;
};

// Invalid input (CLI-level contract problems and schema violations).
struct ContractError {
    std::string code;     // stable identifier
    std::string message;  // human-readable
    std::string pointer;  // location inside the contract, e.g. "/fds/required/2/fd"; empty if n/a
    int err = 0;          // errno for file-level failures
};

// ---------------------------------------------------------------- text helpers

// Strict UTF-8 validation (rejects overlong forms, surrogates, > U+10FFFF).
// Returns the offset of the first invalid byte, or nullopt if valid.
std::optional<std::size_t> first_invalid_utf8(std::string_view text) noexcept;

// "0022" style four-digit octal rendering of a mode mask.
std::string format_umask(unsigned mask);

}  // namespace execcontract
