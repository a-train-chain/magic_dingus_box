#pragma once

namespace video {

// Should the kiosk use the V4L2 stateless hardware HEVC decoder
// (v4l2slh265dec) instead of software avdec_h265?
//
// ANSWER: NO — on measurement, not on principle. This function exists to
// record WHY, because "the Pi has a hardware HEVC block, why aren't we
// using it?" is an obvious question that has now been investigated
// twice, and the second investigation nearly shipped a 2x CPU
// regression.
//
// History:
//   - Originally the decoder was disabled because it could not NEGOTIATE
//     at all. The Pi's rpivid driver emits Broadcom's proprietary SAND
//     tiled formats (NC12 8-bit / NC30 10-bit) and mainline GStreamer's
//     v4l2codecs didn't understand them: playbin auto-plugged the
//     element, downstream reported "format UNKNOWN", and playback never
//     reached PLAYING. The fix (MR gstreamer/gstreamer#9247) was
//     expected to make hardware HEVC "free" once GStreamer 1.26 landed.
//   - GStreamer 1.26.2 (Debian Trixie) now DOES negotiate it. Verified
//     2026-07-26 on the Pi 5 bench: playbin auto-selects the element,
//     reaches PLAYING through an RGBA sink, /dev/video19 is open, and
//     raw decode hits 162.8fps (6.79x realtime) with fakesink.
//   - But raw decode throughput is NOT the kiosk's cost. The kiosk needs
//     RGBA to upload into a GL texture, so every frame must be detiled
//     out of SAND by videoconvert on the CPU — and that costs more than
//     software decoding did. Measured on the SAME file, same output:
//
//         HEVC 1080p -> RGBA   software  41% of 400%   HARDWARE  87%
//         HEVC 1080p -> I420   software  38% of 400%   HARDWARE  73%
//
//     Hardware is ~2x more expensive end-to-end, in BOTH output formats,
//     so the SAND detiling — not the RGBA conversion — is the cost.
//
// Revisit only if one of these changes:
//   (a) the renderer learns to sample SAND/NV12 tiled buffers directly
//       in the shader (zero-copy, no videoconvert), or
//   (b) GStreamer/rpivid gains a linear (non-tiled) output format, or
//   (c) DMABUF import lets the GPU do the detiling.
// Until then software decode is both cheaper and simpler. Note this is
// comfortably sustainable either way: 41% of 400% is 0.41 of one core on
// a 4-core board, so the kiosk is not decode-limited on Pi 5.
constexpr bool hw_hevc_beneficial() {
    return false;
}

} // namespace video
