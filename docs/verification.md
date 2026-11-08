# Verification record (release 0.1.0)

This file records what was actually executed, by whom, on which commit, and
with which result. Nothing here is extrapolated.

**Who ran what.** Every build, test run and experiment below was executed by
the AI coding agent (Claude Code) in the author's WSL environment, unless a
row says otherwise. Two later AI reviews read the code; the second could not
execute Linux at all. The author has not personally executed these runs; see
[docs/INTERVIEW_PREPARATION.md](INTERVIEW_PREPARATION.md#12-before-claiming-personal-implementation-or-testing-experience).

**Final code commit:** `9ccb680` (2026-11-08). Documentation-only commits
follow it; they do not change what was tested.

## 1. Reference environment

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

All final runs used a fresh `git clone` on the WSL ext4 file system.

A second environment, Ubuntu 24.04 with GCC 13 on GitHub Actions, is defined
in `.github/workflows/ci.yml`. Its first run (run 42427800544, commit
`771ea5d`, 2026-11-08) built both configurations without warnings and passed
**104/104 in the normal build and 104/104 in the ASan + UBSan build**, then
failed at the formatting step: the runner image ships clang-format 18, which
formats `struct stat st{}` differently from the clang-format 21 used here.
The workflow now skips that one check with a printed reason
(`EXECCONTRACT_SKIP_FORMAT=1`); later runs are on the repository's Actions
tab.

## 2. Results at the final code commit `9ccb680`

| Check | Command (from the clone) | Result |
|---|---|---|
| Full verification | `scripts/verify.sh <fresh build root>` | exit 0 |
| Normal build | RelWithDebInfo, `BUILD_TESTS=ON`, `SANITIZE=OFF`, `-Werror` | 0 warnings; 103 unit tests + 1 component entry registered |
| Normal tests | `ctest --no-tests=error` | 104/104 (103 unit + component entry) |
| Component tests, direct | `pytest tests/component` | **97 passed, 0 skipped** |
| Sanitizer build | Debug, ASan + UBSan, `-fno-sanitize-recover=all`, `-Werror` | 0 warnings; 104/104 |
| Component under ASan | compatible tests (`-m "not exact_fds"`) | 82 passed, 15 deselected by design; the 15 `exact_fds` tests also pass under ASan when run separately (informational) |
| Formatting | `clang-format --dry-run --Werror` | clean |
| Static analysis | `clang-tidy` (`bugprone-*`, `performance-*`, `cert-err33-c`, `misc-const-correctness`) on `src/` | 0 findings |
| Offline build | configure + build inside `unshare -rn` (no name resolution) | OK |
| README demo | `examples/demo/run-demo.sh <binary>` | exit 0 (defective launcher 1, corrected 0) |
| Install | `DESTDIR=… cmake --install` | `usr/local/bin/execcontract`; `--version` → `execcontract 0.1.0` |

### Descriptor-limit matrix

Run with `prlimit --nofile=SOFT:HARD`:

| Limit | Unit | Component |
|---|---|---|
| 10240 : 1048576 (default) | 103 passed | 97 passed |
| 4096 : 4096 | 103 passed | 97 passed |
| 1024 : 1024 | 103 passed | 95 passed, 2 skipped (inventory-cap tests need hard ≥ 2048) |
| 256 : 4096 | 103 passed | 97 passed |
| 128 : 128 | 102 passed, 1 skipped (fd 500 cannot be placed) | 89 passed, 8 skipped (cap, high-fd, corrected-launcher demo and two limit tests; each states the limit it needs) |

`prlimit --nofile=128:128 -- ctest`: 104/104 (skips count as passing in CTest).

### Latency (NFR-8, target: median ≤ 20 ms)

`scripts/measure_latency.py <binary> 200`: 200 sequential runs of the binary
with `examples/minimal.json`, timed around `subprocess.run`, so the figure
includes fork/exec, dynamic loading and teardown. Normal build, contract on
ext4, machine otherwise idle:

| Run | Median | p95 | Max |
|---|---|---|---|
| 1 | 0.99 ms | 1.39 ms | 7.23 ms |
| 2 | 0.98 ms | 1.39 ms | 2.19 ms |
| 3 | 0.99 ms | 1.42 ms | 6.04 ms |

Target met. Earlier measurements during development (4–13 ms) read the
contract from the Windows drive through WSL's drvfs mount and mostly measured
that file system. Not measured on other machines.

## 3. Mutation check

Each mutant injects one realistic defect into a scratch copy of `9ccb680`,
is built in a fresh build directory, and runs both full suites. A mutant is
*detected* if at least one test fails.

| Mutant | Unit tests failing | Component tests failing |
|---|---|---|
| E1: scanner does not exclude its own `dirfd()` | 3 | 11 failed |
| E2: raw-NUL guard removed | 1 | 1 failed |
| `readdir` error treated as end of directory | 1 | 0 |
| Partial inventory treated as complete | 1 | 0 |
| Violation outranks inspection error | 6 | 4 failed |
| Report-delivery failure ignored | 0 | 6 failed |
| SIGPIPE not ignored | 0 | 1 failed |
| Duplicate JSON keys accepted | 1 | 1 failed |
| `RLIM_INFINITY` reported as a number | 1 | 2 failed |
| Working directory taken from `$PWD` | 4 | 12 failed |
| A descriptor opened before the snapshot | 0 | 11 failed |
| Probe reads inherited stdin | 0 | 1 failed |
| Probe seeks a borrowed descriptor | 0 | 1 failed |
| Probe sets `O_NONBLOCK` on a borrowed descriptor | 0 | 3 failed |
| Parse-error messages not ASCII-escaped (F3) | 3 | 3 failed |
| `--report` may overwrite the contract (F4) | 0 | 1 failed |
| Report opened without `O_NONBLOCK`: FIFO hang (F5) | 0 | 1 failed (by the 20 s subprocess timeout) |

**17 of 17 mutants detected.** Mutants in `main.cpp` (delivery, SIGPIPE,
ordering) are caught only by component tests, as expected: the unit tests do
not link `main.cpp`. A first run of this check was discarded: the harness
reused one build directory while copying sources with preserved timestamps,
so earlier mutants stayed compiled into later builds and inflated the
counts; the table above is from the corrected run with a fresh build
directory per mutant.

## 4. E1 and E2 — the two key regressions

### E1: the scanner must not count its own descriptor

Listing `/proc/self/fd` requires `opendir`, which allocates the lowest free
descriptor number. If a launcher fails to pass a required fd 3, the directory
stream itself becomes fd 3 and appears in its own listing — a naive scanner
then reports "fd 3 present" and the requirement falsely passes. The scanner
excludes exactly the number returned by `dirfd()` (not a hard-coded 3; with
fd 0 closed the stream becomes fd 0). The report exposes
`fd.inventory.observed.scanner_fd`, so `test_scanner_contamination_fd3`
proves the stream really took fd 3 *and* that fd 3 was still reported
missing.

### E2: an embedded NUL byte must not truncate the contract

nlohmann/json treats a raw NUL byte as the end of input. A contract
`{"version":1,"umask":"0022"}\0{…stricter…}` was therefore read as the weak
first object and passed (exit 0) — a false pass found by the first AI review.
Valid JSON never contains a raw NUL (inside strings it must be `\u0000`), so
`parse_strict()` rejects it up front as `malformed_json` (exit 2).

### Recorded runs

Following `DEVELOPER_NOTES.md` section 7 in a practice clone (remove the
guard, rebuild, test; restore, rebuild, test):

| Run | Commit | E1 guard removed | E1 restored | E2 guard removed | E2 restored |
|---|---|---|---|---|---|
| 2026-11-07, agent | `2d51573` | both scanner tests fail (`count` 4 instead of 3; fd 0 `pass` instead of `fail`/`missing`) | 2 passed | CLI exit **0**; `EmbeddedNul` unit test fails | exit 2; test passes |
| 2026-11-08, agent, fresh clone from GitHub | `3e892a1` | same | 2 passed | same | exit 2; test passes |
| 2026-11-08, agent, mutation suite | `9ccb680` | see section 3 | — | see section 3 | — |

An attempt by an external AI reviewer at `3e892a1` was blocked by its
sandbox (`Wsl/E_ACCESSDENIED`, Docker API denied) and ran no tests. **None of
these runs is the author's own rehearsal.**

## 5. Tests added in the final audit

| Test | What it establishes | Detects (mutants, section 3) |
|---|---|---|
| `test_fds.py::test_inherited_descriptors_are_not_read_moved_or_modified` | The probe does not read, seek or change flags of inherited descriptors. The test keeps copies sharing the open file descriptions of a regular file (offset 4, `O_APPEND`), a pipe with unread data and stdin, and checks them after the probe exits. Previously NFR-6 rested on code review. | reading stdin, `lseek` and `F_SETFL O_NONBLOCK` on a borrowed descriptor |
| `ContractRobustness.RandomMutationsStayOnTheDocumentedErrorPath` | 20 000 fixed-seed byte mutations of valid contracts: exactly one result, only documented error codes, UTF-8-safe messages and pointers, report always renders. In one run 247 mutants were still valid contracts and 11 distinct error codes occurred. | parse-error messages not ASCII-escaped (review finding F3) |

## 6. Defects found and fixed

Every behavioural fix landed with a test that fails on the previous version.

| Found by | Defect | Fix |
|---|---|---|
| Development | ASan's runtime lowers `RLIMIT_CORE` before `main()`, so sanitizer builds failed the core-limit tests (instrumentation contaminating the observation) | `ASAN_OPTIONS=disable_coredump=0` for sanitizer test runs; exact-descriptor tests only on the normal build |
| Development | clang-tidy: evaluation dereferenced `Observed<T>` without checking it held a value | Fails closed with `observation_missing` |
| Development | Demo failed with a relative binary path (found by following the README in a clean clone) | Path resolved before `cd` |
| Development | Processes started by `wsl.exe` inherit fd 7 (`/dev/ptmx`, no `O_CLOEXEC`) | Not a project defect; ExecContract reports it, the corrected demo launcher closes it |
| Review 1 (F1) | **False pass:** raw NUL ended parsing (E2) | NUL rejected |
| Review 1 (F2–F3) | `1e999` escaped as an uncaught exception (exit 3, no report); invalid UTF-8 made the error message unserialisable | `out_of_range` / ASCII-escaped messages, exit 2 with report |
| Review 1 (F4–F5) | `--report` could overwrite the contract; `--report` to a FIFO without reader hung | Refused (same inode); `O_NONBLOCK` open → `ENXIO` |
| Review 1 (F9) | `readdir` error branch untested | Injectable `readdir` seam + tests |
| Review 1 (nits) | `--contract --help` printed help; error pointers not RFC 6901-escaped | Fixed |
| Review 2 | `verify.sh` could pass with zero tests from a stale build cache ("No tests were found", exit 0) | Options forced, registration checked, `--no-tests=error` |
| Review 2 | Test launcher demanded 1 564 descriptors: 90 of 96 component tests failed under nofile 1024:1024 | Staging only when needed, within limits; `test_nofile_mismatch` also fixed (found under 128:128) |
| Review 2 | Pipe test assumed 64 KiB pipe capacity (possible deadlock) | Concurrent reader, 30 s per-test CTest timeout |
| Review 2 | Overstated guarantees (non-blocking output, unchanged report file, atomic snapshot) | Documentation narrowed to what the code establishes |
| Follow-up review | Demo fixture skipped the defective-launcher test at low limits | Prerequisite moved to the corrected-launcher tests |
| Final audit | Borrowed-descriptor non-interference had no test; no input robustness test | Section 5 |
| First CI run | Formatting check failed on the runner's clang-format 18 (version-dependent output), stopping the job after both test suites had passed | Check skipped in CI with a printed reason; still enforced in the reference environment |

Essay length was checked by rendering (Edge headless, A4, 11 pt Times,
2.5 cm margins): 2 pages.

## 7. What this evidence does not cover

| Item | Status |
|---|---|
| The author's own execution of builds, tests, E1/E2 | Not done (by the author's account and these records) |
| Second environment | CI on Ubuntu 24.04 / GCC 13: first run passed both builds' tests (section 1); formatting is not checked there |
| Clang, musl, other distributions, non-WSL kernels | Not run |
| Toolchain minimum versions in `CMakeLists.txt` | Requested by the build files, not tested |
| A real procfs `readdir` failure | Not reproducible unprivileged; covered by the injected-`readdir` unit test |
| Output to a connected reader that stops reading | Not tested; documented as blocking (callers must time out) |
| Coverage-guided fuzzing | Not done; the mutation test is deterministic, not coverage-guided |

## 8. Traceability

| Req. | Unit tests | Component tests / other evidence |
|---|---|---|
| FR-1 CLI | `Cli.*` | `test_input.py::test_usage_errors`, `test_help_version`, `test_option_value_named_help_is_a_path` |
| FR-2 contract file | `ContractFile.*` | `test_input.py::test_missing_contract`, `test_unreadable_contract`, `test_directory_and_fifo_contracts`, `test_oversized_contract_boundary` |
| FR-3 validation | `ContractParse.*` (27), `ContractRobustness.*` | `test_input.py::test_invalid_contracts`, `test_malformed_contract`, `test_duplicate_key`, `test_embedded_nul_does_not_truncate_the_contract`, `test_number_overflow_is_invalid_contract`, `test_invalid_utf8_contract_still_gets_a_report`, `test_utf8_bom_is_accepted`; `test_umask.py::test_numeric_umask_is_rejected`; `test_rlimits.py::test_rlim_infinity_as_number_is_rejected`; `test_cwd.py::test_trailing_slash_in_contract_is_rejected` |
| FR-4 inventory | `FdScan.*` | `test_fds.py::test_scanner_contamination_fd3`, `test_scanner_contamination_fd0`, `test_descriptor_above_lowered_soft_limit`, `test_inventory_at_cap_is_complete`, `test_inventory_beyond_cap_is_inspection_error` |
| FR-5 policy | `EvaluateFds.*` | `test_fds.py::test_required_present_passes`, `test_required_missing_fails`, `test_permitted_absent_passes`, `test_unexpected_descriptor_fails` |
| FR-6 types | `FdType.*`, `EvaluateFds.TypeMatchAndMismatch`, `EvaluateFds.UnavailableTypeIsAnErrorOnlyWhenATypeIsExpected` | `test_fds.py::test_type_matches` (6), `test_type_mismatch_fails` (3), `test_deleted_file_descriptor_still_classified` |
| FR-7 cwd | `CwdTest.*`, `EvaluateCwd.*`, `Utf8.*` | `test_cwd.py` (11) |
| FR-8 umask | `Umask.*`, `EvaluateUmask.*` | `test_umask.py` (6) |
| FR-9 limits | `Rlimit.*`, `EvaluateRlimit.*`, `LimitValueEquality.*`, `ContractParse.NumericBoundaries` | `test_rlimits.py` (8) |
| FR-10 report | `Report.*`, `Evaluate.AreaOrderIsFixed` | `test_reporting.py::test_report_schema_and_check_order`, `test_deterministic_output` |
| FR-11 outcomes | `Aggregate.*`, `Evaluate.ObservationWithNeitherValueNorErrorFailsClosed` | `test_reporting.py::test_mixed_violation_and_inspection_error`, `test_input.py::test_invalid_contract_takes_precedence_over_inspection_failure`; every component test checks the report's `exit_code` against the real exit status |
| FR-12 output failure | `WriteAll.*`, `UniqueFd.CloseCheckedReleasesEvenOnError` | `test_reporting.py::test_stdout_closed_never_succeeds`, `test_stdout_full_never_succeeds`, `test_broken_pipe_exits_3_not_sigpipe`, `test_report_file_failure_never_succeeds` (2), `test_report_to_fifo_without_reader_fails_instead_of_hanging`, `test_report_path_equal_to_contract_is_refused`, `test_report_file_when_stdout_is_dev_null` |
| FR-13 order | `FdScan.LiveProcfsSeesOwnDescriptorAndClosesScanner` | `test_fds.py::test_scanner_contamination_fd3`; `test_reporting.py::test_report_file_when_stdout_is_dev_null`; mutant "descriptor opened before the snapshot" |
| FR-14 demo | — | `test_demo.py` (4); README demo run (section 2) |
| NFR-1 unprivileged | — | All runs as uid 1000 |
| NFR-2 bounded | `ContractFile.SizeCapBoundary`, `ContractParse.DescriptorEntryCap`, `ContractParse.CwdLengthCap`, `ContractParse.DepthLimit`, `FdScan.CapBoundary`, `Umask.ReadFromFileAndMissingFile`, `CwdTest.LongPathGrowsBuffer`, `ReadBounded.CapBoundary` | `test_input.py::test_oversized_contract_boundary`, `test_fds.py::test_inventory_*` |
| NFR-3 deterministic | `Report.Deterministic` | `test_reporting.py::test_deterministic_output` |
| NFR-4 offline | — | Offline build (section 2); `find_package` only |
| NFR-5 reproducible | — | `scripts/verify.sh` from a fresh clone (section 2) |
| NFR-6 read-only | `Umask.LiveObservationMatchesAndDoesNotChangeIt`, `FdScan.LiveProcfsSeesOwnDescriptorAndClosesScanner` | `test_fds.py::test_inherited_descriptors_are_not_read_moved_or_modified`; mutants in section 3 |
| NFR-7 single-threaded | — | Review: no threads in `src/`; no static initialisers that open resources |
| NFR-8 latency | — | Section 2 |
| NFR-9 warnings, sanitizers | — | Section 2 |
