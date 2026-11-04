"""Automatic, isolated dependency installation for the DC-OGX-W Python tools.

No system-wide package writes and no shell commands. Runs only when the user
explicitly launches the program, never when imported by another application.
"""
from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import venv

ROOT = Path(__file__).resolve().parent
VENV = ROOT / '.venv-ogx'
REQUIREMENTS = {'aiohttp': 'aiohttp>=3.12,<4',
                'PySide6': 'PySide6>=6.8,<7',
                'pygame': 'pygame>=2.6,<3'}


def desktop_mode(argv: list[str]) -> bool:
    # Default invocation opens the desktop application.
    if '--help' in argv or '-h' in argv:
        return False
    return not any(word in argv for word in ('web', 'status', 'tap', 'send', 'run', 'profile', 'desktop-sdl'))


def venv_python() -> Path:
    return VENV / ('Scripts/python.exe' if os.name == 'nt' else 'bin/python')


def ensure_and_exec(argv: list[str] | None = None) -> None:
    argv = list(sys.argv[1:] if argv is None else argv)
    if os.environ.get('OGX_SKIP_BOOTSTRAP') == '1':
        return
    if sys.version_info < (3, 10):
        raise SystemExit('DC-OGX-W requires Python 3.10 or newer.')
    required = ['aiohttp']
    if desktop_mode(argv):
        required += ['PySide6', 'pygame']
    elif 'desktop-sdl' in argv:
        required += ['pygame']
    missing = [p for p in required if importlib.util.find_spec(p) is None]
    if not missing:
        return
    # This is the project's own venv; never install into system Python.
    python = venv_python()
    if not python.is_file():
        print('DC-OGX-W: creating isolated Python environment...')
        venv.EnvBuilder(with_pip=True).create(VENV)
    specs = [REQUIREMENTS[p] for p in required]
    print('DC-OGX-W: installing dependencies:', ', '.join(required))
    try:
        subprocess.run([str(python), '-m', 'pip', 'install', '--disable-pip-version-check', *specs],
                       check=True, cwd=ROOT)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit('Dependency installation failed. Connect to the internet and retry.\n'
                         f'Details: {exc}') from exc
    # Prevent recursive bootstrap on a broken/incompatible wheel installation.
    env = os.environ.copy()
    env['OGX_SKIP_BOOTSTRAP'] = '1'
    os.execve(str(python), [str(python), str(ROOT / 'ogx_remote.py'), *argv], env)
