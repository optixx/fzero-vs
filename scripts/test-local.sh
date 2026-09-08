#!/bin/sh
# Launch a local F-Zero VS test session from any working directory.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$SCRIPT_DIR/local_test.py" "$@"
