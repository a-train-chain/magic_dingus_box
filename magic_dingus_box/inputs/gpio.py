from __future__ import annotations

try:
    from gpiozero import Button, RotaryEncoder
except Exception:  # pragma: no cover - not available on macOS
    Button = None  # type: ignore
    RotaryEncoder = None  # type: ignore

from .abstraction import InputEvent, InputProvider


class GPIOInputProvider(InputProvider):
    def __init__(self, pin_a: int, pin_b: int, pin_select: int, pin_next: int | None = None, pin_prev: int | None = None, pin_play_pause: int | None = None, pin_loop: int | None = None) -> None:
        if RotaryEncoder is None or Button is None:
            raise RuntimeError("gpiozero not available")
        self.encoder = RotaryEncoder(a=pin_a, b=pin_b, max_steps=0)
        self.btn_select = Button(pin_select, pull_up=True)
        self.btn_next = Button(pin_next, pull_up=True) if pin_next else None
        self.btn_prev = Button(pin_prev, pull_up=True) if pin_prev else None
        self.btn_play_pause = Button(pin_play_pause, pull_up=True) if pin_play_pause else None
        self.btn_loop = Button(pin_loop, pull_up=True) if pin_loop else None

    def translate(self, raw_event):  # type: ignore[no-untyped-def]
        # GPIO inputs don't go through pygame events; this provider is polled elsewhere.
        return None

    # Polling methods to consume GPIO state and emit events
    def poll(self):  # type: ignore[no-untyped-def]
        for _ in range(abs(self.encoder.steps)):
            delta = 1 if self.encoder.steps > 0 else -1
            yield InputEvent(InputEvent.Type.ROTATE, delta=delta)
        self.encoder.steps = 0
        if self.btn_select.is_pressed:
            yield InputEvent(InputEvent.Type.SELECT)
        if self.btn_next and self.btn_next.is_pressed:
            yield InputEvent(InputEvent.Type.NEXT)
        if self.btn_prev and self.btn_prev.is_pressed:
            yield InputEvent(InputEvent.Type.PREV)
        if self.btn_play_pause and self.btn_play_pause.is_pressed:
            yield InputEvent(InputEvent.Type.PLAY_PAUSE)
        if self.btn_loop and self.btn_loop.is_pressed:
            yield InputEvent(InputEvent.Type.TOGGLE_LOOP)

