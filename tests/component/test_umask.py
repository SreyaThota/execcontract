"""Umask checks: the launcher sets the mask, the probe only reads it."""

import pytest


@pytest.mark.parametrize("mask,text", [(0o027, "0027"), (0o000, "0000"), (0o777, "0777"), (0o022, "0022")])
def test_matching_umask(run_probe, mask, text):
    r = run_probe({"version": 1, "umask": text}, launch={"umask": mask})
    assert r.code == 0, r.stdout
    c = r.check("umask")
    assert c["expected"] == text
    assert c["observed"] == text


def test_mismatching_umask(run_probe):
    r = run_probe({"version": 1, "umask": "0077"}, launch={"umask": 0o022})
    assert r.code == 1
    c = r.check("umask")
    assert (c["status"], c["reason"]) == ("fail", "mismatch")
    assert (c["expected"], c["observed"]) == ("0077", "0022")


def test_numeric_umask_is_rejected(run_probe):
    r = run_probe({"version": 1, "umask": 18})  # 18 decimal == 022 octal: ambiguous
    assert r.code == 2
    assert r.report["error"]["code"] == "invalid_type"
