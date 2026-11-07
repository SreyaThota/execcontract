# Engineering decisions

Each entry: decision — alternative considered — reason — accepted trade-off.

**D1. C++17, not C++20.** Alternative: C++20 (`std::span`, `starts_with`,
designated initialisers). Nothing in the design needs C++20; C++17 builds on
GCC ≥ 9 / older LTS images that automotive and embedded CI often still use.
Trade-off: a few lines that C++20 would make shorter.

**D2. CMake + system packages via `find_package`, no `FetchContent`.**
Alternative: download dependencies at configure time. Builds must be
offline after setup (NFR-4), and an unpinned download at configure time is a
supply-chain and reproducibility risk. Trade-off: contributors must install
three distro packages first.

**D3. nlohmann/json for parsing, plus a small schema validator.**
Alternatives: hand-written parser (error-prone for a 64 KiB input with
escapes and Unicode), RapidJSON/simdjson (faster, but speed is irrelevant
here), a JSON Schema library (a large dependency for six keys). nlohmann is
header-only, MIT, packaged everywhere, rejects invalid UTF-8 and has a parser
callback. Trade-off: it silently keeps the last of duplicated keys, so
duplicate detection is done in the parser callback; integers beyond 64 bits
become floats, which the validator rejects as a type error.

**D4. GoogleTest.** Alternative: Catch2 (nicer syntax). GoogleTest is the
common choice in automotive C++ code bases, is packaged in Ubuntu, and
`gtest_discover_tests` gives one CTest entry per test. Trade-off: macro-heavy
style.

**D5. pytest component tests through an isolated `python -I` launcher.**
Alternatives: `preexec_fn` (unsafe in potentially threaded processes; it runs
arbitrary Python between `fork` and `exec`), a C launcher helper (more code
to review), shell fixtures (hard to make exact). A fresh single-threaded
Python process sets descriptors, cwd, umask and limits and then `execv`s the
real binary; PEP 446 makes every descriptor Python opens non-inheritable, so
only explicitly installed descriptors (and 0–2) reach the probe. pytest's own
state is never changed. Trade-off: one extra Python interpreter start per
test (the 85-test suite still runs in about 3–5 s here).

**D6. `/proc/self/fd` + `dirfd()` exclusion, not a loop to `RLIMIT_NOFILE`.**
A loop with `fcntl(F_GETFD)` up to the soft limit misses descriptors above a
lowered soft limit (they stay open), and looping to the hard limit instead
can mean a million `fcntl` calls (this WSL environment's hard limit is
1 048 576). procfs gives the exact set; the only contamination is
the stream's own descriptor, which `dirfd()` identifies precisely.
Trade-off: requires a mounted `/proc` (a documented requirement).

**D7. `fstat()` for types.** Alternative: parse `readlink(/proc/self/fd/N)`
(`pipe:[…]`, `socket:[…]`, paths). Link text is a label, can be ambiguous
(a file literally named `pipe:[1]`) and says nothing reliable about deleted
files. `fstat` classifies the open object itself. Trade-off: `socket` does
not say *which* kind of socket.

**D8. Umask from `/proc/self/status`.** Alternative: `umask(x); umask(old)`.
That briefly changes process state and is racy with threads (none here, but
the property should not depend on that). Inference by creating a file is
wrong under default ACLs. Trade-off: needs Linux ≥ 4.7.

**D9. Closed descriptor allowlist; 0–2 not special.** Alternatives: open
policy (only check listed descriptors), implicit permission for 0–2. Leaks
are the main defect class this tool exists to find, so "everything else is
unexpected" is the useful default, and making stdio explicit avoids hidden
policy (a daemon may legitimately have fd 0 closed). Trade-off: every
descriptor contract lists 0–2. An opt-out is on the roadmap.

**D10. Strict, non-normalising contract.** Expected paths must already be
canonical; numbers must be JSON integers; umask is a string. The tool never
guesses what the author meant — `"/srv/app/"` or `18` (decimal for octal 22)
are rejected with a JSON pointer. Trade-off: slightly less forgiving input.

**D11. Physical cwd, exact bytes.** `getcwd` reports the kernel's resolved
path; `$PWD` is shell state and can be stale or a symlink. Comparison is
exact bytes; the expected path is not resolved. Trade-off: contracts written
with a symlinked path fail — intentionally, and the report shows the
physical path to use.

**D12. Three statuses and precedence.** `fail` means *known and wrong*,
`error` means *unknown*. An inspection error outranks violations in the
outcome (exit 3) because a run with unknowns cannot be called merely
"violated", but known violations are still listed. A missing observation is
never encoded as `0`, `[]` or `"unlimited"`.

**D13. Report delivery is checked; SIGPIPE ignored.** The report is the
product, so a short write, `EPIPE`, `ENOSPC` or failing `close` exits 3.
With the default SIGPIPE disposition the process would die by signal
(no diagnostic, shell status 141); ignoring it turns that into a handled
`EPIPE`. Changing signal disposition happens after the snapshot and signal
state is out of scope for inspection.

**D14. `ordered_json` for `Check::expected/observed`.** Alternative: typed
variants per area. Expected/observed are inherently heterogeneous report
values; typed variants would add ~150 lines of conversion code for no
behavioural benefit. Trade-off: evaluation depends on the JSON library type.

**D15. Commit granularity.** The code and tests were written while the
development machine had no compiler; they were then verified in one pass.
Intermediate "verified" states were not fabricated; history therefore has a
specification commit followed by one implementation commit, and smaller
commits from then on.
