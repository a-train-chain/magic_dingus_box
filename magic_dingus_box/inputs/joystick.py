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
        # Button mapping for N64 controller
        self.BTN_A = 1          # Select (A Button)
        self.BTN_B = 2          # Settings menu toggle (B Button)
        self.BTN_X = 0
        self.BTN_Y = 3
        self.BTN_L = 4          # Skip backward (Left Trigger)
        self.BTN_R = 5          # Skip forward (Right Trigger)
        self.BTN_SELECT = 6
        self.BTN_START = 7      # Pause (Z Trigger)

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
            return None

        if et == pygame.JOYHATMOTION:
            # Use D-pad up/down for playlist navigation (ROTATE)
            hat_x, hat_y = raw_event.value
            # Trigger only on edge transitions
            ev = None
            if hat_y == -1 and self._hat_last[1] != -1:
                # D-pad Down = navigate down (ROTATE +1)
                ev = InputEvent(InputEvent.Type.ROTATE, delta=1)
            elif hat_y == 1 and self._hat_last[1] != 1:
                # D-pad Up = navigate up (ROTATE -1)
                ev = InputEvent(InputEvent.Type.ROTATE, delta=-1)
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
        """Emit repeated rotate events while stick is held past the deadzone.
        Also poll buttons directly (works even when window doesn't have focus).
        
        NOTE: Button polling is used as a fallback when pygame events aren't available.
        The translate() method handles button presses from pygame events, so we only
        poll buttons here if we detect the window might not have focus (by checking
        if we've received any pygame events recently).
        """
        events = []
        
        # Poll stick for continuous rotation
        if self._rotate_dir != 0:
            now = time.time()
            interval = 1.0 / self.rotate_repeat_hz
            if (now - self._last_rotate_emit) >= interval:
                events.append(InputEvent(InputEvent.Type.ROTATE, delta=self._rotate_dir))
                self._last_rotate_emit = now
        
        # Only poll buttons directly if we haven't received pygame events recently
        # This prevents double-presses when pygame events are working
        if not hasattr(self, '_last_pygame_event_time'):
            self._last_pygame_event_time = 0.0
        
        now = time.time()
        # If we've received a pygame event in the last 0.1 seconds, don't poll buttons
        # (pygame events are working, so we'll get duplicates)
        use_polling = (now - self._last_pygame_event_time) > 0.1
        
        if use_polling:
            # Poll buttons directly (works even without window focus)
            # This is critical when pygame window is behind mpv
            try:
                # Track button states to detect presses
                if not hasattr(self, '_button_states'):
                    self._button_states = {}
                
                # Check all buttons
                for btn_id in [self.BTN_A, self.BTN_B, self.BTN_L, self.BTN_R, self.BTN_START, self.BTN_SELECT]:
                    current_state = self.js.get_button(btn_id)
                    prev_state = self._button_states.get(btn_id, False)
                    
                    # Detect button press (transition from False to True)
                    if current_state and not prev_state:
                        if btn_id == self.BTN_A:
                            events.append(InputEvent(InputEvent.Type.SELECT))
                        elif btn_id == self.BTN_B:
                            events.append(InputEvent(InputEvent.Type.SETTINGS_MENU))
                        elif btn_id == self.BTN_START:
                            events.append(InputEvent(InputEvent.Type.PLAY_PAUSE))
                        elif btn_id == self.BTN_L:
                            events.append(InputEvent(InputEvent.Type.PREV))
                        elif btn_id == self.BTN_R:
                            events.append(InputEvent(InputEvent.Type.NEXT))
                        elif btn_id == self.BTN_SELECT:
                            events.append(InputEvent(InputEvent.Type.TOGGLE_LOOP))
                    
                    self._button_states[btn_id] = current_state
                
                # Poll hat (D-pad) directly - but only if we're using polling mode
                if self.js.get_numhats() > 0:
                    hat_value = self.js.get_hat(0)
                    hat_x, hat_y = hat_value
                    prev_hat = self._hat_last
                    
                    # Detect hat changes
                    if hat_y != prev_hat[1]:
                        if hat_y == -1:
                            events.append(InputEvent(InputEvent.Type.ROTATE, delta=1))
                        elif hat_y == 1:
                            events.append(InputEvent(InputEvent.Type.ROTATE, delta=-1))
                        self._hat_last = (hat_x, hat_y)
            except Exception:
                # If polling fails, fall back to event-based input
                pass
        
        return events
    
    def translate(self, raw_event):  # type: ignore[no-untyped-def]
        """Translate pygame joystick events to InputEvents.
        Also updates timestamp to indicate pygame events are working."""
        # Mark that we received a pygame event (so polling knows not to duplicate)
        self._last_pygame_event_time = time.time()
        
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
            return None

        if et == pygame.JOYHATMOTION:
            # Use D-pad up/down for playlist navigation (ROTATE)
            hat_x, hat_y = raw_event.value
            # Trigger only on edge transitions
            ev = None
            if hat_y == -1 and self._hat_last[1] != -1:
                # D-pad Down = navigate down (ROTATE +1)
                ev = InputEvent(InputEvent.Type.ROTATE, delta=1)
            elif hat_y == 1 and self._hat_last[1] != 1:
                # D-pad Up = navigate up (ROTATE -1)
                ev = InputEvent(InputEvent.Type.ROTATE, delta=-1)
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


