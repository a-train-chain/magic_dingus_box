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

# Button codes — selected to match what InputManager::map_button_to_action
# in magic_dingus_box_cpp/src/platform/input_manager.cpp already maps.
# Picking codes the kiosk doesn't recognize means presses route nowhere.
#
# Linux input-event-codes.h reference:
#   BTN_SOUTH = 0x130 = 304   (kiosk: SELECT)
#   BTN_EAST  = 0x131 = 305   (kiosk: SETTINGS_MENU)
#   BTN_X     = 0x133 = 307   (NB: this differs from BTN_NORTH on some kernels)
#   BTN_Y     = 0x134 = 308   (kiosk: PREV)
#   BTN_C     = 0x132 = 306   (kiosk: SELECT, alt)
#   ID 309    = BTN_TL2/Z     (kiosk: NEXT)
#   ID 310    = BTN_TR2       (kiosk: PLAY_PAUSE)
BTN_SOUTH = 304     # gold OK center
BTN_EAST  = 305     # BLACK / MENU
BTN_PREV  = 308     # YELLOW / PREV
BTN_NEXT  = 309     # GREEN / NEXT
BTN_PAUSE = 310     # RED / PAUSE
BTN_START = 0x13B   # for QUIT_GAME RetroArch hotkey
KEY_Z     = 44      # KEY_Z for QUIT_GAME RetroArch hotkey

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
# Codes match what the kiosk's InputManager::map_button_to_action understands.
_MAP: dict[ButtonName, _KeyEvent | _AxisEvent] = {
    ButtonName.OK:        _KeyEvent(BTN_SOUTH),  # 304 → SELECT
    ButtonName.UP:        _AxisEvent(ABS_HAT0Y, -1),
    ButtonName.DOWN:      _AxisEvent(ABS_HAT0Y,  1),
    ButtonName.LEFT:      _AxisEvent(ABS_HAT0X, -1),
    ButtonName.RIGHT:     _AxisEvent(ABS_HAT0X,  1),
    ButtonName.YELLOW:    _KeyEvent(BTN_PREV),   # 308 → PREV
    ButtonName.RED:       _KeyEvent(BTN_PAUSE),  # 310 → PLAY_PAUSE
    ButtonName.GREEN:     _KeyEvent(BTN_NEXT),   # 309 → NEXT
    ButtonName.BLACK:     _KeyEvent(BTN_EAST),   # 305 → SETTINGS_MENU
    # QUIT_GAME emits Z + Start — RetroArch's "exit core" hotkey.
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
        # NB: AbsInfo lives at evdev's top level on python3-evdev (Bookworm),
        # not on evdev.ecodes — the latter only has the integer constants.
        from evdev import UInput, AbsInfo, ecodes as e

        capabilities = {
            e.EV_KEY: [BTN_SOUTH, BTN_EAST, BTN_PREV, BTN_NEXT, BTN_PAUSE,
                       BTN_START, KEY_Z],
            e.EV_ABS: [
                (ABS_HAT0X, AbsInfo(value=0, min=-1, max=1, fuzz=0, flat=0, resolution=0)),
                (ABS_HAT0Y, AbsInfo(value=0, min=-1, max=1, fuzz=0, flat=0, resolution=0)),
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
