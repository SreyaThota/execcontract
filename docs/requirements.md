# ExecContract — Scope and Requirements (release 0.1.0)

## 1. Problem

A service can behave differently when it is started by hand, by a shell script,
by systemd, or by a test runner, because each launcher hands it a different
execution context: leaked descriptors, a different working directory, a
different umask, different resource limits. Launcher bugs of this kind are hard
to see from inside the service and are usually discovered in production.

ExecContract is a small native executable that a launcher integration test
starts **in place of the real service**. It inspects its own execution context,
compares it with a declared *execution contract*, and emits a machine-readable
verdict with evidence and a well-defined exit code.

## 2. Scope

### In scope (exactly five inspection areas)

| Area | Capability |
|---|---|
| File descriptors | Closed allowlist of required and permitted descriptors, optional coarse type per descriptor. |
| Working directory | Physical working directory, exact byte comparison with a canonical absolute path. |
| Umask | Observed from `/proc/self/status`, compared exactly. |
| `RLIMIT_NOFILE` | Soft and hard limits, independently comparable, `"unlimited"` token. |
| `RLIMIT_CORE` | Soft and hard limits, independently comparable, `"unlimited"` token. |

### Out of scope

Process supervision, process-tree cleanup, inspecting other processes, security
auditing, namespaces, cgroups, environment variables, signal state, a general
policy language, network services, GUIs, dashboards, AI/ML features. Proposed
extensions are recorded in [ROADMAP.md](../ROADMAP.md).

### What a passing result proves

A `passed` report proves that **the values observed during ExecContract's
initial collection phase** matched the contract: the contracted descriptors
with the contracted coarse types, physical working directory, umask and
limits. Collection is the first thing `main()` does (after the dynamic loader
and C++ runtime have initialised) and reads the five areas sequentially —
descriptor inventory, umask, limits, working directory — so the result is a
sequence of early observations, not an atomic snapshot. It does not prove the
state at the instant of `execve`, it does not prove anything about the real
service's behaviour after startup, and it does not prove the host's complete
core-dump policy (`core_pattern`, `PR_SET_DUMPABLE`, systemd-coredump).

## 3. Design decisions that resolve ambiguities

| Question | Decision |
|---|---|
| Do required descriptors count as permitted? | Yes. Listing the same descriptor in `required` and `permitted` is rejected as a duplicate. |
| Do 0, 1, 2 need explicit permission? | Yes. No descriptor is special. A contract that cares about descriptors lists 0–2 like any other. |
| May permitted descriptors be absent? | Yes. Absent permitted descriptor → check passes. |
| When does a type expectation apply? | Only when the descriptor is present. A missing required descriptor is a `missing` violation; its type is not evaluated. |
| Unsupported / unrecognised types | `fstat` modes other than regular, directory, FIFO, socket, char device, block device are reported as `"other"`. If `fstat` itself fails, the type is unavailable and a type expectation becomes an inspection error. |
| Is the descriptor allowlist open or closed? | Closed. When `fds` is present, every open descriptor outside `required ∪ permitted` is an `unexpected` violation. |
| Mandatory fields? | Only `version`. Each area is optional; an omitted area is not checked and is listed in the report's `unchecked` array. `null` is never accepted as "unchecked" — omit the key instead. |
| Empty contract (`{"version":1}`) | Rejected (`empty_contract`). A contract that checks nothing would pass vacuously. |
| Empty area objects (`"fds": {}`, `"rlimit_core": {}`) | Rejected (`empty_area`). Use `{"required": [], "permitted": []}` to assert that *no* descriptor is open. |
| Duplicate JSON keys | Rejected at any depth (`duplicate_key`). |
| Duplicate descriptor entries | Rejected (`duplicate_fd`), within or across `required`/`permitted`. |
| Contradictory limits | Finite soft > finite hard, or `"unlimited"` soft with finite hard, are rejected (`contradictory_limits`): no process can be in that state. |
| Non-canonical expected cwd | Rejected (`noncanonical_path`): must start with `/`, no trailing `/` (except `/` itself), no empty, `.` or `..` components. ExecContract never rewrites or resolves the expected path. |
| Contract from stdin | Not supported. fd 0 is an inspected, borrowed descriptor; reading it would consume the launcher's data. |

## 4. CLI

```
execcontract --contract PATH [--report PATH]
execcontract --help
execcontract --version
```

* The contract path is opened relative to the observed working directory
  (ExecContract never calls `chdir`). Absolute paths are recommended.
* The report is written to stdout unless `--report PATH` is given, in which
  case the file is created/truncated (`O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NONBLOCK`,
  mode `0666 & ~umask`; `O_NONBLOCK` is cleared after opening) **after** the
  snapshot and nothing is written to stdout. This is useful when the launcher
  under test redirects stdout to `/dev/null`. A FIFO without a reader fails
  with `ENXIO` (exit 3) instead of blocking. If the report file cannot be
  opened, an existing file at that path is left unchanged and may be stale; if
  it was opened (`O_TRUNC`) but a later write or `close` fails, it may be
  empty or partial. Either way the exit status is 3: callers must always check
  the exit status, not only the file.
* All output (stdout, stderr, report file) uses blocking writes. A connected
  reader that stops draining makes ExecContract wait indefinitely
  (backpressure); bounding the run time is the caller's responsibility, e.g.
  with a subprocess timeout. This is a deliberate simplicity trade-off, not a
  bounded-delivery guarantee.
* `--report` naming the contract file (same device and inode, symlinks
  followed) is a usage error (exit 2, stderr only): truncating it would
  destroy the contract.
* `--help` and `--version` write to stdout and exit 0; they are the only
  non-JSON stdout outputs. They are recognised only in option position: in
  `--contract --help` the word `--help` is the contract path.
* Usage errors (unknown option, missing/duplicated `--contract`, missing
  option value) write a message to stderr, nothing to stdout, and exit 2.
* stderr carries a one-line human summary after the report has been delivered.

## 5. Contract schema (version 1)

```json
{
  "version": 1,
  "fds": {
    "required":  [ {"fd": 0, "type": "char_device"}, {"fd": 1}, {"fd": 2} ],
    "permitted": [ {"fd": 3, "type": "socket"} ]
  },
  "cwd": "/srv/app",
  "umask": "0027",
  "rlimit_nofile": {"soft": 1024, "hard": 524288},
  "rlimit_core":   {"soft": 0, "hard": "unlimited"}
}
```

| Field | Type and validation |
|---|---|
| `version` | Required. JSON integer `1`. `1.0`, `"1"`, `true` are type errors; other integers are `unsupported_version`. |
| `fds` | Object with optional `required` and `permitted` arrays (at least one key present). Each entry is an object `{"fd": <int 0..2147483647>, "type": <type>?}`. At most 1024 entries in total. |
| `type` | One of `regular`, `directory`, `pipe`, `socket`, `char_device`, `block_device`, `other`. |
| `cwd` | String, valid UTF-8 (guaranteed by the JSON parser), no NUL, canonical absolute path, at most 4095 bytes. |
| `umask` | String matching `^0[0-7]{3}$`, e.g. `"0022"` (the same form as `/proc/self/status`). A JSON number is rejected to avoid decimal/octal confusion. |
| `rlimit_nofile`, `rlimit_core` | Object with optional `soft` and `hard` (at least one). Each value is a non-negative JSON integer strictly below `RLIM_INFINITY` (2^64−1), or the string `"unlimited"`. Units: `RLIMIT_NOFILE` is "one greater than the highest descriptor number that may be opened"; `RLIMIT_CORE` is bytes. |

General rules: the top level must be an object; unknown keys are rejected at
every level (`unknown_field`); nesting deeper than 8 levels is rejected
(`too_deep`); integers that do not fit in 64 bits are parsed as floating point
by the JSON library and therefore rejected as a type error, never coerced;
numbers that do not even fit in a double (`1e999`) are `out_of_range`. A raw
NUL byte anywhere in the file is `malformed_json` (the JSON library would
otherwise treat it as end of input and ignore the rest). Invalid UTF-8 is
`malformed_json`; error messages quote input bytes only as printable ASCII
(`\xNN`). A leading UTF-8 byte-order mark is accepted. Error pointers follow
RFC 6901 (`~` → `~0`, `/` → `~1`).

### Size limits and behaviour at the caps

| Limit | Value | At the cap | Beyond the cap |
|---|---|---|---|
| Contract file size | 65 536 bytes | Accepted | `contract_too_large`, exit 2 |
| Descriptor entries in a contract | 1 024 | Accepted | `too_many_fd_entries`, exit 2 |
| Observed descriptor inventory | 1 024 descriptors | Complete inventory | `fd.inventory` is an inspection error (`inventory_capped`), exit 3; already-observed descriptors still yield known evidence |
| `/proc/self/status` read | 65 536 bytes | Parsed | `umask_unavailable` inspection error |
| Working-directory path | 65 536 bytes buffer growth | Observed | `cwd_too_long` inspection error |
| Expected `cwd` length | 4 095 bytes | Accepted | `invalid_path`, exit 2 |

## 6. Report schema (report_version 1)

```json
{
  "report_version": 1,
  "tool": {"name": "execcontract", "version": "0.1.0"},
  "outcome": "violated",
  "exit_code": 1,
  "summary": {"checks": 6, "passed": 4, "failed": 2, "errors": 0},
  "unchecked": ["rlimit_core"],
  "checks": [
    {"id": "fd.inventory", "status": "pass", "expected": "complete inventory",
     "observed": {"complete": true, "count": 3, "scanner_fd": 3}, "reason": null,
     "diagnostic": "enumerated 3 descriptor(s) from /proc/self/fd", "error": null},
    {"id": "fd.3", "status": "fail", "expected": {"presence": "required", "type": null},
     "observed": {"present": false, "type": null}, "reason": "missing",
     "diagnostic": "required descriptor 3 is not open", "error": null}
  ],
  "error": null
}
```

* `outcome` ∈ `passed`, `violated`, `inspection_failed`, `invalid_contract`,
  `internal_error`.
* Each check always has the keys, in this order, `id`, `status`
  (`pass`/`fail`/`error`), `expected`, `observed` (`null` when unavailable —
  never `0`, `[]` or `"unlimited"`), `reason` (stable violation id for `fail`:
  `missing`, `type_mismatch`, `unexpected`, `mismatch`; otherwise `null`),
  `diagnostic` (human text, not stable), `error` (`null` or
  `{"code": <stable id>, "errno": <int|null>, "errno_name": <"ENOENT"|null>, "message": <text>}`).
* `fd.inventory.observed.scanner_fd` is the descriptor number the enumeration
  stream itself occupied (and was excluded). It is reported so tests can prove
  the contamination path was actually exercised.
* Stable check-error codes: `fd_enumeration_failed`, `fd_entry_unparseable`,
  `inventory_capped`, `fd_inventory_incomplete`, `fd_type_unavailable`,
  `cwd_deleted`, `cwd_unreachable`, `cwd_unavailable`, `cwd_too_long`,
  `cwd_not_utf8`, `umask_unavailable`, `umask_malformed`, `rlimit_unavailable`,
  `observation_missing` (internal invariant violated; fails closed).
* Stable contract-error codes: `contract_not_found`, `contract_unreadable`,
  `contract_open_failed`, `contract_not_regular_file`, `contract_too_large`,
  `contract_read_failed`, `malformed_json`, `duplicate_key`, `too_deep`,
  `invalid_type`, `missing_version`, `unsupported_version`, `unknown_field`,
  `empty_contract`, `empty_area`, `missing_field`, `out_of_range`,
  `invalid_fd_type`, `duplicate_fd`, `too_many_fd_entries`, `invalid_path`,
  `noncanonical_path`, `invalid_umask`, `contradictory_limits`;
  internal: `serialization_failed`.
* Check ids and order are stable: `fd.inventory`; `fd.<n>` for each contracted
  descriptor in ascending order; `fd.unexpected.<n>` in ascending order; `cwd`;
  `umask`; `rlimit_nofile.soft`; `rlimit_nofile.hard`; `rlimit_core.soft`;
  `rlimit_core.hard` (each only if contracted).
* For `invalid_contract`/`internal_error`, `checks` is empty and the top-level
  `error` holds `{"code", "message", "errno", "errno_name", "pointer"}`, where
  `pointer` is a JSON-pointer-like location in the contract (or `null`).
* Tests must assert on `id`, `status`, `error.code`, `expected`, `observed` —
  never on `diagnostic` or `message`, which may contain locale-dependent text.

## 7. Outcomes and exit codes

| Exit | Outcome | Meaning |
|---|---|---|
| 0 | `passed` | Every requested check conclusively passed and the report was delivered. |
| 1 | `violated` | Valid contract, ≥1 check failed, no check errored. |
| 2 | `invalid_contract` / usage | CLI error or missing, unreadable, non-regular, oversized, malformed, unsupported or contradictory contract. |
| 3 | `inspection_failed` / `internal_error` / delivery failure | ≥1 observation could not be made, an internal error occurred, serialisation failed, or the report could not be completely written. |

Precedence: usage error → invalid contract → inspection failure → violation →
pass. Delivery failure overrides everything to exit 3 (a report that cannot be
delivered cannot carry its own error). Known violations remain listed in an
`inspection_failed` report.

## 8. Functional requirements

| ID | Requirement | Verification |
|---|---|---|
| FR-1 | CLI as in §4; usage errors exit 2 with empty stdout; `--help`/`--version` exit 0. | Unit (`Cli.*`), component (`test_input.py::test_usage_*`, `test_help_version`) |
| FR-2 | Contract read from a regular file ≤ 64 KiB, opened without blocking; missing, unreadable, non-regular or oversized input → `invalid_contract`. | Unit (`ContractFile.*`), component (`test_input.py`) |
| FR-3 | Contract validation per §5 including duplicate keys, unknown fields, wrong types, booleans as numbers, negative/overflowing integers and doubles, embedded NUL bytes, invalid UTF-8, unsupported version, empty contract/areas, duplicates, contradictions, depth. | Unit (`ContractParse.*`), component (`test_input.py`) |
| FR-4 | Descriptor inventory from `/proc/self/fd`, excluding exactly the enumeration stream's own descriptor (`dirfd`), distinguishing complete, failed and capped enumeration; not bounded by `RLIMIT_NOFILE`. | Unit (`FdScan.*`), component (`test_fds.py`) |
| FR-5 | Descriptor policy per §3: required, permitted, unexpected, closed allowlist. | Unit (`EvaluateFds.*`), component (`test_fds.py`) |
| FR-6 | Coarse type from `fstat` on the descriptor; never reopening targets or reading symlink text. | Unit (`FdType.*`), component (`test_fds.py::test_type_*`) |
| FR-7 | Physical cwd via `getcwd` (not `$PWD`), exact byte comparison, canonical expected path; deleted/unreachable/too long/non-UTF-8 → inspection error with bounded diagnostic. | Unit (`CwdTest.*`, `EvaluateCwd.*`), component (`test_cwd.py`) |
| FR-8 | Umask from the `Umask:` field of `/proc/self/status`, never by calling `umask()` or creating files; missing/malformed → inspection error. | Unit (`Umask.*`), component (`test_umask.py`), review |
| FR-9 | Soft/hard limits via `getrlimit` for `RLIMIT_NOFILE` and `RLIMIT_CORE`, `"unlimited"` for `RLIM_INFINITY`, independent soft/hard checks. | Unit (`Rlimit.*`, `EvaluateRlimit.*`), component (`test_rlimits.py`) |
| FR-10 | Report per §6 with stable ids, deterministic order, explicit `null` for unavailable observations. | Unit (`Report.*`, `Evaluate.AreaOrderIsFixed`), component (`test_reporting.py`) |
| FR-11 | Outcomes, exit codes and precedence per §7. | Unit (`Aggregate.*`), component (all) |
| FR-12 | Output failure (closed stdout, `ENOSPC`, broken pipe, unopenable `--report`, FIFO without reader) never exits 0; opening a FIFO without a reader does not block; `SIGPIPE` does not kill the process silently; `--report` cannot overwrite the contract. Writes to a connected but non-draining reader block (documented in §4; not a failure mode the tool bounds). | Unit (`WriteAll.*`), component (`test_reporting.py::test_stdout_*`, `test_broken_pipe_*`, `test_report_file_failure_*`, `test_report_to_fifo_*`, `test_report_path_equal_to_contract_is_refused`) |
| FR-13 | Observation order: the descriptor inventory is taken before ExecContract opens any descriptor of its own; contract and report are opened afterwards. | Code review (`src/main.cpp`), component (`test_fds.py::test_scanner_contamination_fd3`) |
| FR-14 | A runnable demonstration: one contract fails under a defective launcher and passes under the corrected launcher. | Component (`test_demo.py`), manual (`examples/demo/run-demo.sh`) |

## 9. Non-functional requirements

| ID | Requirement | Verification |
|---|---|---|
| NFR-1 | Runs as an ordinary unprivileged user; needs only a mounted `/proc`. | All component tests run unprivileged |
| NFR-2 | All inputs and collections are bounded (§5 table); every exceeded cap is an explicit error, never a truncated pass. | Unit + component cap tests |
| NFR-3 | Deterministic: identical state and contract give a byte-identical report. | Unit (`Report.Deterministic`), component (`test_reporting.py::test_deterministic`) |
| NFR-4 | Build and tests work offline once the documented distro packages are installed; no download-at-configure. | `find_package` only; offline verification run recorded in `docs/verification.md` |
| NFR-5 | Reproducible: one documented script (`scripts/verify.sh`) configures, builds and runs all tests from a clean build directory. | Recorded run in `docs/verification.md` |
| NFR-6 | Observation is read-only: ExecContract never closes, reads, seeks or changes flags of inherited descriptors, never calls `umask()`, `setrlimit()` or `chdir()`; only the designated report channel is written. | Code review, unit tests on collectors, component tests |
| NFR-7 | Single-threaded; no static initialisers that open resources. | Code review |
| NFR-8 | Latency: median wall-clock time of one run ≤ 20 ms on the reference environment (§10), measured by `scripts/measure_latency.py` (200 sequential runs, `time.perf_counter_ns` around `subprocess.run`, includes process creation). | Measurement recorded in `docs/verification.md` |
| NFR-9 | Project code compiles without warnings under `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow`; unit tests pass under ASan+UBSan. | Build logs in `docs/verification.md` |

## 10. Supported environment

* Linux ≥ 4.7 (for the `Umask:` field of `/proc/self/status`), glibc ≥ 2.32
  (`strerrorname_np`), `/proc` mounted.
* Reference/verified environment: see `docs/verification.md` (WSL2 Ubuntu 26.04,
  GCC 15.2, CMake 4.2, Python 3.14). Other distributions and compilers are
  expected to work but are unverified.
* Very restrictive inherited limits (e.g. `RLIMIT_NOFILE` soft limit 0–2, or
  limits that stop the dynamic loader) can prevent ExecContract from starting
  or from reaching its reporting code. The caller must treat a launch failure
  or a missing/unparseable report as a failure; a JSON report cannot be
  guaranteed in that case.

## 11. Traceability

The traceability matrix mapping each requirement to concrete tests is in
[docs/verification.md](verification.md#traceability).
