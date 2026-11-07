#!/usr/bin/env bash
# Full local verification: normal build + all tests, sanitizer build + unit
# and compatible component tests, formatting check. Uses only installed
# packages (no network). Usage: scripts/verify.sh [BUILD_ROOT]
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd -P)
out=${1:-"$root/build-verify"}
gen=()
command -v ninja >/dev/null 2>&1 && gen=(-G Ninja)

step() { printf '\n==> %s\n' "$*"; }

step "toolchain"
uname -sr
"${CXX:-c++}" --version | head -n1
cmake --version | head -n1
python3 --version
python3 -c 'import pytest; print("pytest", pytest.__version__)'

step "normal build (RelWithDebInfo, -Werror)"
cmake -S "$root" -B "$out/normal" "${gen[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DEXECCONTRACT_WERROR=ON
cmake --build "$out/normal"
step "normal tests: C++ unit + Python component (exact descriptor assertions included)"
ctest --test-dir "$out/normal" --output-on-failure

step "sanitizer build (ASan + UBSan, Debug)"
cmake -S "$root" -B "$out/asan" "${gen[@]}" -DCMAKE_BUILD_TYPE=Debug -DEXECCONTRACT_SANITIZE=ON -DEXECCONTRACT_WERROR=ON
cmake --build "$out/asan"
step "sanitizer tests (component tests marked exact_fds are deselected)"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    ctest --test-dir "$out/asan" --output-on-failure

step "formatting"
if command -v clang-format >/dev/null 2>&1; then
    (cd "$root" && clang-format --dry-run --Werror src/*.cpp src/*.hpp tests/unit/*.cpp tests/unit/*.hpp)
    echo "clang-format: clean"
else
    echo "clang-format: NOT RUN (not installed)"
fi

step "verification complete"
