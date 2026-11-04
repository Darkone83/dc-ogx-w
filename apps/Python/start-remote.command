#!/bin/sh
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
exec /usr/bin/env python3 ogx_remote.py "$@"
