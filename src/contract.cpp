#include "contract.hpp"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <initializer_list>
#include <set>
#include <vector>

namespace execcontract {
namespace {

using json = nlohmann::json;

// Internal control flow only: converted to ContractError at the API boundary.
struct Invalid {
    ContractError error;
};

[[noreturn]] void fail(std::string code, std::string message, std::string pointer) {
    throw Invalid{ContractError{std::move(code), std::move(message), std::move(pointer), 0}};
}

std::string type_name(const json& v) {
    if (v.is_number_float()) {
        return "number (non-integer)";
    }
    return v.type_name();
}

// Parse with duplicate-key and depth detection. nlohmann::json keeps the last
// value of a duplicated key silently, so duplicates are caught in the parser
// callback, which sees every key event in document order.
json parse_strict(std::string_view text) {
    std::vector<std::set<std::string>> keys;
    int depth = 0;
    auto callback = [&](int /*lib_depth*/, json::parse_event_t event, json& parsed) -> bool {
        switch (event) {
            case json::parse_event_t::object_start:
            case json::parse_event_t::array_start:
                if (++depth > kMaxJsonDepth) {
                    fail("too_deep", "nesting deeper than " + std::to_string(kMaxJsonDepth) + " levels", "");
                }
                if (event == json::parse_event_t::object_start) {
                    keys.emplace_back();
                }
                break;
            case json::parse_event_t::object_end:
                keys.pop_back();
                --depth;
                break;
            case json::parse_event_t::array_end: --depth; break;
            case json::parse_event_t::key: {
                const auto& k = parsed.get_ref<const std::string&>();
                if (!keys.back().insert(k).second) {
                    fail("duplicate_key", "duplicate key \"" + k + "\"", "");
                }
                break;
            }
            case json::parse_event_t::value: break;
        }
        return true;
    };
    try {
        return json::parse(text.begin(), text.end(), callback, /*allow_exceptions=*/true,
                           /*ignore_comments=*/false);
    } catch (const json::parse_error& e) {
        fail("malformed_json", e.what(), "");
    }
}

void require_object(const json& v, const std::string& ptr) {
    if (!v.is_object()) {
        fail("invalid_type", "expected object, got " + type_name(v), ptr);
    }
}

void reject_unknown(const json& obj, std::initializer_list<const char*> allowed, const std::string& ptr) {
    for (const auto& item : obj.items()) {
        const bool known =
            std::any_of(allowed.begin(), allowed.end(), [&](const char* a) { return item.key() == a; });
        if (!known) {
            fail("unknown_field", "unknown field \"" + item.key() + "\"", ptr + "/" + item.key());
        }
    }
}

// Non-negative JSON integer. Booleans and floats (including integers beyond
// 64 bits, which the library parses as floating point) are type errors.
std::uint64_t get_unsigned(const json& v, const std::string& ptr) {
    if (v.is_number_unsigned()) {
        return v.get<std::uint64_t>();
    }
    if (v.is_number_integer()) {  // signed storage is only used for negatives
        fail("out_of_range", "value must not be negative", ptr);
    }
    fail("invalid_type", "expected non-negative integer, got " + type_name(v), ptr);
}

LimitValue get_limit(const json& v, const std::string& ptr) {
    if (v.is_string()) {
        if (v.get_ref<const std::string&>() == "unlimited") {
            return LimitValue::infinity();
        }
        fail("invalid_type", "the only string accepted for a limit is \"unlimited\"", ptr);
    }
    const std::uint64_t n = get_unsigned(v, ptr);
    if (n >= static_cast<std::uint64_t>(RLIM_INFINITY)) {
        // Numerically equal to RLIM_INFINITY: write "unlimited" instead.
        fail("out_of_range", "finite limits must be below RLIM_INFINITY; use \"unlimited\"", ptr);
    }
    return LimitValue::finite(n);
}

LimitRule parse_limit_rule(const json& v, const std::string& ptr) {
    require_object(v, ptr);
    reject_unknown(v, {"soft", "hard"}, ptr);
    if (v.empty()) {
        fail("empty_area", "specify at least one of \"soft\" or \"hard\"", ptr);
    }
    LimitRule rule;
    if (v.contains("soft")) {
        rule.soft = get_limit(v.at("soft"), ptr + "/soft");
    }
    if (v.contains("hard")) {
        rule.hard = get_limit(v.at("hard"), ptr + "/hard");
    }
    if (rule.soft && rule.hard && !rule.hard->unlimited &&
        (rule.soft->unlimited || rule.soft->value > rule.hard->value)) {
        fail("contradictory_limits", "soft limit exceeds hard limit", ptr);
    }
    return rule;
}

void parse_fd_list(const json& v, bool required, const std::string& ptr, std::vector<FdRule>& out) {
    if (!v.is_array()) {
        fail("invalid_type", "expected array, got " + type_name(v), ptr);
    }
    for (std::size_t i = 0; i < v.size(); ++i) {
        const std::string eptr = ptr + "/" + std::to_string(i);
        const json& e = v[i];
        require_object(e, eptr);
        reject_unknown(e, {"fd", "type"}, eptr);
        if (!e.contains("fd")) {
            fail("missing_field", "descriptor entry needs \"fd\"", eptr);
        }
        const std::uint64_t n = get_unsigned(e.at("fd"), eptr + "/fd");
        if (n > static_cast<std::uint64_t>(INT_MAX)) {
            fail("out_of_range", "descriptor number exceeds INT_MAX", eptr + "/fd");
        }
        FdRule rule;
        rule.fd = static_cast<int>(n);
        rule.required = required;
        if (e.contains("type")) {
            const json& t = e.at("type");
            if (!t.is_string()) {
                fail("invalid_type", "expected string, got " + type_name(t), eptr + "/type");
            }
            rule.type = fd_type_from_string(t.get_ref<const std::string&>());
            if (!rule.type) {
                fail("invalid_fd_type", "unknown descriptor type \"" + t.get<std::string>() + "\"",
                     eptr + "/type");
            }
        }
        if (out.size() == kMaxContractFdEntries) {
            fail("too_many_fd_entries",
                 "more than " + std::to_string(kMaxContractFdEntries) + " descriptor entries", eptr);
        }
        out.push_back(rule);
    }
}

std::vector<FdRule> parse_fds(const json& v, const std::string& ptr) {
    require_object(v, ptr);
    reject_unknown(v, {"required", "permitted"}, ptr);
    if (v.empty()) {
        fail("empty_area", "specify \"required\" and/or \"permitted\"", ptr);
    }
    std::vector<FdRule> rules;
    if (v.contains("required")) {
        parse_fd_list(v.at("required"), true, ptr + "/required", rules);
    }
    if (v.contains("permitted")) {
        parse_fd_list(v.at("permitted"), false, ptr + "/permitted", rules);
    }
    std::stable_sort(rules.begin(), rules.end(),
                     [](const FdRule& a, const FdRule& b) { return a.fd < b.fd; });
    for (std::size_t i = 1; i < rules.size(); ++i) {
        if (rules[i].fd == rules[i - 1].fd) {
            fail("duplicate_fd", "descriptor " + std::to_string(rules[i].fd) + " listed more than once", ptr);
        }
    }
    return rules;
}

// Canonical absolute path: leading '/', no trailing '/' (except "/"), no
// empty, "." or ".." components. Nothing is resolved or rewritten.
std::string parse_cwd(const json& v, const std::string& ptr) {
    if (!v.is_string()) {
        fail("invalid_type", "expected string, got " + type_name(v), ptr);
    }
    const std::string& p = v.get_ref<const std::string&>();
    if (p.empty() || p.size() > kMaxExpectedPathBytes || p.find('\0') != std::string::npos) {
        fail("invalid_path",
             "path must be 1.." + std::to_string(kMaxExpectedPathBytes) + " bytes without NUL", ptr);
    }
    if (p[0] != '/') {
        fail("noncanonical_path", "path must be absolute", ptr);
    }
    if (p == "/") {
        return p;
    }
    std::size_t start = 1;
    for (;;) {
        const std::size_t slash = p.find('/', start);
        const std::string_view comp =
            std::string_view(p).substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (comp.empty() || comp == "." || comp == "..") {
            fail("noncanonical_path",
                 "path must not contain empty, \".\" or \"..\" components or a trailing \"/\"", ptr);
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return p;
}

unsigned parse_umask(const json& v, const std::string& ptr) {
    if (!v.is_string()) {
        fail("invalid_type", "expected string such as \"0022\", got " + type_name(v), ptr);
    }
    const std::string& s = v.get_ref<const std::string&>();
    const bool ok = s.size() == 4 && s[0] == '0' &&
                    std::all_of(s.begin() + 1, s.end(), [](char c) { return c >= '0' && c <= '7'; });
    if (!ok) {
        fail("invalid_umask", "umask must match ^0[0-7]{3}$", ptr);
    }
    return static_cast<unsigned>(std::stoul(s, nullptr, 8));
}

Contract validate(const json& root) {
    require_object(root, "");
    reject_unknown(root, {"version", "fds", "cwd", "umask", "rlimit_nofile", "rlimit_core"}, "");
    if (!root.contains("version")) {
        fail("missing_version", "\"version\" is required", "/version");
    }
    const json& version = root.at("version");
    if (!version.is_number_integer()) {  // excludes booleans, floats, strings
        fail("invalid_type", "expected integer version, got " + type_name(version), "/version");
    }
    if (!version.is_number_unsigned() || version.get<std::uint64_t>() != 1) {
        fail("unsupported_version", "only contract version 1 is supported", "/version");
    }
    if (root.size() == 1) {
        fail("empty_contract", "the contract checks nothing; specify at least one area", "");
    }
    Contract c;
    if (root.contains("fds")) {
        c.fds = parse_fds(root.at("fds"), "/fds");
    }
    if (root.contains("cwd")) {
        c.cwd = parse_cwd(root.at("cwd"), "/cwd");
    }
    if (root.contains("umask")) {
        c.umask = parse_umask(root.at("umask"), "/umask");
    }
    if (root.contains("rlimit_nofile")) {
        c.rlimit_nofile = parse_limit_rule(root.at("rlimit_nofile"), "/rlimit_nofile");
    }
    if (root.contains("rlimit_core")) {
        c.rlimit_core = parse_limit_rule(root.at("rlimit_core"), "/rlimit_core");
    }
    return c;
}

ContractError file_error(const char* code, int err, std::string message) {
    return ContractError{code, std::move(message), "", err};
}

}  // namespace

ContractText load_contract_file(const std::string& path, std::size_t cap) {
    ContractText out;
    const UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK));
    if (!fd) {
        const int err = errno;
        const char* code = err == ENOENT   ? "contract_not_found"
                           : err == EACCES ? "contract_unreadable"
                                           : "contract_open_failed";
        out.error = file_error(code, err, "cannot open contract");
        return out;
    }
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0) {
        out.error = file_error("contract_open_failed", errno, "cannot stat contract");
        return out;
    }
    if (!S_ISREG(st.st_mode)) {
        out.error = file_error("contract_not_regular_file", 0, "contract must be a regular file");
        return out;
    }
    const ReadResult r = read_bounded(fd.get(), cap);
    if (r.err != 0) {
        out.error = file_error("contract_read_failed", r.err, "cannot read contract");
        return out;
    }
    if (r.too_large) {
        out.error = file_error("contract_too_large", 0, "contract exceeds " + std::to_string(cap) + " bytes");
        return out;
    }
    out.text = r.data;
    return out;  // fd closed here, before the report channel is opened
}

ContractResult parse_contract(std::string_view text) {
    ContractResult result;
    try {
        result.contract = validate(parse_strict(text));
    } catch (const Invalid& e) {
        result.error = e.error;
    }
    return result;
}

std::vector<std::string> unchecked_areas(const Contract& c) {
    std::vector<std::string> out;
    if (!c.fds)
        out.emplace_back("fds");
    if (!c.cwd)
        out.emplace_back("cwd");
    if (!c.umask)
        out.emplace_back("umask");
    if (!c.rlimit_nofile)
        out.emplace_back("rlimit_nofile");
    if (!c.rlimit_core)
        out.emplace_back("rlimit_core");
    return out;
}

}  // namespace execcontract
