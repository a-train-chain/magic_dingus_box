#pragma once

// The clock behind the CRT shaders' time effects (interlacing field
// alternation, flicker). Header-only, pure, unit-tested on the Mac
// (tests/app/test_redraw_gate.cpp) because the redraw gate and the renderer
// must agree on it: the gate draws the static CRT menu once per interlace
// FIELD (30 fps), and that only alternates fields if both sides count
// fields off the same clock.
//
// WHY the wrap: the shaders used to receive steady_clock seconds since boot
// as a float. A float has 24 bits of mantissa, so on a box up for 3 days
// (~260k s) the uniform moves in 31 ms steps and at 10 days in 62 ms steps
// — coarser than a 33 ms field — so `mod(time * 30.0, 2.0)` stopped
// alternating and the interlace stripes froze or stuttered on long-running
// units. The shader now gets the time modulo kCrtTimeWrap: 60 s holds 1800
// fields (an even number), so the field parity is continuous across the
// wrap; flicker is noise by design and its once-a-minute phase jump is not
// visible.

#include <chrono>
#include <cstdint>

namespace ui {

constexpr std::int64_t kCrtTimeWrapUs = 60'000'000;  // 60 s
constexpr std::int64_t kCrtFieldsPerSecond = 30;     // shader: mod(time * 30.0, 2.0)
static_assert((kCrtTimeWrapUs / 1'000'000 * kCrtFieldsPerSecond) % 2 == 0,
              "the wrap must hold an even number of fields or parity flips at it");

namespace detail {
inline std::int64_t crt_clock_us(std::chrono::steady_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch())
        .count();
}
inline std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
}  // namespace detail

// Value for the CRT shaders' `time` uniform: seconds, wrapped to [0, 60).
inline float crt_shader_time(std::chrono::steady_clock::time_point tp) {
    std::int64_t us = detail::crt_clock_us(tp) % kCrtTimeWrapUs;
    if (us < 0) us += kCrtTimeWrapUs;
    return static_cast<float>(us) / 1'000'000.0f;
}

// Monotonic interlace field counter, floor(seconds * 30). Its parity equals
// the shader's floor(mod(crt_shader_time * 30, 2)) — the line set the
// interlacing darkens.
inline std::int64_t crt_field_index(std::chrono::steady_clock::time_point tp) {
    return detail::floor_div(detail::crt_clock_us(tp) * kCrtFieldsPerSecond,
                             1'000'000);
}

// Shader time for a frame pinned to interlace field `field`: the middle of
// that field, wrapped like crt_shader_time. Mid-field so the shader's
// floor(time * 30) lands on `field` with no float rounding doubt.
inline float crt_field_time(std::int64_t field) {
    const std::int64_t fields_per_wrap =
        kCrtTimeWrapUs / 1'000'000 * kCrtFieldsPerSecond;  // 1800
    std::int64_t f = field % fields_per_wrap;
    if (f < 0) f += fields_per_wrap;
    return (static_cast<float>(f) + 0.5f) / static_cast<float>(kCrtFieldsPerSecond);
}

// Which field a frame drawn at CRT field rate (30 fps) shows. The wall
// clock alone is not enough: frames are vblank-locked, fields are not, and
// when a frame lands within wakeup jitter of a field boundary two
// consecutive frames can sample the same field (or skip one) — the
// interlace stripes would then hold for two frames. So: always an ODD step
// from the last drawn field (parity alternates on every drawn frame),
// staying within one field of the wall clock.
//   wall_field: crt_field_index(now); last_field: the previous frame's.
inline std::int64_t crt_render_field(std::int64_t wall_field, std::int64_t last_field) {
    const std::int64_t d = wall_field - last_field;
    if (d > 0 && (d % 2) != 0) return wall_field;  // odd advance: on the clock
    if (d >= 2) return wall_field - 1;              // even jump: one behind
    return last_field + 1;                          // same field / backwards
}

}  // namespace ui
