"""RLIMIT_NOFILE / RLIMIT_CORE checks.

Limits are changed only inside the disposable launcher process, and only to
values the inherited hard limits permit (lowering a hard limit is allowed
for unprivileged processes; raising it is not).
"""

import resource

import pytest

INF = resource.RLIM_INFINITY


def hard(res):
    return resource.getrlimit(res)[1]


def finite_below_hard(res, wanted):
    h = hard(res)
    if h != INF and h < wanted:
        pytest.skip(f"inherited hard limit {h} < {wanted}")
    return wanted


def test_nofile_soft_and_hard_match(run_probe):
    new_hard = finite_below_hard(resource.RLIMIT_NOFILE, 512)
    r = run_probe(
        {"version": 1, "rlimit_nofile": {"soft": 256, "hard": new_hard}},
        launch={"rlimits": {"nofile": [256, new_hard]}},
    )
    assert r.code == 0, r.stdout
    assert r.check("rlimit_nofile.soft")["observed"] == 256
    assert r.check("rlimit_nofile.hard")["observed"] == 512


def test_nofile_mismatch(run_probe):
    r = run_probe({"version": 1, "rlimit_nofile": {"soft": 512}}, launch={"rlimits": {"nofile": [256, None]}})
    assert r.code == 1
    c = r.check("rlimit_nofile.soft")
    assert (c["status"], c["expected"], c["observed"]) == ("fail", 512, 256)


def test_soft_and_hard_are_independent(run_probe):
    finite_below_hard(resource.RLIMIT_NOFILE, 512)
    r = run_probe(
        {"version": 1, "rlimit_nofile": {"soft": 256, "hard": 1000}},
        launch={"rlimits": {"nofile": [256, 512]}},
    )
    assert r.statuses() == {"rlimit_nofile.soft": "pass", "rlimit_nofile.hard": "fail"}
    assert r.code == 1


def test_core_finite_match_and_mismatch(run_probe):
    finite_below_hard(resource.RLIMIT_CORE, 1 << 20)
    launch = {"rlimits": {"core": [4096, 1 << 20]}}
    ok = run_probe({"version": 1, "rlimit_core": {"soft": 4096, "hard": 1 << 20}}, launch=launch)
    assert ok.code == 0, ok.stdout
    bad = run_probe({"version": 1, "rlimit_core": {"soft": 0, "hard": 1 << 20}}, launch=launch)
    assert bad.statuses() == {"rlimit_core.soft": "fail", "rlimit_core.hard": "pass"}
    assert bad.check("rlimit_core.soft")["observed"] == 4096


def test_core_actual_unlimited(run_probe):
    if hard(resource.RLIMIT_CORE) != INF:
        pytest.skip("inherited hard RLIMIT_CORE is finite; an unlimited soft limit cannot be set")
    r = run_probe(
        {"version": 1, "rlimit_core": {"soft": "unlimited", "hard": "unlimited"}},
        launch={"rlimits": {"core": ["unlimited", None]}},
    )
    assert r.code == 0, r.stdout
    assert r.check("rlimit_core.soft")["observed"] == "unlimited"
    assert r.check("rlimit_core.hard")["observed"] == "unlimited"


def test_unlimited_expected_but_finite_observed(run_probe):
    r = run_probe({"version": 1, "rlimit_core": {"soft": "unlimited"}}, launch={"rlimits": {"core": [0, None]}})
    c = r.check("rlimit_core.soft")
    assert (c["status"], c["expected"], c["observed"]) == ("fail", "unlimited", 0)


def test_finite_expected_but_unlimited_observed(run_probe):
    if hard(resource.RLIMIT_CORE) != INF:
        pytest.skip("inherited hard RLIMIT_CORE is finite")
    r = run_probe({"version": 1, "rlimit_core": {"hard": 0}}, launch={"rlimits": {"core": [0, "unlimited"]}})
    c = r.check("rlimit_core.hard")
    assert (c["status"], c["observed"]) == ("fail", "unlimited")


def test_rlim_infinity_as_number_is_rejected(run_probe):
    r = run_probe({"version": 1, "rlimit_core": {"soft": 2**64 - 1}})
    assert r.code == 2
    assert (r.report["error"]["code"], r.report["error"]["pointer"]) == ("out_of_range", "/rlimit_core/soft")
