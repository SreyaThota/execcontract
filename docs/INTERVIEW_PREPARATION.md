# Interview preparation — ExecContract

This guide is for explaining and defending ExecContract in a technical
interview. It is written for an author who directed the project and had the
implementation, tests and verification carried out with AI assistance, and
who now needs to understand the design well enough to explain it, reason
about it and change it. Read it together with the code; every claim here can
be checked in the repository.

Section 12 lists what must be true before you describe any of the work as
your own. Read it first.

---

## 1. The project in plain words

When a launcher (a shell script, systemd, a container entrypoint, a test
harness) starts a service, the service inherits part of its environment from
the launcher: open file descriptors, the working directory, the umask, and
resource limits. Launchers get these wrong in quiet ways — a leaked log file
descriptor, the wrong directory, a too-permissive umask, a descriptor limit
of 1024 instead of 65536. The service still starts; the bug appears later.

ExecContract is a small Linux program you start **in place of the real
service** during a launcher test. It looks at its own process — descriptors,
working directory, umask, `RLIMIT_NOFILE`, `RLIMIT_CORE` — compares that with
a JSON *contract* describing what the launcher should provide, and prints a
JSON report plus an exit code a test can assert on:

| Exit | Meaning |
|---|---|
| 0 | everything matched |
| 1 | at least one requirement was violated (known wrong value) |
| 2 | the command line or the contract itself was invalid |
| 3 | something could not be observed, or the report could not be delivered |

**One-sentence version:** "It's a probe that a launcher test starts instead of
the service, and it reports — with evidence and a reliable exit code —
whether the launcher handed over the process state the service expects."

---

## 2. Architecture walkthrough

```
main()
 1. collect_snapshot()      observe.cpp   descriptors first, then umask, limits, cwd
 2. parse_args()            cli.cpp       pure
 3. ignore_sigpipe()                      broken pipe → EPIPE, not death by signal
 4. load + parse contract   contract.cpp  bounded read, strict validation
 5. evaluate() + aggregate() evaluate.cpp pure: contract × snapshot → checks → outcome
 6. render_report()         report.cpp    versioned JSON
    deliver()               main.cpp      stdout or --report file, every write checked
 7. one-line summary on stderr; exit code
```

Three ideas carry the whole design:

1. **Observe before touching anything.** ExecContract inspects *itself*, so
   any file it opens becomes part of what it observes. The descriptor
   inventory is therefore the very first thing `main()` does, before the
   contract or report file is opened.
2. **Pure core, thin edges.** Only collection (and file I/O for the contract
   and report) talks to the operating system. Validation, evaluation and
   rendering are plain functions over value types, which is why most logic
   is unit-tested without any special process state.
3. **"Wrong" and "unknown" are different results.** A check is `fail` when
   the value was observed and is wrong, `error` when it could not be
   observed. Unknown is never disguised as `0`, an empty list or
   `"unlimited"`.

---

## 3. Important files and what they do

| File | Responsibility | Names to know |
|---|---|---|
| `src/main.cpp` | Orchestration, report delivery, exit code | `main`, `run_check`, `deliver`, `same_file` |
| `src/observe.cpp` | Reads Linux process state | `scan_fd_dir`, `classify_fds`, `read_umask`, `observe_rlimit`, `observe_cwd`, `collect_snapshot` |
| `src/contract.cpp` | Loads and validates the JSON contract | `load_contract_file`, `parse_strict`, `validate` |
| `src/evaluate.cpp` | Compares contract and observations; precedence | `evaluate`, `aggregate`, `exit_code_for` |
| `src/report.cpp` | Builds the JSON report | `render_report` |
| `src/sys.{hpp,cpp}` | Ownership wrappers and checked I/O | `UniqueFd`, `DirStream`, `read_bounded`, `write_all` |
| `src/model.hpp` | Value types and limits | `Snapshot`, `Contract`, `Observed<T>`, `LimitValue` |
| `tests/unit/` | GoogleTest, one file per module (103 tests) | `test_observe.cpp`, `test_contract.cpp`, `test_robustness.cpp` |
| `tests/component/` | pytest against the built binary (97 tests) | `launcher.py`, `conftest.py`, `test_fds.py` |
| `scripts/verify.sh` | Full verification: normal + sanitizer builds, formatting | — |

---

## 4. E1 — the scanner descriptor problem

**What could go wrong.** To list its open descriptors, the probe opens the
directory `/proc/self/fd` with `opendir`. Opening a directory *creates a new
descriptor*, and Linux always gives out the **lowest free number**. Suppose
the contract requires fd 3 (for example a socket the launcher must pass) and
the launcher forgot it. Then 0, 1, 2 are open, 3 is free — so `opendir` gets
fd 3. The directory listing now contains "3" — the probe's own directory
handle — and a naive scanner reports "fd 3 is present". The requirement
passes, although the launcher never provided it. The tool would be lying in
exactly the situation it exists to detect.

**The fix.** `scan_fd_dir()` asks `dirfd()` which number its own stream got
and skips *exactly that number*. Not a hard-coded 3: if fd 0 was closed, the
stream becomes fd 0, and the same rule still works.

**The experiment.** Remove the three lines that skip `inv.scanner_fd`,
rebuild, run the scanner tests:

| | Result (agent-run, recorded in `docs/verification.md`) |
|---|---|
| Normal code | `test_scanner_contamination_fd3` and `_fd0` pass |
| Exclusion removed | both fail: inventory `count` is 4 instead of 3 (fd 3 counted), and fd 0 is reported `pass` instead of `fail`/`missing` |
| Restored | both pass again |

**How the test knows the dangerous case really happened.** The report
includes `fd.inventory.observed.scanner_fd`. The test asserts it is 3, so it
proves the scanner *did* take fd 3 — and that fd 3 was still reported missing.

---

## 5. E2 — the embedded NUL problem

**What could go wrong.** The contract is parsed with the nlohmann/json
library. Its lexer treats a raw NUL byte (`\0`) as the end of the input.
A file containing

```
{"version":1,"umask":"0022"}\0{"version":1,"fds":{"required":[]}}
```

was read as just the first, weaker object; everything after the NUL — here a
much stricter descriptor requirement — was silently ignored, and the run
returned exit 0 `passed`. Every other test was green, because none of them
contained a raw NUL. This was found by an AI reviewer, not by the original
test suite.

**Why a NUL can be rejected safely.** Valid JSON text never contains a raw
NUL byte. Inside a string a NUL must be written as the escape `\u0000`; the
raw byte has no legal place. So rejecting it loses nothing.

**The fix.** `parse_strict()` checks for `'\0'` in the raw text before calling
the library and reports `malformed_json` with the byte offset (exit 2).

**The experiment.**

| | Result (agent-run, recorded) |
|---|---|
| Normal code | CLI exit 2; unit test `ContractParse.EmbeddedNulIsRejectedNotTreatedAsEndOfInput` passes |
| Guard removed | CLI exit **0** (false pass); the unit test fails |
| Restored | exit 2; the test passes |

---

## 6. Why these bugs occurred

Both are the same kind of mistake: **trusting a lower layer's behaviour
without checking its edge cases.**

- E1: "listing a directory doesn't change it" is true for normal directories
  but not for `/proc/self/fd`, whose contents *are* the process's descriptors.
  The act of looking changes what is seen.
- E2: "a JSON library parses the whole input" is not quite true; this one
  stops at a NUL. A validator inherits its parser's idea of where the input
  ends.

General lesson worth saying in an interview: *a checker must be checked
against its own interference and its dependencies' quirks; test inputs need
to include the hostile cases, not just the expected ones.*

---

## 7. How the tests detect them (and other bugs)

- **Unit tests** (GoogleTest) call the production functions directly. Some
  use narrow seams that are just parameters: the directory to scan, the
  status file to parse, an injectable `readdir` function for one failure
  that procfs cannot produce.
- **Component tests** (pytest) always run the real binary. A tiny helper,
  `launcher.py`, runs as a separate Python process: it opens exactly the
  descriptors a test wants, sets cwd, umask and limits, then `exec`s the
  probe. pytest's own process is never modified.
- **Mutation check.** To show the tests actually detect bugs, realistic
  defects were injected one at a time (scanner exclusion removed, NUL guard
  removed, a descriptor opened too early, stdin read, a borrowed descriptor
  seeked, report failure ignored, ...). Each must make at least one test
  fail; the results are in `docs/verification.md`.
- **Mutation-based robustness test.** `test_robustness.cpp` mutates valid
  contracts 20 000 times with a fixed seed and checks that every input ends
  in exactly one documented result and a report that can be rendered.
- **Borrowed-descriptor test.** The test hands the probe a file, a pipe with
  unread data and stdin, keeping its own copies of the same open file
  descriptions; afterwards the offset, flags and data must be unchanged.

---

## 8. C++ and Linux concepts to understand

| Concept | What to be able to say |
|---|---|
| File descriptor | A small integer index into the process's table of open files; the kernel always allocates the lowest free number. |
| Open file description | The kernel object behind a descriptor; holds the offset and status flags. Duplicated descriptors (and inherited ones) share it — that is how the borrowed-descriptor test can see a seek. |
| `/proc/self/fd` | A directory listing this process's descriptors. Reading it opens one more. |
| `fstat` vs. procfs link text | `fstat(fd)` reports the type of the open object itself (works for deleted files and pipes). The link text in `/proc/self/fd` is only a label. |
| `O_CLOEXEC` | Close this descriptor on `exec`. Used on everything the probe opens, so nothing leaks onward. Leaks happen when launchers forget it — e.g. WSL leaks fd 7 (`/dev/ptmx`) into processes it starts, which ExecContract detected. |
| `RLIMIT_NOFILE` | Soft limit = highest descriptor number *that may be newly opened* + 1; existing descriptors above it stay open. Hard limit = ceiling for the soft limit; unprivileged processes can lower it, not raise it. |
| `RLIM_INFINITY` | The "unlimited" value; reported as `"unlimited"`, never confused with "unknown". |
| umask | Bits removed from permissions of newly created files. `umask(2)` can only read by writing, so ExecContract reads `Umask:` from `/proc/self/status` instead. |
| `getcwd` vs `$PWD` | `getcwd` is the kernel's physical path; `$PWD` is shell bookkeeping and can be stale or a symlink. |
| RAII | Resource tied to an object's lifetime: `UniqueFd`'s destructor closes its descriptor. Move-only, so a descriptor is never closed twice. |
| Owned vs borrowed | Owned (opened by the probe) → `UniqueFd`, closed exactly once. Borrowed (inherited) → plain `int`, never closed, read, seeked or modified. |
| Why `close()` is not retried | On Linux the descriptor is released even if `close` reports `EINTR`; retrying could close a different file that has since reused the number. |
| `readdir` and `errno` | `readdir` returns NULL both at the end and on error; only `errno` (cleared just before the call) tells them apart. |
| SIGPIPE | Writing to a pipe nobody reads kills the process by default. ExecContract ignores it so the write returns `EPIPE` and exits 3 with a message. |
| `std::optional` | Explicit "value or nothing"; `LimitValue` deliberately does *not* use it for "unlimited", so unlimited and unknown cannot be confused. |

---

## 9. Likely interview questions with sample answers

Use these as a starting point, then say them in your own words.

**"Walk me through what happens when it runs."**
"The first thing `main` does is collect the observations: it opens
`/proc/self/fd`, records every descriptor except the one the directory
listing itself uses, checks each descriptor's type with `fstat`, then reads
the umask from `/proc/self/status`, the two limits with `getrlimit`, and the
working directory with `getcwd`. Only after that does it parse the
arguments and open the contract. The contract is validated completely, then
compared with the observations, which gives a list of checks; the worst
status decides the exit code. The JSON report is written with every write
checked, and a summary goes to stderr."

**"Why collect before reading the contract?"**
"Because opening the contract creates a descriptor. If I opened it first, it
would appear in the inventory — or, if a required descriptor were missing,
it could even take that number. Collecting first means nothing of the
probe's own is open yet, apart from the directory stream, which is excluded
by number."

**"What does a passing result actually prove?"**
"That the values observed at the start of the probe's `main()` matched the
contract. It doesn't prove what happened before `main` — the loader runs
first — it isn't one atomic snapshot, and it says nothing about what the
real service does later. And `RLIMIT_CORE` is only one part of core-dump
behaviour."

**"Why `fstat` instead of reading the procfs link?"**
"The link text is a display label; a file could even be named
`pipe:[123]`. `fstat` asks the kernel about the open object itself, and it
works for files that have been deleted while still open."

**"How do you tell a violation from an inspection failure?"**
"A violation means we know the value and it's wrong, like umask `0022`
instead of `0027`. An inspection failure means we couldn't find out, like a
deleted working directory. Inspection failures outrank violations in the
exit code — you can't call a run 'merely violated' if part of it is unknown —
but the known violations are still listed."

**"Why a separate Python launcher in the tests?"**
"The test needs to start the probe with exact descriptors, limits and umask.
Changing those in pytest's own process would affect every later test, and
lowering a hard limit can't be undone. So a short-lived Python process sets
everything up and then `exec`s the probe. It also avoids `preexec_fn`, which
is unsafe in threaded processes."

**"How did the NUL bug get past the tests?"** — see section 5: the tests used
well-formed inputs and none contained a raw NUL; a reviewer constructed one;
now there is a regression test and a mutation-based robustness test.

**"Why C++ and not Python for the probe?"**
"The probe must observe the process before a runtime adds its own state. A
Python interpreter opens files, imports modules and sets signal handlers
before the first line of the script runs, so it would change exactly what it
is trying to observe. Python is a good fit for the test harness, which is
where it is used."

**"What would you improve next?"**
"Atomic report writing via a temporary file and rename, coverage-guided
fuzzing of the contract parser, and broader CI. As a feature, environment
variables would be the next inspection area, but with care, because values
can be secrets."

---

## 10. Difficult follow-up questions

- **"What if another thread opens a file during the scan?"** ExecContract is
  single-threaded, so its own code cannot race with itself. It does not claim
  an atomic snapshot of the whole process.
- **"Why not loop over descriptor numbers up to `RLIMIT_NOFILE`?"** Existing
  descriptors can sit above a lowered soft limit (a test places fd 500 under
  a soft limit of 64), and looping to the hard limit can mean a million calls.
- **"Why is 1024 the inventory cap, and what happens above it?"** It bounds
  work and report size; more than 1024 descriptors is an inspection error,
  never a truncated pass.
- **"Can the probe block forever?"** Yes, if a reader of its output stops
  reading: writes are blocking. That is documented; callers use a timeout.
  Only opening a FIFO with no reader fails fast.
- **"Does the sanitizer build test the same thing?"** Not exactly: the ASan
  runtime lowers `RLIMIT_CORE` before `main` unless configured not to, and
  may hold descriptors. That is why exact-descriptor tests run only on the
  normal build.
- **"How do you know your verification script really ran the tests?"** It
  forces the test options, checks that unit and component tests are
  registered and uses `ctest --no-tests=error`; an earlier version could
  succeed with zero tests from a stale build cache.
- **"Show me how you would add `RLIMIT_STACK`."** Add a `LimitRule` field and
  key in `contract.cpp`, an `observe_rlimit(RLIMIT_STACK)` call in
  `collect_snapshot`, an `evaluate_limit` call, tests in all three places,
  and the docs. Practising this change once is the best preparation.

---

## 11. Short glossary

**Contract** – the JSON file stating expected state. **Check** – one evaluated
requirement with `id`, `status`, `expected`, `observed`. **Outcome** – the
overall result (`passed`, `violated`, `inspection_failed`,
`invalid_contract`). **Launcher** – whatever starts the service. **Mutation
check** – deliberately breaking code to prove a test notices.

---

## 12. Before claiming personal implementation or testing experience

The repository's records state who did what: the implementation, the tests
and the recorded test runs and experiments (including E1/E2) were carried out
by AI agents; the author initiated and directed the project and requested the
reviews. Keep your statements consistent with that.

You can truthfully say you **designed the scope with AI assistance, directed
the work, commissioned critical reviews and understand the design** — once
you do. Before you say you **implemented, debugged or tested** something
yourself, you should have:

1. Built the project and run both test suites yourself.
2. Reproduced E1 and E2 yourself (`DEVELOPER_NOTES.md`, section 7) and be
   able to explain each failing assertion.
3. Traced one run through `main()` in the code without notes.
4. Made one small change yourself (for example the `RLIMIT_STACK` exercise
   above), with a test that fails before and passes after.
5. Be able to answer sections 9 and 10 without reading.

If you have not done these, say so plainly: *"I directed this project and had
the implementation done with AI assistance; I've studied the design and can
walk you through it."* Interviewers generally respect an accurate account of
how something was built far more than an overstated one they can easily test.
