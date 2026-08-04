// GENERATED DATA — do not hand-edit.
//
// The two controllers Magic Dingus Box recommends buyers use, captured with
// the on-box Controller Setup wizard on the production Pi and compiled in so
// every unit recognises them the moment they are plugged in.
//
// Regenerate after capturing a new pad you intend to ship:
//   scp magic@<box>:/opt/magic_dingus_box/config/controller_profiles.json - \
//     | python3 -c 'import json,sys;print(json.dumps(json.load(sys.stdin),indent=2,sort_keys=True))'
// and paste the result between the delimiters below.
//
// Parsed by profiles_from_json(), the same never-throws parser used for the
// on-disk file, so a mistake here degrades to "this pad has no built-in
// profile" rather than breaking the kiosk.
static constexpr const char* kBuiltinControllerProfilesJson = R"MDBJSON(
{
  "profiles": {
    "2563:0526": {
      "captured_at": "2026-07-30T02:44:33Z",
      "controls": {
        "circle": {
          "code": 305,
          "kind": "button",
          "token": "1"
        },
        "cross": {
          "code": 304,
          "kind": "button",
          "token": "0"
        },
        "dpad_down": {
          "code": 17,
          "direction": 1,
          "kind": "hat",
          "token": "h0down"
        },
        "dpad_left": {
          "code": 16,
          "direction": -1,
          "kind": "hat",
          "token": "h0left"
        },
        "dpad_right": {
          "code": 16,
          "direction": 1,
          "kind": "hat",
          "token": "h0right"
        },
        "dpad_up": {
          "code": 17,
          "direction": -1,
          "kind": "hat",
          "token": "h0up"
        },
        "l1": {
          "code": 310,
          "kind": "button",
          "token": "6"
        },
        "l2": {
          "code": 312,
          "kind": "button",
          "token": "8"
        },
        "l3": {
          "code": 317,
          "kind": "button",
          "token": "13"
        },
        "lstick_down": {
          "code": 1,
          "direction": 1,
          "kind": "axis",
          "token": "+1"
        },
        "lstick_left": {
          "code": 0,
          "direction": -1,
          "kind": "axis",
          "token": "-0"
        },
        "lstick_right": {
          "code": 0,
          "direction": 1,
          "kind": "axis",
          "token": "+0"
        },
        "lstick_up": {
          "code": 1,
          "direction": -1,
          "kind": "axis",
          "token": "-1"
        },
        "r1": {
          "code": 311,
          "kind": "button",
          "token": "7"
        },
        "r2": {
          "code": 313,
          "kind": "button",
          "token": "9"
        },
        "r3": {
          "code": 318,
          "kind": "button",
          "token": "14"
        },
        "rstick_down": {
          "code": 5,
          "direction": 1,
          "kind": "axis",
          "token": "+3"
        },
        "rstick_left": {
          "code": 2,
          "direction": -1,
          "kind": "axis",
          "token": "-2"
        },
        "rstick_right": {
          "code": 2,
          "direction": 1,
          "kind": "axis",
          "token": "+2"
        },
        "rstick_up": {
          "code": 5,
          "direction": -1,
          "kind": "axis",
          "token": "-3"
        },
        "select": {
          "code": 314,
          "kind": "button",
          "token": "10"
        },
        "square": {
          "code": 307,
          "kind": "button",
          "token": "3"
        },
        "start": {
          "code": 315,
          "kind": "button",
          "token": "11"
        },
        "triangle": {
          "code": 308,
          "kind": "button",
          "token": "4"
        }
      },
      "name": "SHANWAN Android Gamepad",
      "style": "ps_style"
    },
    "2563:0575": {
      "captured_at": "",
      "controls": {
        "n64_a": {
          "code": 305,
          "kind": "button",
          "token": "1"
        },
        "n64_b": {
          "code": 306,
          "kind": "button",
          "token": "2"
        },
        "n64_c_down": {
          "code": 304,
          "kind": "button",
          "token": "0"
        },
        "n64_c_left": {
          "code": 307,
          "kind": "button",
          "token": "3"
        },
        "n64_c_right": {
          "code": 312,
          "kind": "button",
          "token": "8"
        },
        "n64_c_up": {
          "code": 313,
          "kind": "button",
          "token": "9"
        },
        "n64_dpad_down": {
          "code": 17,
          "direction": 1,
          "kind": "hat",
          "token": "h0down"
        },
        "n64_dpad_left": {
          "code": 16,
          "direction": -1,
          "kind": "hat",
          "token": "h0left"
        },
        "n64_dpad_right": {
          "code": 16,
          "direction": 1,
          "kind": "hat",
          "token": "h0right"
        },
        "n64_dpad_up": {
          "code": 17,
          "direction": -1,
          "kind": "hat",
          "token": "h0up"
        },
        "n64_l": {
          "code": 308,
          "kind": "button",
          "token": "4"
        },
        "n64_r": {
          "code": 309,
          "kind": "button",
          "token": "5"
        },
        "n64_start": {
          "code": 316,
          "kind": "button",
          "token": "12"
        },
        "n64_stick_down": {
          "code": 1,
          "direction": 1,
          "kind": "axis",
          "token": "+1"
        },
        "n64_stick_left": {
          "code": 0,
          "direction": -1,
          "kind": "axis",
          "token": "-0"
        },
        "n64_stick_right": {
          "code": 0,
          "direction": 1,
          "kind": "axis",
          "token": "+0"
        },
        "n64_stick_up": {
          "code": 1,
          "direction": -1,
          "kind": "axis",
          "token": "-1"
        },
        "n64_z": {
          "code": 310,
          "kind": "button",
          "token": "6"
        }
      },
      "name": "SWITCH CO.,LTD. Controller (Dinput)",
      "style": "n64_style"
    }
  },
  "version": 1
}
)MDBJSON";
