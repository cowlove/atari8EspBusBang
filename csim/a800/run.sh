#!/bin/bash
#
set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
make -C "$SCRIPT_DIR" TEST_SEC=-1 hybrid
cd "$SCRIPT_DIR"
ATARI800="$SCRIPT_DIR/build-atari/atari800-csim"
if [[ ${1-} == --headless ]]; then
	shift
	exec xvfb-run -a "$ATARI800" -xl -nopatchall "$@"
fi
exec "$ATARI800" -xl -nopatchall "$@"
