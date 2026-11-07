# ExecContract technical reflection

## Contribution and use of AI

I asked for a comparison of projects for this assignment, pursued ExecContract,
and requested critical reviews and follow-up fixes. The detailed engineering
brief was developed with AI assistance. Claude Code wrote the implementation,
tests and initial documentation and ran the Linux checks recorded in
`docs/verification.md`. Further AI reviews identified defects and overstated
claims. This essay was also drafted with AI assistance. My contribution at
this stage is initiating the project and requesting assistance and reviews;
I have not yet independently reviewed or exercised the implementation.
The recorded results describe agent-run verification.

## Purpose and scope

A service can inherit different process settings when started manually,
through a shell wrapper, or by a service manager. ExecContract is a small
native probe that a launcher integration test starts instead of the service.
It compares its own execution context with a versioned JSON contract and
returns a structured report and an exit code.

The scope covers five areas: inherited file descriptors, physical working
directory, umask, `RLIMIT_NOFILE` and `RLIMIT_CORE`. These are concrete launcher
properties with useful failure cases and manageable tests. Environment
variables, signal-state inspection and process supervision remain outside
this release. Existing tools such as procenv already expose extensive process
information. ExecContract focuses on enforceable expectations and clear
failure semantics. Its observations describe the probe during initial
collection, after the loader and runtime have run; they are sequential,
not an atomic snapshot of `execve` or proof of the real service's later state.

## Organisation and verification

The C++17 implementation consists of a static core library and a thin
`main()`. The observation module reads Linux process state. Contract
validation, evaluation and report rendering operate on value types. Resource
ownership is explicit: RAII wrappers close descriptors the probe opens,
while inherited descriptor numbers are borrowed and must remain open.

GoogleTest exercises parsing, evaluation, reporting and the collectors.
Narrow seams allow tests to use a controlled directory or status file and to
inject a `readdir` failure. Python component tests launch the compiled binary
through a separate `python -I` helper. That helper installs the requested
descriptors, directory, umask and resource limits, restores default SIGPIPE
handling and executes the probe. This keeps process-wide changes out of the
pytest process. CTest and subprocess timeouts bound the verification runs.

The recorded Linux verification at commit `582ffa3` reports 102 unit tests
and 96 component tests passing normally. The sanitizer configuration runs
the unit tests and 81 component tests, excluding 15 cases whose exact
descriptor expectations can be disturbed by instrumentation. The evidence
also records a clean clone, reduced descriptor limits and ten detected
mutants at an earlier revision. These are distinct checks, not a guarantee
of correctness. Only one WSL Linux configuration has been verified; the
external review could inspect the source but could not reproduce those
Linux runs.

## Decisions and limitations

The most important implementation rule is to inspect descriptors before
opening the contract or report. Even enumeration changes the state being
observed: `opendir("/proc/self/fd")` opens a descriptor. If fd 3 was absent,
the scanner may take number 3. Excluding the actual `dirfd()` prevents the
tool from satisfying a requirement with its own temporary resource.
`fstat` identifies each inherited object's type, including an open file
whose pathname has been deleted. Reading `Umask:` from `/proc/self/status`
avoids temporarily changing the umask to inspect it.

The report distinguishes a known violation from an observation failure.
Unknown values are never represented as zero, an empty inventory or
"unlimited". An inspection error takes precedence over violations while
preserving them in the report. Strict input validation also matters: review
found that a raw NUL could terminate JSON parsing early and conceal a
stricter suffix. The parser now rejects that input, with regression coverage.

Reviews also exposed weaknesses in the tests themselves. The old launcher
required 1,564 descriptor numbers even when a test needed none, and the
verification script could succeed without running tests from a stale build
cache. The revised launcher stages descriptors when needed; verification
forces its build options and checks test registration. A pipe test now
drains concurrently instead of assuming sufficient pipe capacity. Other
findings required narrower documentation: output may block behind a stalled
reader, and a report write failure can leave an empty or partial file. The
caller needs a timeout; atomic report publication remains future work.

## Reflection

The decision I would defend most is the separation between a failed
requirement and missing evidence. A launcher can provide the wrong state,
but a probe can also fail to obtain trustworthy state. An automated test
must distinguish these cases. This is a more useful basis for the design
than collecting as many process attributes as possible.

The review findings also show why a green test run needs context. Test
selection, inherited limits and instrumentation affect what was actually
checked. I would treat the verification script and launcher fixtures as
part of the engineering problem, with explicit assumptions and observable
failures. The same discipline applies to documentation: a narrower promise
that matches the implementation is more useful than an ambitious guarantee
the code cannot support.

My contribution so far has been directing the project and requesting
critical review. Independent implementation and debugging ability need
separate evidence. My next step is to reproduce the scanner and NUL
regressions, explain their fixes without notes, and make a small change
whose consequences I can demonstrate. If I repeated the project, I would
alternate smaller assisted implementation steps with my own builds and
debugging sessions, so that understanding developed alongside the code.
