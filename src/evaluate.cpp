#include "evaluate.hpp"

#include <map>

namespace execcontract {
namespace {

using ojson = nlohmann::ordered_json;

ojson limit_json(const LimitValue& v) {
    return v.unlimited ? ojson("unlimited") : ojson(v.value);
}

ojson type_json(const std::optional<FdType>& t) {
    return t ? ojson(to_string(*t)) : ojson(nullptr);
}

Check make(std::string id, ojson expected) {
    Check c;
    c.id = std::move(id);
    c.expected = std::move(expected);
    c.observed = nullptr;
    return c;
}

void set_error(Check& c, const SysError& e, std::string diagnostic) {
    c.status = Status::error;
    c.error = e;
    c.diagnostic = std::move(diagnostic);
}

void set_fail(Check& c, const char* reason, std::string diagnostic) {
    c.status = Status::fail;
    c.reason = reason;
    c.diagnostic = std::move(diagnostic);
}

void set_pass(Check& c, std::string diagnostic) {
    c.status = Status::pass;
    c.diagnostic = std::move(diagnostic);
}

void evaluate_fds(const std::vector<FdRule>& rules, const FdInventory& inv, std::vector<Check>& out) {
    Check inventory = make("fd.inventory", "complete inventory");
    inventory.observed = ojson{
        {"complete", inv.complete},
        {"count", inv.entries.size()},
        {"scanner_fd", inv.scanner_fd >= 0 ? ojson(inv.scanner_fd) : ojson(nullptr)},
    };
    if (inv.complete) {
        set_pass(inventory,
                 "enumerated " + std::to_string(inv.entries.size()) + " descriptor(s) from /proc/self/fd");
    } else {
        // An incomplete inventory is never an empty successful one.
        const SysError e = inv.error.value_or(SysError{"fd_enumeration_failed", 0, "unknown"});
        set_error(inventory, e, "descriptor inventory incomplete: " + e.detail);
    }
    out.push_back(std::move(inventory));

    std::map<int, const FdEntry*> open;
    for (const FdEntry& e : inv.entries) {
        open.emplace(e.fd, &e);
    }

    for (const FdRule& rule : rules) {
        Check c = make(
            "fd." + std::to_string(rule.fd),
            ojson{{"presence", rule.required ? "required" : "permitted"}, {"type", type_json(rule.type)}});
        const std::string label = "descriptor " + std::to_string(rule.fd);
        const auto it = open.find(rule.fd);
        if (it != open.end()) {
            const FdEntry& e = *it->second;
            c.observed = ojson{{"present", true}, {"type", type_json(e.type)}};
            if (!rule.type) {
                set_pass(c, label + " is open");
            } else if (!e.type) {
                set_error(c, e.type_error.value_or(SysError{"fd_type_unavailable", 0, ""}),
                          label + " is open but its type could not be determined");
            } else if (*e.type != *rule.type) {
                set_fail(c, "type_mismatch",
                         label + " is a " + to_string(*e.type) + ", expected " + to_string(*rule.type));
            } else {
                set_pass(c, label + " is open with type " + to_string(*e.type));
            }
        } else if (inv.complete) {
            c.observed = ojson{{"present", false}, {"type", nullptr}};
            if (rule.required) {
                set_fail(c, "missing", "required " + label + " is not open");
            } else {
                set_pass(c, "permitted " + label + " is not open");
            }
        } else {
            // Not seen in a partial inventory: presence is unknown.
            set_error(c, SysError{"fd_inventory_incomplete", 0, ""},
                      label + " not seen, but the inventory is incomplete");
        }
        out.push_back(std::move(c));
    }

    std::map<int, bool> contracted;
    for (const FdRule& rule : rules) {
        contracted[rule.fd] = true;
    }
    for (const FdEntry& e : inv.entries) {  // ascending; known evidence even if partial
        if (contracted.count(e.fd) != 0) {
            continue;
        }
        Check c = make("fd.unexpected." + std::to_string(e.fd),
                       ojson{{"presence", "forbidden"}, {"type", nullptr}});
        c.observed = ojson{{"present", true}, {"type", type_json(e.type)}};
        set_fail(c, "unexpected",
                 "descriptor " + std::to_string(e.fd) + " is open but not permitted by the contract");
        out.push_back(std::move(c));
    }
}

// An Observed<T> without a value is always an inspection error, even if (by
// a programming error) it carries no error either: fail closed, never UB.
template <class T>
SysError missing(const Observed<T>& obs) {
    return obs.error.value_or(SysError{"observation_missing", 0, "no value recorded"});
}

void evaluate_cwd(const std::string& expected, const Observed<std::string>& obs, std::vector<Check>& out) {
    Check c = make("cwd", expected);
    if (!obs.value) {
        const SysError e = missing(obs);
        set_error(c, e, "working directory unavailable: " + e.detail);
    } else {
        c.observed = *obs.value;
        if (*obs.value == expected) {  // exact byte comparison, no normalisation
            set_pass(c, "working directory matches");
        } else {
            set_fail(c, "mismatch", "working directory differs");
        }
    }
    out.push_back(std::move(c));
}

void evaluate_umask(unsigned expected, const Observed<unsigned>& obs, std::vector<Check>& out) {
    Check c = make("umask", format_umask(expected));
    if (!obs.value) {
        const SysError e = missing(obs);
        set_error(c, e, "umask unavailable: " + e.detail);
    } else {
        c.observed = format_umask(*obs.value);
        if (*obs.value == expected) {
            set_pass(c, "umask matches");
        } else {
            set_fail(c, "mismatch",
                     "umask is " + format_umask(*obs.value) + ", expected " + format_umask(expected));
        }
    }
    out.push_back(std::move(c));
}

void evaluate_limit(const std::string& name, const LimitRule& rule, const Observed<Limits>& obs,
                    std::vector<Check>& out) {
    auto one = [&](const char* which, const std::optional<LimitValue>& expected, auto select) {
        if (!expected) {
            return;
        }
        Check c = make(name + "." + which, limit_json(*expected));
        if (!obs.value) {
            const SysError e = missing(obs);
            set_error(c, e, name + " unavailable: " + e.detail);
        } else {
            const LimitValue& actual = select(*obs.value);
            c.observed = limit_json(actual);
            if (actual == *expected) {
                set_pass(c, name + " " + which + " limit matches");
            } else {
                set_fail(c, "mismatch", name + " " + which + " limit differs");
            }
        }
        out.push_back(std::move(c));
    };
    one("soft", rule.soft, [](const Limits& l) -> const LimitValue& { return l.soft; });
    one("hard", rule.hard, [](const Limits& l) -> const LimitValue& { return l.hard; });
}

}  // namespace

const char* to_string(Status s) noexcept {
    switch (s) {
        case Status::pass: return "pass";
        case Status::fail: return "fail";
        case Status::error: return "error";
    }
    return "error";
}

const char* to_string(Outcome o) noexcept {
    switch (o) {
        case Outcome::passed: return "passed";
        case Outcome::violated: return "violated";
        case Outcome::inspection_failed: return "inspection_failed";
        case Outcome::invalid_contract: return "invalid_contract";
        case Outcome::internal_error: return "internal_error";
    }
    return "internal_error";
}

std::vector<Check> evaluate(const Contract& contract, const Snapshot& s) {
    std::vector<Check> out;
    if (contract.fds) {
        evaluate_fds(*contract.fds, s.fds, out);
    }
    if (contract.cwd) {
        evaluate_cwd(*contract.cwd, s.cwd, out);
    }
    if (contract.umask) {
        evaluate_umask(*contract.umask, s.umask, out);
    }
    if (contract.rlimit_nofile) {
        evaluate_limit("rlimit_nofile", *contract.rlimit_nofile, s.rlimit_nofile, out);
    }
    if (contract.rlimit_core) {
        evaluate_limit("rlimit_core", *contract.rlimit_core, s.rlimit_core, out);
    }
    return out;
}

Outcome aggregate(const std::vector<Check>& checks) noexcept {
    if (checks.empty()) {
        return Outcome::internal_error;
    }
    bool failed = false;
    for (const Check& c : checks) {
        if (c.status == Status::error) {
            return Outcome::inspection_failed;
        }
        failed = failed || c.status == Status::fail;
    }
    return failed ? Outcome::violated : Outcome::passed;
}

int exit_code_for(Outcome o) noexcept {
    switch (o) {
        case Outcome::passed: return 0;
        case Outcome::violated: return 1;
        case Outcome::invalid_contract: return 2;
        case Outcome::inspection_failed:
        case Outcome::internal_error: return 3;
    }
    return 3;
}

}  // namespace execcontract
