# Roadmap

Release 0.1.0 is deliberately small. This file records what was left out on
purpose, what the environment limits, what could be hardened, and known
defects.

## Deliberate product exclusions

Not planned, because they would turn a focused probe into a different tool:

* Process supervision, restart policies, process-tree cleanup.
* Inspecting *other* processes (`/proc/<pid>`); ExecContract only inspects
  itself.
* Security auditing, capability/seccomp/LSM policy evaluation.
* Namespace or cgroup management.
* A general policy language, contract inheritance or plugins.
* Network services, GUIs, dashboards, AI/ML features.

## Candidate extensions (each needs a scope discussion)

Ordered by expected value for launcher testing:

1. **Environment variables** — required/forbidden names, exact values.
   Common launcher bug class (`LANG`, `PATH`, leaked secrets); needs a careful
   policy for values (secrets must not be echoed in reports).
2. **Signal state** — blocked mask and ignored signals (`SigBlk`/`SigIgn` in
   `/proc/self/status`); e.g. an inherited `SIG_IGN` for `SIGPIPE` or `SIGCHLD`.
   Requires deciding how to account for ExecContract's own SIGPIPE handling
   (it changes disposition only after the snapshot).
3. **Close-on-exec flags** of permitted descriptors (`fcntl(F_GETFD)`) and
   open-file status flags (`O_NONBLOCK`, `O_APPEND`) — read-only queries.
4. **Other resource limits** (`RLIMIT_NPROC`, `RLIMIT_STACK`, `RLIMIT_AS`,
   ...): mechanical extension of the existing limit code.
5. **Credentials** (uid/gid/supplementary groups) and **no_new_privs**.
6. **Open descriptor policy option** (`"others": "allow"`) for contracts that
   only care about specific descriptors.
7. **Socket detail** for `socket` descriptors (family/type via
   `getsockopt(SO_DOMAIN/SO_TYPE)`), without reading data.

## Environmental limitations

* Observation happens at the start of `main()`, after the dynamic loader and
  C++ runtime; earlier state is invisible. A statically linked, `-nostdlib`
  probe could observe earlier but costs portability.
* Requires `/proc` mounted and Linux ≥ 4.7 (`Umask:` field). Hidden or
  restricted procfs yields inspection errors, not passes.
* Very restrictive inherited limits can prevent the probe from starting; no
  report is possible then and callers must treat that as a failure.
* Sanitizer runtimes alter observed state (ASan and `RLIMIT_CORE`, possible
  extra descriptors).
* `RLIMIT_CORE` is only one input to core-dump behaviour (`core_pattern`,
  `PR_SET_DUMPABLE`, systemd-coredump are not inspected).
* Verified on one environment only (WSL2, Ubuntu 26.04, GCC 15). Clang,
  musl, older glibc and non-WSL kernels are unverified.

## Hardening with more time

* The `readdir` failure branch is covered by a unit test through an
  injected `readdir` function, not by a real procfs failure (which cannot be
  produced unprivileged). A FUSE-based fixture in CI would give end-to-end
  evidence.
* `--report` truncates the target in place. Writing to a temporary file in
  the same directory and `rename`-ing it would make the report atomic and
  avoid leaving a partial file on a mid-write failure (the exit status is
  already correct in that case).
* Opening the contract can have side effects for device files (it is opened
  `O_NONBLOCK|O_NOCTTY` and rejected if not regular, but the `open` itself
  happens first). Opening with `O_PATH`, checking, then reopening would avoid
  that.
* CI workflow running `scripts/verify.sh` on several distributions and with
  Clang; a fuzz target for `parse_contract` (libFuzzer).
* JSON consumers in JavaScript lose precision for limits above 2^53; a
  string encoding option for large limits.
* Packaging (`.deb`), a man page.

## Known defects

None known in the declared scope as of 0.1.0. Report issues with the
contract, the full report and the launcher command line.
