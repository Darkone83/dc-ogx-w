#!/bin/sh
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PYTHON=${PYTHON:-python3}
exec "$PYTHON" ogx_remote.py "$@"
