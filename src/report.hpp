// Reporting: build the versioned JSON report. Pure; delivery is done by main.
#pragma once

#include "evaluate.hpp"
#include "model.hpp"

#include <optional>
#include <string>
#include <vector>

namespace execcontract {

inline constexpr int kReportVersion = 1;
const char* tool_version() noexcept;

// Top-level error for invalid_contract / internal_error reports.
struct ReportError {
    std::string code;
    std::string message;
    int err = 0;
    std::string pointer;
};

struct ReportInput {
    Outcome outcome = Outcome::internal_error;
    std::vector<Check> checks;
    std::vector<std::string> unchecked;
    std::optional<ReportError> error;
};

// Render as pretty-printed JSON with a trailing newline. Throws
// nlohmann::json::exception if a string is not valid UTF-8 (strict mode);
// callers map that to a serialisation failure, never to replaced bytes.
std::string render_report(const ReportInput& input);

// Minimal hand-written report used when normal rendering itself failed.
// Contains only ASCII constants, so it cannot fail to serialise.
std::string render_internal_error_fallback(const char* code);

}  // namespace execcontract
