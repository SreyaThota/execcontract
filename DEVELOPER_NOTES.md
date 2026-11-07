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
(gdb) finish          # look at inv.scanner_fd and inv.entries
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
| `write` | short writes and `EINTR` are normal; `EPIPE` only if SIGPIPE is ignored | `write_all` |
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
- Only one environment (WSL2 Ubuntu 26.04, GCC 15) has been verified.

## 7. Rehearsal exercises

Do these yourself, without an assistant, and keep notes of what you saw.

**E1 — Reproduce the scanner false pass.** In `scan_fd_dir()`, delete the
`if (fd == inv.scanner_fd) { continue; }` block. Rebuild and run
`pytest … -k scanner_contamination -vv`. Explain from the failure output why
fd 3 is now reported present, then restore the block (`git checkout src/observe.cpp`).

**E2 — Reproduce the NUL false pass.** In `parse_strict()`, comment out the
NUL check. Create `printf '{"version":1,"umask":"0022"}\0{"version":1,"fds":{"required":[]}}' > /tmp/nul.json`,
run `build/execcontract --contract /tmp/nul.json; echo $?`, and explain the
exit status 0. Run `ctest -R EmbeddedNul` to see the regression test fail.
Restore.

**E3 — A real, small open issue: fix it yourself.** `tests/component/test_demo.py`
skips *all four* demo tests when the hard `RLIMIT_NOFILE` is below 1024
(`demo` fixture), although only the corrected launcher (`ulimit -S -n 1024`)
needs that. Under `prlimit --nofile=128:128` the defective-launcher test is
therefore skipped needlessly.
- Make only the tests that run `launch-fixed.sh` skip; keep the reason text.
- Prove it: run
  `prlimit --nofile=128:128 -- env EXECCONTRACT_BIN=$PWD/build/execcontract python3 -m pytest tests/component/test_demo.py -rs`
  before and after (the defective test should go from *skipped* to *passed*),
  and run the full suite normally.
- Commit with a message that explains the change.

**E4 — Explain without notes.** Answer the questions in
[docs/verification.md](docs/verification.md#independent-review) aloud: for
each review finding, say what input triggered it, why the old code
misbehaved, and which test now fails without the fix.
