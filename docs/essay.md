# ExecContract — a short technical reflection

## The project and why this scope

A service behaves differently when it is started by hand, by a shell wrapper,
by systemd or by a test harness, because each launcher hands it a slightly
different process: leaked descriptors, a different working directory or
umask, different resource limits. These bugs are invisible from inside a
healthy-looking service and tend to surface late. ExecContract is a small
native probe that a launcher integration test starts *instead of* the service.
It inspects its own execution context in five areas — open descriptors,
physical working directory, umask, `RLIMIT_NOFILE`, `RLIMIT_CORE` — compares
them with a versioned JSON contract, and returns a report and an exit code a
test can assert on.

I chose this problem because it sits where the role I am applying for sits:
automated testing, finding issues before production, and tools that plug into
automation. I chose the scope by asking what a launcher test most often gets
wrong and what can be observed *without changing the thing observed*. That
excluded a supervisor, inspecting other processes, environment variables and
signal state (all candidates in the roadmap), and left five areas that I could
specify exactly and test from the outside. Existing tools such as `procenv`
dump far more information; ExecContract's contribution is narrower: a strict
contract, a verdict with stable identifiers, and explicit failure semantics.

## Organisation, testing and documentation

The code is a static library plus a thin `main()`. Collection
(`observe.cpp`) is the only module that talks to the kernel about process
state; contract handling, evaluation and reporting are pure functions over
plain value types; `main.cpp` only orchestrates and delivers the report. That
split decided the test strategy. GoogleTest unit tests (102) cover validation,
evaluation, precedence and rendering directly, plus the collectors through two
narrow seams that are just parameters — the directory to enumerate and the
status file to parse — so cap boundaries and malformed procfs content are
tested with real files instead of mocks.

The Python component tests (96) always run the compiled binary. The hard part
was controlling the launch context without disturbing pytest itself: a tiny
launcher script runs as a fresh `python -I` process, installs exactly the
descriptors, working directory, umask and limits a test asks for, restores the
default SIGPIPE disposition, and `exec`s the probe. Python's non-inheritable
descriptors (PEP 446) make the inventory exact, and nothing in pytest's own
process is changed. These tests cover the behavioural matrix, including the
case I considered most important: if fd 3 is required but absent,
`opendir("/proc/self/fd")` itself becomes fd 3. The report exposes the
scanner's own descriptor number, so the test can prove that the dangerous
situation actually occurred and that fd 3 is still reported missing.

Passing tests prove little if they cannot fail, so I ran a small mutation
check: ten realistic defects injected one at a time. Nine were caught at
first. The survivor — treating a `readdir` error as end-of-directory — cannot
be provoked unprivileged on procfs, so I added the one dependency-injection
seam in the project, a defaulted `readdir` function pointer, and the mutant
is now caught at unit level. The
documentation follows the audience: README for users, CONTRIBUTING for
contributors (including the rules that protect correctness), requirements with
identifiers and a traceability table, and a verification record that lists
commands and results, including what was not verified.

## Technical decisions and trade-offs

*C++17* rather than C++20: nothing needed the newer standard, and older
toolchains remain common in embedded and automotive CI. *CMake with
`find_package` only*: builds work offline once three distro packages are
installed, which I checked in a network-less namespace. *nlohmann/json* for
parsing rather than a hand-written parser, with a small application-specific
validator on top; its one surprising behaviour — silently keeping the last
duplicated key — is caught in the parser callback. *GoogleTest* because it is
the common choice in C++ industry code and integrates with CTest.

The design decisions I would defend most are about honesty of the verdict.
The descriptor policy is a closed allowlist with no special cases, because
leaks are the defect class the tool exists for. A check is `fail` only when
the value is known and wrong, and `error` when it is unknown; an error
outranks violations in the outcome but never hides them, and an unavailable
value is never encoded as `0`, `[]` or `"unlimited"`. Types come from `fstat`
on the descriptor, not from procfs link text. The umask is read from
`/proc/self/status`, because `umask(2)` can only read by writing. The report
write, the final `close` and SIGPIPE are all handled, because a report that
silently failed to arrive must never look like success.

## What I found and what I would change

Three things surprised me. First, the AddressSanitizer runtime lowers
`RLIMIT_CORE` before `main()` runs, so the sanitizer build initially failed
the core-limit tests; the probe was right and the instrumentation was
contaminating the observation. The fix was to configure the runtime
(`disable_coredump=0`) rather than skip tests, and to document it. Second, the
demonstration revealed a genuine leak on my own machine: processes started
through `wsl.exe` inherit descriptor 7, an `O_RDWR` handle to `/dev/ptmx`
without close-on-exec. That is exactly the kind of defect ExecContract is
for, found by accident. Third, `clang-tidy` pointed at an unchecked
invariant: evaluation assumed every observation held either a value or an
error. A default-constructed one would have been undefined behaviour; it now
fails closed.

The most instructive defect came from the independent review: a NUL byte in
the contract made the JSON library stop reading, so a weak contract followed
by `\0` and a stricter one *passed*. Every test was green because none of
them contained a raw NUL — exactly the kind of false pass the project exists
to prevent, and a reminder that a validator inherits its parser's notion of
"end of input". The same review found that numbers beyond double range and
invalid UTF-8 escaped the input-error path. Separately, following my own
README in a clean clone showed that the demo script broke with a relative
binary path. Each fix landed with a test that fails on the old binary.

With hindsight I would change the order of work. The development machine had
no compiler for the first part of the project, so specification, code and
tests were written before anything could be built, and then verified in one
pass. The code held up, but the commit history is coarser than I would like —
one specification commit and one implementation commit — because I did not
want to fabricate intermediate states that were never verified. I would also
fuzz the contract parser from the start (it would likely have found the NUL
case), write the report atomically via a temporary file, and add a CI
workflow across distributions and compilers. The scope itself I would keep.
