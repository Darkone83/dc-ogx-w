"""Dependency-free keyboard/on-screen input model for the PySide6 remote.

Only complete controller snapshots are produced. The UI owns key focus and the
network sender owns timing; nothing in this module touches the adapter.
"""
from __future__ import annotations

from dataclasses import dataclass

DEFAULT_BINDINGS: dict[str, str] = {
    "a": "Space", "b": "E", "x": "Q", "y": "R",
    "white": "Z", "black": "C", "l3": "F", "r3": "G",
    "back": "Backspace", "start": "Return", "guide": "Home",
    "lt": "1", "rt": "2",
    "up": "Up", "down": "Down", "left": "Left", "right": "Right",
    "lx_pos": "D", "lx_neg": "A", "ly_pos": "W", "ly_neg": "S",
    "rx_pos": "L", "rx_neg": "J", "ry_pos": "I", "ry_neg": "K",
}

BUTTON_BITS = {"a":1, "b":2, "x":4, "y":8, "l3":16, "r3":32,
               "back":64, "start":128, "white":256, "black":512,
               "guide":1024}
DPAD_BITS = {"up":1, "down":2, "left":4, "right":8}


def check_bindings(value: object) -> dict[str, str]:
    """Recover valid saved bindings, ignoring malformed or duplicate key values."""
    result = DEFAULT_BINDINGS.copy()
    if isinstance(value, dict):
        for action, name in value.items():
            if action in result and isinstance(name, str) and 1 <= len(name) <= 30:
                # The release hotkeys must always remain available.
                if name not in {"F8", "Escape"} and not any(ord(c) < 32 for c in name):
                    result[action] = name
    seen: set[str] = set()
    for action in result:
        if result[action] in seen:
            result[action] = DEFAULT_BINDINGS[action]
        seen.add(result[action])
    return result


def rebind(bindings: dict[str, str], action: str, key: str) -> dict[str, str]:
    if action not in DEFAULT_BINDINGS or not key or key in {"Escape", "F8"}:
        raise ValueError("Invalid key assignment (Escape and F8 are reserved).")
    result = bindings.copy()
    previous = result[action]
    for other, value in result.items():
        if other != action and value == key:
            result[other] = previous
            break
    result[action] = key
    return result


def keyboard_snapshot(pressed: set[str], bindings: dict[str, str], held: set[str] | None = None) -> dict[str, int]:
    active = {action for action, name in bindings.items() if name in pressed}
    active.update(held or ())
    buttons = sum(bit for action, bit in BUTTON_BITS.items() if action in active)
    dpad = sum(bit for action, bit in DPAD_BITS.items() if action in active)
    if dpad & 3 == 3: dpad &= ~3
    if dpad & 12 == 12: dpad &= ~12
    def axis(pos: str, neg: str) -> int:
        return 32767 * (int(pos in active) - int(neg in active))
    return dict(buttons=buttons, dpad=dpad,
                lx=axis("lx_pos", "lx_neg"), ly=axis("ly_pos", "ly_neg"),
                rx=axis("rx_pos", "rx_neg"), ry=axis("ry_pos", "ry_neg"),
                lt=255 if "lt" in active else 0, rt=255 if "rt" in active else 0)
