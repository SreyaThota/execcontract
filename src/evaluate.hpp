// Evaluation: compare a validated contract with a snapshot. Pure functions;
// no system calls.
#pragma once

#include "model.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace execcontract {

enum class Status { pass, fail, error };
const char* to_string(Status s) noexcept;

// One evaluated requirement. `expected`/`observed` are already in report form
// (observed is JSON null when the observation is unavailable). `reason` is a
// stable violation identifier for failed checks ("missing", "type_mismatch",
// "unexpected", "mismatch"); empty otherwise.
struct Check {
    std::string id;
    Status status = Status::error;
    nlohmann::ordered_json expected;
    nlohmann::ordered_json observed;
    std::string reason;
    std::string diagnostic;
    std::optional<SysError> error;
};

std::vector<Check> evaluate(const Contract& contract, const Snapshot& snapshot);

enum class Outcome { passed, violated, inspection_failed, invalid_contract, internal_error };
const char* to_string(Outcome o) noexcept;

// Inspection errors take precedence over violations; an empty check list is
// an internal error (a valid contract always yields at least one check).
Outcome aggregate(const std::vector<Check>& checks) noexcept;
int exit_code_for(Outcome o) noexcept;

}  // namespace execcontract
