# Verification record (release 0.1.0)

Execution records dated 2026-11-07. The follow-up submission preparation
section records later agent-run Linux verification of the working-tree edits.
Figures refer to commit `582ffa3` (after both reviews) unless a row says
otherwise; this record itself was committed afterwards. All runs were made by
the AI coding agent in the author's WSL environment; the external reviewer
could not execute Linux builds, so none of these results has been reproduced
independently yet.

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
git clone <repo> ~/clean2 && cd ~/clean2
scripts/verify.sh ~/clean2-verify     # fresh clone, fresh build root
```

Exit status 0 at `582ffa3`. Steps and results:

| Step | Configuration | Result |
|---|---|---|
| Normal build | RelWithDebInfo, `BUILD_TESTS=ON`, `SANITIZE=OFF`, `WERROR=ON` | 0 warnings; registered 102 unit tests + 1 component entry |
| Normal tests | `ctest --no-tests=error` | 100 % passed, 0 failed out of 103 (102 unit + 1 component entry running 96 pytest tests) |
| Sanitizer build | Debug, `BUILD_TESTS=ON`, `SANITIZE=ON` (ASan + UBSan, `-fno-sanitize-recover=all`), `WERROR=ON` | 0 warnings; registered 102 + 1 |
| Sanitizer tests | `ctest --no-tests=error` | 100 % passed, 0 failed out of 103 (102 unit; 81 component, 15 `exact_fds` deselected by design) |
| Formatting | `clang-format --dry-run --Werror` on `src/`, `tests/unit/` | clean |

Component tests run directly on the normal build: **96 passed, 0 skipped**.
Under ASan the 81 compatible tests pass; the 15 `exact_fds` tests were also
run against the ASan binary for information and passed in this environment,
but they are not part of the sanitizer configuration because extra runtime
descriptors are environment-dependent. No ASan or UBSan report occurred.

### The verification script cannot pass without tests

Before `6c4993b`, `verify.sh` reused build directories without forcing test
options. Evidence, with a build root whose caches were configured with
`EXECCONTRACT_BUILD_TESTS=OFF` (and `SANITIZE=ON` in the "normal" directory):

| Script | Exit | What happened |
|---|---|---|
| old (`d69ba67`) | **0** | ctest printed "No tests were found!!!" for both builds; the "normal" build was sanitized |
| new (`6c4993b`+) | 0 | options forced; 102 unit + 1 component registered in both builds; 103/103 each; normal cache `SANITIZE=OFF` |

The new script also exits non-zero if either test kind is not registered, and
unit tests have a 30 s CTest timeout.

## Descriptor-limit matrix

The suite was run from the clean clone under `prlimit --nofile=SOFT:HARD`
(component: direct pytest; unit: the gtest binary):

| Limit | Unit | Component |
|---|---|---|
| 10240 : 1048576 (default here) | 102 passed | 96 passed |
| 4096 : 4096 | 102 passed | 96 passed |
| 1024 : 1024 | 102 passed | 94 passed, 2 skipped (inventory-cap tests need hard ≥ 2048) |
| 256 : 4096 | 102 passed | 96 passed |
| 128 : 128 | 101 passed, 1 skipped (fd 500 cannot be placed) | 87 passed, 9 skipped (cap, high-fd, demo and two limit tests: each states the limit it needs) |

`prlimit --nofile=128:128 -- ctest` (the CONTRIBUTING command): 100 % of 103.

Before `6c4993b` the launcher demanded 1 564 descriptor numbers for every
test: the old suite under 1024:1024 gave **90 failed, 4 passed, 2 skipped**
("launcher setup failed"), and `test_nofile_mismatch` failed under 128:128.
Both were found by running this matrix, the first also by the external
review.

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

Measured at `58da58d` (the later changes touch tests and fixtures, not the
mutated code paths). All 10 mutants are detected. Before the review fixes, the `readdir`-error
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
`examples/minimal.json` (located next to the script), timed around
`subprocess.run` (includes fork/exec, loading and teardown), normal build.
Every measurement taken is listed:

| Run | Commit | Contract on | Median | p95 | Max |
|---|---|---|---|---|---|
| 1 | `a81a40a` | drvfs (`/mnt/d`) | 4.14 ms | 5.91 ms | 9.37 ms |
| 2 | `a81a40a` | drvfs | 3.97 ms | 6.90 ms | 10.28 ms |
| 3 | `58da58d` (directly after two builds) | drvfs | 7.11 ms | 26.56 ms | 33.31 ms |
| 4 | `58da58d` | drvfs | 6.34 ms | 29.07 ms | 35.18 ms |
| 5 | `58da58d` | drvfs | 12.70 ms | 30.28 ms | 35.18 ms |
| 6 | `58da58d` | drvfs | 8.10 ms | 28.47 ms | 39.72 ms |
| 7 | `582ffa3` | ext4 (clean clone) | 0.90 ms | 1.64 ms | 2.33 ms |
| 8 | `582ffa3` | ext4 | 0.93 ms | 1.34 ms | 2.15 ms |
| 9 | `582ffa3` | ext4 | 0.93 ms | 1.42 ms | 3.25 ms |

Target ≤ 20 ms median: **met in every run** on the reference environment.
Correction to the earlier interpretation: runs 1–6 opened the contract from
the Windows drive through WSL's drvfs mount, so they mostly measured that
file system (and its variance), not the probe. With the contract on ext4 the
median is about 0.9 ms. No p95 target was set. Not measured elsewhere.

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
`6b5ee65`, with a regression test that fails on the old script.

Run at `d69ba67`: configure OK, build 0 warnings, `ctest` 100 % of 103; demo
exit 0 (defective launcher 1, corrected 0); `ctest -L unit` 102/102,
`ctest -L component` 1/1, direct pytest 96 passed; install to a `DESTDIR`
staging directory produced `usr/local/bin/execcontract`; `--version` prints
`execcontract 0.1.0`. Scripts kept their executable bit and LF line endings.

Final run at `582ffa3` (fresh clone `~/clean2`): `verify.sh` exit 0 (table
above); README commands — build 0 warnings, `ctest` 100 % of 103, demo exit
0; `prlimit --nofile=128:128 -- ctest` 100 % of 103; descriptor-limit matrix
above. All files in the index have LF line endings (`git ls-files --eol`).

## Skipped or unverified

| Item | Status |
|---|---|
| Conditional component skips | None at the default limits; at reduced limits exactly the skips listed in the matrix above, each with its reason. Non-UTF-8-name, finite-hard-`RLIMIT_CORE` and root skips never triggered |
| `readdir` failure on real procfs | Not reproducible unprivileged; covered by the injected-`readdir` unit test |
| Python other than 3.14, CMake other than 4.2, nlohmann/json other than 3.11.3, GCC other than 15.2 | Not run; the README no longer presents the build files' minimum versions as supported |
| Output to a connected reader that stops draining | Not tested; documented as blocking (caller must time out) |
| Independent reproduction of these results | Not done: the external reviewer could not run WSL or Docker |
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

## Second review (external, static)

A second reviewer, outside the development session, read the source, tests,
build files, documentation and history but could not execute Linux builds
(WSL access denied, Docker engine unavailable); it syntax-checked the Python
files and reproduced the launcher limit problem with mocked limits. It found
no new false pass in the inspection core. Its items and their resolution:

| # | Finding | Resolution | Evidence |
|---|---|---|---|
| 1 | `verify.sh` could pass without tests (stale cache), normal build not forced unsanitized | Options forced, registration checked, `--no-tests=error` (`6c4993b`) | Poisoned-cache comparison above |
| 2 | 60 000-byte pipe test assumed pipe capacity; no unit-test timeouts | Concurrent reader, 1 MiB write, cleanup cannot deadlock; 30 s CTest timeout (`6c4993b`) | Unit suite passes in both builds |
| 3 | Launcher required 1 564 descriptors even without fixtures; unit test checked the hard instead of the soft limit | Staging only when needed, above targets, within limits; soft limit raised and restored (`6c4993b`). Matrix run also found `test_nofile_mismatch` exceeding a 128 hard limit — fixed | Old suite: 90 failures at 1024:1024; new: none at any limit |
| 4 | README promised an unchanged report file on write failure, but the file is opened `O_TRUNC` | Claim narrowed to open failure; partial/empty file documented (`c4c425a`) | — |
| 5 | FR-12 "never blocks" unsupported | Blocking/backpressure documented; caller timeouts required (`c4c425a`) | — |
| 6 | Three exact-inventory tests unmarked | Marked `exact_fds` after an audit of all tests (`6c4993b`) | ASan split 81 + 15 |
| 7 | "State when `main()` began" and "never both, never neither" overstated | Reworded to sequential initial collection; `Observed<T>` comment corrected (`c4c425a`) | — |
| 8 | Reproduce from a clean environment, exercise low limits | Clean-clone run and limit matrix above | Same environment as before — a second Linux environment was not available |
| 9 | Essay presented the agent's actions as personal experience; page count unverified | Essay rewritten with accurate authorship; author sections marked to write (`582ffa3`) | Rendered via Edge headless, A4, 11 pt Times, 2.5 cm margins: 2 pages; still 2 pages with a 290-word author section |
| 10 | No developer notes; no personal debugging rehearsal | `DEVELOPER_NOTES.md` with exercises, including one small open issue for the author (`582ffa3`) | The rehearsal itself is the author's to do |

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

## Follow-up submission preparation

Prepared after `2d51573` by the external AI reviewer on 2026-11-07. The
Linux execution tables above remain evidence for their explicitly named
revisions. Later Linux runs against the follow-up working tree are recorded
in the results table below.

- The demo prerequisite is now scoped to `corrected_demo`. The defective
  launcher test no longer skips merely because the hard descriptor limit is
  below 1024. Source-level checks with mocked limits verify the fixture
  branching; subsequent demo and full-suite Linux runs are recorded below.
- `DEVELOPER_NOTES.md` now gives complete practice-clone commands, rebuilds
  before and after mutations, controls the umask for the NUL demonstration,
  and makes CTest fail if the targeted test is absent. The candidate has not
  reported completing this rehearsal.
- The essay placeholders were replaced with an assisted draft grounded in
  the candidate's project direction and review requests. It makes no claim
  of personal coding or debugging. The candidate must review its first-person
  statements before using it as their reflection.
- The concurrent pipe-test comment now describes complete delivery without
  claiming deterministic short-write coverage. Executable behavior is unchanged.

The external review could not execute WSL (`Wsl/E_ACCESSDENIED`). Windows
Python syntax and fixture-branch checks do not establish Linux correctness.

### Linux results for the follow-up changes

Run afterwards by the in-session AI coding agent on the same WSL environment,
against the working tree containing the follow-up edits (base `2d51573`):

| Check | Result |
|---|---|
| `scripts/verify.sh` (fresh build root) | exit 0; both builds register 102 unit + 1 component entry; 103/103 each; clang-format clean |
| `test_demo.py` at normal limits | 4 passed |
| `test_demo.py` under `prlimit --nofile=128:128` | 1 passed (defective launcher), 3 skipped (corrected launcher) |
| Component suite at 10240:1048576 / 1024:1024 / 128:128 | 96 passed / 94 passed, 2 skipped / **88 passed, 8 skipped** (previously 87 / 9: the defective-launcher test now runs) |
| `DEVELOPER_NOTES.md` E1 commands, verbatim, in a practice clone of `2d51573` | Baseline 103/103. Without the scanner exclusion both contamination tests fail (`count` 4 instead of 3 for fd 3; fd 0 reported `pass` instead of `fail`/`missing`); after `git restore`, 2 passed |
| E2 commands, verbatim | Baseline exit 2 and `EmbeddedNul` test passes; without the NUL guard exit **0** and the test fails; after restore exit 2 and passing |

These runs check that the rehearsal instructions work as written. They are
not the author's rehearsal, which remains to be done.

### Profile and licence documentation update

The subsequent documentation update uses the candidate's supplied profile
to connect the reflection to their Python, ECU diagnostic-test, pytest and
CI/CD background. It retains the distinction between project direction and
agent-performed implementation, testing and debugging. Personal completion
of E1 and E2 has not been reported.

An MIT licence was added at the candidate's request to choose the licence
on their behalf, with the supplied name Sreya Thota in the notice. README,
progress notes and stale statements about pending Linux execution were
updated. This update changes documentation and licensing only; the recorded
Linux runs above preceded it. It does not claim another execution of the
test suites or a candidate-performed rehearsal.
