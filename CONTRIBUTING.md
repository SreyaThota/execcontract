# Contributing to ExecContract

## Development environment

Linux is required (procfs, `getrlimit`, Linux `getcwd` semantics); on Windows
use WSL2. Ubuntu/Debian packages:

```bash
sudo apt install -y build-essential cmake ninja-build python3-pytest \
    libgtest-dev libgmock-dev nlohmann-json3-dev clang-format clang-tidy
```

No other downloads are needed; the build never accesses the network.

## Build and test

```bash
cmake -S . -B build -G Ninja -DEXECCONTRACT_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure        # unit + component
ctest --test-dir build -L unit                    # C++ unit tests only
ctest --test-dir build -L component -V            # component tests, verbose
```

Run the component tests directly (faster iteration, pytest options):

```bash
EXECCONTRACT_BIN=$PWD/build/execcontract python3 -m pytest tests/component -q
EXECCONTRACT_BIN=$PWD/build/execcontract python3 -m pytest tests/component -k scanner -vv
```

The component tests refuse to run (they fail, not skip) without
`EXECCONTRACT_BIN`.

Before sending a change, run the full verification (normal build, ASan+UBSan
build, formatting):

```bash
scripts/verify.sh            # build trees under ./build-verify
```

`verify.sh` sets every test-relevant CMake option explicitly (so a stale
cache cannot disable tests or leave sanitizers on in the normal build), fails
if no unit tests or no component entry are registered, and runs `ctest` with
`--no-tests=error`. Unit tests have a 30 s per-test CTest timeout and every
component subprocess a 20 s timeout, so a hang fails instead of stalling.

Also run the suite under a restrictive descriptor limit before changing test
fixtures; tests must pass or skip with a stated reason, never fail:

```bash
prlimit --nofile=128:128 -- ctest --test-dir build --output-on-failure
```

CMake options: `EXECCONTRACT_WERROR` (warnings as errors),
`EXECCONTRACT_SANITIZE` (ASan + UBSan for everything; component tests marked
`exact_fds` are deselected and `ASAN_OPTIONS=disable_coredump=0` is set),
`EXECCONTRACT_BUILD_TESTS` (default ON).

## Repository layout

```
src/            implementation (static library execcontract_core + main.cpp)
  sys.*         RAII wrappers, bounded read, checked write
  model.*       value types, limits, UTF-8 validation
  observe.*     collection from Linux
  contract.*    contract loading and v1 schema validation
  evaluate.*    contract × snapshot → checks, outcome, exit code
  report.*      JSON report
  cli.*         argument parsing
  main.cpp      orchestration and report delivery
tests/unit/       GoogleTest unit tests (one file per module)
tests/component/  pytest tests driving the built binary; launcher.py sets
                  up the execution context and execs the probe
examples/         contracts and the failing-and-fixed launcher demo
scripts/          verify.sh, measure_latency.py
docs/             requirements, architecture, decisions, verification, essay
```

Read [docs/architecture.md](docs/architecture.md) before changing
`observe.cpp` or `main.cpp`.

## Rules that protect correctness

1. **Nothing may open a descriptor before `collect_snapshot()`** — not in
   `main()`, not in a static initialiser. New observations that need a
   descriptor must open it after the inventory and close it before returning
   (use `UniqueFd`).
2. **Inherited descriptors are borrowed**: never close, read, seek or change
   their flags. Owned descriptors are `UniqueFd`/`DirStream`; borrowed ones are
   plain `int`.
3. **Never retry `close()`.** Retry `read`/`write` on `EINTR` only.
4. **Unknown is not a value.** A failed observation is a `SysError` with a
   stable `code`; never substitute `0`, an empty list or `"unlimited"`.
   Evaluation must fail closed on a missing value.
5. **Exceeded caps are errors**, never silent truncation.
6. **Stable identifiers** (`id`, `status`, `reason`, `error.code`) are API;
   changing them is a breaking change that needs a new `report_version`.
   Human text (`diagnostic`, `message`) may change freely.
7. **No new inspection area** without a roadmap discussion; the five-area
   scope is deliberate.

## Testing expectations

* Pure logic (validation, evaluation, rendering) gets **unit tests**; anything
  depending on how the process was launched gets a **component test** that
  runs the real binary via `launcher.py`.
* Component tests never change pytest's own process state. Put setup in the
  launcher spec (`fds`, `close`, `cwd`, `delete_cwd`, `umask`, `rlimits`,
  `env`, `unlink`, `fill_to`), keep values within the inherited hard limits,
  and use `tmp_path`.
* Assert on stable fields, not on diagnostic text.
* A test that asserts the exact descriptor inventory must be marked
  `@pytest.mark.exact_fds`.
* Choose limit and descriptor values from the limits the test process
  actually inherited (both soft and hard); never assume a generous developer
  machine. A skip needs a specific reason in the message (e.g. "hard
  RLIMIT_NOFILE < 600").
* Never rely on pipe capacity (`pipe(7)` guarantees none): drain pipes
  concurrently in tests that write more than a few bytes. Do not skip or loosen a failing assertion to make a run green —
  investigate it.
* For a bug fix, first add a test that fails without the fix.

## Conventions

* C++17, no exceptions across module boundaries except inside the contract
  parser (converted to `ContractError` at its API). No global mutable state,
  no threads, no inheritance.
* Formatting: `clang-format` with the repository `.clang-format`
  (`clang-format -i src/*.[ch]pp tests/unit/*.[ch]pp`). `scripts/verify.sh`
  checks it.
* Warnings: project code builds clean with the flags in `CMakeLists.txt`
  (including `-Wconversion -Wsign-conversion`); keep `-DEXECCONTRACT_WERROR=ON`
  clean.
* Static analysis (optional but recommended):
  `clang-tidy -p build -checks='-*,bugprone-*,performance-*,misc-const-correctness,-bugprone-easily-swappable-parameters' src/*.cpp`
  is clean for this release.
* Python: standard library + pytest only.
* Commits: one coherent change per commit, imperative subject line, tests in
  the same commit as the code they cover.

## Review checklist

- [ ] Does any new code open a descriptor before or during the inventory?
- [ ] Can any failure path produce exit 0 or a check status `pass`?
- [ ] Is every new cap enforced with an explicit error, and tested at and
      beyond the cap?
- [ ] Are new stable ids documented in `docs/requirements.md`?
- [ ] Do the tests fail without the change?
