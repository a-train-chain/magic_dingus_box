from __future__ import annotations

import time
import pygame

from .abstraction import InputEvent, InputProvider


class JoystickInputProvider(InputProvider):
    def __init__(self, joystick: pygame.joystick.Joystick) -> None:
        self.js = joystick
        self.js.init()
        self.axis_deadzone = 0.4
        self.rotate_repeat_hz = 8.0  # repeats per second when held
        self._last_rotate_emit = 0.0
        self._rotate_dir = 0  # -1, 0, +1
        self._hat_last = (0, 0)
        # Button mapping (may vary per controller; these are common defaults)
        self.BTN_A = 0          # Select
        self.BTN_B = 1          # Settings menu toggle
        self.BTN_X = 2
        self.BTN_Y = 3
        self.BTN_L = 4          # Previous
        self.BTN_R = 5          # Next
        self.BTN_SELECT = 6
        self.BTN_START = 7      # Play/Pause

    def translate(self, raw_event):  # type: ignore[no-untyped-def]
        et = raw_event.type
        if et == pygame.JOYBUTTONDOWN:
            btn = raw_event.button
            if btn == self.BTN_A:
                return InputEvent(InputEvent.Type.SELECT)
            if btn == self.BTN_B:
                return InputEvent(InputEvent.Type.SETTINGS_MENU)
            if btn == self.BTN_START:
                return InputEvent(InputEvent.Type.PLAY_PAUSE)
            if btn == self.BTN_L:
                return InputEvent(InputEvent.Type.PREV)
            if btn == self.BTN_R:
                return InputEvent(InputEvent.Type.NEXT)
            if btn == self.BTN_SELECT:
                # Map to toggle loop for convenience
                return InputEvent(InputEvent.Type.TOGGLE_LOOP)
            # Map additional buttons if needed (X, Y buttons)
            if btn == self.BTN_X:
                return InputEvent(InputEvent.Type.SEEK_LEFT)  # Quick seek back
            if btn == self.BTN_Y:
                return InputEvent(InputEvent.Type.SEEK_RIGHT)  # Quick seek forward
            return None

        if et == pygame.JOYHATMOTION:
            # Use D-pad for navigation: up/down for playlist navigation, left/right for seek
            hat_x, hat_y = raw_event.value
            # Trigger only on edge transitions
            ev = None
            # D-pad up/down for playlist navigation (user wants this for UI navigation)
            if hat_y == -1 and self._hat_last[1] != -1:  # Up
                ev = InputEvent(InputEvent.Type.ROTATE, delta=-1)
            elif hat_y == 1 and self._hat_last[1] != 1:  # Down
                ev = InputEvent(InputEvent.Type.ROTATE, delta=1)
            # D-pad left/right for seek
            elif hat_x == -1 and self._hat_last[0] != -1:  # Left
                ev = InputEvent(InputEvent.Type.SEEK_LEFT)
            elif hat_x == 1 and self._hat_last[0] != 1:  # Right
                ev = InputEvent(InputEvent.Type.SEEK_RIGHT)
            self._hat_last = (hat_x, hat_y)
            return ev

        if et == pygame.JOYAXISMOTION:
            # Left stick X controls ROTATE
            if raw_event.axis == 0:
                val = raw_event.value
                new_dir = 0
                if val <= -self.axis_deadzone:
                    new_dir = -1
                elif val >= self.axis_deadzone:
                    new_dir = 1
                self._rotate_dir = new_dir
                # Emit immediate tick on direction change
                if new_dir != 0:
                    self._last_rotate_emit = 0.0  # allow immediate emit in poll()
            return None

        return None

    def poll(self):  # type: ignore[no-untyped-def]
        """Emit repeated rotate events while stick is held past the deadzone."""
        events = []
        if self._rotate_dir != 0:
            now = time.time()
            interval = 1.0 / self.rotate_repeat_hz
            if (now - self._last_rotate_emit) >= interval:
                events.append(InputEvent(InputEvent.Type.ROTATE, delta=self._rotate_dir))
                self._last_rotate_emit = now
        return events


