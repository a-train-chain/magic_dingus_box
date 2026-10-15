#pragma once
#include <json/json.h>

namespace app {
struct AppState;

// The kiosk status document, schema 1 — what StatusWriter persists as
// kiosk_status.json and what the engine publishes as `snapshot`/`status`.
// Deliberately excludes `ts`: the serialized body without a timestamp is the
// change-detection key both callers compare.
Json::Value build_status_json(const AppState& state);
}  // namespace app
