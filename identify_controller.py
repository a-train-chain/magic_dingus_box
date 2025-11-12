#!/usr/bin/env python3
"""N64 Controller Button Identification Script"""

import time
import sys
import os

# Add the magic_dingus_box module to path
sys.path.insert(0, '/opt/magic_dingus_box')

try:
    from magic_dingus_box.inputs.evdev_joystick import EvdevJoystickInputProvider
    print("✓ Successfully imported evdev joystick provider")
except ImportError as e:
    print(f"✗ Failed to import evdev: {e}")
    print("Make sure you're running this on the Raspberry Pi with Magic Dingus Box installed")
    sys.exit(1)

print("\n" + "="*60)
print("N64 CONTROLLER BUTTON IDENTIFICATION")
print("="*60)
print()
print("This script will show you the exact button codes your N64 controller sends.")
print("Press each button one at a time and note down the codes.")
print()
print("INSTRUCTIONS:")
print("1. Hold the controller and press ONE button at a time")
print("2. Write down which physical button produced which code")
print("3. When done, press Ctrl+C to exit")
print()
print("Expected N64 buttons to test:")
print("- A button (bottom face button)")
print("- B button (top face button)")
print("- Z trigger (underneath, labeled Z)")
print("- L trigger (top left shoulder)")
print("- R trigger (top right shoulder)")
print("- Start button (bottom center)")
print("- D-pad (up, down, left, right)")
print("- Analog stick (push left/right/up/down)")
print("- C buttons (right side, labeled C)")
print()
print("Ready? Press Enter to start monitoring...")
input()

print("\n" + "="*60)
print("MONITORING CONTROLLER - Press buttons now!")
print("="*60)
print("Look for lines like: 'Button event - code=X (BTN_NAME)'")
print("Press Ctrl+C when done\n")

provider = EvdevJoystickInputProvider()

try:
    while True:
        events = provider.poll()
        # Small delay to avoid spam
        time.sleep(0.1)
except KeyboardInterrupt:
    print("\n" + "="*60)
    print("BUTTON IDENTIFICATION COMPLETE")
    print("="*60)
    print()
    print("Now share the results with me!")
    print("For each button you pressed, tell me:")
    print("- Which physical button you pressed")
    print("- What code number it showed")
    print("- What BTN_NAME it showed (if any)")
    print()
    print("Example:")
    print("A button: code=304 (BTN_SOUTH)")
    print("B button: code=305 (BTN_EAST)")
    print("Start button: code=315 (BTN_START)")
    print()</contents>
</xai:function_call">Create diagnostic script
