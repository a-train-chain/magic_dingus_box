from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto


class InputEvent:
    class Type(Enum):
        ROTATE = auto()
        SELECT = auto()
        NEXT = auto()
        PREV = auto()
        SEEK_LEFT = auto()
        SEEK_RIGHT = auto()
        PLAY_PAUSE = auto()
        TOGGLE_LOOP = auto()
        QUIT = auto()
        ENTER_SAMPLE_MODE = auto()
        EXIT_SAMPLE_MODE = auto()
        MARKER_ACTION = auto()
        UNDO_MARKER = auto()
        SETTINGS_MENU = auto()  # Toggle settings menu

    def __init__(self, type_: "InputEvent.Type", delta: int = 0) -> None:
        self.type = type_
        self.delta = delta


class InputProvider:
    def translate(self, raw_event):  # type: ignore[no-untyped-def]
        raise NotImplementedError

