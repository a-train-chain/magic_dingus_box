"""Verifies that pressing each named button emits the correct evdev event sequence.
Uses a fake uinput device that captures writes instead of opening /dev/uinput."""

import pytest
from remote.uinput_writer import UinputWriter, ButtonName, EV_KEY, BTN_SOUTH, ABS_HAT0Y


class FakeDevice:
    def __init__(self):
        self.events = []  # list of (type, code, value) tuples

    def write(self, type_, code, value):
        self.events.append((type_, code, value))

    def syn(self):
        self.events.append(("SYN",))


@pytest.fixture
def writer():
    fake = FakeDevice()
    w = UinputWriter(device=fake)
    return w, fake


def test_ok_press_emits_btn_south_down_up(writer):
    w, fake = writer
    w.press(ButtonName.OK, phase="tap")
    # tap = down + up
    assert fake.events[0][:2] == (EV_KEY, BTN_SOUTH)
    assert fake.events[0][2] == 1            # press
    assert fake.events[1] == ("SYN",)
    assert fake.events[2][:2] == (EV_KEY, BTN_SOUTH)
    assert fake.events[2][2] == 0            # release
    assert fake.events[3] == ("SYN",)


def test_dpad_up_emits_hat_y_down_then_zero(writer):
    w, fake = writer
    w.press(ButtonName.UP, phase="tap")
    # ABS_HAT0Y, value -1 then 0
    assert fake.events[0] == (3, ABS_HAT0Y, -1)   # EV_ABS, ABS_HAT0Y, -1
    assert fake.events[2] == (3, ABS_HAT0Y, 0)


def test_phase_down_does_not_release(writer):
    w, fake = writer
    w.press(ButtonName.OK, phase="down")
    # Only down event; no auto-release
    keydowns = [e for e in fake.events if e[:2] == (EV_KEY, BTN_SOUTH) and e[2] == 1]
    keyups = [e for e in fake.events if e[:2] == (EV_KEY, BTN_SOUTH) and e[2] == 0]
    assert len(keydowns) == 1
    assert len(keyups) == 0


def test_unknown_button_raises(writer):
    w, _ = writer
    with pytest.raises(ValueError):
        w.press("BOGUS", phase="tap")  # type: ignore
