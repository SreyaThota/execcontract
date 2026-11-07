"""Report format, aggregate outcomes and output-channel failures."""

import json
import resource

import pytest

from conftest import STD_FDS

TOP_KEYS = ["report_version", "tool", "outcome", "exit_code", "summary", "unchecked", "checks", "error"]
CHECK_KEYS = ["id", "status", "expected", "observed", "reason", "diagnostic", "error"]


def full_contract(cwd):
    nofile_soft, nofile_hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    as_json = lambda v: "unlimited" if v == resource.RLIM_INFINITY else v  # noqa: E731
    return {
        "version": 1,
        "fds": {"required": STD_FDS},
        "cwd": cwd,
        "umask": "0022",
        "rlimit_nofile": {"soft": as_json(nofile_soft), "hard": as_json(nofile_hard)},
        "rlimit_core": {"soft": 0},
    }


@pytest.mark.exact_fds
def test_report_schema_and_check_order(run_probe, tmp_path):
    cwd = str(tmp_path.resolve())
    r = run_probe(full_contract(cwd), launch={"cwd": cwd, "umask": 0o022, "rlimits": {"core": [0, None]}})
    assert r.code == 0, r.stdout
    assert list(r.report) == TOP_KEYS
    assert r.report["report_version"] == 1
    assert r.report["tool"]["name"] == "execcontract"
    assert r.report["summary"] == {"checks": 9, "passed": 9, "failed": 0, "errors": 0}
    assert r.report["unchecked"] == []
    assert r.report["error"] is None
    assert r.check_ids() == [
        "fd.inventory", "fd.0", "fd.1", "fd.2", "cwd", "umask",
        "rlimit_nofile.soft", "rlimit_nofile.hard", "rlimit_core.soft",
    ]
    for c in r.report["checks"]:
        assert list(c) == CHECK_KEYS
        assert c["status"] == "pass"
        assert c["error"] is None and c["reason"] is None


@pytest.mark.exact_fds
def test_deterministic_output(run_probe, tmp_path):
    cwd = str(tmp_path.resolve())
    launch = {"cwd": cwd, "umask": 0o022, "rlimits": {"core": [0, None]}}
    first = run_probe(full_contract(cwd), launch=launch)
    second = run_probe(full_contract(cwd), launch=launch)
    assert first.stdout == second.stdout


def test_mixed_violation_and_inspection_error(run_probe, tmp_path):
    d = tmp_path / "gone"
    d.mkdir()
    r = run_probe(
        {"version": 1, "cwd": "/somewhere", "umask": "0077"},
        launch={"cwd": str(d), "delete_cwd": True, "umask": 0o022},
    )
    assert r.code == 3
    assert r.report["outcome"] == "inspection_failed"
    assert r.statuses() == {"cwd": "error", "umask": "fail"}  # the violation is retained
    assert r.report["summary"] == {"checks": 2, "passed": 0, "failed": 1, "errors": 1}


def test_stdout_closed_never_succeeds(run_probe):
    r = run_probe({"version": 1, "umask": "0022"}, launch={"umask": 0o022, "close": [1]})
    assert r.code == 3
    assert "report delivery failed" in r.stderr


def test_stdout_full_never_succeeds(run_probe):
    r = run_probe({"version": 1, "umask": "0022"}, launch={"umask": 0o022, "fds": [{"fd": 1, "kind": "devfull"}]})
    assert r.code == 3
    assert "report delivery failed" in r.stderr


def test_broken_pipe_exits_3_not_sigpipe(run_probe):
    r = run_probe({"version": 1, "umask": "0022"}, launch={"umask": 0o022, "fds": [{"fd": 1, "kind": "broken_pipe"}]})
    assert r.code == 3  # not -SIGPIPE: the probe handles EPIPE itself
    assert "report delivery failed" in r.stderr


def test_report_file_when_stdout_is_dev_null(run_probe, tmp_path):
    out = tmp_path / "report.json"
    r = run_probe(
        {"version": 1, "fds": {"required": [STD_FDS[0], {"fd": 1, "type": "char_device"}, STD_FDS[2]]}},
        launch={"fds": [{"fd": 1, "kind": "devnull"}]},
        extra_args=["--report", str(out)],
    )
    assert r.code == 0, r.stderr
    report = json.loads(out.read_text())
    assert report["outcome"] == "passed"
    # The report file is opened after the inventory, so it is not "unexpected".
    assert [c["id"] for c in report["checks"]] == ["fd.inventory", "fd.0", "fd.1", "fd.2"]


@pytest.mark.parametrize("target", ["missing-dir/report.json", "/dev/full"])
def test_report_file_failure_never_succeeds(run_probe, tmp_path, target):
    path = target if target.startswith("/") else str(tmp_path / target)
    r = run_probe({"version": 1, "umask": "0022"}, launch={"umask": 0o022}, extra_args=["--report", path])
    assert r.code == 3
    assert r.stdout == ""
