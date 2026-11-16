#pragma once

// The web admin's op log for live Tube tuning: data/tube_request.json, written
// only by the web admin, read only by the kiosk. Each op is numbered (`seq`,
// strictly increasing); the kiosk applies the ones above the highest it has
// handled, in order, each through the core's own setters.
//
//   {"version": 1, "ops": [
//     {"seq": 12, "op": "look", "look": "tv"},
//     {"seq": 13, "op": "row", "key": "hum", "value": 20},
//     {"seq": 14, "op": "mask", "mask": "dots"},
//     {"seq": 15, "op": "reset"},
//     {"seq": 16, "op": "engine", "engine": "tube"},
//     {"seq": 17, "op": "degauss"}]}
//
// Pure: no file is read or written here (the poller does that). The text comes
// from outside the kiosk, so parsing trusts nothing in it and never throws.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/app_state.h"
#include "tube/tube_rows.h"

namespace app {

// A longer file is refused unread.
inline constexpr std::size_t kTubeOpsMaxBytes = 16 * 1024;

enum class TubeOpKind { Row, Mask, Look, Reset, Engine, Degauss };

// One validated op. Only the fields its kind names are meaningful.
struct TubeOp {
    std::uint64_t seq = 0;
    TubeOpKind kind = TubeOpKind::Reset;
    tube::Effect effect = tube::Effect::brightness;  // Row
    int value = 0;                                   // Row, already is_row_value()
    tube::Mask mask = tube::Mask::grille;            // Mask
    tube::Look look = tube::kLookDefault;            // Look
    CrtEngine engine = CrtEngine::Classic;           // Engine
};

// `ops` when the whole file is valid; otherwise nullopt and `error` says why.
struct TubeOpsParse {
    std::optional<std::vector<TubeOp>> ops;
    std::string error;
};

// All or nothing: one bad op refuses the whole file. Extra fields are ignored;
// an empty `ops` array is valid. Never throws.
TubeOpsParse parse_tube_ops(std::string_view text);

// The first look after boot. Whatever the file already holds was written before
// this kiosk started (and is already in settings.json, or was never meant for
// it), so it is marked handled, never applied. `found` is nullptr when there was
// no file or it could not be used.
void adopt_tube_ops(const std::vector<TubeOp>* found, TubeLiveState& live) noexcept;

struct TubeOpsResult {
    bool changed = false;     // the engine or the tube setting differs from before
    std::size_t applied = 0;
    std::size_t skipped = 0;  // consumed but not applied (Tube not offered, refused row)
};

// Applies, in order, every op with seq above live.handled_seq, advancing
// handled_seq past each. When `offered` is false (app::tube_offered), ops are
// consumed and counted skipped without touching anything.
TubeOpsResult apply_tube_ops(const std::vector<TubeOp>& ops, AppState::DisplaySettings& display,
                             TubeLiveState& live, bool offered);

}  // namespace app
