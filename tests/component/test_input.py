"""CLI and contract-input errors: exit 2, structured error, no checks."""

import os

import pytest

VALID = {"version": 1, "umask": "0022"}


def assert_invalid(r, code):
    assert r.code == 2, r.stdout
    assert r.report["outcome"] == "invalid_contract"
    assert r.report["checks"] == []
    assert r.report["error"]["code"] == code


def test_missing_contract(run_probe, tmp_path):
    r = run_probe(tmp_path / "does-not-exist.json")
    assert_invalid(r, "contract_not_found")
    assert r.report["error"]["errno_name"] == "ENOENT"


def test_unreadable_contract(run_probe, tmp_path):
    if os.geteuid() == 0:
        pytest.skip("root bypasses permission checks; unreadable file not reproducible")
    p = tmp_path / "secret.json"
    p.write_text('{"version": 1, "umask": "0022"}')
    p.chmod(0)
    try:
        r = run_probe(p)
    finally:
        p.chmod(0o600)
    assert_invalid(r, "contract_unreadable")


def test_directory_and_fifo_contracts(run_probe, tmp_path):
    assert_invalid(run_probe(tmp_path), "contract_not_regular_file")
    fifo = tmp_path / "fifo"
    os.mkfifo(fifo)
    assert_invalid(run_probe(fifo), "contract_not_regular_file")  # returns without blocking


@pytest.mark.parametrize(
    "text,code",
    [
        ("", "malformed_json"),
        ("{", "malformed_json"),
        ('{"version": 1,}', "malformed_json"),
        ('{"version": 1} {}', "malformed_json"),
        ("[1]", "invalid_type"),
    ],
)
def test_malformed_contract(run_probe, text, code):
    assert_invalid(run_probe(text), code)


def test_embedded_nul_does_not_truncate_the_contract(run_probe):
    """Everything after a NUL must not be silently ignored (false pass)."""
    weak = b'{"version": 1, "umask": "0022"}'
    strict = b'{"version": 1, "umask": "0022", "fds": {"required": []}}'
    r = run_probe(weak + b"\0" + strict, launch={"umask": 0o022})
    assert_invalid(r, "malformed_json")


@pytest.mark.parametrize(
    "text",
    ['{"version": 1, "rlimit_core": {"soft": 1e999}}', '{"version": 1e999, "umask": "0022"}'],
)
def test_number_overflow_is_invalid_contract(run_probe, text):
    assert_invalid(run_probe(text), "out_of_range")


@pytest.mark.parametrize(
    "raw", [b'{"version": 1, "cwd": "/\xff"}', b"\xff{}", b'{"\xc3\x28": 1}'], ids=["value", "lead", "key"]
)
def test_invalid_utf8_contract_still_gets_a_report(run_probe, raw):
    r = run_probe(raw)
    assert_invalid(r, "malformed_json")  # not exit 3 / serialization_failed


def test_utf8_bom_is_accepted(run_probe):
    r = run_probe(b'\xef\xbb\xbf{"version": 1, "umask": "0022"}', launch={"umask": 0o022})
    assert r.code == 0, r.stdout


def test_oversized_contract_boundary(run_probe, tmp_path):
    body = '{"version": 1, "umask": "0022"}'
    at_cap = body + " " * (65536 - len(body))
    over = at_cap + " "
    ok = run_probe(at_cap.encode(), launch={"umask": 0o022})
    assert ok.code == 0, ok.stdout
    assert_invalid(run_probe(over.encode()), "contract_too_large")


@pytest.mark.parametrize(
    "contract,code",
    [
        ({"version": 2, "umask": "0022"}, "unsupported_version"),
        ({"umask": "0022"}, "missing_version"),
        ({"version": True, "umask": "0022"}, "invalid_type"),
        ({"version": 1}, "empty_contract"),
        ({"version": 1, "fds": {}}, "empty_area"),
        ({"version": 1, "env": {"PATH": "/bin"}}, "unknown_field"),
        ({"version": 1, "rlimit_nofile": {"soft": 2048, "hard": 1024}}, "contradictory_limits"),
        ({"version": 1, "fds": {"required": [{"fd": 3}], "permitted": [{"fd": 3}]}}, "duplicate_fd"),
        ({"version": 1, "fds": {"required": [{"fd": -1}]}}, "out_of_range"),
        ({"version": 1, "fds": {"required": [{"fd": 1, "type": "tty"}]}}, "invalid_fd_type"),
        ({"version": 1, "cwd": "relative/path"}, "noncanonical_path"),
    ],
)
def test_invalid_contracts(run_probe, contract, code):
    assert_invalid(run_probe(contract), code)


def test_duplicate_key(run_probe):
    assert_invalid(run_probe('{"version": 1, "umask": "0022", "umask": "0077"}'), "duplicate_key")


def test_invalid_contract_takes_precedence_over_inspection_failure(run_probe, tmp_path):
    d = tmp_path / "gone"
    d.mkdir()
    r = run_probe({"version": 1, "cwd": "/x", "bogus": 1}, launch={"cwd": str(d), "delete_cwd": True})
    assert_invalid(r, "unknown_field")


@pytest.mark.parametrize(
    "args",
    [[], ["--contract"], ["--unknown"], ["--contract", "a", "--contract", "b"], ["positional"], ["--report", "r"]],
)
def test_usage_errors(run_probe, args):
    r = run_probe(args=args)
    assert r.code == 2
    assert r.stdout == ""  # usage errors never produce a report
    assert "Usage:" in r.stderr


def test_option_value_named_help_is_a_path(run_probe):
    r = run_probe(args=["--contract", "--help"])
    assert r.code == 2
    assert r.report["error"]["code"] == "contract_not_found"


def test_help_version(run_probe):
    h = run_probe(args=["--help"])
    assert (h.code, h.stdout.startswith("Usage: execcontract")) == (0, True)
    v = run_probe(args=["--version"])
    assert v.code == 0
    assert v.stdout.startswith("execcontract ")
