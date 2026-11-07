# ExecContract technical reflection

## Background and contribution

I am an M.Sc. Artificial Intelligence student at BTU Cottbus-Senftenberg
with about two years of professional Python experience. At Nav Tech
Electronics, my work included generating structured ECU diagnostic test
cases, writing pytest suites and improving CI/CD workflows. ExecContract
connects that testing background to Linux process state and C++ resource
ownership.

I initiated the project, requested a comparison of ideas, and commissioned
implementation, critical reviews and follow-up fixes. The engineering brief
was developed with AI assistance. Claude Code wrote the implementation,
tests and initial documentation and ran the recorded Linux verification.
Codex contributed review, targeted fixes and documentation. This reflection
was also prepared with AI assistance. My contribution has been project
direction and requesting review; the recorded implementation, testing and
debugging were performed by the agents.

## Purpose and scope

A service can inherit different process settings when started manually,
through a shell wrapper, or by a service manager. ExecContract is a small
native probe that a launcher integration test starts instead of the service.
It compares its own execution context with a versioned JSON contract and
returns a structured report and an exit code.

The scope covers five areas: inherited file descriptors, physical working
directory, umask, `RLIMIT_NOFILE` and `RLIMIT_CORE`. These are concrete
launcher properties with useful failure cases and manageable tests.
Environment variables, signal-state inspection and process supervision
remain outside this release. Existing tools such as procenv already expose
extensive process information. ExecContract focuses on enforceable
expectations and explicit failure semantics. Its observations are collected
sequentially after the loader and runtime have run; they are not an atomic
snapshot of `execve` or proof of the real service's later state.

## Organisation and verification

The C++17 implementation consists of a static core library and a thin
`main()`. The observation module reads Linux process state. Contract
validation, evaluation and report rendering operate on value types.
RAII wrappers release descriptors the probe opens, while inherited
descriptor numbers are borrowed and must remain open.

GoogleTest exercises parsing, evaluation, reporting and the collectors.
Narrow seams allow tests to use a controlled directory or status file and
inject a `readdir` failure. Python component tests launch the compiled binary
through a separate `python -I` helper. That helper installs the requested
descriptors, directory, umask and limits, restores default SIGPIPE handling
and executes the probe. This keeps process-wide changes out of pytest.
CTest and subprocess timeouts bound the verification runs.

The verification record reports 102 unit tests and 96 component tests passing
normally. Sanitizer runs include the unit tests and 81 component tests;
15 cases requiring an exact descriptor inventory are excluded because
instrumentation can alter that inventory. The follow-up Linux run reports
88 component tests passing and eight skipping under a hard descriptor limit
of 128. Ten injected mutants were detected at an earlier recorded revision.
The record also documents a clean clone and the successful removal and
restoration of the scanner and NUL guards. These checks were performed by
agents on one WSL Linux configuration; other distributions remain unverified.

## Decisions and limitations

The most important implementation rule is to inspect descriptors before
opening the contract or report. Even enumeration changes the state being
observed: `opendir("/proc/self/fd")` opens a descriptor. If fd 3 was absent,
the scanner may take number 3. Excluding the actual `dirfd()` prevents the
tool from satisfying a requirement with its own temporary resource.
`fstat` identifies an inherited object's type even when its pathname has
been deleted. Reading `Umask:` from `/proc/self/status` avoids temporarily
changing the umask to inspect it.

The report distinguishes a known violation from an observation failure.
Unknown values are never represented as zero, an empty inventory or
"unlimited". An inspection error takes precedence over violations while
preserving them in the report. Strict input validation also matters: review
found that a raw NUL could terminate JSON parsing early and conceal a
stricter suffix. The parser now rejects that input, with regression coverage.

Reviews exposed weaknesses in the tests themselves. The old launcher
required 1,564 descriptor numbers even when a test needed none, and the
verification script could succeed without running tests from a stale build
cache. The revised launcher stages descriptors when needed; verification
forces its build options and checks test registration. A pipe test now
drains concurrently instead of assuming sufficient capacity. Other findings
required narrower documentation: output can block behind a stalled reader,
and a report write failure can leave an empty or partial file. Callers need
a timeout; atomic report publication remains future work.

## Reflection

My professional experience includes building a Python pipeline for structured
ECU diagnostic test cases, writing pytest suites and improving CI/CD workflows.
ExecContract connects that background to an earlier point in the testing
chain: whether a launcher supplies the execution context a component expects.

The decision I would defend most is distinguishing a violated requirement
from an inspection failure. A known incorrect value and an unavailable
observation need different outcomes. For automation, that distinction makes
a report actionable: it separates a launch-configuration problem from a
problem with collecting evidence.

The recorded review findings also make test infrastructure an important part
of this project. A verification script can finish successfully without running
tests, and a fixture can fail because it assumes generous descriptor limits.
These cases reinforce the need to check what actually ran and under which
conditions, rather than treating a green status as sufficient evidence.

I would preserve the narrow scope and explicit failure semantics. For a
subsequent release, I would prioritize automated Linux verification across
another environment and atomic report publication. Those improvements would
strengthen repeatability and reliability without expanding the probe into a
larger monitoring framework.
