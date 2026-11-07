#!/usr/bin/env python3
"""Measure end-to-end wall-clock latency of one probe run (NFR-8).

Method: N sequential runs of the binary with examples/minimal.json via
subprocess.run (stdout/stderr captured), timed with time.perf_counter_ns
around each call. The figure therefore includes fork/exec, dynamic loading
and process teardown, which is what a launcher test pays. Prints median,
p95 and max in milliseconds. No warm-up runs are discarded.

Usage: scripts/measure_latency.py PATH/TO/execcontract [RUNS]
"""

import statistics
import subprocess
import sys
import time
from pathlib import Path


def main():
    binary = Path(sys.argv[1]).resolve()
    runs = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    contract = Path(__file__).resolve().parent.parent / "examples" / "minimal.json"
    samples = []
    for _ in range(runs):
        start = time.perf_counter_ns()
        proc = subprocess.run([str(binary), "--contract", str(contract)], capture_output=True, check=False)
        samples.append((time.perf_counter_ns() - start) / 1e6)
        if proc.returncode not in (0, 1):
            sys.exit(f"unexpected exit status {proc.returncode}: {proc.stderr.decode()}")
    samples.sort()
    p95 = samples[min(len(samples) - 1, int(round(0.95 * len(samples))) - 1)]
    print(f"runs={runs} median_ms={statistics.median(samples):.2f} p95_ms={p95:.2f} max_ms={samples[-1]:.2f}")


if __name__ == "__main__":
    main()
