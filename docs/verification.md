# Verification record (release 0.1.0)

Everything below was executed; nothing is extrapolated. Date: 2026-11-07.
Figures refer to the final tree (after the independent-review fixes).

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

Source tree on the Windows drive (`/mnt/d/...`, drvfs) for development; build
trees and all test temporary directories on the WSL ext4 file system. The
clean-clone check below used a clone on ext4.

## Full verification

```bash
scripts/verify.sh ~/ec-verify     # fresh build root
```

Exit status 0. Steps and results:

| Step | Configuration | Result |
|---|---|---|
| Normal build | RelWithDebInfo, `-DEXECCONTRACT_WERROR=ON` | 0 warnings |
| Normal tests | `ctest` | 100 % passed, 0 failed out of 103 (102 unit tests + 1 component entry running 96 pytest tests) |
| Sanitizer build | Debug, `-DEXECCONTRACT_SANITIZE=ON` (ASan + UBSan, `-fno-sanitize-recover=all`), `-Werror` | 0 warnings |
| Sanitizer tests | `ctest` | 100 % passed, 0 failed out of 103 (102 unit; 85 component, 11 `exact_fds` deselected by design) |
| Formatting | `clang-format --dry-run --Werror` on `src/`, `tests/unit/` | clean |

Component tests run directly on the normal build: **96 passed, 0 skipped**.
Under ASan the 85 compatible tests pass; the 11 `exact_fds` tests were also
run against the ASan binary for information and passed in this environment,
but they are not part of the sanitizer configuration because extra runtime
descriptors are environment-dependent. No ASan or UBSan report occurred.

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
`Evaluate.ObservationWithNeitherValueNorErrorFailsClosed`. Final tree:
0 findings.

## Mutation check

Each mutant injects one plausible defect into a scratch copy, rebuilds and
runs both suites. A useful test suite must fail.

| Mutant | Unit failures | Component failures |
|---|---|---|
| Scanner does not exclude its own `dirfd()` | 3 | 11 |
| `readdir` error treated as end-of-directory | 1 | 0 |
| Partial inventory treated as complete | 1 | 0 |
| Violation outranks inspection error | 6 | 4 |
| Report delivery failure ignored | 0 | 6 |
| SIGPIPE not ignored | 0 | 1 |
| Duplicate JSON keys accepted | 1 | 1 |
| `RLIM_INFINITY` reported as a number | 1 | 2 |
| cwd taken from `$PWD` | 4 | 12 |
| A descriptor opened before the snapshot | 0 | 11 |

All 10 mutants are detected. Before the review fixes, the `readdir`-error
mutant survived both suites; it is now caught by
`FdScan.ReaddirErrorMidwayIsAnErrorNotEndOfDirectory` through the injected
`readdir` seam (unit level only — a real procfs `readdir` failure still
cannot be produced unprivileged).

## Offline build (NFR-4)

Configure and build inside an unprivileged network namespace
(`unshare -rn`), where name resolution of `archive.ubuntu.com` fails:
succeeded. (`/sys/class/net` still lists the host interfaces because sysfs is
not remounted; the failed lookup is the evidence of no network.)

## Latency (NFR-8)

`scripts/measure_latency.py build/execcontract 200` — 200 sequential runs with
`examples/minimal.json`, timed around `subprocess.run` (includes fork/exec,
loading and teardown), normal build. Every measurement taken is listed:

| Run | Build | Median | p95 | Max |
|---|---|---|---|---|
| 1 | before review fixes | 4.14 ms | 5.91 ms | 9.37 ms |
| 2 | before review fixes | 3.97 ms | 6.90 ms | 10.28 ms |
| 3 | final (directly after two builds) | 7.11 ms | 26.56 ms | 33.31 ms |
| 4 | final | 6.34 ms | 29.07 ms | 35.18 ms |
| 5 | final | 12.70 ms | 30.28 ms | 35.18 ms |
| 6 | final | 8.10 ms | 28.47 ms | 39.72 ms |

Target ≤ 20 ms median: **met in every run** on the reference environment.
Variance between runs on WSL2 is large (median 4–13 ms, p95 up to 30 ms); the
review fixes add one `stat` only when `--report` is used, which these runs do
not use, so the spread is attributed to host load rather than the code, but
that attribution is not proven. No p95 target was set. Not measured elsewhere.

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

## Clean-clone README check

Fresh `git clone` into WSL ext4, then exactly the README commands
(`cmake -S . -B build -G Ninja`, `cmake --build build`,
`ctest --test-dir build --output-on-failure`,
`examples/demo/run-demo.sh build/execcontract`), the CONTRIBUTING label
commands (`ctest -L unit`, `ctest -L component`, direct pytest) and
`DESTDIR=… cmake --install build`.

First attempt (commit `d968192`): build and all tests passed, but the README
demo command failed — `run-demo.sh` changed directory before running a
**relative** binary path (`build/execcontract: not found`). Fixed in
`6b5ee65`, with a regression test that fails on the old script. Final result:
see the end of this file.

## Skipped or unverified

| Item | Status |
|---|---|
| Conditional component skips (hard limits too low, non-UTF-8 names rejected, finite hard `RLIMIT_CORE`, root) | None triggered in the reference environment |
| `readdir` failure on real procfs | Not reproducible unprivileged; covered by the injected-`readdir` unit test |
| Python 3.8–3.13 | Claimed compatible (`from __future__ import annotations` added); only 3.14 run |
| Other distributions, Clang, musl, non-WSL kernels | Not verified |
| CI | No CI workflow in this release; nothing was run in CI |

## Independent review

A separate reviewer agent (an AI subagent with no part in writing the code)
was given the specification, sources, tests and this evidence and asked to
attack false-pass paths. It ran its own build and experiments. Findings and
resolution:

| # | Severity | Finding (confirmed by the reviewer by running it) | Resolution |
|---|---|---|---|
| F1 | major — false pass | A raw NUL byte ended the contract: `{…weak…}\0{…strict…}` → exit 0 `passed` (nlohmann treats NUL as end of input) | NUL rejected as `malformed_json`; unit + component tests (fail on the old binary) |
| F2 | major | `1e999` threw `json::out_of_range`, which escaped to "internal error", exit 3, no report | Mapped to `out_of_range`, exit 2 with report; tests |
| F3 | minor/major | Invalid UTF-8 in the contract → library message quoted raw bytes → strict serialiser failed → exit 3 `serialization_failed`; the unit test only checked the parser | Messages ASCII-escaped; end-to-end unit test renders the report; component tests |
| F4 | minor | `--report` = contract path silently overwrote the contract | Refused (same dev/inode, symlinks followed), exit 2; test |
| F5 | minor | `--report` to a FIFO without reader hung forever | Opened `O_NONBLOCK` (cleared afterwards) → `ENXIO`, exit 3; test |
| F6 | minor | A stale earlier report file remains if the new one cannot be opened (exit status correct) | Documented in README/requirements; atomic write on the roadmap |
| F7 | minor | Tests need Python ≥ 3.10 (`dict \| None` annotation) despite claiming 3.8 | `from __future__ import annotations`; README says only 3.14 verified |
| F8 | minor | Documented code `status_too_large` does not exist; stale test counts | Docs corrected |
| F9 | minor | `readdir` error branch untested | Injected-`readdir` seam + two unit tests; mutant now caught |
| nit | — | `--contract --help` printed help (exit 0) | Help/version only in option position; tests |
| nit | — | Error pointers not RFC 6901-escaped | Escaped; test |
| nit | — | BOM accepted but undocumented | Documented; tests |
| nit | — | `test_malformed_contract` accepted two codes for every input | One expected code per input |

The reviewer also reported as sound: scanner exclusion (including with each of
fds 0, 1, 2 closed), contamination ordering (no static initialisers per `nm`),
enumeration-failure handling, duplicate detection through `\u` escapes,
numeric boundaries, precedence, every output-failure case listed above, RAII
and scope compliance. This review was performed by an AI agent; no human
review has taken place.

## Traceability

| Req. | Unit tests | Component tests / other evidence |
|---|---|---|
| FR-1 CLI | `Cli.*` | `test_input.py::test_usage_errors`, `test_help_version`, `test_option_value_named_help_is_a_path` |
| FR-2 contract file | `ContractFile.*` | `test_input.py::test_missing_contract`, `test_unreadable_contract`, `test_directory_and_fifo_contracts`, `test_oversized_contract_boundary` |
| FR-3 validation | `ContractParse.*` (27 tests) | `test_input.py::test_invalid_contracts`, `test_malformed_contract`, `test_duplicate_key`, `test_embedded_nul_does_not_truncate_the_contract`, `test_number_overflow_is_invalid_contract`, `test_invalid_utf8_contract_still_gets_a_report`, `test_utf8_bom_is_accepted`; `test_umask.py::test_numeric_umask_is_rejected`; `test_rlimits.py::test_rlim_infinity_as_number_is_rejected`; `test_cwd.py::test_trailing_slash_in_contract_is_rejected` |
| FR-4 inventory | `FdScan.*` | `test_fds.py::test_scanner_contamination_fd3`, `test_scanner_contamination_fd0`, `test_descriptor_above_lowered_soft_limit`, `test_inventory_at_cap_is_complete`, `test_inventory_beyond_cap_is_inspection_error` |
| FR-5 policy | `EvaluateFds.*` | `test_fds.py::test_required_present_passes`, `test_required_missing_fails`, `test_permitted_absent_passes`, `test_unexpected_descriptor_fails` |
| FR-6 types | `FdType.*`, `EvaluateFds.TypeMatchAndMismatch`, `EvaluateFds.UnavailableTypeIsAnErrorOnlyWhenATypeIsExpected` | `test_fds.py::test_type_matches` (6), `test_type_mismatch_fails` (3), `test_deleted_file_descriptor_still_classified` |
| FR-7 cwd | `CwdTest.*`, `EvaluateCwd.*`, `Utf8.*` | `test_cwd.py` (all 11) |
| FR-8 umask | `Umask.*`, `EvaluateUmask.*` | `test_umask.py` (6); non-mutation by design + `Umask.LiveObservationMatchesAndDoesNotChangeIt` |
| FR-9 limits | `Rlimit.*`, `EvaluateRlimit.*`, `LimitValueEquality.*`, `ContractParse.NumericBoundaries` | `test_rlimits.py` (8) |
| FR-10 report | `Report.*`, `Evaluate.AreaOrderIsFixed` | `test_reporting.py::test_report_schema_and_check_order`, `test_deterministic_output` |
| FR-11 outcomes | `Aggregate.*`, `Evaluate.ObservationWithNeitherValueNorErrorFailsClosed` | `test_reporting.py::test_mixed_violation_and_inspection_error`, `test_input.py::test_invalid_contract_takes_precedence_over_inspection_failure`; exit code checked against the report in every component test (`conftest.run_probe`) |
| FR-12 output failure | `WriteAll.*`, `UniqueFd.CloseCheckedReleasesEvenOnError` | `test_reporting.py::test_stdout_closed_never_succeeds`, `test_stdout_full_never_succeeds`, `test_broken_pipe_exits_3_not_sigpipe`, `test_report_file_failure_never_succeeds` (2), `test_report_to_fifo_without_reader_fails_instead_of_hanging`, `test_report_path_equal_to_contract_is_refused`, `test_report_file_when_stdout_is_dev_null` |
| FR-13 order | `FdScan.LiveProcfsSeesOwnDescriptorAndClosesScanner` | `test_fds.py::test_scanner_contamination_fd3`; `test_reporting.py::test_report_file_when_stdout_is_dev_null`; mutation "descriptor opened before the snapshot"; review of `src/main.cpp` |
| FR-14 demo | — | `test_demo.py` (4, incl. relative binary path); `run-demo.sh` run above |
| NFR-1 unprivileged | — | All runs as uid 1000 |
| NFR-2 bounded | `ContractFile.SizeCapBoundary`, `ContractParse.DescriptorEntryCap`, `ContractParse.CwdLengthCap`, `ContractParse.DepthLimit`, `FdScan.CapBoundary`, `Umask.ReadFromFileAndMissingFile`, `CwdTest.LongPathGrowsBuffer`, `ReadBounded.CapBoundary` | `test_input.py::test_oversized_contract_boundary`, `test_fds.py::test_inventory_*` |
| NFR-3 deterministic | `Report.Deterministic` | `test_reporting.py::test_deterministic_output` |
| NFR-4 offline | — | Offline build above; `find_package` only |
| NFR-5 reproducible | — | `scripts/verify.sh` run above; clean-clone check |
| NFR-6 read-only | `Umask.LiveObservationMatchesAndDoesNotChangeIt`, `FdScan.LiveProcfsSeesOwnDescriptorAndClosesScanner` | Review: no `umask`, `setrlimit`, `chdir`, `dup`, or `close` on borrowed descriptors in `src/` (the only `fcntl` is on the owned report descriptor) |
| NFR-7 single-threaded | — | Review: no threads; reviewer checked `nm` for static initialisers |
| NFR-8 latency | — | Measurements above |
| NFR-9 warnings, sanitizers | — | `verify.sh` run above |
