from __future__ import annotations

import os
import time
import logging
from typing import Optional

try:
    import evdev
    from evdev import InputDevice, categorize, ecodes
    EVDEV_AVAILABLE = True
except ImportError:
    EVDEV_AVAILABLE = False
    evdev = None
    InputDevice = None
    categorize = None
    ecodes = None

from .abstraction import InputEvent, InputProvider


class EvdevJoystickInputProvider(InputProvider):
    """Joystick input provider using evdev to read joystick events directly.
    
    This bypasses pygame's focus requirement and can read joystick events
    even when the pygame window doesn't have focus. Works globally.
    
    N64 Controller Mapping:
    - Start (button 9) -> SELECT (Enter/Space/Slash)
    - A Button (button 1) -> SELECT (Enter/Space/Slash)
    - B Button (button 2) -> SETTINGS_MENU (key 4)
    - Z Trig (button 7) -> PLAY_PAUSE (key 2)
    - R Trig (button 5) -> NEXT (key 3)
    - L Trig (button 4) -> PREV (key 1)
    - C Button R (axis 3+) -> NEXT/SEEK_RIGHT (key 3)
    - C Button L (axis 3-) -> PREV/SEEK_LEFT (key 1)
    - C Button D (axis 2+) -> (unused)
    - C Button U (axis 2-) -> (unused)
    - DPad Left/Right (hat 0) -> ROTATE
    - DPad Up/Down (hat 0) -> (unused)
    """
    
    def __init__(self) -> None:
        if not EVDEV_AVAILABLE:
            raise RuntimeError("evdev not available - install with: pip install evdev")
        
        self._log = logging.getLogger("evdev_joystick")
        self.hold_threshold = 0.3
        self.seek_rate = 2.0
        
        # N64 Controller button mappings (using evdev button codes)
        # Based on actual evdev codes from logs:
        # Button 304 = BTN_A/BTN_SOUTH (A button)
        # Button 305 = BTN_B/BTN_EAST (B button)
        # Button 312 = BTN_TL2 (L Trigger)
        # Button 313 = BTN_TR2 (R Trigger)
        # Button 9 = BTN_START (Start button) - need to verify code
        # Button 7 = BTN_TL (Z Trigger) - need to verify code
        
        # Use evdev constants for button codes
        # Based on actual N64 controller mapping (SWITCH CO.,LTD. Controller):
        # Standard evdev codes: BTN_SOUTH=304 (A), BTN_EAST=305 (B)
        # N64 controller actually uses: 306 (A button), 305 (B button)
        # User wants: A button = SELECT, B button = SETTINGS_MENU
        # - A button = code 306 -> SELECT (user wants A to select)
        # - B button = code 305 (BTN_EAST) -> SETTINGS_MENU (user wants B to open menu)
        # - Start button = code 316 (BTN_MODE) -> SELECT
        # - Z button = code 310 -> PLAY_PAUSE
        # - Left trigger = code 308 -> PREV
        # - Right trigger = code 309 -> NEXT
        self.BTN_START = ecodes.BTN_START if hasattr(ecodes, 'BTN_START') else 9      # Fallback for standard START
        self.BTN_MODE = ecodes.BTN_MODE if hasattr(ecodes, 'BTN_MODE') else 316      # Start button (N64)
        # N64 controller uses 306 for A button (non-standard, but that's what it sends)
        # Also check standard BTN_SOUTH (304) as fallback for other controllers
        self.BTN_A = 306                                                              # A button (N64) -> SELECT
        self.BTN_A_STD = ecodes.BTN_SOUTH if hasattr(ecodes, 'BTN_SOUTH') else 304   # Standard A button (fallback)
        self.BTN_B = ecodes.BTN_EAST if hasattr(ecodes, 'BTN_EAST') else 305          # B button (N64) -> SETTINGS_MENU
        self.BTN_Z = 310                                                              # Z button (N64) -> PLAY_PAUSE
        self.BTN_R = 309                                                              # Right trigger (N64) -> NEXT
        self.BTN_L = 308                                                              # Left trigger (N64) -> PREV
        self.BTN_MEMPAK = 6     # (unused for now)
        
        self._log.info(f"N64 Controller button mappings: START={self.BTN_MODE}, A={self.BTN_A}/{self.BTN_A_STD}, B={self.BTN_B}, Z={self.BTN_Z}, R={self.BTN_R}, L={self.BTN_L}")
        
        # Axis mappings
        self.AXIS_C_R = 3       # axis(3+) -> NEXT (key 3)
        self.AXIS_C_L = 3       # axis(3-) -> PREV (key 1)
        self.AXIS_C_D = 2       # axis(2+) -> (unused)
        self.AXIS_C_U = 2       # axis(2-) -> (unused)
        self.AXIS_X = 0         # X Axis -> ROTATE
        self.AXIS_Y = 1         # Y Axis -> (unused)
        
        # State tracking
        self._button_states = {}  # button_code -> (pressed, timestamp)
        self._axis_states = {}    # axis_num -> value
        self._hat_state = (0, 0)  # (x, y)
        self._last_rotate_emit = 0.0
        self._rotate_dir = 0
        self.rotate_repeat_hz = 8.0
        
        # Hold detection for C buttons (they act like keys 1 and 3)
        self.c_left_down_time: Optional[float] = None
        self.c_right_down_time: Optional[float] = None
        self.c_left_seeking = False
        self.c_right_seeking = False
        
        # Find joystick devices
        self.devices: list[InputDevice] = []
        self._find_joystick_devices()
        
        if not self.devices:
            self._log.warning("No joystick device found via evdev - joystick input may not work")
        else:
            self._log.info(f"Found {len(self.devices)} joystick device(s) via evdev")
            # Try to grab devices to prevent other apps from reading them
            # But continue using them even if grab fails (we'll still get events)
            for device in self.devices:
                self._log.info(f"Using evdev joystick device: {device.path} ({device.name})")
                # Set device to non-blocking mode
                try:
                    import fcntl
                    flags = fcntl.fcntl(device.fd, fcntl.F_GETFL)
                    fcntl.fcntl(device.fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
                    self._log.info(f"Set device {device.name} to non-blocking mode")
                except Exception as exc:
                    self._log.warning(f"Could not set non-blocking mode for {device.name}: {exc}")
                
                # Try to grab device, but continue even if grab fails
                # Grabbing gives us exclusive access, but we can still read events without it
                try:
                    device.grab()
                    self._log.info(f"Successfully grabbed evdev device: {device.name} (exclusive access)")
                except Exception as exc:
                    self._log.info(f"Could not grab evdev device {device.name} (non-exclusive, will still work): {exc}")
                    # Continue using device even if grab fails - we'll still get events
                    # This allows pygame to also read events if it needs to
    
    def _find_joystick_devices(self) -> None:
        """Find all joystick/gamepad input devices."""
        try:
            all_devices = [evdev.InputDevice(path) for path in evdev.list_devices()]
            self._log.debug(f"Found {len(all_devices)} total input devices")
            
            # Collect all devices that have joystick/gamepad capabilities
            joystick_devices = []
            
            for device in all_devices:
                try:
                    name_lower = device.name.lower()
                    caps = device.capabilities()
                    
                    # Look for devices with joystick/gamepad in the name (including "Broadcom Bluetooth")
                    if any(keyword in name_lower for keyword in ["joystick", "gamepad", "controller", "n64", "bluetooth", "broadcom"]):
                        if ecodes.EV_ABS in caps or ecodes.EV_KEY in caps:
                            joystick_devices.append(device)
                            self._log.info(f"Found joystick device by name: {device.name}")
                            continue
                    
                    # Fallback: look for devices with ABS axes (joystick-like)
                    # This catches devices that might not have "joystick" in the name
                    if ecodes.EV_ABS in caps:
                        axes = caps[ecodes.EV_ABS]
                        # Check for common joystick axes (X, Y, RX, RY, HAT, etc.)
                        if any(axis in axes for axis in [ecodes.ABS_X, ecodes.ABS_Y, ecodes.ABS_RX, ecodes.ABS_RY, ecodes.ABS_HAT0X, ecodes.ABS_HAT0Y]):
                            # Make sure it's not a keyboard (keyboards don't have joystick axes)
                            if ecodes.EV_KEY in caps:
                                keys = caps[ecodes.EV_KEY]
                                # If it has joystick-like buttons (BTN_GAMEPAD, BTN_JOYSTICK) or many buttons, it's likely a joystick
                                if any(btn in keys for btn in [ecodes.BTN_GAMEPAD, ecodes.BTN_JOYSTICK]) or len(keys) > 10:
                                    joystick_devices.append(device)
                                    self._log.info(f"Found joystick device by capabilities: {device.name}")
                except Exception as exc:
                    self._log.debug(f"Error checking device {device.name}: {exc}")
                    continue
            
            self.devices = joystick_devices
            if self.devices:
                self._log.info(f"Found {len(self.devices)} joystick device(s) for evdev input")
        except Exception as exc:
            self._log.warning(f"Error finding joystick devices: {exc}", exc_info=True)
            self.devices = []
    
    def translate(self, raw_event) -> Optional[InputEvent]:
        """Not used for evdev - we poll instead."""
        return None
    
    def poll(self) -> list[InputEvent]:
        """Poll evdev for joystick events and return InputEvents."""
        events = []
        if not self.devices:
            return events
        
        # Poll all joystick devices
        total_event_count = 0
        for device in self.devices:
            try:
                device_event_count = 0
                # Read events (non-blocking due to O_NONBLOCK flag)
                try:
                    for event in device.read():
                        device_event_count += 1
                        total_event_count += 1
                        
                        if event.type == ecodes.EV_KEY:
                            # Button event
                            # Use event.code directly (not scancode) - this is the button code
                            button_code = event.code
                            is_press = event.value == 1  # 1 = press, 0 = release, 2 = repeat
                            
                            # Log ALL button events for debugging
                            # ecodes.BTN.get() can return a tuple, so convert to string
                            button_name_raw = ecodes.BTN.get(button_code, f"BTN_{button_code}")
                            if isinstance(button_name_raw, tuple):
                                button_name = '/'.join(button_name_raw)
                            else:
                                button_name = str(button_name_raw)
                            # Log at INFO level so we can see button presses during video
                            self._log.info(f"Evdev joystick [{device.name}]: Button event - code={button_code} ({button_name}), value={event.value} (press={is_press})")
                            
                            if is_press:
                                self._button_states[button_code] = (True, time.time())
                                
                                # Map N64 controller buttons using actual evdev codes
                                # User wants: A button = SELECT, B button = SETTINGS_MENU
                                # N64 controller: A=306, B=305 (BTN_EAST)
                                # Also check standard codes (304 for A) as fallback
                                
                                if button_code == self.BTN_A or button_code == self.BTN_A_STD or button_code == self.BTN_MODE:
                                    # A button (306 for N64, or 304 standard) or Start button (316) -> SELECT
                                    events.append(InputEvent(InputEvent.Type.SELECT))
                                    self._log.info(f"Evdev joystick [{device.name}]: Button {button_code} (A/Start) -> SELECT")
                                elif button_code == self.BTN_B:
                                    # B button (305/BTN_EAST) -> SETTINGS_MENU
                                    events.append(InputEvent(InputEvent.Type.SETTINGS_MENU))
                                    self._log.info(f"Evdev joystick [{device.name}]: Button {button_code} (B) -> SETTINGS_MENU")
                                elif button_code == self.BTN_Z:
                                    # Z button (310) -> PLAY_PAUSE
                                    events.append(InputEvent(InputEvent.Type.PLAY_PAUSE))
                                    self._log.info(f"Evdev joystick [{device.name}]: Button {button_code} (Z) -> PLAY_PAUSE")
                                elif button_code == self.BTN_R:
                                    # Right trigger (309) -> NEXT
                                    if self.c_right_down_time is None:
                                        self.c_right_down_time = time.time()
                                        self.c_right_seeking = False
                                    self._log.info(f"Evdev joystick [{device.name}]: Button {button_code} (R trigger) -> NEXT")
                                elif button_code == self.BTN_L:
                                    # Left trigger (308) -> PREV
                                    if self.c_left_down_time is None:
                                        self.c_left_down_time = time.time()
                                        self.c_left_seeking = False
                                    self._log.info(f"Evdev joystick [{device.name}]: Button {button_code} (L trigger) -> PREV")
                                else:
                                    self._log.warning(f"Evdev joystick [{device.name}]: Unmapped button {button_code} ({button_name}) - START={self.BTN_START}, A={self.BTN_A}, B={self.BTN_B}, Z={self.BTN_Z}, R={self.BTN_R}, L={self.BTN_L}")
                            else:
                                # Button released
                                if button_code in self._button_states:
                                    del self._button_states[button_code]
                                
                                # Handle hold detection for triggers
                                now = time.time()
                                if button_code == self.BTN_R and self.c_right_down_time is not None:
                                    duration = now - self.c_right_down_time
                                    self.c_right_down_time = None
                                    self.c_right_seeking = False
                                    if duration < self.hold_threshold:
                                        events.append(InputEvent(InputEvent.Type.NEXT))
                                        self._log.debug(f"Evdev joystick [{device.name}]: Button {button_code} (R) released -> NEXT")
                                elif button_code == self.BTN_L and self.c_left_down_time is not None:
                                    duration = now - self.c_left_down_time
                                    self.c_left_down_time = None
                                    self.c_left_seeking = False
                                    if duration < self.hold_threshold:
                                        events.append(InputEvent(InputEvent.Type.PREV))
                                        self._log.debug(f"Evdev joystick [{device.name}]: Button {button_code} (L) released -> PREV")
                        
                        elif event.type == ecodes.EV_ABS:
                            # Axis event (analog stick, C buttons, etc.)
                            abs_event = categorize(event)
                            axis_code = abs_event.event.code
                            axis_value = abs_event.event.value
                            
                            # Log axis events for important axes
                            axis_name = ecodes.ABS.get(axis_code, f"ABS_{axis_code}")
                            # Log all axis events at INFO level to help identify D-pad and C buttons
                            if abs(axis_value) > 500:  # Only log significant movements
                                self._log.info(f"Evdev joystick [{device.name}]: Axis event - code={axis_code} ({axis_name}), value={axis_value}")
                            
                            self._axis_states[axis_code] = axis_value
                            
                            # C Button Right (axis 3+) -> NEXT (key 3)
                            if axis_code == self.AXIS_C_R:
                                if axis_value > 1000:  # Positive threshold
                                    if self.c_right_down_time is None:
                                        self.c_right_down_time = time.time()
                                        self.c_right_seeking = False
                                        events.append(InputEvent(InputEvent.Type.NEXT))
                                        self._log.debug(f"Evdev joystick [{device.name}]: C Button R (axis 3+) -> NEXT (key 3)")
                                elif axis_value < 1000 and self.c_right_down_time is not None:
                                    # Released
                                    now = time.time()
                                    duration = now - self.c_right_down_time
                                    self.c_right_down_time = None
                                    self.c_right_seeking = False
                            
                            # C Button Left (axis 3-) -> PREV (key 1)
                            elif axis_code == self.AXIS_C_L:
                                if axis_value < -1000:  # Negative threshold
                                    if self.c_left_down_time is None:
                                        self.c_left_down_time = time.time()
                                        self.c_left_seeking = False
                                        events.append(InputEvent(InputEvent.Type.PREV))
                                        self._log.debug(f"Evdev joystick [{device.name}]: C Button L (axis 3-) -> PREV (key 1)")
                                elif axis_value > -1000 and self.c_left_down_time is not None:
                                    # Released
                                    now = time.time()
                                    duration = now - self.c_left_down_time
                                    self.c_left_down_time = None
                                    self.c_left_seeking = False
                            
                            # X Axis (analog stick) -> ROTATE
                            elif axis_code == self.AXIS_X:
                                deadzone = 4096  # From controller config
                                if axis_value <= -deadzone:
                                    self._rotate_dir = -1
                                    self._last_rotate_emit = 0.0  # Allow immediate emit
                                elif axis_value >= deadzone:
                                    self._rotate_dir = 1
                                    self._last_rotate_emit = 0.0  # Allow immediate emit
                                else:
                                    self._rotate_dir = 0
                            
                            # D-Pad (hat switch) - only ABS_HAT0Y for up/down navigation
                            # ABS_HAT0X (left/right) disabled - only using up/down for navigation
                            # elif axis_code == ecodes.ABS_HAT0X:
                            #     # D-Pad Left/Right disabled
                            #     pass
                            elif axis_code == ecodes.ABS_HAT0Y:
                                # D-Pad Up/Down for playlist navigation
                                if axis_value == -1:  # Up
                                    events.append(InputEvent(InputEvent.Type.ROTATE, delta=-1))
                                    self._log.debug(f"Evdev joystick [{device.name}]: D-Pad Up -> ROTATE -1 (previous)")
                                elif axis_value == 1:  # Down
                                    events.append(InputEvent(InputEvent.Type.ROTATE, delta=1))
                                    self._log.debug(f"Evdev joystick [{device.name}]: D-Pad Down -> ROTATE +1 (next)")
                    
                        elif event.type == ecodes.EV_KEY:
                            # D-pad buttons (some controllers use buttons instead of hat)
                            # Note: This is a duplicate check - D-pad buttons are already handled above
                            # But keeping this for controllers that report D-pad as separate EV_KEY events
                            button_code = event.code
                            is_press = event.value == 1
                            if is_press and button_code in (ecodes.BTN_DPAD_UP, ecodes.BTN_DPAD_DOWN, ecodes.BTN_DPAD_LEFT, ecodes.BTN_DPAD_RIGHT):
                                if button_code == ecodes.BTN_DPAD_LEFT:
                                    events.append(InputEvent(InputEvent.Type.ROTATE, delta=-1))
                                    self._log.debug(f"Evdev joystick [{device.name}]: D-Pad Left (button) -> ROTATE -1")
                                elif button_code == ecodes.BTN_DPAD_RIGHT:
                                    events.append(InputEvent(InputEvent.Type.ROTATE, delta=1))
                                    self._log.debug(f"Evdev joystick [{device.name}]: D-Pad Right (button) -> ROTATE +1")
            
                except BlockingIOError:
                    # No events available from this device (non-blocking read) - this is normal
                    continue
                except Exception as exc:
                    self._log.warning(f"Error reading evdev events from {device.name}: {exc}", exc_info=True)
                    continue
            except Exception as exc:
                self._log.warning(f"Error accessing device {device.name}: {exc}", exc_info=True)
                continue
        
        # Handle continuous rotation from analog stick
        if self._rotate_dir != 0:
            now = time.time()
            interval = 1.0 / self.rotate_repeat_hz
            if (now - self._last_rotate_emit) >= interval:
                events.append(InputEvent(InputEvent.Type.ROTATE, delta=self._rotate_dir))
                self._last_rotate_emit = now
        
        # Handle hold-to-seek for C buttons
        events.extend(self.poll_seeking())
        
        if total_event_count > 0:
            self._log.debug(f"Evdev joystick processed {total_event_count} events from {len(self.devices)} device(s), generated {len(events)} InputEvents")
        
        return events
    
    def poll_seeking(self) -> list[InputEvent]:
        """Check if C buttons are being held for seeking."""
        events = []
        now = time.time()
        
        # C Button Right (R Trigger or axis 3+) -> SEEK_RIGHT (key 3 hold)
        if self.c_right_down_time is not None:
            held_duration = now - self.c_right_down_time
            if held_duration >= self.hold_threshold:
                if not self.c_right_seeking:
                    self.c_right_seeking = True
                events.append(InputEvent(InputEvent.Type.SEEK_RIGHT))
        
        # C Button Left (L Trigger or axis 3-) -> SEEK_LEFT (key 1 hold)
        if self.c_left_down_time is not None:
            held_duration = now - self.c_left_down_time
            if held_duration >= self.hold_threshold:
                if not self.c_left_seeking:
                    self.c_left_seeking = True
                events.append(InputEvent(InputEvent.Type.SEEK_LEFT))
        
        return events
    
    def __del__(self) -> None:
        """Release device grabs on cleanup."""
        for device in self.devices:
            try:
                device.ungrab()
            except Exception:
                pass

