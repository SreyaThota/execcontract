# Demonstration: one contract, a defective and a corrected launcher

Real output of `examples/demo/run-demo.sh build/execcontract`, captured on
2026-11-08 from a fresh clone at commit `8957220` (WSL2 Ubuntu 26.04, GCC 15)
by the AI coding agent. Two edits only: the clone path is shown as
`~/execcontract`, and descriptor 7 — which `wsl.exe` leaks into every process
it starts — was closed before the run, so the output matches an ordinary
Linux shell (without that, the defective run also reports `fd.unexpected.7`).
stdout and stderr were redirected to files for the capture, which is why fds
1 and 2 are classified as `regular`; in a terminal they would be
`char_device`. The contract does not constrain their type.

The contract ([examples/demo/contract.json](../examples/demo/contract.json))
requires descriptors 0–2 only, working directory `/`, umask `0027`, a soft
`RLIMIT_NOFILE` of 1024 and a soft `RLIMIT_CORE` of 0. The caller
deliberately has a dirty context: a leaked fd 3, umask `0022`, and the demo
directory as working directory.

## Defective launcher → exit 1 (`violated`)

[launch-broken.sh](../examples/demo/launch-broken.sh) just `exec`s the service, passing on whatever it inherited.

| Check | Status | Reason | Expected | Observed |
|---|---|---|---|---|
| `fd.inventory` | pass |  | `complete inventory` | `{"complete": true, "count": 4, "scanner_fd": 4}` |
| `fd.0` | pass |  | `{"presence": "required", "type": null}` | `{"present": true, "type": "char_device"}` |
| `fd.1` | pass |  | `{"presence": "required", "type": null}` | `{"present": true, "type": "regular"}` |
| `fd.2` | pass |  | `{"presence": "required", "type": null}` | `{"present": true, "type": "regular"}` |
| `fd.unexpected.3` | fail | unexpected | `{"presence": "forbidden", "type": null}` | `{"present": true, "type": "char_device"}` |
| `cwd` | fail | mismatch | `/` | `~/execcontract/examples/demo` |
| `umask` | fail | mismatch | `0027` | `0022` |
| `rlimit_nofile.soft` | fail | mismatch | `1024` | `10240` |
| `rlimit_core.soft` | pass |  | `0` | `0` |

Summary: `{"checks": 9, "passed": 5, "failed": 4, "errors": 0}`, exit code 1.

## Corrected launcher → exit 0 (`passed`)

[launch-fixed.sh](../examples/demo/launch-fixed.sh) closes descriptors 3–9, runs `cd /`, `umask 0027`, `ulimit -S -n 1024` and `ulimit -S -c 0`, then `exec`s the service.

| Check | Status | Observed |
|---|---|---|
| `fd.inventory` | pass | `{"complete": true, "count": 3, "scanner_fd": 3}` |
| `fd.0` | pass | `{"present": true, "type": "char_device"}` |
| `fd.1` | pass | `{"present": true, "type": "regular"}` |
| `fd.2` | pass | `{"present": true, "type": "regular"}` |
| `cwd` | pass | `/` |
| `umask` | pass | `0027` |
| `rlimit_nofile.soft` | pass | `1024` |
| `rlimit_core.soft` | pass | `0` |

Summary: `{"checks": 8, "passed": 8, "failed": 0, "errors": 0}`, exit code 0.

## Terminal output

Script banners and the one-line summaries ExecContract writes to stderr:

```text
== ~/execcontract/examples/demo/launch-broken.sh
== exit status 1 (expected 1: violated)
== ~/execcontract/examples/demo/launch-fixed.sh
== exit status 0 (expected 0: passed)
execcontract: violated (9 checks: 5 passed, 4 failed, 0 errors)
execcontract: passed (8 checks: 8 passed, 0 failed, 0 errors)
```

## Full report from the defective run

```json
{
  "report_version": 1,
  "tool": {
    "name": "execcontract",
    "version": "0.1.0"
  },
  "outcome": "violated",
  "exit_code": 1,
  "summary": {
    "checks": 9,
    "passed": 5,
    "failed": 4,
    "errors": 0
  },
  "unchecked": [],
  "checks": [
    {
      "id": "fd.inventory",
      "status": "pass",
      "expected": "complete inventory",
      "observed": {
        "complete": true,
        "count": 4,
        "scanner_fd": 4
      },
      "reason": null,
      "diagnostic": "enumerated 4 descriptor(s) from /proc/self/fd",
      "error": null
    },
    {
      "id": "fd.0",
      "status": "pass",
      "expected": {
        "presence": "required",
        "type": null
      },
      "observed": {
        "present": true,
        "type": "char_device"
      },
      "reason": null,
      "diagnostic": "descriptor 0 is open",
      "error": null
    },
    {
      "id": "fd.1",
      "status": "pass",
      "expected": {
        "presence": "required",
        "type": null
      },
      "observed": {
        "present": true,
        "type": "regular"
      },
      "reason": null,
      "diagnostic": "descriptor 1 is open",
      "error": null
    },
    {
      "id": "fd.2",
      "status": "pass",
      "expected": {
        "presence": "required",
        "type": null
      },
      "observed": {
        "present": true,
        "type": "regular"
      },
      "reason": null,
      "diagnostic": "descriptor 2 is open",
      "error": null
    },
    {
      "id": "fd.unexpected.3",
      "status": "fail",
      "expected": {
        "presence": "forbidden",
        "type": null
      },
      "observed": {
        "present": true,
        "type": "char_device"
      },
      "reason": "unexpected",
      "diagnostic": "descriptor 3 is open but not permitted by the contract",
      "error": null
    },
    {
      "id": "cwd",
      "status": "fail",
      "expected": "/",
      "observed": "~/execcontract/examples/demo",
      "reason": "mismatch",
      "diagnostic": "working directory differs",
      "error": null
    },
    {
      "id": "umask",
      "status": "fail",
      "expected": "0027",
      "observed": "0022",
      "reason": "mismatch",
      "diagnostic": "umask is 0022, expected 0027",
      "error": null
    },
    {
      "id": "rlimit_nofile.soft",
      "status": "fail",
      "expected": 1024,
      "observed": 10240,
      "reason": "mismatch",
      "diagnostic": "rlimit_nofile soft limit differs",
      "error": null
    },
    {
      "id": "rlimit_core.soft",
      "status": "pass",
      "expected": 0,
      "observed": 0,
      "reason": null,
      "diagnostic": "rlimit_core soft limit matches",
      "error": null
    }
  ],
  "error": null
}
```
