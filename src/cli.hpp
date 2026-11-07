// Command-line parsing. Pure function over the argument list.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace execcontract {

struct CliOptions {
    enum class Mode { check, help, version };
    Mode mode = Mode::check;
    std::string contract_path;
    std::optional<std::string> report_path;
};

struct CliResult {
    std::optional<CliOptions> options;
    std::string error;  // set iff !options
};

// `args` excludes argv[0].
CliResult parse_args(const std::vector<std::string_view>& args);

std::string usage_text();

}  // namespace execcontract
