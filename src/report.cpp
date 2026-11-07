#include "report.hpp"

#include <nlohmann/json.hpp>

#ifndef EXECCONTRACT_VERSION
#error "EXECCONTRACT_VERSION must be defined by the build system"
#endif

namespace execcontract {
namespace {

using ojson = nlohmann::ordered_json;

ojson nullable_string(const std::optional<std::string>& s) {
    return s ? ojson(*s) : ojson(nullptr);
}

ojson errno_json(int err) {
    return err != 0 ? ojson(err) : ojson(nullptr);
}

ojson check_error_json(const std::optional<SysError>& e) {
    if (!e) {
        return nullptr;
    }
    return ojson{
        {"code", e->code},
        {"errno", errno_json(e->err)},
        {"errno_name", nullable_string(errno_name(e->err))},
        {"message", e->err != 0 ? e->detail + ": " + errno_message(e->err) : e->detail},
    };
}

}  // namespace

const char* tool_version() noexcept {
    return EXECCONTRACT_VERSION;
}

std::string render_report(const ReportInput& in) {
    std::size_t passed = 0, failed = 0, errors = 0;
    ojson checks = ojson::array();
    for (const Check& c : in.checks) {
        passed += c.status == Status::pass;
        failed += c.status == Status::fail;
        errors += c.status == Status::error;
        checks.push_back(ojson{
            {"id", c.id},
            {"status", to_string(c.status)},
            {"expected", c.expected},
            {"observed", c.observed},
            {"reason", c.reason.empty() ? ojson(nullptr) : ojson(c.reason)},
            {"diagnostic", c.diagnostic},
            {"error", check_error_json(c.error)},
        });
    }
    ojson error = nullptr;
    if (in.error) {
        const ReportError& e = *in.error;
        error = ojson{
            {"code", e.code},
            {"message", e.err != 0 ? e.message + ": " + errno_message(e.err) : e.message},
            {"errno", errno_json(e.err)},
            {"errno_name", nullable_string(errno_name(e.err))},
            {"pointer", e.pointer.empty() ? ojson(nullptr) : ojson(e.pointer)},
        };
    }
    const ojson report{
        {"report_version", kReportVersion},
        {"tool", ojson{{"name", "execcontract"}, {"version", tool_version()}}},
        {"outcome", to_string(in.outcome)},
        {"exit_code", exit_code_for(in.outcome)},
        {"summary",
         ojson{{"checks", in.checks.size()}, {"passed", passed}, {"failed", failed}, {"errors", errors}}},
        {"unchecked", in.unchecked},
        {"checks", std::move(checks)},
        {"error", std::move(error)},
    };
    return report.dump(2, ' ', /*ensure_ascii=*/false, nlohmann::json::error_handler_t::strict) + "\n";
}

std::string render_internal_error_fallback(const char* code) {
    return std::string(
               "{\n  \"report_version\": 1,\n  \"tool\": {\"name\": \"execcontract\", \"version\": \"") +
           tool_version() +
           "\"},\n  \"outcome\": \"internal_error\",\n  \"exit_code\": 3,\n"
           "  \"summary\": {\"checks\": 0, \"passed\": 0, \"failed\": 0, \"errors\": 0},\n"
           "  \"unchecked\": [],\n  \"checks\": [],\n  \"error\": {\"code\": \"" +
           code +
           "\", \"message\": \"internal error\", \"errno\": null, \"errno_name\": null, \"pointer\": "
           "null}\n}\n";
}

}  // namespace execcontract
