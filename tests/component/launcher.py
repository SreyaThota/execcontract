"""Isolated launcher: set up an execution context, then exec the probe.

Runs as its own short-lived, single-threaded Python process (started with
``python -I``), so pytest's own process state (descriptors, umask, limits,
working directory) is never modified and no ``preexec_fn`` is needed.

Usage: python -I launcher.py SPEC_JSON

The spec is a JSON object:
    argv        list[str]   program and arguments (absolute program path)
    fds         list[obj]   {"fd": N, "kind": K, ...} descriptors to install
                            kinds: devnull, devfull, file(path, mode r|w),
                            dir(path), pipe_read, pipe_write, broken_pipe,
                            socket
    fill_to     int         open /dev/null on every free number 3..fill_to-1
    unlink      list[str]   paths to unlink after descriptors are installed
    close       list[int]   descriptor numbers to close last
    cwd         str         chdir target
    delete_cwd  bool        rmdir the cwd after entering it
    umask       int         umask to set
    env         obj         environment variables to set (e.g. a stale PWD)
    rlimits     obj         {"nofile"|"core": [soft, hard]}; values are int,
                            "unlimited" or null (= keep current)

Python creates descriptors non-inheritable (PEP 446); only the descriptors
installed here with ``inheritable=True`` and 0-2 survive ``execv``. Exit 97
means the spec could not be applied; the probe never ran.
"""

import json
import os
import resource
import signal
import socket
import sys

STAGE_BASE = 1500
RESOURCES = {"nofile": resource.RLIMIT_NOFILE, "core": resource.RLIMIT_CORE}


def die(msg):
    try:
        os.write(2, f"launcher: {msg}\n".encode())
    except OSError:
        pass
    os._exit(97)


def to_rlim(value, current):
    if value is None:
        return current
    if value == "unlimited":
        return resource.RLIM_INFINITY
    return int(value)


def make_fixture(spec, keep):
    kind = spec["kind"]
    if kind == "devnull":
        return os.open(os.devnull, os.O_RDWR)
    if kind == "devfull":
        return os.open("/dev/full", os.O_WRONLY)
    if kind == "file":
        flags = os.O_RDONLY if spec.get("mode", "r") == "r" else os.O_WRONLY | os.O_CREAT
        return os.open(spec["path"], flags, 0o600)
    if kind == "dir":
        return os.open(spec["path"], os.O_RDONLY | os.O_DIRECTORY)
    if kind in ("pipe_read", "pipe_write", "broken_pipe"):
        r, w = os.pipe()
        if kind == "pipe_read":
            keep.append(w)  # keep the other end open until exec (CLOEXEC)
            return r
        if kind == "pipe_write":
            keep.append(r)
            return w
        os.close(r)  # no reader left: writes fail with EPIPE
        return w
    if kind == "socket":
        a, b = socket.socketpair()
        keep.append(b.detach())
        return a.detach()
    die(f"unknown fixture kind {kind!r}")


def main():
    spec = json.loads(sys.argv[1])
    argv = spec["argv"]

    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    original_nofile = (soft, hard)
    wanted_stage = STAGE_BASE + 64
    if soft != resource.RLIM_INFINITY and soft < wanted_stage:
        if hard != resource.RLIM_INFINITY and hard < wanted_stage:
            die("hard RLIMIT_NOFILE too low for staging")
        resource.setrlimit(resource.RLIMIT_NOFILE, (wanted_stage, hard))

    # Create fixtures and move them to high, close-on-exec staging numbers so
    # installing one target can never clobber another fixture's source.
    keep = []
    staged = []
    for i, fd_spec in enumerate(spec.get("fds", [])):
        src = make_fixture(fd_spec, keep)
        hi = os.dup2(src, STAGE_BASE + i, inheritable=False)
        os.close(src)
        staged.append((fd_spec["fd"], hi))
    for target, hi in staged:
        os.dup2(hi, target, inheritable=True)
    targets = {t for t, _ in staged}

    fill_to = spec.get("fill_to")
    if fill_to:
        null = os.open(os.devnull, os.O_RDONLY)
        null_hi = os.dup2(null, STAGE_BASE + 63, inheritable=False)
        os.close(null)
        for n in range(3, fill_to):
            if n not in targets:
                os.dup2(null_hi, n, inheritable=True)
        os.close(null_hi)

    for _, hi in staged:
        os.close(hi)
    for path in spec.get("unlink", []):
        os.unlink(path)
    for n in spec.get("close", []):
        try:
            os.close(n)
        except OSError:
            pass

    if "cwd" in spec:
        os.chdir(spec["cwd"])
        if spec.get("delete_cwd"):
            os.rmdir(spec["cwd"])
    if "umask" in spec:
        os.umask(spec["umask"])
    for key, value in spec.get("env", {}).items():
        os.environ[key] = value

    resource.setrlimit(resource.RLIMIT_NOFILE, original_nofile)
    for name, (want_soft, want_hard) in spec.get("rlimits", {}).items():
        res = RESOURCES[name]
        cur_soft, cur_hard = resource.getrlimit(res)
        new_hard = to_rlim(want_hard, cur_hard)
        new_soft = to_rlim(want_soft, cur_soft)
        resource.setrlimit(res, (new_soft, new_hard))

    # Python ignores SIGPIPE, and SIG_IGN survives exec. Restore the default
    # a normal launcher would pass on, so the probe's own handling is tested.
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    os.execv(argv[0], argv)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:  # noqa: BLE001 - any setup failure is fatal
        die(f"{type(exc).__name__}: {exc}")
