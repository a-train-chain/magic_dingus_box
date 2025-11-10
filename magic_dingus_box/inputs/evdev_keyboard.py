from __future__ import annotations

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


class EvdevKeyboardInputProvider(InputProvider):
    """Keyboard input provider using evdev to read keyboard events directly.
    
    This bypasses pygame's focus requirement and can read keyboard events
    even when the pygame window doesn't have focus. Useful for controllers
    that map to keyboard events (like N64 controller).
    """
    
    def __init__(self) -> None:
        if not EVDEV_AVAILABLE:
            raise RuntimeError("evdev not available - install with: pip install evdev")
        
        self._log = logging.getLogger("evdev_keyboard")
        self.hold_threshold = 0.3
        self.seek_rate = 2.0
        self.key_1_down_time: Optional[float] = None
        self.key_3_down_time: Optional[float] = None
        self.key_4_down_time: Optional[float] = None
        self.key_1_seeking = False
        self.key_3_seeking = False
        self.sample_mode = False
        
        # Key state tracking
        self._key_states = {}  # key_code -> (pressed, timestamp)
        
        # Find keyboard devices (try multiple devices)
        self.devices: list[InputDevice] = []
        self._find_keyboard_devices()
        
        if not self.devices:
            self._log.warning("No keyboard device found via evdev - keyboard input may not work")
        else:
            self._log.info(f"Found {len(self.devices)} keyboard device(s) via evdev")
            # Try to grab devices to prevent other apps from reading them
            # But continue using them even if grab fails (we'll still get events)
            for device in self.devices:
                self._log.info(f"Using evdev keyboard device: {device.path} ({device.name})")
                try:
                    device.grab()
                    self._log.info(f"Successfully grabbed device: {device.name}")
                except Exception as exc:
                    self._log.warning(f"Could not grab evdev device {device.name} (may need root, but will continue): {exc}")
                    # Continue using device even if grab fails - we'll still get events
    
    def _find_keyboard_devices(self) -> None:
        """Find all keyboard input devices."""
        try:
            all_devices = [evdev.InputDevice(path) for path in evdev.list_devices()]
            self._log.debug(f"Found {len(all_devices)} total input devices")
            
            # Collect all devices that have keyboard capabilities
            keyboard_devices = []
            
            # First, look for devices with "keyboard" in the name
            for device in all_devices:
                try:
                    if "keyboard" in device.name.lower() or "Keyboard" in device.name:
                        if ecodes.EV_KEY in device.capabilities():
                            keys = device.capabilities()[ecodes.EV_KEY]
                            if ecodes.KEY_LEFT in keys or ecodes.KEY_RIGHT in keys or ecodes.KEY_ENTER in keys or ecodes.KEY_SPACE in keys:
                                keyboard_devices.append(device)
                                self._log.info(f"Found keyboard device by name: {device.name}")
                except Exception:
                    continue
            
            # Fallback: look for any device with keyboard keys (if we didn't find any by name)
            if not keyboard_devices:
                for device in all_devices:
                    try:
                        if ecodes.EV_KEY in device.capabilities():
                            keys = device.capabilities()[ecodes.EV_KEY]
                            # Check for common keyboard keys (arrow keys, return, space)
                            if ecodes.KEY_LEFT in keys or ecodes.KEY_RIGHT in keys or ecodes.KEY_ENTER in keys or ecodes.KEY_SPACE in keys:
                                keyboard_devices.append(device)
                                self._log.info(f"Found keyboard device by capabilities: {device.name}")
                    except Exception:
                        continue
            
            self.devices = keyboard_devices
            if self.devices:
                self._log.info(f"Found {len(self.devices)} keyboard device(s) for evdev input")
        except Exception as exc:
            self._log.warning(f"Error finding keyboard devices: {exc}", exc_info=True)
            self.devices = []
    
    def translate(self, raw_event) -> Optional[InputEvent]:
        """Not used for evdev - we poll instead."""
        return None
    
    def poll(self) -> list[InputEvent]:
        """Poll evdev for keyboard events and return InputEvents."""
        events = []
        if not self.devices:
            return events
        
        # Poll all keyboard devices
        total_event_count = 0
        for device in self.devices:
            try:
                # Read available events (non-blocking)
                # read() returns an iterator - will raise BlockingIOError if no events available
                device_event_count = 0
                for event in device.read():
                    device_event_count += 1
                    total_event_count += 1
                    if event.type == ecodes.EV_KEY:
                        key_event = categorize(event)
                        key_code = key_event.scancode
                        is_press = event.value == 1  # 1 = press, 0 = release, 2 = repeat
                        
                        # Log key events for debugging (only for important keys)
                        key_name = ecodes.KEY.get(key_code, f"KEY_{key_code}")
                        if key_code in (ecodes.KEY_ENTER, ecodes.KEY_SPACE, ecodes.KEY_SLASH, ecodes.KEY_LEFT, ecodes.KEY_RIGHT):
                            self._log.debug(f"Evdev [{device.name}]: {key_name} {'PRESS' if is_press else 'RELEASE'} (code={key_code})")
                        
                        if is_press:
                            # Key pressed
                            self._key_states[key_code] = (True, time.time())
                            
                            # Map key codes to actions
                            if key_code == ecodes.KEY_LEFT:
                                events.append(InputEvent(InputEvent.Type.ROTATE, delta=-1))
                                self._log.debug(f"Evdev [{device.name}]: LEFT arrow -> ROTATE -1")
                            elif key_code == ecodes.KEY_RIGHT:
                                events.append(InputEvent(InputEvent.Type.ROTATE, delta=1))
                                self._log.debug(f"Evdev [{device.name}]: RIGHT arrow -> ROTATE +1")
                            elif key_code in (ecodes.KEY_ENTER, ecodes.KEY_SPACE, ecodes.KEY_KPENTER, ecodes.KEY_SLASH):
                                events.append(InputEvent(InputEvent.Type.SELECT))
                                self._log.debug(f"Evdev [{device.name}]: {key_name} (code={key_code}) -> SELECT")
                            elif key_code == ecodes.KEY_1:
                                if self.key_1_down_time is None:
                                    self.key_1_down_time = time.time()
                                    self.key_1_seeking = False
                            elif key_code == ecodes.KEY_2:
                                events.append(InputEvent(InputEvent.Type.PLAY_PAUSE))
                            elif key_code == ecodes.KEY_3:
                                if self.key_3_down_time is None:
                                    self.key_3_down_time = time.time()
                                    self.key_3_seeking = False
                            elif key_code == ecodes.KEY_4:
                                if self.key_4_down_time is None:
                                    self.key_4_down_time = time.time()
                            else:
                                self._log.debug(f"Evdev [{device.name}]: Unmapped key {key_name} (code={key_code})")
                        else:
                            # Key released
                            if key_code in self._key_states:
                                del self._key_states[key_code]
                            
                            # Handle key releases for hold detection
                            now = time.time()
                            if key_code == ecodes.KEY_1 and self.key_1_down_time is not None:
                                duration = now - self.key_1_down_time
                                self.key_1_down_time = None
                                self.key_1_seeking = False
                                if duration < self.hold_threshold:
                                    events.append(InputEvent(InputEvent.Type.PREV))
                            elif key_code == ecodes.KEY_3 and self.key_3_down_time is not None:
                                duration = now - self.key_3_down_time
                                self.key_3_down_time = None
                                self.key_3_seeking = False
                                if duration < self.hold_threshold:
                                    events.append(InputEvent(InputEvent.Type.NEXT))
                            elif key_code == ecodes.KEY_4 and self.key_4_down_time is not None:
                                duration = now - self.key_4_down_time
                                self.key_4_down_time = None
                                if duration < self.hold_threshold:
                                    events.append(InputEvent(InputEvent.Type.SETTINGS_MENU))
            except BlockingIOError:
                # No events available from this device (non-blocking read) - this is normal
                continue
            except Exception as exc:
                self._log.warning(f"Error reading evdev events from {device.name}: {exc}", exc_info=True)
                continue
        
        if total_event_count > 0:
            self._log.debug(f"Evdev processed {total_event_count} events from {len(self.devices)} device(s), generated {len(events)} InputEvents")
        
        # Check for hold-to-seek events
        events.extend(self.poll_seeking())
        
        return events
    
    def poll_seeking(self) -> list[InputEvent]:
        """Check if any keys are being held for seeking or sample mode actions."""
        events = []
        now = time.time()
        
        if self.sample_mode:
            if self.key_1_down_time is not None:
                held_duration = now - self.key_1_down_time
                if held_duration >= self.hold_threshold:
                    if not self.key_1_seeking:
                        self.key_1_seeking = True
                        self.key_1_down_time = None
                        events.append(InputEvent(InputEvent.Type.UNDO_MARKER))
            if self.key_4_down_time is not None:
                held_duration = now - self.key_4_down_time
                if held_duration >= self.hold_threshold:
                    self.key_4_down_time = None
                    events.append(InputEvent(InputEvent.Type.EXIT_SAMPLE_MODE))
        else:
            if self.key_1_down_time is not None:
                held_duration = now - self.key_1_down_time
                if held_duration >= self.hold_threshold:
                    if not self.key_1_seeking:
                        self.key_1_seeking = True
                    events.append(InputEvent(InputEvent.Type.SEEK_LEFT))
            if self.key_3_down_time is not None:
                held_duration = now - self.key_3_down_time
                if held_duration >= self.hold_threshold:
                    if not self.key_3_seeking:
                        self.key_3_seeking = True
                    events.append(InputEvent(InputEvent.Type.SEEK_RIGHT))
            if self.key_4_down_time is not None:
                held_duration = now - self.key_4_down_time
                if held_duration >= self.hold_threshold:
                    self.key_4_down_time = None
                    events.append(InputEvent(InputEvent.Type.ENTER_SAMPLE_MODE))
        
        return events
    
    def __del__(self) -> None:
        """Release device grabs on cleanup."""
        for device in self.devices:
            try:
                device.ungrab()
            except Exception:
                pass

