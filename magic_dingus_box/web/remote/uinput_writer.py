"""Writes evdev events to a /dev/uinput-backed virtual gamepad.

The kiosk's InputManager reads evdev devices and maps button codes to
high-level InputAction values. This writer emits the codes that
mapping table already understands — see input_manager.cpp."""
from __future__ import annotations

import enum
from dataclasses import dataclass
from typing import Optional, Protocol

# evdev type constants
EV_KEY = 1
EV_ABS = 3
EV_SYN = 0
SYN_REPORT = 0

# Button codes (matches Linux input-event-codes.h)
BTN_SOUTH = 0x130
BTN_EAST  = 0x131
BTN_NORTH = 0x133
BTN_WEST  = 0x134
BTN_TL    = 0x136
BTN_TR    = 0x137
BTN_START = 0x13B
KEY_Z     = 44       # KEY_Z for RetroArch hotkey

# Axis codes
ABS_HAT0X = 0x10
ABS_HAT0Y = 0x11


class ButtonName(str, enum.Enum):
    OK         = "OK"
    UP         = "UP"
    DOWN       = "DOWN"
    LEFT       = "LEFT"
    RIGHT      = "RIGHT"
    YELLOW     = "YELLOW"
    RED        = "RED"
    GREEN      = "GREEN"
    BLACK      = "BLACK"
    QUIT_GAME  = "QUIT_GAME"


@dataclass(frozen=True)
class _AxisEvent:
    code: int   # ABS_HAT0X / ABS_HAT0Y
    value: int  # -1, 0, +1


@dataclass(frozen=True)
class _KeyEvent:
    code: int


# Mapping: ButtonName → either a single key code (for buttons) or an axis event (for D-pad).
_MAP: dict[ButtonName, _KeyEvent | _AxisEvent] = {
    ButtonName.OK:        _KeyEvent(BTN_SOUTH),
    ButtonName.UP:        _AxisEvent(ABS_HAT0Y, -1),
    ButtonName.DOWN:      _AxisEvent(ABS_HAT0Y,  1),
    ButtonName.LEFT:      _AxisEvent(ABS_HAT0X, -1),
    ButtonName.RIGHT:     _AxisEvent(ABS_HAT0X,  1),
    ButtonName.YELLOW:    _KeyEvent(BTN_TL),
    ButtonName.RED:       _KeyEvent(BTN_EAST),
    ButtonName.GREEN:     _KeyEvent(BTN_TR),
    ButtonName.BLACK:     _KeyEvent(BTN_NORTH),
    # QUIT_GAME emits Z + Start as the kiosk's existing hotkey for "quit RetroArch".
    # Implemented specially in press() below.
}


class _DeviceProto(Protocol):
    def write(self, type_: int, code: int, value: int) -> None: ...
    def syn(self) -> None: ...


class UinputWriter:
    """Writes events to a uinput-backed device. Pass `device=None` to open
    the real /dev/uinput; pass a fake for tests."""

    def __init__(self, device: Optional[_DeviceProto] = None):
        if device is None:
            device = self._open_real_device()
        self._dev = device

    @staticmethod
    def _open_real_device() -> _DeviceProto:
        # Imported lazily so unit tests don't need uinput available.
        from evdev import UInput, ecodes as e

        capabilities = {
            e.EV_KEY: [BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
                       BTN_TL, BTN_TR, BTN_START, KEY_Z],
            e.EV_ABS: [
                (ABS_HAT0X, e.AbsInfo(value=0, min=-1, max=1, fuzz=0, flat=0, resolution=0)),
                (ABS_HAT0Y, e.AbsInfo(value=0, min=-1, max=1, fuzz=0, flat=0, resolution=0)),
            ],
        }
        return UInput(capabilities, name="MagicDingus Phone Remote", phys="flask-remote/0")

    def press(self, btn, phase: str = "tap") -> None:
        """Phase: 'down', 'up', or 'tap' (down+up)."""
        if isinstance(btn, str) and not isinstance(btn, ButtonName):
            try:
                btn = ButtonName(btn)
            except ValueError:
                raise ValueError(f"unknown button: {btn!r}")

        if btn == ButtonName.QUIT_GAME:
            # Z + Start chord — RetroArch's exit-core hotkey.
            self._emit_key_phase(KEY_Z, phase)
            self._emit_key_phase(BTN_START, phase)
            return

        ev = _MAP[btn]
        if isinstance(ev, _KeyEvent):
            self._emit_key_phase(ev.code, phase)
        else:  # _AxisEvent
            self._emit_axis_phase(ev.code, ev.value, phase)

    def _emit_key_phase(self, code: int, phase: str) -> None:
        if phase in ("down", "tap"):
            self._dev.write(EV_KEY, code, 1); self._dev.syn()
        if phase in ("up", "tap"):
            self._dev.write(EV_KEY, code, 0); self._dev.syn()

    def _emit_axis_phase(self, code: int, value: int, phase: str) -> None:
        if phase in ("down", "tap"):
            self._dev.write(EV_ABS, code, value); self._dev.syn()
        if phase in ("up", "tap"):
            self._dev.write(EV_ABS, code, 0); self._dev.syn()
