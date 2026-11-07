# Developer notes — understanding ExecContract

A guided tour for whoever has to explain, debug or change this code (the
author first). It complements [docs/architecture.md](docs/architecture.md)
(what the design is) with *how to read it* and *how to practise on it*.
Function names are used instead of line numbers so the references stay valid.

Reading this file does not establish that you can work on the code; the
exercises at the end do.

## 1. One invocation, from `main()` to exit

Open `src/main.cpp` and follow `main()`:

1. **`collect_snapshot()`** (`src/observe.cpp`) — the first statement. It
   calls, in order:
   - `scan_fd_dir("/proc/self/fd", 1024)` — `opendir`, then `dirfd()` to learn
     which number the stream itself took, then a `readdir` loop that skips
     `.`/`..`, parses names, **skips the stream's own number**, and stops with
     an error on an unparseable name, a `readdir` error or more than 1024
     entries. The `DirStream` destructor closes the stream on return.
   - `classify_fds()` — `fstat` on every recorded number → `FdType`.
   - `read_umask("/proc/self/status")` — opens, reads ≤ 64 KiB, closes,
     `parse_umask_status()`.
   - `observe_rlimit()` twice, `observe_cwd()` (growing buffer).
   *Why first?* Everything ExecContract opens is a descriptor that the
   inventory would see. Nothing of its own may exist yet.
2. **`parse_args()`** (`src/cli.cpp`) — pure. Usage errors → stderr, exit 2.
3. **`ignore_sigpipe()`** — from here on a broken pipe is an `EPIPE` return
   value, not a fatal signal.
4. **`run_check()`**:
   - refuses `--report` pointing at the contract file (`same_file()`);
   - `load_contract_file()` (`src/contract.cpp`) — `open(O_NONBLOCK)`,
     `fstat` must be a regular file, `read_bounded()` ≤ 64 KiB, close;
   - `parse_contract()` — NUL check, `parse_strict()` (nlohmann with a
     callback for duplicate keys and depth), then `validate()` builds a
     `Contract` or throws the internal `Invalid`, converted to `ContractError`;
   - `evaluate()` (`src/evaluate.cpp`) — pure: contract × snapshot → ordered
     `Check`s; `aggregate()` → `Outcome`;
   - `render_report()` (`src/report.cpp`) — strict UTF-8 JSON;
   - `deliver()` — stdout, or `--report` file (`O_NONBLOCK` open, flag cleared,
     `write_all`, `close_checked`);
   - stderr summary; return `exit_code_for(outcome)`.

Exercise the path in a debugger once (`gdb` is not in the dependency list;
install it with `sudo apt install gdb`):

```bash
cmake -S . -B build-dbg -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build-dbg
gdb --args build-dbg/execcontract --contract examples/minimal.json
(gdb) break execcontract::scan_fd_dir
(gdb) run
(gdb) finish          # inspect the returned inventory; local inv is now out of scope
```

## 2. Ownership: owned vs borrowed descriptors

| Kind | Type in code | May ExecContract close it? | Examples |
|---|---|---|---|
| Owned | `UniqueFd`, `DirStream` | Yes, exactly once (destructor or `close_checked()`) | directory stream, status file, contract file, report file |
| Borrowed | plain `int` | **Never** | everything inherited: 0, 1, 2 and any leaked descriptor |

Questions to be able to answer:
- Why is `UniqueFd` move-only? (Copying would close the same number twice;
  the second close could hit a descriptor reused in between.)
- Why does `close_checked()` not retry on `EINTR`? (On Linux the descriptor is
  already released; a retry can close an unrelated, newly opened descriptor.)
- Why is the report file closed with `close_checked()` but the status file
  with the destructor? (A failing `close` can report a lost write for the
  report; for a read-only file there is nothing to lose.)

## 3. The APIs this code depends on

| API | What to know | Where |
|---|---|---|
| `opendir`/`readdir`/`dirfd` | `readdir` returns NULL for end *and* error; only `errno` set to 0 beforehand distinguishes them | `scan_fd_dir` |
| `fstat` | classifies the open object, works for deleted files and pipes | `classify_fds` |
| `/proc/self/status` `Umask:` | read-only umask (Linux ≥ 4.7); `umask(2)` can only read by writing | `read_umask` |
| `getrlimit` | `RLIM_INFINITY` ≠ any finite value; soft limit restricts *new* descriptors only | `observe_rlimit` |
| `getcwd` | kernel's physical path; `ENOENT` if the directory was removed; `$PWD` is irrelevant | `observe_cwd` |
| `write` | handle short writes and `EINTR`; this program ignores SIGPIPE so it can handle broken-pipe errors | `write_all` |
| nlohmann `parse` with callback | keeps the last duplicate key; stops at a NUL byte; throws `out_of_range` for `1e999` | `parse_strict` |

## 4. Error precedence

```
usage error (2) > invalid contract (2) > inspection error (3) > violation (1) > pass (0)
report delivery failure → 3, overriding everything
```

- `Status::fail` = observed and wrong (`reason` says how).
- `Status::error` = could not be observed (`observed` is `null`, `error.code`
  says why). One error makes the outcome `inspection_failed` but known
  violations stay in the report.
- `"unlimited"` is a *value* (`RLIM_INFINITY`), never a placeholder for
  "unknown".

Code: `aggregate()` and `exit_code_for()` in `src/evaluate.cpp`.

## 5. How the tests are built

- **Unit tests** (`tests/unit/`, one file per module) call production code
  directly. Seams are ordinary parameters: the directory for `scan_fd_dir`,
  the path for `read_umask`, a `readdir` function pointer for the one failure
  procfs cannot produce.
- **Component tests** (`tests/component/`) never change pytest's own process.
  `run_probe()` in `conftest.py` starts `launcher.py` as a fresh `python -I`
  process with a JSON spec; the launcher creates fixtures, moves them to
  staging numbers above all targets (`F_DUPFD_CLOEXEC`), installs them with
  `dup2`, sets cwd/umask/limits, restores SIGPIPE and `execv`s the probe.
- `exact_fds` marks tests that assert the complete descriptor inventory;
  they are deselected under sanitizers, whose runtimes may hold descriptors.
- Every component test that gets a report checks that its `exit_code` field
  equals the real exit status.
- `run_probe(pass_fds=...)` hands descriptors of the test process down
  unchanged; because the test keeps copies sharing the same open file
  descriptions, `test_inherited_descriptors_are_not_read_moved_or_modified`
  can check afterwards that the probe did not read, seek or `fcntl` them.
- `tests/unit/test_robustness.cpp` mutates valid contracts 20 000 times with a
  fixed seed (reproducible) and checks invariants that must hold for any
  input: one result, a documented error code, UTF-8-safe messages, a report
  that renders.

Run single tests while learning:

```bash
ctest --test-dir build -R 'FdScan' --output-on-failure
EXECCONTRACT_BIN=$PWD/build/execcontract python3 -m pytest tests/component -k scanner -vv
```

## 6. Limitations you must be able to state

- Observations are sequential and start after the loader and C++ runtime;
  they are not an atomic snapshot of the `execve` instant.
- Output uses blocking writes; a reader that stops draining blocks the probe.
  Callers must use a timeout.
- A report file that was opened and then failed mid-write can be empty or
  partial (exit 3 still signals it).
- Sanitizer builds alter the observed state (ASan and `RLIMIT_CORE`).
- Verified locally on one environment (WSL2 Ubuntu 26.04, GCC 15); the CI
  workflow adds Ubuntu 24.04 with GCC 13. Other distributions, Clang and musl
  are untested (`strerrorname_np` also ties the code to glibc ≥ 2.32).

## 7. Rehearsal exercises

These exercises are still for the candidate to perform. Reading the answers
or asking an assistant to run them does not establish independent debugging
ability. Record the commands, observed failures and your explanation.

### Prepare an isolated practice clone

Run these commands in Linux, from the submission repository. The clone uses
the current committed revision, so commit any changes you intend to practise
first. All deliberate mutations and restores below happen only in this clone.

```bash
repo=$(pwd -P)
practice_root=$(mktemp -d "${TMPDIR:-/tmp}/execcontract-rehearsal.XXXXXX")
git clone --no-hardlinks "$repo" "$practice_root/repo"
cd "$practice_root/repo"
cmake -S . -B build -G Ninja -DEXECCONTRACT_BUILD_TESTS=ON -DEXECCONTRACT_SANITIZE=OFF
cmake --build build
export EXECCONTRACT_BIN="$PWD/build/execcontract"
ctest --test-dir build --output-on-failure --no-tests=error
```

Stop if this baseline fails. Use the normal build: a sanitizer runtime can
change the descriptor inventory you are trying to observe.

### E1 Reproduce scanner contamination

In the practice clone's `src/observe.cpp`, remove only the block that skips
`inv.scanner_fd` in `scan_fd_dir()`. Rebuild and run the targeted tests:

```bash
cmake --build build
python3 -m pytest -p no:cacheprovider tests/component/test_fds.py -k scanner_contamination -vv
```

Expect regression failures. Read the test's assertions as well as the output:
an earlier inventory-count assertion may fail before the verdict assertion.
Explain why an absent inherited fd 3 can be occupied by `opendir`, and why
that temporary descriptor must not count as evidence that the launcher
supplied fd 3. Also explain the case with fd 0 closed.

Restore only your deliberate mutation, rebuild and rerun:

```bash
git restore --source=HEAD -- src/observe.cpp
cmake --build build
python3 -m pytest -p no:cacheprovider tests/component/test_fds.py -k scanner_contamination -vv
```

The targeted tests should pass again. Never run the restore command against
unrelated edits in the submission working tree.

### E2 Reproduce the NUL truncation bug

Create a private fixture inside the practice build directory. This command
sets the umask in a subshell, so the weak prefix has a known passing value:

```bash
printf '%s\0%s' '{"version":1,"umask":"0022"}' '{"version":1,"fds":{"required":[]}}' > build/nul-contract.json
status=0
(umask 0022; "$EXECCONTRACT_BIN" --contract build/nul-contract.json) || status=$?
printf 'exit=%s\n' "$status"
ctest --test-dir build -R EmbeddedNul --output-on-failure --no-tests=error
```

Baseline: the CLI should reject the input with exit 2, and the regression
unit test should pass. Remove only the raw-NUL guard at the beginning of
`parse_strict()` in the practice clone's `src/contract.cpp`, then rebuild:

```bash
cmake --build build
status=0
(umask 0022; "$EXECCONTRACT_BIN" --contract build/nul-contract.json) || status=$?
printf 'exit=%s\n' "$status"
ctest --test-dir build -R EmbeddedNul --output-on-failure --no-tests=error
```

With the recorded nlohmann/json version, expect the mutant to accept the
weak prefix (exit 0) and the regression test to fail. Explain the difference
between a raw NUL byte in the input and an escaped JSON string character.
If your result differs, investigate the library version and actual report;
do not record the expected result as an observation.

Restore, rebuild and confirm rejection plus a passing regression test:

```bash
git restore --source=HEAD -- src/contract.cpp
cmake --build build
status=0
(umask 0022; "$EXECCONTRACT_BIN" --contract build/nul-contract.json) || status=$?
printf 'exit=%s\n' "$status"
ctest --test-dir build -R EmbeddedNul --output-on-failure --no-tests=error
```

### E3 Explain the demo prerequisite fix

The original shared `demo` fixture skipped all four demo cases below a hard
`RLIMIT_NOFILE` of 1024. Only the corrected launcher sets that soft limit.
The revised `demo` fixture just finds the example directory; `corrected_demo`
adds the limit check for tests that actually run the corrected launcher.

```bash
prlimit --nofile=128:128 -- env EXECCONTRACT_BIN="$EXECCONTRACT_BIN" python3 -m pytest -p no:cacheprovider tests/component/test_demo.py -rs
python3 -m pytest -p no:cacheprovider tests/component/test_demo.py -rs
ctest --test-dir build --output-on-failure --no-tests=error
```

The agent-run Linux validation recorded 1 passed and 3 skipped at 128:128,
and 4 passed at normal limits; see `docs/verification.md`, the follow-up
Linux results. These are the expected results when you reproduce this
exercise. The recorded agent run does not establish your personal rehearsal.

In the practice clone, move the guard back to the shared fixture and repeat
the low-limit command. Explain why this reduces coverage without changing
the production executable. Restore the original practice-clone file when
finished. The assistant prepared this fix; describe it as your own only if
you independently implement or reproduce it, with that distinction clear.

### E4 Explain without notes

For each review finding, state the triggering input, the old behavior, the
fix and the check that detects its removal. Trace one invocation from
`main()` to exit. Explain owned versus borrowed descriptors, error
precedence, `RLIM_INFINITY`, and why a successful test count does not alone
establish that the intended tests ran.

Finish by naming what you personally ran or changed and what an assistant
did. Use actual observations, including anything you could not reproduce.
