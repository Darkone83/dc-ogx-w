#!/bin/sh
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
if [ ! -x .venv-qt/bin/python ]; then
    python3 -m venv .venv-qt
fi
.venv-qt/bin/python -m pip install --disable-pip-version-check -r requirements.txt
exec .venv-qt/bin/python ogx_remote.py "$@"
