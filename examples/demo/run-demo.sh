#!/bin/sh
# Run one contract under the defective and the corrected launcher.
#
#   examples/demo/run-demo.sh path/to/execcontract
#
# The caller deliberately has a "dirty" context (leaked fd 3, umask 0022,
# working directory = this demo directory), as a sloppy supervisor or test
# harness might. Expected result: defective -> exit 1, corrected -> exit 0.
set -u

here=$(cd "$(dirname "$0")" && pwd -P)
bin=${1:-${EXECCONTRACT_BIN:-}}
if [ -z "$bin" ] || [ ! -x "$bin" ]; then
    echo "usage: $0 PATH/TO/execcontract   (or set EXECCONTRACT_BIN)" >&2
    exit 2
fi
# The launchers run from this directory, so a relative path must be made
# absolute first.
case $bin in
    /*) ;;
    *) bin="$(pwd -P)/$bin" ;;
esac
contract="$here/contract.json"

run() {
    launcher=$1
    (
        cd "$here" || exit 97
        umask 0022
        exec 3</dev/null   # the leaked descriptor
        exec sh "$here/$launcher" "$bin" --contract "$contract"
    )
}

echo "== $here/launch-broken.sh"
run launch-broken.sh
broken=$?
echo "== exit status $broken (expected 1: violated)"
echo
echo "== $here/launch-fixed.sh"
run launch-fixed.sh
fixed=$?
echo "== exit status $fixed (expected 0: passed)"

[ "$broken" -eq 1 ] && [ "$fixed" -eq 0 ]
