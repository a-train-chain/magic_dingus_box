#include "controller_profile.h"

#include <json/json.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

#include "../utils/config.h"

#ifdef __linux__
#include <linux/input-event-codes.h>  // only for the code constants; see below
#endif

namespace retroarch {

// NOTE ON PORTABILITY: this file must build on macOS for the unit tests.
// linux/input-event-codes.h does not exist there, so define the handful of
// codes we need when the header is absent (or not included, as on Mac).
#ifndef ABS_X
#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_Z 0x02
#define ABS_RZ 0x05
#define ABS_HAT0X 0x10
#define ABS_HAT0Y 0x11
#endif

namespace {
using L = LogicalControl;
using K = PhysicalBinding::Kind;

PhysicalBinding btn(uint16_t code, const char* tok) { return {K::BUTTON, code, 0, tok}; }
PhysicalBinding hat(uint16_t code, int dir, const char* tok) { return {K::HAT, code, dir, tok}; }
PhysicalBinding axis(uint16_t code, int dir, const char* tok) { return {K::AXIS, code, dir, tok}; }
}  // namespace

const PhysicalProfile& builtin_n64_adapter_profile() {
    static const PhysicalProfile p = [] {
        PhysicalProfile p;
        p.name = "SWITCH CO.,LTD. Controller (N64 adapter)";
        p.style = ControllerStyle::N64_STYLE;
        p.vid = 0x0e6d; p.pid = 0x111d;
        // Legacy 0e6d:111d adapter profile: joystick index i lives at evdev code 304+i
        // (contiguous BTN_GAMEPAD range). The physical layout used to be
        // a separate table at the top of controller_mapping.cpp; that
        // table was deleted by the semantic/physical split (this task),
        // and the layout now lives here, in the profile itself.
        //
        // The 2563:0575 hardware capture (2026-07-29, see .superpowers/sdd/
        // hardware-evidence.md) confirms the capability bitmap, evdev codes,
        // and joydev token numbering: EV_KEY = 0x1fff<<48 -> codes 304..316
        // inclusive, 13 contiguous buttons; EV_ABS = 0x30027 -> bits
        // 0,1,2,5,16,17 = ABS_X/Y/Z/RZ plus a real ABS_HAT0X/Y. It does not
        // establish which physical face labels own those tokens on this
        // different built-in VID/PID. A wizard-captured profile for its
        // matching VID/PID is authoritative and overrides this legacy
        // built-in profile (and the fallback) during resolution.
        p.controls = {
            {L::N64_C_LEFT, btn(304, "0")},  {L::N64_B, btn(305, "1")},
            {L::N64_A, btn(306, "2")},       {L::N64_C_DOWN, btn(307, "3")},
            // NOTE: evdev names are misleading on this adapter -- BTN_Z
            // (evdev code 309) is physically the R shoulder, and BTN_TL
            // (evdev code 310) is physically the Z trigger. A reader
            // checking linux/input-event-codes.h against the two bindings
            // below would see N64_R at code 309 (BTN_Z) and N64_Z at code
            // 310 (BTN_TL) and might "correct" them -- don't; they are
            // right as written.
            {L::N64_L, btn(308, "4")},       {L::N64_R, btn(309, "5")},
            {L::N64_Z, btn(310, "6")},       {L::N64_C_RIGHT, btn(312, "8")},
            {L::N64_C_UP, btn(313, "9")},    {L::N64_START, btn(316, "12")},
            {L::N64_DPAD_UP, hat(ABS_HAT0Y, -1, "h0up")},
            {L::N64_DPAD_DOWN, hat(ABS_HAT0Y, +1, "h0down")},
            {L::N64_DPAD_LEFT, hat(ABS_HAT0X, -1, "h0left")},
            {L::N64_DPAD_RIGHT, hat(ABS_HAT0X, +1, "h0right")},
            {L::N64_STICK_UP, axis(ABS_Y, -1, "-1")},
            {L::N64_STICK_DOWN, axis(ABS_Y, +1, "+1")},
            {L::N64_STICK_LEFT, axis(ABS_X, -1, "-0")},
            {L::N64_STICK_RIGHT, axis(ABS_X, +1, "+0")},
        };
        return p;
    }();
    return p;
}

const PhysicalProfile& builtin_dragonrise_profile() {
    static const PhysicalProfile p = [] {
        PhysicalProfile p;
        p.name = "DragonRise Generic USB Joystick";
        p.style = ControllerStyle::PS_STYLE;
        p.vid = 0x0079; p.pid = 0x0006;
        // Joystick index i lives at evdev code 288+i (BTN_TRIGGER range,
        // see input_manager.cpp map_button_to_action comment block --
        // that function documents BUTTON codes only, nothing about axes
        // or hats, so the HAT/AXIS assignments below are this profile's
        // own, not transcribed from anywhere).
        //
        // Right-stick tokens +2/+3: NOT merely "legacy" -- per
        // .superpowers/sdd/hardware-evidence.md these are the correct
        // joydev axis INDICES for ABS_Z (raw evdev code 2) / ABS_RZ (raw
        // evdev code 5) respectively. controller_mapping.cpp's DragonRise
        // comment ("Right stick: axes 2 (Rx) / 5 (Ry)") looks like it
        // disagrees but is quoting RAW ABS codes -- a different numbering
        // system from joydev indices: ABS_Z (code 2) is joydev axis index
        // 2, ABS_RZ (code 5) is joydev axis index 3. Both the comment and
        // these tokens are correct; no fix needed.
        p.controls = {
            {L::TRIANGLE, btn(288, "0")}, {L::CIRCLE, btn(289, "1")},
            {L::CROSS, btn(290, "2")},    {L::SQUARE, btn(291, "3")},
            {L::L1, btn(292, "4")},       {L::R1, btn(293, "5")},
            {L::L2, btn(294, "6")},       {L::R2, btn(295, "7")},
            {L::SELECT, btn(296, "8")},   {L::START, btn(297, "9")},
            // PROVISIONAL / UNVERIFIED: assumes a real hat (ABS_HAT0X/Y).
            // input_manager.cpp's axis_is_8bit handling documents that THIS
            // SAME VID/PID (0079:0006) can instead report its d-pad via
            // ABS_X/Y extremes in an 8-bit (0..255) range, with no hat at
            // all. No DragonRise pad was on hand when hardware-evidence.md was
            // captured, and none was attached when controller_probe ran
            // on 2026-07-29 either, so which revision this shipped pad
            // actually is REMAINS OPEN. Run
            // `controller_probe /dev/input/eventN` with the pad plugged
            // in: it prints each axis's range and whether ABS_HAT0X/Y is
            // present, which answers it outright. If the 8-bit variant is
            // confirmed, these four bindings must change to AXIS on
            // ABS_X/Y instead of HAT.
            //
            // DO NOT try to infer the answer from the axis range alone.
            // controller_probe measured the N64-style adapter attached to
            // the bench Pi (2563:0575) reporting ABS_X/Y/Z/RZ as
            // range=[0..255] AND a real ABS_HAT0X/Y at the same time --
            // 8-bit axes and a genuine hat coexist happily on one pad, so
            // "min==0 && max<=255" says nothing about where the d-pad is.
            // (input_manager.cpp's open_joystick_devices() keys its
            // d-pad-on-ABS_X/Y handling off exactly that range test, so
            // axis_is_8bit is set for the N64 adapter too; harmless there,
            // because pushing that pad's analog stick to an extreme to
            // navigate the kiosk menu is the documented behavior -- but it
            // is not evidence about any pad's hat.)
            {L::DPAD_UP, hat(ABS_HAT0Y, -1, "h0up")},
            {L::DPAD_DOWN, hat(ABS_HAT0Y, +1, "h0down")},
            {L::DPAD_LEFT, hat(ABS_HAT0X, -1, "h0left")},
            {L::DPAD_RIGHT, hat(ABS_HAT0X, +1, "h0right")},
            {L::LSTICK_UP, axis(ABS_Y, -1, "-1")},
            {L::LSTICK_DOWN, axis(ABS_Y, +1, "+1")},
            {L::LSTICK_LEFT, axis(ABS_X, -1, "-0")},
            {L::LSTICK_RIGHT, axis(ABS_X, +1, "+0")},
            {L::RSTICK_UP, axis(ABS_RZ, -1, "-3")},
            {L::RSTICK_DOWN, axis(ABS_RZ, +1, "+3")},
            {L::RSTICK_LEFT, axis(ABS_Z, -1, "-2")},
            {L::RSTICK_RIGHT, axis(ABS_Z, +1, "+2")},
        };
        return p;
    }();
    return p;
}

platform::MenuNavOverlay menu_overlay_from_profile(const PhysicalProfile& p) {
    using platform::InputAction;
    platform::MenuNavOverlay o;

    // Only a BUTTON-kind binding can name an EV_KEY code. A control captured
    // as an AXIS or HAT is skipped entirely rather than contributing a code
    // that InputManager would then interpret in the wrong event type.
    auto add_btn = [&](L c, InputAction a) {
        const PhysicalBinding* b = p.binding(c);
        if (b && b->kind == K::BUTTON) o.buttons[b->code] = a;
    };

    // Mirrors input_manager.cpp map_button_to_action's operator semantics.
    // Two logical controls may map to the same action (Cross and Start both
    // confirm); the PS and N64 vocabularies are disjoint, so listing both
    // here is safe for either style -- only one family is ever bound.
    add_btn(L::CROSS, InputAction::SELECT);
    add_btn(L::START, InputAction::SELECT);
    add_btn(L::N64_A, InputAction::SELECT);
    add_btn(L::N64_START, InputAction::SELECT);
    add_btn(L::CIRCLE, InputAction::SETTINGS_MENU);
    add_btn(L::N64_B, InputAction::SETTINGS_MENU);
    add_btn(L::TRIANGLE, InputAction::PLAY_PAUSE);
    add_btn(L::N64_Z, InputAction::PLAY_PAUSE);
    add_btn(L::R1, InputAction::NEXT);
    add_btn(L::N64_R, InputAction::NEXT);
    add_btn(L::L1, InputAction::PREV);
    add_btn(L::N64_L, InputAction::PREV);

    // Only an AXIS-kind binding names an ABS code the overlay can treat as
    // an analog stick. A HAT-kind binding is excluded on purpose: hats
    // already have dedicated, hardware-proven handling in poll().
    auto stick_axis = [&](L c) -> int {
        const PhysicalBinding* b = p.binding(c);
        return (b && b->kind == K::AXIS) ? static_cast<int>(b->code) : -1;
    };
    const bool n64 = (p.style == ControllerStyle::N64_STYLE);
    o.nav_x_abs = stick_axis(n64 ? L::N64_STICK_RIGHT : L::LSTICK_RIGHT);
    o.seek_abs = stick_axis(n64 ? L::N64_C_RIGHT : L::RSTICK_RIGHT);
    return o;
}

std::string vidpid_key(uint16_t vid, uint16_t pid) {
    char buf[10];
    std::snprintf(buf, sizeof(buf), "%04x:%04x", vid, pid);
    return buf;
}

namespace {

const char* kind_key(PhysicalBinding::Kind k) {
    switch (k) {
        case PhysicalBinding::Kind::BUTTON: return "button";
        case PhysicalBinding::Kind::HAT: return "hat";
        case PhysicalBinding::Kind::AXIS: return "axis";
    }
    return "button";
}

std::optional<PhysicalBinding::Kind> kind_from_key(const std::string& s) {
    if (s == "button") return PhysicalBinding::Kind::BUTTON;
    if (s == "hat") return PhysicalBinding::Kind::HAT;
    if (s == "axis") return PhysicalBinding::Kind::AXIS;
    return std::nullopt;
}

// Parse a 4-hex-digit field of a "vvvv:pppp" vidpid_key(). Never throws --
// std::stoul throws on a malformed key (e.g. non-hex characters), which
// would otherwise propagate out of profiles_from_json and violate its
// never-throw contract.
std::optional<uint16_t> parse_hex4_field(const std::string& s) {
    try {
        size_t pos = 0;
        unsigned long v = std::stoul(s, &pos, 16);
        if (pos != s.size() || v > 0xFFFF) return std::nullopt;
        return static_cast<uint16_t>(v);
    } catch (...) {
        return std::nullopt;
    }
}

// jsoncpp's Value::asString()/asUInt()/asInt() THROW (Json::LogicError, by
// default build config) when the value's actual JSON type isn't convertible
// -- e.g. a "code" field written as a string or an array. A corrupt or
// hand-edited controller_profiles.json can easily contain a field with the
// wrong JSON type, so every field read in profiles_from_json goes through
// one of these wrappers, which catch that and fall back to `def` instead of
// letting the exception escape.
std::string safe_str(const Json::Value& obj, const char* key, const std::string& def) {
    try {
        return obj.get(key, def).asString();
    } catch (...) {
        return def;
    }
}

unsigned safe_uint(const Json::Value& obj, const char* key, unsigned def) {
    try {
        return obj.get(key, def).asUInt();
    } catch (...) {
        return def;
    }
}

int safe_int(const Json::Value& obj, const char* key, int def) {
    try {
        return obj.get(key, def).asInt();
    } catch (...) {
        return def;
    }
}

}  // namespace

std::string profiles_to_json(const std::map<std::string, PhysicalProfile>& profiles) {
    Json::Value root;
    root["version"] = 1;
    Json::Value& out = root["profiles"] = Json::Value(Json::objectValue);
    for (const auto& [key, p] : profiles) {
        Json::Value jp;
        jp["name"] = p.name;
        jp["style"] = controller_style_key(p.style);
        jp["captured_at"] = p.captured_at;
        Json::Value& jc = jp["controls"] = Json::Value(Json::objectValue);
        for (const auto& [control, b] : p.controls) {
            Json::Value jb;
            jb["kind"] = kind_key(b.kind);
            jb["code"] = b.code;
            if (b.direction != 0) jb["direction"] = b.direction;
            jb["token"] = b.token;
            jc[logical_control_key(control)] = jb;
        }
        // Write under the CALLER's map key, as-is. std::map guarantees the
        // caller's keys are unique, but two distinct entries can easily
        // share the same vid/pid fields (e.g. both cloned from a builtin
        // template without overriding vid/pid, or both left at the
        // zero-initialized default) -- deriving the on-disk key from
        // p.vid/p.pid instead of the caller's key would make the second
        // such entry silently overwrite the first, with no error and no
        // log. Writing the caller's guaranteed-unique key means distinct
        // entries can never collide on disk; profiles_from_json's
        // canonicalization (see below) already self-corrects a
        // non-canonical key spelling on the next load.
        out[key] = jp;
    }
    Json::StreamWriterBuilder w;
    w["indentation"] = "  ";
    return Json::writeString(w, root);
}

// NEVER throws and never propagates a parse failure -- every malformed
// piece (bad JSON, a non-object "profiles" node, a non-object profile entry,
// a map key that isn't a valid "vvvv:pppp" vid/pid pair, an unknown style,
// an unknown control key, or an unknown binding kind) degrades to skipping
// just that piece, so a corrupt controller_profiles.json can never prevent
// the kiosk from booting.
std::map<std::string, PhysicalProfile> profiles_from_json(const std::string& text) {
    std::map<std::string, PhysicalProfile> result;
    if (text.empty()) return result;

    Json::Value root;
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream in(text);
    if (!Json::parseFromStream(rb, in, &root, &errs)) return result;
    // A syntactically valid JSON document that isn't an object (e.g. a bare
    // "42" or "[1,2,3]") makes root's type non-object -- jsoncpp's
    // operator[] asserts (and, with exceptions enabled, THROWS) if called on
    // a non-object/non-null Value, so this check must come before touching
    // root["profiles"] at all, not just before using its result.
    if (!root.isObject()) return result;

    const Json::Value& profiles = root["profiles"];
    if (!profiles.isObject()) return result;

    for (const auto& key : profiles.getMemberNames()) {
        const Json::Value& jp = profiles[key];
        if (!jp.isObject()) continue;  // malformed entry: skip just this one

        // vid/pid parsed back from the "vvvv:pppp" map key.
        if (key.size() != 9 || key[4] != ':') continue;
        auto vid = parse_hex4_field(key.substr(0, 4));
        auto pid = parse_hex4_field(key.substr(5, 4));
        if (!vid || !pid) continue;

        auto style = controller_style_from_key(safe_str(jp, "style", ""));
        if (!style) continue;  // unknown/malformed style: skip this profile

        PhysicalProfile p;
        p.name = safe_str(jp, "name", "");
        p.style = *style;
        p.vid = *vid;
        p.pid = *pid;
        p.captured_at = safe_str(jp, "captured_at", "");

        const Json::Value& jc = jp["controls"];
        if (jc.isObject()) {
            for (const auto& ck : jc.getMemberNames()) {
                auto control = logical_control_from_key(ck);
                if (!control) continue;  // unknown control key: skip it

                const Json::Value& jb = jc[ck];
                if (!jb.isObject()) continue;
                auto kind = kind_from_key(safe_str(jb, "kind", ""));
                if (!kind) continue;  // unknown/malformed binding kind: skip it

                PhysicalBinding b;
                b.kind = *kind;
                b.code = static_cast<uint16_t>(safe_uint(jb, "code", 0));
                b.direction = safe_int(jb, "direction", 0);
                b.token = safe_str(jb, "token", "");
                p.controls[*control] = b;
            }
        }
        // Key by the CANONICAL vidpid_key derived from the parsed vid/pid,
        // not by the raw JSON key text. vidpid_key() always emits lowercase
        // hex, and resolve_mapping_for_pad's lookup (controller_mapping.cpp)
        // always builds its lookup key the same way -- but the parser above
        // accepts upper- and lower-case hex equally. Without this, a
        // profile stored under an uppercase-hex key (e.g. hand-edited, or
        // written by some future non-canonical writer) parses successfully
        // but is silently unreachable by lookup: resolution falls all the
        // way through to the legacy/builtin fallback with no error.
        result[vidpid_key(*vid, *pid)] = p;
    }
    return result;
}

std::map<std::string, PhysicalProfile> load_profile_store() {
    std::ifstream f(config::get_controller_profiles_file());
    if (!f.good()) return {};  // missing/unreadable file: empty store, not an error
    std::ostringstream ss;
    ss << f.rdbuf();
    return profiles_from_json(ss.str());
}

bool save_profile_store(const std::map<std::string, PhysicalProfile>& profiles) {
    const std::string path = config::get_controller_profiles_file();

    // Ensure the config directory exists (mirrors SettingsPersistence's
    // save path) -- a fresh box or a test pointed at a not-yet-created
    // directory must not crash the wizard's save step.
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return false;
    }

    const std::string tmp = path + ".tmp";
    {
        // Scoped so the file is fully flushed and closed (RAII) before the
        // rename below -- renaming over a still-open/unflushed handle would
        // risk a truncated destination file.
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.good()) return false;
        f << profiles_to_json(profiles);
        f.close();
        if (f.fail()) {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace retroarch
