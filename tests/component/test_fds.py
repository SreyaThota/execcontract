"""Descriptor inventory, policy and type checks against the real binary."""

import resource

import pytest

from conftest import STD_FDS


def contract(required=(), permitted=()):
    return {"version": 1, "fds": {"required": [*STD_FDS, *required], "permitted": list(permitted)}}


@pytest.mark.exact_fds
def test_required_present_passes(run_probe, tmp_path):
    f = tmp_path / "data.txt"
    f.write_text("x")
    r = run_probe(
        contract(required=[{"fd": 3, "type": "regular"}]),
        launch={"fds": [{"fd": 3, "kind": "file", "path": str(f)}]},
    )
    assert r.code == 0, r.stdout
    assert r.report["outcome"] == "passed"
    assert r.check_ids() == ["fd.inventory", "fd.0", "fd.1", "fd.2", "fd.3"]
    assert r.check("fd.3")["observed"] == {"present": True, "type": "regular"}
    assert r.check("fd.inventory")["observed"]["complete"] is True


@pytest.mark.exact_fds
def test_required_missing_fails(run_probe):
    r = run_probe(contract(required=[{"fd": 5}]))
    assert r.code == 1
    c = r.check("fd.5")
    assert (c["status"], c["reason"]) == ("fail", "missing")
    assert c["observed"] == {"present": False, "type": None}


@pytest.mark.exact_fds
def test_permitted_absent_passes(run_probe):
    r = run_probe(contract(permitted=[{"fd": 7, "type": "socket"}]))
    assert r.code == 0, r.stdout
    assert r.check("fd.7")["status"] == "pass"
    assert r.check("fd.7")["observed"] == {"present": False, "type": None}


@pytest.mark.exact_fds
def test_unexpected_descriptor_fails(run_probe):
    r = run_probe(contract(), launch={"fds": [{"fd": 4, "kind": "devnull"}, {"fd": 9, "kind": "socket"}]})
    assert r.code == 1
    assert r.check_ids()[-2:] == ["fd.unexpected.4", "fd.unexpected.9"]
    assert r.check("fd.unexpected.9")["observed"] == {"present": True, "type": "socket"}
    assert r.check("fd.unexpected.9")["reason"] == "unexpected"


@pytest.mark.parametrize(
    "kind,expected_type",
    [
        ("file", "regular"),
        ("dir", "directory"),
        ("pipe_read", "pipe"),
        ("pipe_write", "pipe"),
        ("socket", "socket"),
        ("devnull", "char_device"),
    ],
)
def test_type_matches(run_probe, tmp_path, kind, expected_type):
    f = tmp_path / "f"
    f.write_text("x")
    spec = {"fd": 3, "kind": kind, "path": str(tmp_path if kind == "dir" else f)}
    r = run_probe(
        {"version": 1, "fds": {"permitted": [{"fd": 3, "type": expected_type}]}},
        launch={"fds": [spec]},
    )
    assert r.check("fd.3")["status"] == "pass", r.check("fd.3")
    assert r.check("fd.3")["observed"] == {"present": True, "type": expected_type}


@pytest.mark.parametrize("kind,wrong", [("socket", "pipe"), ("file", "directory"), ("pipe_read", "socket")])
def test_type_mismatch_fails(run_probe, tmp_path, kind, wrong):
    f = tmp_path / "f"
    f.write_text("x")
    r = run_probe(
        {"version": 1, "fds": {"required": [{"fd": 3, "type": wrong}]}},
        launch={"fds": [{"fd": 3, "kind": kind, "path": str(f)}]},
    )
    c = r.check("fd.3")
    assert (c["status"], c["reason"]) == ("fail", "type_mismatch")
    assert c["observed"]["type"] != wrong
    assert r.report["outcome"] == "violated"


def test_deleted_file_descriptor_still_classified(run_probe, tmp_path):
    # The launcher opens the file as fd 3 and unlinks it before exec. The
    # descriptor stays valid; classification must come from fstat on the
    # descriptor, not from the (now dangling) procfs link text.
    f = tmp_path / "gone.txt"
    f.write_text("x")
    r = run_probe(
        {"version": 1, "fds": {"permitted": [{"fd": 3, "type": "regular"}]}},
        launch={"fds": [{"fd": 3, "kind": "file", "path": str(f)}], "unlink": [str(f)]},
    )
    assert not f.exists()
    assert r.check("fd.3")["observed"] == {"present": True, "type": "regular"}


@pytest.mark.exact_fds
def test_scanner_contamination_fd3(run_probe):
    """Required fd 3 is absent; opendir() on /proc/self/fd receives fd 3.

    The scanner must exclude its own stream descriptor and still report fd 3
    as missing, and it must not report its own descriptor as unexpected.
    """
    r = run_probe(contract(required=[{"fd": 3}]), launch={"close": [3]})
    assert r.check("fd.inventory")["observed"] == {"complete": True, "count": 3, "scanner_fd": 3}
    c = r.check("fd.3")
    assert (c["status"], c["reason"]) == ("fail", "missing")
    assert not [i for i in r.check_ids() if i.startswith("fd.unexpected")]
    assert r.code == 1


@pytest.mark.exact_fds
def test_scanner_contamination_fd0(run_probe):
    """Same regression when stdin is closed: the scanner then receives fd 0."""
    r = run_probe(
        {"version": 1, "fds": {"required": [{"fd": 0}, STD_FDS[1], STD_FDS[2]]}},
        launch={"close": [0]},
        stdin=None,
    )
    assert r.check("fd.inventory")["observed"]["scanner_fd"] == 0
    assert (r.check("fd.0")["status"], r.check("fd.0")["reason"]) == ("fail", "missing")
    assert r.code == 1


@pytest.mark.exact_fds
def test_descriptor_above_lowered_soft_limit(run_probe):
    _, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if hard != resource.RLIM_INFINITY and hard < 600:
        pytest.skip(f"hard RLIMIT_NOFILE {hard} < 600")
    r = run_probe(
        {**contract(permitted=[{"fd": 500, "type": "char_device"}]), "rlimit_nofile": {"soft": 64}},
        launch={"fds": [{"fd": 500, "kind": "devnull"}], "rlimits": {"nofile": [64, None]}},
    )
    assert r.code == 0, r.stdout
    assert r.check("fd.500")["observed"] == {"present": True, "type": "char_device"}
    assert r.check("rlimit_nofile.soft")["observed"] == 64


def _cap_contract():
    permitted = [{"fd": n} for n in range(3, 1024)]
    return {"version": 1, "fds": {"required": STD_FDS, "permitted": permitted}}


def _require_nofile(n):
    _, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if hard != resource.RLIM_INFINITY and hard < n:
        pytest.skip(f"hard RLIMIT_NOFILE {hard} < {n}")


@pytest.mark.exact_fds
def test_inventory_at_cap_is_complete(run_probe):
    _require_nofile(2048)
    r = run_probe(_cap_contract(), launch={"fill_to": 1024, "rlimits": {"nofile": [2048, None]}})
    assert r.check("fd.inventory")["observed"]["count"] == 1024
    assert r.code == 0, r.report["summary"]


@pytest.mark.exact_fds
def test_inventory_beyond_cap_is_inspection_error(run_probe):
    _require_nofile(2048)
    r = run_probe(_cap_contract(), launch={"fill_to": 1025, "rlimits": {"nofile": [2048, None]}})
    inv = r.check("fd.inventory")
    assert inv["status"] == "error"
    assert inv["error"]["code"] == "inventory_capped"
    assert inv["observed"]["complete"] is False
    assert r.report["outcome"] == "inspection_failed"
    assert r.code == 3
