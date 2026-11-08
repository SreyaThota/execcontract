# ExecContract

[![CI](https://github.com/SreyaThota/execcontract/actions/workflows/ci.yml/badge.svg)](https://github.com/SreyaThota/execcontract/actions/workflows/ci.yml)

ExecContract is a small Linux executable that you start **in place of a
service** during launcher integration tests. It inspects its own execution
context, compares it with a declared *execution contract*, and prints a
machine-readable JSON verdict with evidence and a well-defined exit code.

It answers questions such as *"Does my launcher leak file descriptors into the
service?"*, *"Is the service started in `/` with umask `0027`?"* and *"Does it
get a soft `RLIMIT_NOFILE` of 1024?"* — for systemd units, shell wrappers,
container entrypoints or test harnesses.

```
launcher under test ──exec──▶ execcontract --contract service.json ──▶ JSON report + exit code
```

It checks exactly five things: **file descriptors** (closed allowlist,
optional coarse type), **physical working directory**, **umask**,
**`RLIMIT_NOFILE`** and **`RLIMIT_CORE`** (soft and hard).

## What a passing result means

`passed` means: during ExecContract's initial collection phase — which is the
first thing `main()` does, after the dynamic loader and C++ runtime have
initialised, and which reads the descriptor inventory, umask, limits and
working directory one after another — the values observed matched the
contract exactly. The observations are sequential, not one atomic snapshot,
and nothing earlier than `main()` is visible. It says nothing about the real
service's behaviour after startup, and checking `RLIMIT_CORE` does not
establish the host's whole core-dump policy.

## How it works

```
main()
 1. collect_snapshot()       observe.cpp   descriptor inventory FIRST, then umask, limits, cwd
 2. parse_args()             cli.cpp
 3. load + validate contract contract.cpp  bounded, strict JSON schema
 4. evaluate() + aggregate() evaluate.cpp  pure: contract × observations → checks → outcome
 5. render + deliver report  report.cpp, main.cpp  every write checked
```

* **Observation before anything else.** ExecContract inspects its own
  process, so anything it opens would contaminate the result. The descriptor
  inventory is taken before the contract or report is opened. Listing
  `/proc/self/fd` itself needs a descriptor — if the launcher forgot fd 3,
  that listing *becomes* fd 3 — so the scanner excludes exactly the number
  returned by `dirfd()`.
* **Read-only observation.** Inherited descriptors are only enumerated and
  classified with `fstat`; they are never read, seeked, closed or modified.
  The umask is read from `/proc/self/status` (`umask(2)` can only read by
  writing).
* **Pure core.** Validation, evaluation and rendering are plain functions
  over value types; only collection and file I/O touch the OS.
* **Three-valued checks.** `pass`, `fail` (observed and wrong) and `error`
  (could not be observed). Unknown values are never reported as `0`, `[]` or
  `"unlimited"`, and an error always outranks a violation.

Details: [docs/architecture.md](docs/architecture.md); decisions and
alternatives: [docs/engineering-decisions.md](docs/engineering-decisions.md).

## Key technical decisions

| Decision | Why |
|---|---|
| C++17, single-threaded, RAII wrappers for owned descriptors | Nothing needed C++20; ownership must be explicit (owned `UniqueFd` vs borrowed `int`) |
| `/proc/self/fd` + `dirfd()` exclusion, not a loop to `RLIMIT_NOFILE` | Descriptors can exist above a lowered soft limit; the only self-contamination is identified exactly |
| `fstat` for types, not procfs link text | Classifies the open object itself, also for deleted files |
| nlohmann/json + strict application validator | Mature parser; duplicate keys, raw NUL bytes and out-of-range numbers are rejected explicitly because the library alone would accept or misread them |
| Closed descriptor allowlist, no special case for 0–2 | Leaks are the main defect class to catch |
| Blocking output, caller enforces timeouts | Simplicity; documented rather than half-guaranteed |

## Requirements

* Linux with `/proc` mounted. The code relies on the `Umask:` field of
  `/proc/self/status` (added in Linux 4.7) and on `strerrorname_np` (glibc
  2.32).
* To build: a C++17 compiler, CMake, nlohmann/json. To test: GoogleTest,
  Python 3 with pytest.

**Verified in two environments** (details in
[docs/verification.md](docs/verification.md)):

| Environment | Toolchain | Result |
|---|---|---|
| Ubuntu 26.04, WSL2 (reference) | GCC 15.2, CMake 4.2, GoogleTest 1.17, nlohmann/json 3.11.3, Python 3.14, pytest 9.0 | Normal and ASan + UBSan builds pass all tests; formatting, clang-tidy, mutation check, descriptor-limit matrix |
| Ubuntu 24.04, GitHub Actions ([ci.yml](.github/workflows/ci.yml)) | GCC 13.3, CMake 3.28, GoogleTest 1.14, nlohmann/json 3.11.3, Python 3.12, pytest 7.4 | Normal and ASan + UBSan builds pass all tests; also under `RLIMIT_NOFILE` 1024:1024 and 128:128 |

The minimum versions in `CMakeLists.txt` (CMake 3.20, nlohmann/json 3.9,
Python 3.8) are what the build files request, not versions that have been
tested.

## Install dependencies, build, test

Ubuntu/Debian (the universe component must be enabled, as it is by default):

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build python3-pytest \
    libgtest-dev libgmock-dev nlohmann-json3-dev
```

Build and run all tests (no network access is needed after this point):

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The binary is `build/execcontract`. `cmake --install build` installs it to
`/usr/local/bin` (use `--prefix` to change that). The full verification used
for this release (normal + sanitizer builds, formatting) is
`scripts/verify.sh`.

### What the tests cover

* **103 C++ unit tests** (GoogleTest): contract validation (types, bounds,
  duplicates, depth, NUL bytes, UTF-8), evaluation and precedence, report
  rendering, and the collectors against real directories, files, pipes and
  sockets. Includes a fixed-seed 20 000-input mutation test of the contract
  path.
* **97 Python component tests** (pytest) that run the built binary under
  controlled launch conditions: leaked, missing and mistyped descriptors,
  the scanner-contamination case (fd 3 and fd 0), a descriptor above a
  lowered soft limit, the 1024-descriptor cap, cwd through symlinks and
  deleted directories, umask, both limits including `unlimited`, invalid
  contracts, closed/full/broken output channels, inherited descriptors left
  unread and unmoved, and the demo.
* A recorded **mutation check** shows each important guard is load-bearing:
  removing it makes tests fail ([docs/verification.md](docs/verification.md)).

## Usage

```
execcontract --contract PATH [--report PATH]
execcontract --help | --version
```

* The report goes to **stdout**, or to the `--report` file (created or
  truncated) if given — useful when the launcher under test redirects stdout
  to `/dev/null`. `--report` must not name the contract file (exit 2). If the
  report file cannot be *opened*, a file already at that path is left as it
  was; if it is opened (and therefore truncated) but writing fails, it may be
  left empty or partial. In both cases the exit status is 3 — **always check
  the exit status**, not only the file.
* A one-line human summary goes to **stderr**.
* Output uses ordinary blocking writes. If a reader of stdout, stderr or the
  `--report` target is connected but stops reading, ExecContract waits for it
  (backpressure) and does not time out by itself; run it under a timeout in
  automation (the component tests use a 20 s subprocess timeout). Opening a
  FIFO that has no reader at all fails immediately with exit 3.
* `PATH` is opened relative to the working directory ExecContract was started
  in; prefer absolute paths. The contract cannot be read from stdin (fd 0 is
  one of the inspected descriptors).

### Example: catch a defective launcher

```bash
examples/demo/run-demo.sh build/execcontract
```

The demo runs the contract [examples/demo/contract.json](examples/demo/contract.json)
under [launch-broken.sh](examples/demo/launch-broken.sh) (passes on its
caller's leaked fd 3, cwd, umask and limits) and
[launch-fixed.sh](examples/demo/launch-fixed.sh) (closes descriptors 3–9,
`cd /`, `umask 0027`, `ulimit -S -n 1024`, `ulimit -S -c 0`). The first
exits 1 (`violated`), the second 0 (`passed`). The corrected launcher needs
an inherited hard `RLIMIT_NOFILE` ≥ 1024. A captured run with both reports
is in [docs/demo.md](docs/demo.md).

### Using it in a test

Configure the launcher under test to start ExecContract instead of the
service (how depends on the launcher: a unit file's `ExecStart=`, a wrapper's
argument, an entrypoint override), run it, then assert on the exit status and
the report. With a wrapper that execs its arguments, like the demo launchers:

```python
proc = subprocess.run(
    ["./launch-fixed.sh", "/usr/local/bin/execcontract", "--contract", "/abs/path/contract.json"],
    stdin=subprocess.DEVNULL, capture_output=True, timeout=10)
report = json.loads(proc.stdout)
assert proc.returncode == 0, [c for c in report["checks"] if c["status"] != "pass"]
```

[tests/component/test_demo.py](tests/component/test_demo.py) is a working
example.

Always check the exit status as well as the JSON: if the launcher fails to
start the process at all (or limits are so restrictive that the loader
fails), there is no report.

## Contract format (version 1)

```json
{
  "version": 1,
  "fds": {
    "required":  [{"fd": 0, "type": "char_device"}, {"fd": 1}, {"fd": 2}],
    "permitted": [{"fd": 3, "type": "socket"}]
  },
  "cwd": "/srv/app",
  "umask": "0027",
  "rlimit_nofile": {"soft": 1024, "hard": 524288},
  "rlimit_core":   {"soft": 0, "hard": "unlimited"}
}
```

| Key | Meaning |
|---|---|
| `version` | Required, integer `1`. |
| `fds` | **Closed allowlist.** `required` descriptors must be open; `permitted` ones may be open or absent; *any other open descriptor is a violation*. 0, 1 and 2 are not special — list them. Optional `type`: `regular`, `directory`, `pipe` (pipes and FIFOs), `socket`, `char_device`, `block_device`, `other`. The type is checked only when the descriptor is open. |
| `cwd` | Canonical absolute path compared byte-for-byte with the **physical** working directory (`getcwd`, symlinks resolved, `$PWD` ignored). No trailing `/`, no `.`/`..`/empty components; ExecContract never rewrites it. |
| `umask` | Four octal digits as a string, e.g. `"0022"`. |
| `rlimit_nofile`, `rlimit_core` | `soft` and/or `hard`, each a non-negative integer or `"unlimited"`. `RLIMIT_NOFILE` counts descriptor numbers (one more than the highest number that may be opened) — not the number of open descriptors. `RLIMIT_CORE` is in bytes. |

Every area is optional, but at least one must be present; omit a key to leave
it unchecked (`null` is rejected). Unknown keys, duplicate keys, duplicate
descriptors, `soft` > `hard`, wrong types (`true`, `1.0`, `"1024"` for
numbers) and versions other than 1 are rejected. The full specification and
rationale is in [docs/requirements.md](docs/requirements.md).

### Limits

| Input | Limit | Beyond the limit |
|---|---|---|
| Contract file | 65 536 bytes, regular file | `invalid_contract` (exit 2) |
| Descriptor entries in a contract | 1 024 | `invalid_contract` (exit 2) |
| Open descriptors inspected | 1 024 | `inspection_failed` (exit 3) — never a truncated pass |
| Expected `cwd` | 4 095 bytes | `invalid_contract` (exit 2) |

### Encoding

Linux paths are bytes; JSON is Unicode. The contract must be valid UTF-8 (a
leading byte-order mark is accepted; raw NUL bytes are rejected). If the
observed working directory is not valid UTF-8, the `cwd` check is an
inspection error (`cwd_not_utf8`) with a short diagnostic; bytes are never
replaced or guessed.

## Reading the report

Contract `{"version": 1, "fds": {"required": [{"fd": 0}, {"fd": 1}, {"fd": 2}]}, "cwd": "/"}`,
started from `/tmp` with a leaked fd 3:

```json
{
  "report_version": 1,
  "tool": {"name": "execcontract", "version": "0.1.0"},
  "outcome": "violated",
  "exit_code": 1,
  "summary": {"checks": 6, "passed": 4, "failed": 2, "errors": 0},
  "unchecked": ["umask", "rlimit_nofile", "rlimit_core"],
  "checks": [
    {"id": "fd.inventory", "status": "pass", "expected": "complete inventory",
     "observed": {"complete": true, "count": 4, "scanner_fd": 4},
     "reason": null, "diagnostic": "enumerated 4 descriptor(s) from /proc/self/fd", "error": null},
    {"id": "fd.0", "status": "pass", "expected": {"presence": "required", "type": null},
     "observed": {"present": true, "type": "char_device"},
     "reason": null, "diagnostic": "descriptor 0 is open", "error": null},
    "... fd.1 and fd.2 pass in the same way ...",
    {"id": "fd.unexpected.3", "status": "fail", "expected": {"presence": "forbidden", "type": null},
     "observed": {"present": true, "type": "char_device"}, "reason": "unexpected",
     "diagnostic": "descriptor 3 is open but not permitted by the contract", "error": null},
    {"id": "cwd", "status": "fail", "expected": "/", "observed": "/tmp",
     "reason": "mismatch", "diagnostic": "working directory differs", "error": null}
  ],
  "error": null
}
```

(Abridged and reflowed from a real run; actual output is indented JSON.)

* **`status`**: `pass`, `fail` (a *violation*: the value is known and wrong)
  or `error` (an *inspection failure*: the value could not be observed; then
  `observed` is `null` and `error.code` says why, e.g. `cwd_deleted`).
* **Check ids** are stable and ordered: `fd.inventory`, `fd.<n>` (ascending),
  `fd.unexpected.<n>` (ascending), `cwd`, `umask`, `rlimit_nofile.soft`,
  `rlimit_nofile.hard`, `rlimit_core.soft`, `rlimit_core.hard`.
* **`unchecked`** lists areas the contract did not mention.
* For an invalid contract, `checks` is empty and the top-level `error` holds
  `code`, `message`, `errno`, `errno_name` and a JSON `pointer` into the
  contract (e.g. `/rlimit_core/soft`).
* Automate on `id`, `status`, `reason`, `error.code`, `expected`, `observed`.
  `diagnostic` and `message` are for humans and may change or be localised.

## Exit codes

| Exit | Outcome | Meaning |
|---|---|---|
| 0 | `passed` | Every requested check passed and the report was delivered. |
| 1 | `violated` | Valid contract; at least one requirement violated; no inspection failure. |
| 2 | `invalid_contract` / usage | Bad arguments (stderr only, no JSON) or a missing, unreadable, oversized, malformed, unsupported or contradictory contract (JSON report). |
| 3 | `inspection_failed` / `internal_error` / delivery failure | An observation failed (known violations are still listed), an internal error occurred, or the report could not be written (closed stdout, full disk, broken pipe, unwritable `--report` path). |

An inspection failure always outranks violations, and a failed report delivery
always yields 3 — it can never yield 0.

## Known limitations

* The snapshot is taken early in `main()`, not at the instant of `execve`.
  Anything the loader or C++ runtime does first is invisible, and the
  observation is not an atomic snapshot of all properties.
* Sanitizer runtimes change what is observed (ASan lowers `RLIMIT_CORE` unless
  `ASAN_OPTIONS=disable_coredump=0`, and may hold descriptors). Use a normal
  build for acceptance testing.
* A `socket` type says nothing about the peer or protocol; the probe never
  reads, reopens or `readlink`s inherited descriptors.
* Only the five areas above. Environment variables, signal state,
  capabilities, cgroups and namespaces are out of scope — see
  [ROADMAP.md](ROADMAP.md).

## Similar tools

[procenv](https://github.com/jamesodhunt/procenv) (C, GPLv3) dumps a very
broad view of the environment it runs in — far more than ExecContract — and is
the better choice for exploration or diffing two environments. ExecContract
does not invent process-environment inspection; it deliberately does much
less: it checks a small, versioned contract and returns a pass/violation/error
verdict with stable ids and exit codes designed to be asserted on in automated
tests, and it treats its own contamination of the observation (its directory
descriptor, contract and report handles) as a first-class correctness issue.

## Project documentation

| Document | Content |
|---|---|
| [docs/requirements.md](docs/requirements.md) | Scope, schemas, functional and non-functional requirements |
| [docs/architecture.md](docs/architecture.md) | Data flow, modules, observation order, ownership, failure handling |
| [docs/engineering-decisions.md](docs/engineering-decisions.md) | Decisions, alternatives and trade-offs |
| [docs/verification.md](docs/verification.md) | Evidence: environment, commands, results, traceability |
| [docs/demo.md](docs/demo.md) | Captured demo run: defective vs corrected launcher |
| [docs/essay.md](docs/essay.md) | Technical reflection |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Development setup, structure, tests, conventions |
| [DEVELOPER_NOTES.md](DEVELOPER_NOTES.md) | Guided code walkthrough, ownership, APIs, precedence, rehearsal exercises |
| [docs/INTERVIEW_PREPARATION.md](docs/INTERVIEW_PREPARATION.md) | Plain-language explanation, E1/E2, concepts, interview questions, authorship boundaries |
| [ROADMAP.md](ROADMAP.md) | Exclusions, limitations, future work |

## Licence and third-party code

ExecContract is licensed under the [MIT License](LICENSE).

All code in this repository was written for this project. It uses, without
vendoring, [nlohmann/json](https://github.com/nlohmann/json) (MIT) and
[GoogleTest](https://github.com/google/googletest) (BSD-3-Clause), installed
as system packages. Their respective licences continue to apply.
