#!/usr/bin/env bash
# Full local verification: normal build + all tests, sanitizer build + unit
# and compatible component tests, formatting check. Uses only installed
# packages (no network). Usage: scripts/verify.sh [BUILD_ROOT]
#
# Every option that affects what is tested is set explicitly, so a reused
# build directory with stale cache values cannot silently disable tests or
# leave sanitizers on in the "normal" build. The script fails if no tests, or
# not both test kinds, are registered.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd -P)
out=${1:-"$root/build-verify"}
gen=()
command -v ninja >/dev/null 2>&1 && gen=(-G Ninja)

step() { printf '\n==> %s\n' "$*"; }
die() { printf 'verify.sh: %s\n' "$*" >&2; exit 1; }

# Number of tests registered in build dir $1 with label $2.
count_tests() {
    ctest --test-dir "$1" -N -L "$2" | sed -n 's/^Total Tests: *//p'
}

build_and_test() {
    local dir=$1 label=$2
    shift 2
    step "$label: configure + build"
    cmake -S "$root" -B "$dir" "${gen[@]}" -DEXECCONTRACT_BUILD_TESTS=ON -DEXECCONTRACT_WERROR=ON "$@"
    cmake --build "$dir"

    local units components
    units=$(count_tests "$dir" unit)
    components=$(count_tests "$dir" component)
    [ "${units:-0}" -gt 0 ] || die "$label: no unit tests registered"
    [ "${components:-0}" -eq 1 ] || die "$label: component test entry not registered"
    echo "registered: $units unit test(s), $components component entry"

    step "$label: tests"
    ctest --test-dir "$dir" --output-on-failure --no-tests=error
}

step "toolchain and source"
uname -sr
"${CXX:-c++}" --version | head -n1
cmake --version | head -n1
python3 --version
python3 -c 'import pytest; print("pytest", pytest.__version__)'
echo "RLIMIT_NOFILE soft/hard: $(ulimit -Sn)/$(ulimit -Hn)"
if git -C "$root" rev-parse --verify -q HEAD >/dev/null 2>&1; then
    echo "commit: $(git -C "$root" rev-parse --short HEAD)$([ -n "$(git -C "$root" status --porcelain)" ] && echo ' (working tree modified)')"
fi

build_and_test "$out/normal" "normal build (RelWithDebInfo, no sanitizers; exact descriptor assertions included)" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DEXECCONTRACT_SANITIZE=OFF

# The component entry sets its own ASAN_OPTIONS (see CMakeLists.txt).
build_and_test "$out/asan" "sanitizer build (Debug, ASan + UBSan; exact_fds component tests deselected)" \
    -DCMAKE_BUILD_TYPE=Debug -DEXECCONTRACT_SANITIZE=ON

step "formatting"
# clang-format output differs between major versions (the code follows
# clang-format 21). Environments with another version set
# EXECCONTRACT_SKIP_FORMAT=1; the skip is printed, never silent.
if [ "${EXECCONTRACT_SKIP_FORMAT:-0}" = "1" ]; then
    echo "clang-format: NOT RUN (EXECCONTRACT_SKIP_FORMAT=1; found: $(clang-format --version 2>/dev/null | head -n1 || echo none))"
elif command -v clang-format >/dev/null 2>&1; then
    (cd "$root" && clang-format --dry-run --Werror src/*.cpp src/*.hpp tests/unit/*.cpp tests/unit/*.hpp)
    echo "clang-format: clean"
else
    echo "clang-format: NOT RUN (not installed)"
fi

step "verification complete"
