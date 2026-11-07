// CLI orchestration. Order matters and is part of the product contract:
//
//   1. collect_snapshot()   descriptor inventory is taken before ExecContract
//                           opens anything of its own
//   2. parse arguments      pure
//   3. ignore SIGPIPE       so a broken pipe becomes an EPIPE exit 3
//   4. load + validate the contract (opens, reads, closes the contract file)
//   5. evaluate             pure
//   6. render + deliver     the only write to an inherited descriptor
//   7. stderr summary       best effort, after the report channel is closed
//
// No namespace-scope object in this program opens a resource during static
// initialisation; keep it that way.

#include "cli.hpp"
#include "contract.hpp"
#include "evaluate.hpp"
#include "observe.hpp"
#include "report.hpp"
#include "sys.hpp"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <exception>
#include <string>
#include <vector>

namespace ec = execcontract;

namespace {

void ignore_sigpipe() {
    struct sigaction sa{};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    (void)::sigaction(SIGPIPE, &sa, nullptr);
}

void stderr_line(const std::string& text) {
    (void)ec::write_all(STDERR_FILENO, "execcontract: " + text + "\n");
}

// Returns 0 or the errno of the failure; `stage` names the failing step.
int deliver(const std::string& text, const std::optional<std::string>& path, const char*& stage) {
    if (!path) {
        stage = "write stdout";
        return ec::write_all(STDOUT_FILENO, text);  // borrowed: written, never closed
    }
    stage = "open report file";
    ec::UniqueFd fd(::open(path->c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOCTTY, 0666));
    if (!fd) {
        return errno;
    }
    stage = "write report file";
    if (const int err = ec::write_all(fd.get(), text); err != 0) {
        return err;
    }
    stage = "close report file";
    return fd.close_checked();  // close can report deferred write errors (e.g. NFS)
}

std::string summary_line(const ec::ReportInput& in) {
    std::size_t passed = 0, failed = 0, errors = 0;
    for (const ec::Check& c : in.checks) {
        passed += c.status == ec::Status::pass;
        failed += c.status == ec::Status::fail;
        errors += c.status == ec::Status::error;
    }
    const std::string line = ec::to_string(in.outcome);
    if (in.error) {
        return line + " (" + in.error->code + ")";
    }
    return line + " (" + std::to_string(in.checks.size()) + " checks: " + std::to_string(passed) +
           " passed, " + std::to_string(failed) + " failed, " + std::to_string(errors) + " errors)";
}

int run_check(const ec::Snapshot& snapshot, const ec::CliOptions& opts) {
    ec::ReportInput in;
    const ec::ContractText file = ec::load_contract_file(opts.contract_path);
    if (!file.text) {
        const ec::ContractError e =
            file.error.value_or(ec::ContractError{"contract_read_failed", "no contract text", "", 0});
        in.outcome = ec::Outcome::invalid_contract;
        in.error = ec::ReportError{e.code, e.message, e.err, e.pointer};
    } else {
        // The contract is validated before any requirement is interpreted,
        // even though the snapshot already exists.
        const ec::ContractResult parsed = ec::parse_contract(*file.text);
        if (!parsed.contract) {
            const ec::ContractError e =
                parsed.error.value_or(ec::ContractError{"malformed_json", "no contract", "", 0});
            in.outcome = ec::Outcome::invalid_contract;
            in.error = ec::ReportError{e.code, e.message, 0, e.pointer};
        } else {
            in.checks = ec::evaluate(*parsed.contract, snapshot);
            in.unchecked = ec::unchecked_areas(*parsed.contract);
            in.outcome = ec::aggregate(in.checks);
        }
    }

    std::string text;
    try {
        text = ec::render_report(in);
    } catch (const std::exception&) {
        in = ec::ReportInput{};
        in.outcome = ec::Outcome::internal_error;
        in.error = ec::ReportError{"serialization_failed", "report could not be serialised", 0, ""};
        text = ec::render_internal_error_fallback("serialization_failed");
    }

    const char* stage = "";
    if (const int err = deliver(text, opts.report_path, stage); err != 0) {
        // The report channel failed, so it cannot carry this error.
        stderr_line(std::string("report delivery failed (") + stage + "): " + ec::errno_message(err));
        return 3;
    }
    stderr_line(summary_line(in));
    return ec::exit_code_for(in.outcome);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        // (1) Must be the first thing main does.
        const ec::Snapshot snapshot = ec::collect_snapshot();

        // (2)
        std::vector<std::string_view> args;
        for (int i = 1; i < argc; ++i) {
            args.emplace_back(argv[i]);
        }
        const ec::CliResult cli = ec::parse_args(args);

        // (3)
        ignore_sigpipe();

        if (!cli.options) {
            stderr_line(cli.error);
            (void)ec::write_all(STDERR_FILENO, ec::usage_text());
            return 2;
        }
        switch (cli.options->mode) {
            case ec::CliOptions::Mode::help:
                return ec::write_all(STDOUT_FILENO, ec::usage_text()) == 0 ? 0 : 3;
            case ec::CliOptions::Mode::version:
                return ec::write_all(STDOUT_FILENO,
                                     std::string("execcontract ") + ec::tool_version() + "\n") == 0
                           ? 0
                           : 3;
            case ec::CliOptions::Mode::check: break;
        }
        // (4)-(7)
        return run_check(snapshot, *cli.options);
    } catch (const std::exception&) {
        // e.g. std::bad_alloc. The report channel is not known reliably at
        // this point, so only stderr is used; the exit status carries the
        // failure.
        ignore_sigpipe();
        stderr_line("internal error");
        return 3;
    }
}
