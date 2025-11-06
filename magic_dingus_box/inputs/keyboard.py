from __future__ import annotations

import time
import pygame

from .abstraction import InputEvent, InputProvider


class KeyboardInputProvider(InputProvider):
    def __init__(self) -> None:
        self.hold_threshold = 0.3  # seconds before switching to seek/sample mode
        self.seek_rate = 2.0  # seek 2 seconds per tick when holding
        self.key_1_down_time: float | None = None
        self.key_3_down_time: float | None = None
        self.key_4_down_time: float | None = None
        self.key_1_seeking = False
        self.key_3_seeking = False
        self.sample_mode = False  # Track sample mode state

    def translate(self, raw_event):  # type: ignore[no-untyped-def]
        if raw_event.type == pygame.KEYDOWN:
            key = raw_event.key
            if key in (pygame.K_q, pygame.K_ESCAPE):
                return InputEvent(InputEvent.Type.QUIT)
            # Encoder rotation
            if key == pygame.K_LEFT:
                return InputEvent(InputEvent.Type.ROTATE, delta=-1)
            if key == pygame.K_RIGHT:
                return InputEvent(InputEvent.Type.ROTATE, delta=1)
            # Encoder push
            if key in (pygame.K_RETURN, pygame.K_SPACE):
                return InputEvent(InputEvent.Type.SELECT)
            
            # Sample mode: keys 1-4 have different behavior
            if self.sample_mode:
                if key == pygame.K_1:
                    if self.key_1_down_time is None:
                        self.key_1_down_time = time.time()
                    return None  # Wait to see if it's a hold for undo
                if key == pygame.K_2:
                    # Quick press: marker action
                    return InputEvent(InputEvent.Type.MARKER_ACTION, delta=1)
                if key == pygame.K_3:
                    # Quick press: marker action
                    return InputEvent(InputEvent.Type.MARKER_ACTION, delta=2)
                if key == pygame.K_4:
                    if self.key_4_down_time is None:
                        self.key_4_down_time = time.time()
                    return None  # Wait to see if it's a hold to exit sample mode or quick press for marker
            else:
                # Normal mode: keys 1-4 have normal behavior
                if key == pygame.K_1:
                    if self.key_1_down_time is None:
                        self.key_1_down_time = time.time()
                        self.key_1_seeking = False
                    return None  # Wait to see if it's a hold
                if key == pygame.K_2:
                    return InputEvent(InputEvent.Type.PLAY_PAUSE)
                if key == pygame.K_3:
                    if self.key_3_down_time is None:
                        self.key_3_down_time = time.time()
                        self.key_3_seeking = False
                    return None  # Wait to see if it's a hold
                if key == pygame.K_4:
                    if self.key_4_down_time is None:
                        self.key_4_down_time = time.time()
                    return None  # Wait to see if it's a hold to enter sample mode
        
        elif raw_event.type == pygame.KEYUP:
            key = raw_event.key
            
            # Sample mode key releases
            if self.sample_mode:
                if key == pygame.K_1:
                    if self.key_1_down_time is not None:
                        duration = time.time() - self.key_1_down_time
                        self.key_1_down_time = None
                        self.key_1_seeking = False  # Reset flag
                        # Quick press: marker action, Long press: already handled in poll
                        if duration < self.hold_threshold:
                            return InputEvent(InputEvent.Type.MARKER_ACTION, delta=0)
                if key == pygame.K_4:
                    if self.key_4_down_time is not None:
                        duration = time.time() - self.key_4_down_time
                        self.key_4_down_time = None
                        # Quick press: marker action, Long press already handled in poll
                        if duration < self.hold_threshold:
                            return InputEvent(InputEvent.Type.MARKER_ACTION, delta=3)
                        return None
            else:
                # Normal mode key releases
                if key == pygame.K_1:
                    if self.key_1_down_time is not None:
                        duration = time.time() - self.key_1_down_time
                        self.key_1_down_time = None
                        self.key_1_seeking = False
                        # If it was a quick press, change track
                        if duration < self.hold_threshold:
                            return InputEvent(InputEvent.Type.PREV)
                if key == pygame.K_3:
                    if self.key_3_down_time is not None:
                        duration = time.time() - self.key_3_down_time
                        self.key_3_down_time = None
                        self.key_3_seeking = False
                        # If it was a quick press, change track
                        if duration < self.hold_threshold:
                            return InputEvent(InputEvent.Type.NEXT)
                if key == pygame.K_4:
                    if self.key_4_down_time is not None:
                        duration = time.time() - self.key_4_down_time
                        self.key_4_down_time = None
                        # Quick press: open settings menu
                        if duration < self.hold_threshold:
                            return InputEvent(InputEvent.Type.SETTINGS_MENU)
                        # Long press already handled in poll (enter sample mode)
                        return None
        
        return None
    
    def poll_seeking(self):  # type: ignore[no-untyped-def]
        """Check if any keys are being held for seeking or sample mode actions."""
        events = []
        now = time.time()
        
        if self.sample_mode:
            # Sample mode: check for hold actions
            # Key 1 hold: undo last marker (trigger once)
            if self.key_1_down_time is not None:
                held_duration = now - self.key_1_down_time
                if held_duration >= self.hold_threshold:
                    if not self.key_1_seeking:  # Reuse flag to ensure single trigger
                        self.key_1_seeking = True
                        self.key_1_down_time = None  # Clear to prevent repeat
                        events.append(InputEvent(InputEvent.Type.UNDO_MARKER))
            
            # Key 4 hold: exit sample mode (trigger once)
            if self.key_4_down_time is not None:
                held_duration = now - self.key_4_down_time
                if held_duration >= self.hold_threshold:
                    self.key_4_down_time = None  # Clear to prevent repeat
                    events.append(InputEvent(InputEvent.Type.EXIT_SAMPLE_MODE))
        else:
            # Normal mode: check for seek and sample mode entry
            # Check key 1 (backward seek)
            if self.key_1_down_time is not None:
                held_duration = now - self.key_1_down_time
                if held_duration >= self.hold_threshold:
                    if not self.key_1_seeking:
                        self.key_1_seeking = True
                    events.append(InputEvent(InputEvent.Type.SEEK_LEFT))
            
            # Check key 3 (forward seek)
            if self.key_3_down_time is not None:
                held_duration = now - self.key_3_down_time
                if held_duration >= self.hold_threshold:
                    if not self.key_3_seeking:
                        self.key_3_seeking = True
                    events.append(InputEvent(InputEvent.Type.SEEK_RIGHT))
            
            # Check key 4 (enter sample mode, trigger once)
            if self.key_4_down_time is not None:
                held_duration = now - self.key_4_down_time
                if held_duration >= self.hold_threshold:
                    self.key_4_down_time = None  # Clear to prevent repeat
                    events.append(InputEvent(InputEvent.Type.ENTER_SAMPLE_MODE))
        
        return events

