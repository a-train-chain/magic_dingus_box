#pragma once
#include <map>
#include <string>

// GENERATED from the pre-refactor get_mapping() (Task 1, 2026-07-28).
// DO NOT EDIT BY HAND unless a mapping change is deliberate and
// hardware-verified. Regenerate with the [mapping_snapshot_gen] test.
inline const std::map<std::string, std::string>& mapping_golden() {
    static const std::map<std::string, std::string> kMappingGolden = {
        {"N64|nestopia_libretro", R"GOLD(name=NES (N64 Controller)
adm=0|drv=udev|pad=
btn=1,0,9,12,2,3,5,6,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=6,12,
extra=nestopia_audio_vol_sq1 = "100"
nestopia_audio_vol_sq2 = "100"
nestopia_audio_vol_tri = "100"
nestopia_audio_vol_noise = "100"
nestopia_audio_vol_dpcm = "100"
)GOLD"},
{"N64|snes9x2010_libretro", R"GOLD(name=Super Nintendo
adm=0|drv=udev|pad=
btn=1,3,10,12,2,0,4,5,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=6,12,
extra=)GOLD"},
{"N64|genesis_plus_gx_libretro", R"GOLD(name=Sega Genesis
adm=0|drv=udev|pad=
btn=1,3,10,12,2,4,5,6,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=6,12,
extra=)GOLD"},
{"N64|pcsx_rearmed_libretro", R"GOLD(name=PS1 (N64 Controller)
adm=0|drv=udev|pad=analog
btn=2,1,9,12,3,0,4,5,6,8
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=,,,
hotkeys=,,
extra=)GOLD"},
{"N64|mednafen_pce_fast_libretro", R"GOLD(name=PC Engine / TurboGrafx-16
adm=0|drv=udev|pad=
btn=1,0,10,12,2,3,5,6,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=6,12,
extra=)GOLD"},
{"N64|prosystem_libretro", R"GOLD(name=Atari 7800
adm=0|drv=udev|pad=
btn=1,3,10,12,2,4,5,6,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=6,12,
extra=)GOLD"},
{"N64|fbneo_libretro", R"GOLD(name=Arcade (FinalBurn Neo)
adm=0|drv=udev|pad=
btn=1,0,9,12,2,3,4,5,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=6,12,
extra=)GOLD"},
{"N64|mupen64plus_next_libretro", R"GOLD(name=Nintendo 64 (N64 pad)
adm=0|drv=udev|pad=
btn=2,3,10,12,1,4,4,5,6,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=8,0,3,9
dpad_axis=,,,
hotkeys=6,12,
extra=)GOLD"},
{"N64|parallel_n64_libretro", R"GOLD(name=Nintendo 64 (N64 pad)
adm=0|drv=udev|pad=
btn=2,3,10,12,1,4,4,5,6,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=8,0,3,9
dpad_axis=,,,
hotkeys=6,12,
extra=)GOLD"},
{"N64|flycast_libretro", R"GOLD(name=Dreamcast (N64 pad)
adm=0|drv=udev|pad=
btn=2,0,10,12,1,3,5,6,4,5
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=,,,
hotkeys=6,12,
extra=)GOLD"},
{"N64|totally_unknown_core", R"GOLD(name=Default
adm=1|drv=udev|pad=
btn=1,3,10,2,0,4,5,6,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=,,,
hotkeys=,,
extra=)GOLD"},
{"PS|nestopia_libretro", R"GOLD(name=NES (PS-style)
adm=0|drv=udev|pad=
btn=2,3,8,9,1,0,5,6,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=8,9,
extra=nestopia_audio_vol_sq1 = "100"
nestopia_audio_vol_sq2 = "100"
nestopia_audio_vol_tri = "100"
nestopia_audio_vol_noise = "100"
nestopia_audio_vol_dpcm = "100"
)GOLD"},
{"PS|snes9x2010_libretro", R"GOLD(name=Super Nintendo (PS-style)
adm=0|drv=udev|pad=
btn=2,3,8,9,1,0,4,5,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=8,9,
extra=)GOLD"},
{"PS|genesis_plus_gx_libretro", R"GOLD(name=Sega Genesis (PS-style)
adm=0|drv=udev|pad=
btn=2,3,10,9,1,0,4,5,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=8,9,
extra=)GOLD"},
{"PS|pcsx_rearmed_libretro", R"GOLD(name=PS1 (PS-style, 1:1)
adm=0|drv=udev|pad=analog
btn=2,3,8,9,1,0,4,5,6,7
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=+2,-2,+3,-3
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=8,9,
extra=)GOLD"},
{"PS|mednafen_pce_fast_libretro", R"GOLD(name=PC Engine (PS-style)
adm=0|drv=udev|pad=
btn=2,3,8,9,1,0,5,6,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=8,9,
extra=)GOLD"},
{"PS|prosystem_libretro", R"GOLD(name=Atari 7800 (PS-style)
adm=0|drv=udev|pad=
btn=2,3,8,9,1,4,5,6,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=8,9,
extra=)GOLD"},
{"PS|fbneo_libretro", R"GOLD(name=Arcade / FBNeo (PS-style)
adm=0|drv=udev|pad=
btn=2,3,8,9,1,0,4,5,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=8,9,
extra=)GOLD"},
{"PS|mupen64plus_next_libretro", R"GOLD(name=Nintendo 64 (PS-style)
adm=0|drv=udev|pad=
btn=2,3,4,9,7,0,,1,6,5
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=+2,-2,+3,-3
rs_btn=,,,
dpad_axis=,,,
hotkeys=8,9,
extra=)GOLD"},
{"PS|parallel_n64_libretro", R"GOLD(name=Nintendo 64 (PS-style)
adm=0|drv=udev|pad=
btn=2,3,4,9,7,0,,1,6,5
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=+2,-2,+3,-3
rs_btn=,,,
dpad_axis=,,,
hotkeys=8,9,
extra=)GOLD"},
{"PS|flycast_libretro", R"GOLD(name=Dreamcast (PS-style)
adm=0|drv=udev|pad=
btn=2,3,10,9,1,0,5,6,6,7
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=8,9,
extra=)GOLD"},
{"PS|totally_unknown_core", R"GOLD(name=Default
adm=0|drv=udev|pad=
btn=1,3,10,2,0,4,5,6,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=8,9,
extra=)GOLD"},
{"UNKNOWN|nestopia_libretro", R"GOLD(name=NES (N64 Controller)
adm=0|drv=udev|pad=
btn=1,0,9,12,2,3,5,6,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=6,12,
extra=nestopia_audio_vol_sq1 = "100"
nestopia_audio_vol_sq2 = "100"
nestopia_audio_vol_tri = "100"
nestopia_audio_vol_noise = "100"
nestopia_audio_vol_dpcm = "100"
)GOLD"},
{"UNKNOWN|snes9x2010_libretro", R"GOLD(name=Super Nintendo
adm=0|drv=udev|pad=
btn=1,3,10,12,2,0,4,5,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=6,12,
extra=)GOLD"},
{"UNKNOWN|genesis_plus_gx_libretro", R"GOLD(name=Sega Genesis
adm=0|drv=udev|pad=
btn=1,3,10,12,2,4,5,6,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=6,12,
extra=)GOLD"},
{"UNKNOWN|pcsx_rearmed_libretro", R"GOLD(name=PS1 (N64 Controller)
adm=0|drv=udev|pad=analog
btn=2,1,9,12,3,0,4,5,6,8
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=,,,
hotkeys=,,
extra=)GOLD"},
{"UNKNOWN|mednafen_pce_fast_libretro", R"GOLD(name=PC Engine / TurboGrafx-16
adm=0|drv=udev|pad=
btn=1,0,10,12,2,3,5,6,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=6,12,
extra=)GOLD"},
{"UNKNOWN|prosystem_libretro", R"GOLD(name=Atari 7800
adm=0|drv=udev|pad=
btn=1,3,10,12,2,4,5,6,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=6,12,
extra=)GOLD"},
{"UNKNOWN|fbneo_libretro", R"GOLD(name=Arcade (FinalBurn Neo)
adm=0|drv=udev|pad=
btn=1,0,9,12,2,3,4,5,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=-1,+1,-0,+0
hotkeys=6,12,
extra=)GOLD"},
{"UNKNOWN|mupen64plus_next_libretro", R"GOLD(name=Nintendo 64 (N64 pad)
adm=0|drv=udev|pad=
btn=2,3,10,12,1,4,4,5,6,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=8,0,3,9
dpad_axis=,,,
hotkeys=6,12,
extra=)GOLD"},
{"UNKNOWN|parallel_n64_libretro", R"GOLD(name=Nintendo 64 (N64 pad)
adm=0|drv=udev|pad=
btn=2,3,10,12,1,4,4,5,6,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=8,0,3,9
dpad_axis=,,,
hotkeys=6,12,
extra=)GOLD"},
{"UNKNOWN|flycast_libretro", R"GOLD(name=Dreamcast (N64 pad)
adm=0|drv=udev|pad=
btn=2,0,10,12,1,3,5,6,4,5
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=,,,
hotkeys=6,12,
extra=)GOLD"},
{"UNKNOWN|totally_unknown_core", R"GOLD(name=Default
adm=1|drv=udev|pad=
btn=1,3,10,2,0,4,5,6,,
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=,,,
hotkeys=,,
extra=)GOLD"},
    };
    return kMappingGolden;
}
