# Architecture

## Data flow

```
main()
  │ 1 collect_snapshot()  ── observe.cpp ──▶ Snapshot {FdInventory, umask, nofile, core, cwd}
  │ 2 parse_args()        ── cli.cpp     ──▶ CliOptions | usage error (exit 2, stderr only)
  │ 3 ignore SIGPIPE
  │ 4 load_contract_file() + parse_contract() ── contract.cpp ──▶ Contract | ContractError
  │ 5 evaluate() + aggregate()               ── evaluate.cpp ──▶ [Check], Outcome
  │ 6 render_report()                        ── report.cpp   ──▶ JSON text
  │   deliver()  (stdout or --report file)   ── main.cpp
  │ 7 one-line summary on stderr
  ▼ exit code
```

Each step consumes plain values produced by the previous one. Only steps 1, 4
and 6 make system calls; evaluation and rendering are pure functions, which
is what makes most behaviour unit-testable without live process state.

## Modules

| File | Responsibility | System calls |
|---|---|---|
| `src/sys.{hpp,cpp}` | `UniqueFd`, `DirStream` (RAII), `read_bounded`, `write_all`, errno names | `read`, `write`, `close`, `closedir` |
| `src/model.{hpp,cpp}` | Value types (`Snapshot`, `Contract`, `LimitValue`, `Observed<T>`), limits, UTF-8 validation, umask formatting | none |
| `src/observe.{hpp,cpp}` | Collection: descriptor scan, `fstat` types, umask, limits, cwd | `opendir/readdir/dirfd`, `fstat`, `open/read`, `getrlimit`, `getcwd` |
| `src/contract.{hpp,cpp}` | Bounded contract loading; strict v1 schema validation | `open`, `fstat`, `read` |
| `src/evaluate.{hpp,cpp}` | Contract × snapshot → ordered `Check`s; outcome precedence; exit codes | none |
| `src/report.{hpp,cpp}` | Versioned JSON report (strict UTF-8), fallback report | none |
| `src/cli.{hpp,cpp}` | Argument parsing, usage text | none |
| `src/main.cpp` | Orchestration and report delivery only | `sigaction`, `open`, `write` |

There are no class hierarchies, no global mutable state and no threads. The
core is a static library (`execcontract_core`) linked by both the executable
and the unit tests, so tests exercise production code.

## Observation order and contamination

ExecContract inspects *itself*, so every descriptor it opens is a potential
false observation. The order in `main()` is therefore part of the contract:

1. **Descriptor inventory first.** At this point ExecContract has opened
   nothing of its own. `opendir("/proc/self/fd")` necessarily allocates the
   lowest free descriptor number; if the launcher failed to pass fd 3, the
   stream *becomes* fd 3. The scanner calls `dirfd()` and excludes exactly
   that number, never a hard-coded one. The number is reported as
   `fd.inventory.observed.scanner_fd`, which lets the component test
   `test_scanner_contamination_fd3` prove that the dangerous case really
   occurred and that fd 3 is still reported missing. The stream is closed
   (RAII) before the function returns.
2. **Types** come from `fstat()` on each recorded descriptor number — the
   object itself, including deleted files and anonymous pipes. The procfs
   symlink text is only a label and is never read.
3. **Umask** is read from `/proc/self/status` through a transient, owned
   descriptor that is closed before the next step. `umask(2)` is never
   called: it can only read by writing.
4. **Limits** via `getrlimit` (no descriptors).
5. **Working directory** via `getcwd` into a growing buffer (no descriptors;
   `$PWD` is ignored).
6. Only then are the **contract** (opened `O_NONBLOCK`, read, closed) and the
   **report file** opened. Neither can appear in the inventory.

The snapshot is collected for all five areas before the contract is known,
because the contract cannot be read without opening a descriptor. The
contract is nevertheless validated completely before any requirement is
evaluated, so an invalid contract always yields `invalid_contract` regardless
of what the snapshot contains.

This is not an atomic snapshot of the process. Single-threaded code prevents
ExecContract from changing its own state between steps, but the dynamic
loader and C++ runtime run before `main()` (and sanitizer runtimes do more,
see below), and nothing here is synchronised with external actors.

## Ownership

* **Owned** descriptors are always `UniqueFd` or `DirStream`; they are closed
  exactly once and `close()` is never retried (on Linux the descriptor is
  released even when `close` reports `EINTR`; a retry could close a
  descriptor reused by another `open`). `close_checked()` is used where the
  error matters (the report file, where `close` can surface deferred write
  errors).
* **Borrowed** descriptors — everything inherited — are plain `int`s. They are
  never closed, read, seeked or modified. The only write to a borrowed
  descriptor is the report on fd 1 (and the summary on fd 2).
* Destructors never throw.

## Failure handling

| Situation | Representation | Outcome / exit |
|---|---|---|
| Bad CLI arguments | stderr message + usage; stdout empty | 2 |
| Contract missing/unreadable/non-regular/too large/malformed/invalid | `ContractError` → report with top-level `error`, empty `checks` | `invalid_contract`, 2 |
| One observation failed | `Observed<T>` holds `SysError{code, errno, detail}` → check `status: error`, `observed: null` | `inspection_failed`, 3 |
| Descriptor enumeration failed or capped | `FdInventory.complete == false`; entries already seen are kept | `fd.inventory` errors; seen descriptors still yield known violations; unseen ones become errors |
| `Observed<T>` with neither value nor error (programming error) | `observation_missing` | fails closed as an inspection error |
| Non-UTF-8 observed path | `cwd_not_utf8` with length and offset only | 3 |
| Serialisation throws (strict UTF-8) | constant ASCII fallback report | `internal_error`, 3 |
| Report write/close fails, `EPIPE` (SIGPIPE is ignored), `ENOSPC`, `EBADF`, FIFO without reader (`ENXIO`, report opened `O_NONBLOCK`) | stderr message | 3, never 0 |
| `--report` names the contract file | stderr message, contract untouched | 2 |
| Raw NUL, invalid UTF-8, number beyond double range in the contract | `malformed_json` / `out_of_range`, message bytes ASCII-escaped | `invalid_contract`, 2 |
| Uncaught exception (e.g. `bad_alloc`) | stderr message | 3 |

Precedence: usage → invalid contract → inspection failure → violation → pass;
a delivery failure overrides everything.

Interrupted calls are handled per call: `read` and `write` are retried on
`EINTR` (no data is lost or duplicated), `close` is not, and the remaining
calls (`opendir`, `readdir`, `fstat`, `getrlimit`, `getcwd`) do not return
`EINTR` in practice on these file systems. `readdir`'s end-of-directory versus
error ambiguity is resolved by clearing `errno` immediately before each call
and reading it immediately after.

## Test seams

Three narrow seams exist, all plain parameters rather than mocks:

* `scan_fd_dir(path, cap, next_entry)` — production passes `/proc/self/fd`
  and the default `::readdir`; unit tests pass temporary directories with
  crafted names to exercise numeric parsing, unparseable entries, the cap
  boundary and a missing directory.
* The `next_entry` function pointer of the same function exists for one
  failure only: `readdir` failing mid-enumeration (and a successful call that
  leaves `errno` modified), which cannot be produced unprivileged on procfs.
  The test wrapper calls the real `readdir` and fails at a chosen point.
* `read_umask(path)` / `parse_umask_status(text)` — tests supply status files
  without the field, malformed values or oversized content.

No production CLI option exists to simulate failures.

## Instrumented builds

ASan's runtime runs before `main()` and changes what the probe observes: it
lowers `RLIMIT_CORE` (unless `ASAN_OPTIONS=disable_coredump=0`, which the
sanitizer test configuration sets) and may hold extra descriptors. Exact
descriptor-inventory tests are therefore marked `exact_fds` and run only
against the normal build.
