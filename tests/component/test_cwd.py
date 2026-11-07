"""Working-directory checks: physical path, exact comparison, failures."""

import os

import pytest


def cwd_contract(path):
    return {"version": 1, "cwd": str(path)}


def test_matching_directory_with_spaces(run_probe, tmp_path):
    d = tmp_path / "service dir"
    d.mkdir()
    expected = os.path.realpath(d)
    r = run_probe(cwd_contract(expected), launch={"cwd": str(d)})
    assert r.code == 0, r.stdout
    assert r.check("cwd")["observed"] == expected
    assert r.report["unchecked"] == ["fds", "umask", "rlimit_nofile", "rlimit_core"]


def test_mismatching_directory(run_probe, tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    a.mkdir()
    b.mkdir()
    r = run_probe(cwd_contract(os.path.realpath(a)), launch={"cwd": str(b)})
    assert r.code == 1
    c = r.check("cwd")
    assert (c["status"], c["reason"]) == ("fail", "mismatch")
    assert c["observed"] == os.path.realpath(b)


@pytest.mark.parametrize("name", ['quote"d', "back\\slash", "tab\there", "café ☃", "new\nline"])
def test_escaped_characters_round_trip(run_probe, tmp_path, name):
    d = tmp_path / name
    d.mkdir()
    expected = os.path.realpath(d)
    r = run_probe(cwd_contract(expected), launch={"cwd": str(d)})
    assert r.code == 0, r.stdout
    assert r.check("cwd")["observed"] == expected


def test_physical_path_through_symlink(run_probe, tmp_path):
    real = tmp_path / "real"
    real.mkdir()
    link = tmp_path / "link"
    link.symlink_to(real)
    launch = {"cwd": str(link), "env": {"PWD": str(link)}}  # a shell would leave PWD as the link

    via_link = run_probe(cwd_contract(link), launch=launch)
    assert via_link.code == 1
    assert via_link.check("cwd")["observed"] == os.path.realpath(real)

    physical = run_probe(cwd_contract(os.path.realpath(real)), launch=launch)
    assert physical.code == 0, physical.stdout


def test_deleted_directory_is_inspection_error(run_probe, tmp_path):
    d = tmp_path / "doomed"
    d.mkdir()
    contract_path = tmp_path / "contract.json"  # absolute, so it resolves after cwd is gone
    r = run_probe(cwd_contract(os.path.realpath(d)), launch={"cwd": str(d), "delete_cwd": True})
    assert contract_path.is_absolute()
    assert r.code == 3
    c = r.check("cwd")
    assert c["status"] == "error"
    assert c["observed"] is None
    assert (c["error"]["code"], c["error"]["errno_name"]) == ("cwd_deleted", "ENOENT")
    assert r.report["outcome"] == "inspection_failed"


def test_non_utf8_directory_is_inspection_error(run_probe, tmp_path):
    raw = os.fsencode(tmp_path) + b"/bad\xffname"
    try:
        os.mkdir(raw)
    except OSError as exc:  # some filesystems enforce UTF-8 names
        pytest.skip(f"filesystem rejects non-UTF-8 names: {exc}")
    r = run_probe(cwd_contract(tmp_path / "bad"), launch={"cwd": os.fsdecode(raw)})
    assert r.code == 3
    c = r.check("cwd")
    assert c["error"]["code"] == "cwd_not_utf8"
    assert c["observed"] is None  # bytes are never replaced or guessed


def test_trailing_slash_in_contract_is_rejected(run_probe, tmp_path):
    r = run_probe(cwd_contract(str(tmp_path) + "/"), launch={"cwd": str(tmp_path)})
    assert r.code == 2
    assert r.report["error"]["code"] == "noncanonical_path"
