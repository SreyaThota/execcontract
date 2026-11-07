"""The shipped demonstration: same contract, defective vs corrected launcher."""

import json
import resource
import subprocess

import pytest

TIMEOUT = 20


@pytest.fixture
def demo(source_dir):
    d = source_dir / "examples" / "demo"
    assert (d / "contract.json").is_file()
    _, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if hard != resource.RLIM_INFINITY and hard < 1024:
        pytest.skip(f"the corrected launcher sets a soft RLIMIT_NOFILE of 1024; hard limit is {hard}")
    return d


def launch(demo, launcher, binary):
    # Same dirty caller state for both launchers: leaked fd 3, umask 0022,
    # cwd = demo directory. Built in /bin/sh so pytest's process is untouched.
    script = 'cd "$1" && umask 0022 && exec 3</dev/null && exec sh "$1/$2" "$3" --contract "$1/contract.json"'
    proc = subprocess.run(
        ["/bin/sh", "-c", script, "sh", str(demo), launcher, str(binary)],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=TIMEOUT,
        check=False,
    )
    return proc.returncode, json.loads(proc.stdout)


def test_defective_launcher_violates_contract(demo, binary):
    code, report = launch(demo, "launch-broken.sh", binary)
    assert code == 1
    failed = {c["id"] for c in report["checks"] if c["status"] == "fail"}
    assert {"fd.unexpected.3", "cwd", "umask"} <= failed
    assert not [c for c in report["checks"] if c["status"] == "error"]


def test_corrected_launcher_satisfies_contract(demo, binary):
    code, report = launch(demo, "launch-fixed.sh", binary)
    assert code == 0, json.dumps(report, indent=2)
    assert report["outcome"] == "passed"


@pytest.mark.parametrize("relative", [False, True], ids=["absolute-binary", "relative-binary"])
def test_run_demo_script(demo, binary, relative):
    # The README runs the demo with a relative path (build/execcontract);
    # the script changes directory, so both forms must work.
    arg = f"./{binary.name}" if relative else str(binary)
    proc = subprocess.run(
        ["/bin/sh", str(demo / "run-demo.sh"), arg],
        cwd=binary.parent,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=TIMEOUT,
        check=False,
    )
    assert proc.returncode == 0, proc.stdout.decode() + proc.stderr.decode()
