# ExecContract — a short technical reflection

## How this project was produced

ExecContract was built with an AI coding agent (Claude Code) working from a
detailed written brief: the five inspection areas, the exit-code contract,
the descriptor-contamination regression and the required test matrix were
fixed in that brief before any code existed. The agent wrote the
implementation, the tests and the documentation, ran the builds and tests in
WSL, and recorded the evidence in `docs/verification.md`. Two further AI
reviews — one inside the development session, one external — attacked the
result and found real defects, all fixed with regression tests. The
reflection at the end of this essay is the author's own.

> **[Author — before submitting, state in one or two sentences what you
> personally did: e.g. which decisions in the brief were yours, what you
> reviewed, reproduced or changed yourself. Keep it accurate.]**

## The project and why this scope

A service behaves differently when started by hand, by a shell wrapper, by
systemd or by a test harness, because each launcher hands it a slightly
different process: leaked descriptors, another working directory or umask,
different resource limits. ExecContract is a small native probe that a
launcher integration test starts *instead of* the service. It inspects its
own execution context in five areas — open descriptors, physical working
directory, umask, `RLIMIT_NOFILE`, `RLIMIT_CORE` — compares them with a
versioned JSON contract, and returns a report and an exit code a test can
assert on. The scope was chosen by asking which launcher mistakes are common
and which properties can be observed *without changing them*; environment
variables, signal state and supervision were deferred to the roadmap. Tools
such as `procenv` dump far more; ExecContract's contribution is narrower: a
strict contract, stable identifiers, and explicit failure semantics.

## Organisation, testing and documentation

The code is a static library plus a thin `main()`. Only the collection module
talks to the kernel about process state; contract validation, evaluation and
rendering are pure functions over plain value types. That split decided the
test strategy: 102 GoogleTest unit tests cover validation, evaluation,
precedence and rendering directly, and the collectors through narrow seams
that are plain parameters (the directory to enumerate, the status file to
parse, and one injectable `readdir` for a failure procfs cannot produce
unprivileged).

The 96 pytest component tests always run the compiled binary. A small
launcher runs as a fresh `python -I` process, installs exactly the
descriptors, directory, umask and limits a test asks for, restores the
default SIGPIPE disposition and `exec`s the probe, so pytest's own process is
never changed. The key regression: if fd 3 is required but absent,
`opendir("/proc/self/fd")` itself becomes fd 3. The report exposes the
scanner's descriptor number, so the test proves the dangerous situation
occurred and fd 3 is still reported missing. A mutation check (ten injected
defects, all caught) and a run of the whole suite under descriptor limits
from 10240 down to 128 check that the tests can fail and that they do not
depend on a generous development machine.

## Technical decisions and trade-offs

C++17 rather than C++20, because nothing needed the newer standard. CMake
with `find_package` only, so builds work offline once three distro packages
are installed. nlohmann/json for parsing, with a small application-specific
validator — its quiet behaviours (keeping the last duplicate key, treating a
NUL byte as end of input) had to be countered explicitly. GoogleTest because
it is the common industry choice and integrates with CTest.

The decisions that matter most concern the honesty of the verdict. The
descriptor policy is a closed allowlist with no special cases. `fail` means
known and wrong, `error` means unknown; an error outranks violations but never
hides them, and an unavailable value is never encoded as `0`, `[]` or
`"unlimited"`. Types come from `fstat` on the descriptor, not procfs link
text; the umask is read from `/proc/self/status` because `umask(2)` can only
read by writing. Output failures never yield exit 0. Output is deliberately
*not* bounded in time: a reader that stops draining blocks the probe, and the
caller's timeout is the documented safeguard.

## What was found during development

- The ASan runtime lowers `RLIMIT_CORE` before `main()`, so the sanitizer
  build first failed the core-limit tests — instrumentation contaminating the
  observation. Fixed by configuring the runtime, not by skipping tests.
- The demo exposed a real leak: processes started through `wsl.exe` inherit
  fd 7, an `O_RDWR` `/dev/ptmx` handle without close-on-exec.
- A reviewer found a false pass: a NUL byte made the JSON parser stop, so
  `{weak}\0{strict}` passed. Numbers beyond double range and invalid UTF-8
  also escaped the input-error path.
- Following the README in a clean clone broke the demo with a relative path.
- The second review showed that the verification script itself could report
  success with zero tests from a stale build cache, and that the test
  launcher demanded 1 564 descriptors, so 90 of 96 tests failed under a
  1024/1024 limit before the probe even ran.

Each fix landed with a test that fails on the previous version. The common
lesson is that the test infrastructure needs the same scepticism as the
product.

## Reflection

> **[Author — write this section yourself, roughly 150–250 words. Prompts:
> Which design decision would you defend most, and why? Which finding above
> did you reproduce or understand only after working through it yourself?
> What would you do differently if you started again (e.g. how the work was
> split between you and the assistant, fuzzing the parser earlier, CI)? Do
> not describe actions you did not take.]**
