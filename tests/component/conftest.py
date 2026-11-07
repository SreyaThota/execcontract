"""Shared fixtures for component tests.

Every test runs the real compiled probe. Its path comes from the build/test
configuration (CTest sets EXECCONTRACT_BIN); there is no fallback path.
"""

import json
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
LAUNCHER = HERE / "launcher.py"
TIMEOUT = 20  # seconds, per subprocess

# The descriptors a probe started by run_probe() always has: stdin from
# /dev/null, stdout and stderr as pipes to pytest.
STD_FDS = [
    {"fd": 0, "type": "char_device"},
    {"fd": 1, "type": "pipe"},
    {"fd": 2, "type": "pipe"},
]


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "exact_fds: asserts the exact descriptor inventory; only valid for "
        "uninstrumented builds (sanitizer runtimes may hold descriptors)",
    )


@dataclass
class Result:
    code: int
    stdout: str
    stderr: str
    report: dict | None

    def check(self, check_id):
        assert self.report is not None, f"no report; stderr={self.stderr!r}"
        matches = [c for c in self.report["checks"] if c["id"] == check_id]
        assert len(matches) == 1, f"check {check_id!r} not found in {self.check_ids()}"
        return matches[0]

    def check_ids(self):
        return [c["id"] for c in self.report["checks"]]

    def statuses(self):
        return {c["id"]: c["status"] for c in self.report["checks"]}


@pytest.fixture(scope="session")
def binary() -> Path:
    value = os.environ.get("EXECCONTRACT_BIN")
    if not value:
        pytest.fail("EXECCONTRACT_BIN is not set; run via ctest or set it to the built binary")
    path = Path(value).resolve()
    if not (path.is_file() and os.access(path, os.X_OK)):
        pytest.fail(f"EXECCONTRACT_BIN={value!r} is not an executable file")
    return path


@pytest.fixture(scope="session")
def source_dir() -> Path:
    value = os.environ.get("EXECCONTRACT_SOURCE_DIR")
    return Path(value).resolve() if value else HERE.parent.parent


def parse_report(text):
    # --help/--version print plain text; anything that starts like a report
    # must parse completely.
    if not text.startswith("{"):
        return None
    return json.loads(text)


def write_contract(directory: Path, contract, name="contract.json") -> Path:
    path = directory / name
    if isinstance(contract, (dict, list)):
        path.write_text(json.dumps(contract), encoding="utf-8")
    elif isinstance(contract, bytes):
        path.write_bytes(contract)
    else:
        path.write_text(contract, encoding="utf-8")
    return path


@pytest.fixture
def run_probe(binary, tmp_path):
    """Run the probe through the isolated launcher.

    contract: dict/str/bytes written to a file, or a Path used as-is.
    launch:   launcher spec fields (fds, close, cwd, umask, rlimits, ...).
    args:     replaces the default ["--contract", <path>] arguments.
    """

    def run(contract=None, *, launch=None, args=None, extra_args=(), stdin=subprocess.DEVNULL):
        if args is None:
            path = contract if isinstance(contract, Path) else write_contract(tmp_path, contract)
            args = ["--contract", str(path), *extra_args]
        spec = dict(launch or {})
        spec["argv"] = [str(binary), *args]
        proc = subprocess.run(
            [sys.executable, "-I", str(LAUNCHER), json.dumps(spec)],
            stdin=stdin,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=TIMEOUT,
            close_fds=True,
            check=False,
        )
        stdout = proc.stdout.decode("utf-8")
        stderr = proc.stderr.decode("utf-8", errors="replace")
        assert proc.returncode != 97, f"launcher setup failed: {stderr}"
        report = parse_report(stdout)
        if report is not None:
            assert report["exit_code"] == proc.returncode, "report exit_code disagrees with exit status"
        return Result(proc.returncode, stdout, stderr, report)

    return run
