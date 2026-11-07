#include "cli.hpp"

namespace execcontract {

CliResult parse_args(const std::vector<std::string_view>& args) {
    CliResult result;
    // --help / --version win over everything else, but only in option
    // position: in "--contract --help" the word is the contract path.
    bool help = false;
    bool version = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        if (a == "--contract" || a == "--report") {
            ++i;  // skip the option's value
        } else if (a == "--help" || a == "-h") {
            help = true;
        } else if (a == "--version") {
            version = true;
        }
    }
    if (help || version) {
        result.options =
            CliOptions{help ? CliOptions::Mode::help : CliOptions::Mode::version, {}, std::nullopt};
        return result;
    }

    CliOptions opts;
    bool have_contract = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        std::string_view name = a;
        std::optional<std::string_view> value;
        if (const auto eq = a.find('='); a.substr(0, 2) == "--" && eq != std::string_view::npos) {
            name = a.substr(0, eq);
            value = a.substr(eq + 1);
        }
        if (name != "--contract" && name != "--report") {
            result.error = "unrecognised argument: " + std::string(a);
            return result;
        }
        if (!value) {
            if (i + 1 >= args.size()) {
                result.error = "option " + std::string(name) + " requires a value";
                return result;
            }
            value = args[++i];
        }
        if (value->empty()) {
            result.error = "option " + std::string(name) + " requires a non-empty value";
            return result;
        }
        if (name == "--contract") {
            if (have_contract) {
                result.error = "--contract given more than once";
                return result;
            }
            have_contract = true;
            opts.contract_path = std::string(*value);
        } else {
            if (opts.report_path) {
                result.error = "--report given more than once";
                return result;
            }
            opts.report_path = std::string(*value);
        }
    }
    if (!have_contract) {
        result.error = "missing required option --contract";
        return result;
    }
    result.options = std::move(opts);
    return result;
}

std::string usage_text() {
    return "Usage: execcontract --contract PATH [--report PATH]\n"
           "       execcontract --help | --version\n"
           "\n"
           "Inspect this process's own execution context (descriptors, working\n"
           "directory, umask, RLIMIT_NOFILE, RLIMIT_CORE) and compare it with the\n"
           "JSON execution contract at PATH. The JSON report is written to stdout,\n"
           "or to the --report file.\n"
           "\n"
           "Exit status: 0 passed, 1 violated, 2 usage or contract error,\n"
           "             3 inspection, internal or report-delivery failure.\n";
}

}  // namespace execcontract
