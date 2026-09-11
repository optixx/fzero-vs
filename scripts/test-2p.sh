#!/bin/sh
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec sh "$SCRIPT_DIR/test-local.sh" --players 2 \
  --ares-debug-socket /tmp/fzero-vs-ares-debug.sock "$@"
