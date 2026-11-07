# Verification record (release 0.1.0)

Everything below was executed; nothing is extrapolated. Date: 2026-11-07.

## Reference environment

| Item | Value |
|---|---|
| Host | Windows 11, WSL2 |
| Linux | Ubuntu 26.04.1 LTS, kernel 6.18.33.2-microsoft-standard-WSL2, glibc 2.43 |
| CPU | Intel Core i7-13700H, 20 logical CPUs visible to WSL |
| Compiler | GCC 15.2.0 (Ubuntu 15.2.0-16ubuntu1) |
| Build | CMake 4.2.3, Ninja 1.13.2 |
| Libraries | GoogleTest 1.17.0, nlohmann/json 3.11.3 (Ubuntu packages) |
| Python | 3.14.4, pytest 9.0.2 |
| Tools | clang-format 21.1.8, clang-tidy (LLVM 21.1.8) |
| Inherited limits (login shell) | `RLIMIT_NOFILE` 10240 / 1048576, `RLIMIT_CORE` 0 / unlimited, umask 0022 |
| User | unprivileged (uid 1000) |

Source tree on the Windows drive (`/mnt/d/...`, drvfs); build trees and all
test temporary directories on the WSL ext4 file system.

## Full verification

```bash
scripts/verify.sh ~/ec-verify     # fresh build root
```

Exit status 0. Steps and results:

| Step | Configuration | Result |
|---|---|---|
| Normal build | RelWithDebInfo, `-DEXECCONTRACT_WERROR=ON` | 0 warnings |
| Normal tests | `ctest` | 100 % passed, 0 failed out of 94 (93 unit tests + 1 component entry = 85 pytest tests) |
| Sanitizer build | Debug, `-DEXECCONTRACT_SANITIZE=ON` (ASan + UBSan, `-fno-sanitize-recover=all`), `-Werror` | 0 warnings |
| Sanitizer tests | `ctest` | 100 % passed, 0 failed out of 94 (93 unit; 74 component, 11 `exact_fds` deselected by design) |
| Formatting | `clang-format --dry-run --Werror` on `src/`, `tests/unit/` | clean |

Component tests run directly on the normal build: **85 passed, 0 skipped**.
Under ASan the 74 compatible tests pass; the 11 `exact_fds` tests were also
run against the ASan binary for information and passed in this environment,
but they are not part of the sanitizer configuration because extra runtime
descriptors are environment-dependent.

## Static analysis

```bash
clang-tidy -p build -checks='-*,bugprone-*,performance-*,cert-err33-c,misc-const-correctness,-bugprone-easily-swappable-parameters' src/*.cpp
```

First run: 16 findings — 8 `misc-const-correctness`, 1 unchecked `snprintf`
result, 7 `bugprone-unchecked-optional-access`. The last group was a real
robustness gap: evaluation relied on the invariant "an `Observed<T>` holds a
value or an error" without checking it, so a default-constructed observation
would have dereferenced an empty optional. Evaluation now fails closed
(`observation_missing`), covered by
`Evaluate.ObservationWithNeitherValueNorErrorFailsClosed`. After the fixes:
0 findings.

## Mutation check

Each mutant injects one plausible defect into a scratch copy, rebuilds and
runs both suites. A useful test suite must fail.

| Mutant | Unit failures | Component failures |
|---|---|---|
| Scanner does not exclude its own `dirfd()` | 3 | 10 |
| `readdir` error treated as end-of-directory | 0 | 0 |
| Partial inventory treated as complete | 1 | 0 |
| Violation outranks inspection error | 5 | 4 |
| Report delivery failure ignored | 0 | 5 |
| SIGPIPE not ignored | 0 | 1 |
| Duplicate JSON keys accepted | 1 | 1 |
| `RLIM_INFINITY` reported as a number | 1 | 2 |
| cwd taken from `$PWD` | 4 | 12 |
| A descriptor opened before the snapshot | 0 | 10 |

9 of 10 mutants are detected. The `readdir`-error mutant survives: a procfs
`readdir` failure cannot be produced unprivileged, so that branch is verified
by code review only (recorded in [ROADMAP.md](../ROADMAP.md)).

## Offline build (NFR-4)

Configure and build inside an unprivileged network namespace
(`unshare -rn`), where name resolution of `archive.ubuntu.com` fails:
succeeded. (`/sys/class/net` still lists the host interfaces because sysfs is
not remounted; the failed lookup is the evidence of no network.)

## Latency (NFR-8)

`scripts/measure_latency.py build/execcontract 200` — 200 sequential runs with
`examples/minimal.json`, timed around `subprocess.run` (includes fork/exec,
loading and teardown), normal build:

| Run | Median | p95 | Max |
|---|---|---|---|
| 1 | 4.14 ms | 5.91 ms | 9.37 ms |
| 2 | 3.97 ms | 6.90 ms | 10.28 ms |

Target ≤ 20 ms median: **met** on the reference environment. Not measured
elsewhere.

## Demonstration (FR-14)

`examples/demo/run-demo.sh build/execcontract`: exit 0. The defective launcher
produced `violated` (exit 1) with failed checks `fd.unexpected.3`,
`fd.unexpected.7`, `cwd`, `umask`, `rlimit_nofile.soft`; the corrected launcher
produced `passed` (exit 0).

`fd.unexpected.7` was not planted by the demo. Processes started through
`wsl.exe … bash` inherit descriptor 7 → `/dev/ptmx` from WSL's `Relay`
process, opened `O_RDWR` without `O_CLOEXEC` (`/proc/<pid>/fdinfo/7` flags
`0100002`). ExecContract reported it as an unexpected `char_device`; the
corrected launcher closes descriptors 3–9 and therefore also removes it.
pytest-driven runs are unaffected because `subprocess` closes inherited
descriptors by default.

## Skipped or unverified

| Item | Status |
|---|---|
| Conditional component skips (hard limits too low, non-UTF-8 names rejected, finite hard `RLIMIT_CORE`, root) | None triggered in the reference environment |
| `readdir` error branch | Review only (see mutation check) |
| Other distributions, Clang, musl, non-WSL kernels | Not verified |
| CI | No CI workflow in this release; nothing was run in CI |
| Independent review | See the section below |

## Independent review

See the end of this file (added after the review).

## Traceability

| Req. | Unit tests | Component tests / other evidence |
|---|---|---|
| FR-1 CLI | `Cli.*` | `test_input.py::test_usage_errors`, `test_help_version` |
| FR-2 contract file | `ContractFile.*` | `test_input.py::test_missing_contract`, `test_unreadable_contract`, `test_directory_and_fifo_contracts`, `test_oversized_contract_boundary` |
| FR-3 validation | `ContractParse.*` (22 tests) | `test_input.py::test_invalid_contracts`, `test_malformed_contract`, `test_duplicate_key`; `test_umask.py::test_numeric_umask_is_rejected`; `test_rlimits.py::test_rlim_infinity_as_number_is_rejected`; `test_cwd.py::test_trailing_slash_in_contract_is_rejected` |
| FR-4 inventory | `FdScan.*` | `test_fds.py::test_scanner_contamination_fd3`, `test_scanner_contamination_fd0`, `test_descriptor_above_lowered_soft_limit`, `test_inventory_at_cap_is_complete`, `test_inventory_beyond_cap_is_inspection_error` |
| FR-5 policy | `EvaluateFds.*` | `test_fds.py::test_required_present_passes`, `test_required_missing_fails`, `test_permitted_absent_passes`, `test_unexpected_descriptor_fails` |
| FR-6 types | `FdType.*`, `EvaluateFds.TypeMatchAndMismatch`, `EvaluateFds.UnavailableTypeIsAnErrorOnlyWhenATypeIsExpected` | `test_fds.py::test_type_matches` (6), `test_type_mismatch_fails` (3), `test_deleted_file_descriptor_still_classified` |
| FR-7 cwd | `CwdTest.*`, `EvaluateCwd.*`, `Utf8.*` | `test_cwd.py` (all 11) |
| FR-8 umask | `Umask.*`, `EvaluateUmask.*` | `test_umask.py` (6); non-mutation by design + `Umask.LiveObservationMatchesAndDoesNotChangeIt` |
| FR-9 limits | `Rlimit.*`, `EvaluateRlimit.*`, `LimitValueEquality.*`, `ContractParse.NumericBoundaries` | `test_rlimits.py` (8) |
| FR-10 report | `Report.*`, `Evaluate.AreaOrderIsFixed` | `test_reporting.py::test_report_schema_and_check_order`, `test_deterministic_output` |
| FR-11 outcomes | `Aggregate.*`, `Evaluate.ObservationWithNeitherValueNorErrorFailsClosed` | `test_reporting.py::test_mixed_violation_and_inspection_error`, `test_input.py::test_invalid_contract_takes_precedence_over_inspection_failure`; exit code checked against the report in every component test (`conftest.run_probe`) |
| FR-12 output failure | `WriteAll.*`, `UniqueFd.CloseCheckedReleasesEvenOnError` | `test_reporting.py::test_stdout_closed_never_succeeds`, `test_stdout_full_never_succeeds`, `test_broken_pipe_exits_3_not_sigpipe`, `test_report_file_failure_never_succeeds` (2), `test_report_file_when_stdout_is_dev_null` |
| FR-13 order | `FdScan.LiveProcfsSeesOwnDescriptorAndClosesScanner` | `test_fds.py::test_scanner_contamination_fd3`; `test_reporting.py::test_report_file_when_stdout_is_dev_null`; mutation "descriptor opened before the snapshot"; review of `src/main.cpp` |
| FR-14 demo | — | `test_demo.py` (3); `run-demo.sh` run above |
| NFR-1 unprivileged | — | All runs as uid 1000 |
| NFR-2 bounded | `ContractFile.SizeCapBoundary`, `ContractParse.DescriptorEntryCap`, `ContractParse.CwdLengthCap`, `ContractParse.DepthLimit`, `FdScan.CapBoundary`, `Umask.ReadFromFileAndMissingFile`, `CwdTest.LongPathGrowsBuffer`, `ReadBounded.CapBoundary` | `test_input.py::test_oversized_contract_boundary`, `test_fds.py::test_inventory_*` |
| NFR-3 deterministic | `Report.Deterministic` | `test_reporting.py::test_deterministic_output` |
| NFR-4 offline | — | Offline build above; `find_package` only |
| NFR-5 reproducible | — | `scripts/verify.sh` run above; clean-clone README check (below) |
| NFR-6 read-only | `Umask.LiveObservationMatchesAndDoesNotChangeIt`, `FdScan.LiveProcfsSeesOwnDescriptorAndClosesScanner` | Review: no `umask`, `setrlimit`, `chdir`, `close` on borrowed descriptors in `src/` |
| NFR-7 single-threaded | — | Review: no threads, no static initialisers that open resources |
| NFR-8 latency | — | Measurement above |
| NFR-9 warnings, sanitizers | — | `verify.sh` run above |
