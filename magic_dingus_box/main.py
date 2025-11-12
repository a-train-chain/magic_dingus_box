from __future__ import annotations

import logging
import os
import sys
import threading
import time
import signal
from typing import List

import pygame

from .config import AppConfig
from .persistence.settings_store import SettingsStore
from .logging_setup import setup_logging
from .library.loader import PlaylistLibrary
from .library.watcher import PlaylistWatcher
from .player.mpv_client import MpvClient
from .player.controller import PlaybackController
from .player.sample_mode import SampleModeManager
from .ui.renderer import UIRenderer
from .ui.startup_animation import StartupAnimation
from .ui.settings_menu import SettingsMenuManager, MenuSection
from .ui.settings_renderer import SettingsMenuRenderer
from .inputs.keyboard import KeyboardInputProvider
from .inputs.gpio import GPIOInputProvider  # type: ignore
from .inputs.joystick import JoystickInputProvider
try:
    from .inputs.evdev_keyboard import EvdevKeyboardInputProvider
    from .inputs.evdev_joystick import EvdevJoystickInputProvider
    EVDEV_AVAILABLE = True
except ImportError:
    EVDEV_AVAILABLE = False
from .web.admin import create_app
from .display.display_manager import DisplayManager, DisplayMode
from .display.bezel_loader import BezelLoader
from .display.crt_effects import CRTEffectsManager
from .display.window_manager import WindowManager
from .display.transition_manager import TransitionManager


def run() -> None:
    # CRITICAL: Prevent multiple instances from running simultaneously
    # Create a lock file to ensure only one instance runs at a time
    # Do this BEFORE logging setup since we need to exit early if duplicate
    import fcntl
    lock_file_path = "/tmp/magic_dingus_ui.lock"
    try:
        lock_file = open(lock_file_path, 'w')
        try:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            # Write PID to lock file
            lock_file.write(str(os.getpid()) + '\n')
            lock_file.flush()
        except (IOError, BlockingIOError, OSError):
            # Another instance is already running
            lock_file.close()
            # Use print since logging isn't set up yet
            print("Another instance of Magic Dingus UI is already running. Exiting.", file=sys.stderr)
            sys.exit(1)
    except Exception as lock_exc:
        # Use print since logging isn't set up yet
        print(f"Could not create lock file: {lock_exc}", file=sys.stderr)
        # Continue anyway, but log the warning
    
    # CRITICAL: Check if RetroArch is running BEFORE any initialization
    # Exit immediately if RetroArch is active to prevent UI/intro from interfering
    # Note: os and sys are imported at module level, so they're available here
    retroarch_lock_file = "/tmp/magic_retroarch_active.lock"
    if os.path.exists(retroarch_lock_file):
        try:
            # Verify the PID in the lock file is actually RetroArch
            with open(retroarch_lock_file, 'r') as f:
                lock_pid = f.read().strip()
            if lock_pid:
                # Handle placeholder "starting" value (set before RetroArch launches)
                if lock_pid == "starting":
                    # Lock file exists but RetroArch hasn't started yet - wait briefly then check
                    # Use module-level time import (already imported at top)
                    time.sleep(1)
                    # Re-read lock file in case PID was updated
                    try:
                        with open(retroarch_lock_file, 'r') as f2:
                            lock_pid = f2.read().strip()
                    except Exception:
                        # If we can't re-read, assume stale and remove it
                        try:
                            os.remove(retroarch_lock_file)
                        except Exception:
                            pass
                        lock_pid = ""
                
                if lock_pid and lock_pid != "starting":
                    # Check if process is still running (use basic check, no logging yet)
                    try:
                        import subprocess
                        result = subprocess.run(
                            ["ps", "-p", lock_pid, "-o", "comm="],
                            capture_output=True,
                            text=True,
                            timeout=1.0
                        )
                        if result.returncode == 0 and result.stdout and "retroarch" in result.stdout.lower():
                            # RetroArch is running - exit immediately without any initialization
                            sys.exit(0)
                        else:
                            # Lock file exists but process is dead - remove stale lock
                            try:
                                os.remove(retroarch_lock_file)
                            except Exception:
                                pass
                    except Exception:
                        # If ps command fails, assume process is dead and remove stale lock
                        try:
                            os.remove(retroarch_lock_file)
                        except Exception:
                            pass
                elif lock_pid == "starting":
                    # Still "starting" after wait - likely stale, remove it
                    try:
                        os.remove(retroarch_lock_file)
                    except Exception:
                        pass
            else:
                # Lock file exists but is empty - remove it
                try:
                    os.remove(retroarch_lock_file)
                except Exception:
                    pass
        except (IOError, OSError, PermissionError) as e:
            # File permission or I/O error - try to remove lock file if we can
            # Don't exit on these errors - they're likely stale locks or permission issues
            try:
                os.remove(retroarch_lock_file)
            except Exception:
                pass
            # Continue execution - don't exit on file errors
        except Exception:
            # Other unexpected errors - be defensive and remove lock file
            # Don't exit on unknown errors - they might be transient issues
            try:
                os.remove(retroarch_lock_file)
            except Exception:
                pass
            # Continue execution - we'll check RetroArch process directly below
    
    # Also check if RetroArch process is running directly (backup check)
    try:
        import subprocess
        result = subprocess.run(
            ["pgrep", "-f", "retroarch.*--menu"],
            capture_output=True,
            text=True,
            timeout=1.0
        )
        if result.returncode == 0 and result.stdout.strip():
            # RetroArch is running - exit immediately
            sys.exit(0)
    except Exception:
        pass
    
    # Now safe to initialize
    config = AppConfig()
    setup_logging(config)
    log = logging.getLogger("magic.main")

    config.ensure_data_dirs()
    
    # Load persistent settings
    settings_store = SettingsStore(config.settings_file)
    # Ensure sensible defaults: bezel on (modern_bezel), retro_tv_1, no CRT effects
    settings_store.ensure_defaults({
        "display_mode": "modern_bezel",
        "modern_resolution": "auto",
        "bezel_style": "retro_tv_1",
        # CRT effects all off by default
        "scanlines_mode": "off",
        "color_warmth": 0.0,
        "phosphor_glow": 0.0,
        "phosphor_mask": 0.0,
        "screen_bloom": 0.0,
        "interlacing": 0.0,
        "screen_flicker": 0.0,
        # Playlist video settings (separate from intro video)
        # Options: "desync" (fastest, no sync - RECOMMENDED for Pi), "audio" (sync to audio), 
        #          "display-resample" (CPU intensive, can be slow), "display-vdrop" (drop frames)
        "playlist_video_sync": "desync",  # Default: same as intro video for best performance
        "playlist_video_latency_hacks": False  # Default: disabled (can cause slowdowns)
    })
    
    # Determine display mode (settings override env vars)
    display_mode_str = settings_store.get_display_mode()
    if display_mode_str == "modern_clean":
        display_mode = DisplayMode.MODERN_CLEAN
    elif display_mode_str == "modern_bezel":
        display_mode = DisplayMode.MODERN_WITH_BEZEL
    else:
        display_mode = DisplayMode.CRT_NATIVE

    # Init pygame (let SDL auto-detect best driver)
    # mpv uses X11 for video, pygame handles UI
    pygame.init()
    
    # Auto-detect screen resolution for modern modes
    if display_mode != DisplayMode.CRT_NATIVE:
        display_info = pygame.display.Info()
        # Use current display size, or user's configured size if set
        modern_res_str = settings_store.get_modern_resolution()
        if modern_res_str == "auto":
            # Auto-detect actual screen size but clamp to 1280x720 for performance
            detected = (display_info.current_w, display_info.current_h)
            if detected[0] > 1280 or detected[1] > 720:
                modern_resolution = (1280, 720)
                log.info(f"Auto-detected {detected[0]}x{detected[1]} -> clamped to {modern_resolution[0]}x{modern_resolution[1]} for performance")
            else:
                modern_resolution = detected
                log.info(f"Auto-detected display resolution: {modern_resolution[0]}x{modern_resolution[1]}")
        else:
            modern_resolution = config._parse_resolution(modern_res_str)
    else:
        modern_resolution = (config.screen_width, config.screen_height)
    
    pygame.display.set_caption("Magic Dingus Box")
    # Always use FULLSCREEN to fill entire screen at 1280x720 resolution
    # FULLSCREEN ensures the window fills the entire display
    flags = pygame.FULLSCREEN
    
    # Create screen with appropriate resolution
    if display_mode == DisplayMode.CRT_NATIVE:
        screen = pygame.display.set_mode((config.screen_width, config.screen_height), flags)
        target_resolution = (config.screen_width, config.screen_height)
    else:
        screen = pygame.display.set_mode(modern_resolution, flags)
        target_resolution = modern_resolution
    
    pygame.mouse.set_visible(False)
    
    # CRITICAL: Ensure window is fullscreen-sized and positioned immediately after creation
    # This prevents the window from appearing in the upper-left corner on boot
    # On boot, window manager may not be ready immediately, so we retry multiple times
    if config.platform == "linux":
        def ensure_fullscreen_window(pg_id, resolution, max_attempts=5):
            """Ensure window is fullscreen with retries for boot-time reliability."""
            import subprocess
            env = os.environ.copy()
            env["DISPLAY"] = ":0"
            for attempt in range(max_attempts):
                try:
                    # Force window to fullscreen size and position
                    result1 = subprocess.run(
                        ["xdotool", "windowsize", pg_id, str(resolution[0]), str(resolution[1])],
                        timeout=1.0, check=False, env=env, capture_output=True
                    )
                    result2 = subprocess.run(
                        ["xdotool", "windowmove", pg_id, "0", "0"],
                        timeout=1.0, check=False, env=env, capture_output=True
                    )
                    # Set maximized state hints to ensure fullscreen
                    subprocess.run(
                        ["xprop", "-id", pg_id, "-f", "_NET_WM_STATE", "32a", "-set", "_NET_WM_STATE", "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ"],
                        timeout=0.2, check=False, env=env
                    )
                    # Verify window size (on boot, xdotool might not work immediately)
                    if result1.returncode == 0 and result2.returncode == 0:
                        log.info(f"Set pygame window {pg_id} to fullscreen: {resolution[0]}x{resolution[1]} at (0,0) (attempt {attempt + 1})")
                        return True
                    elif attempt < max_attempts - 1:
                        log.debug(f"Fullscreen positioning attempt {attempt + 1} failed, retrying...")
                        time.sleep(0.3)  # Wait longer between attempts on boot
                except Exception as exc:
                    if attempt < max_attempts - 1:
                        log.debug(f"Fullscreen positioning attempt {attempt + 1} failed: {exc}, retrying...")
                        time.sleep(0.3)
                    else:
                        log.warning(f"Could not set initial fullscreen after {max_attempts} attempts: {exc}")
            return False
        
        try:
            # Get window ID
            wm_info = pygame.display.get_wm_info()
            if "window" in wm_info:
                pg_id = str(wm_info["window"])
                # Wait a moment for window to be fully created (longer on boot)
                time.sleep(0.5)
                # Retry fullscreen positioning (important on boot when window manager may not be ready)
                ensure_fullscreen_window(pg_id, modern_resolution, max_attempts=5)
        except Exception as fullscreen_exc:
            log.warning(f"Could not set initial fullscreen: {fullscreen_exc}")
    
    # Window properties will be set after window_mgr is created (see below)
    
    # Hide cursor at X11 level (more aggressive, persists during transitions)
    if config.platform == "linux":
        try:
            import subprocess
            # Note: os is already imported at module level
            env = os.environ.copy()
            env["DISPLAY"] = ":0"
            # Hide cursor using unclutter or xdotool
            subprocess.run(
                ["unclutter", "-idle", "0", "-root"],
                timeout=2.0, check=False, env=env
            )
            # Also use xdotool to hide cursor
            subprocess.run(
                ["xdotool", "search", "--name", ".*", "key", "XF86ScreenSaver"],
                timeout=1.0, check=False, env=env
            )
        except Exception:
            pass
        # Set cursor to blank using xsetroot
        try:
            subprocess.run(
                ["xsetroot", "-cursor_name", "none"],
                timeout=1.0, check=False
            )
        except Exception:
            pass
    
    # Get pygame window ID for direct window management
    pygame_window_id = None
    if config.platform == "linux":
        try:
            wm_info = pygame.display.get_wm_info()
            if "window" in wm_info:
                pygame_window_id = str(wm_info["window"])
                log.info(f"Got pygame window ID: {pygame_window_id}")
                # Set persistent window hints immediately after window creation
                window_mgr_temp = WindowManager(debounce_ms=0.0, pygame_window_id=pygame_window_id)
                window_mgr_temp.set_window_undecorated_persistent(pygame_window_id)
                # Hide cursor aggressively
                window_mgr_temp.hide_cursor_aggressive()
        except Exception as wid_exc:
            log.warning(f"Could not get pygame window ID: {wid_exc}")
    
    # Initialize display manager
    display_mgr = DisplayManager(display_mode, target_resolution)
    
    # Initialize window manager for X11 stacking control
    window_mgr = WindowManager(debounce_ms=200.0, pygame_window_id=pygame_window_id)
    
    # Set window properties immediately after window_mgr is created
    # This prevents any visible decorations during window creation
    if config.platform == "linux" and pygame_window_id:
        try:
            # Remove decorations MULTIPLE times immediately after window creation
            # Openbox can be stubborn, so we need to be aggressive
            for _ in range(5):
                window_mgr.remove_window_decorations(pygame_window_id)
                time.sleep(0.02)  # Small delay between attempts
            # Set window type to prevent decorations
            import subprocess
            # Note: os is already imported at module level
            env = os.environ.copy()
            env["DISPLAY"] = ":0"
            subprocess.run(
                ["xprop", "-id", pygame_window_id, "-f", "_NET_WM_WINDOW_TYPE", "32a", "-set", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_SPLASH"],
                timeout=0.2, check=False, env=env
            )
            # Set maximized state to prevent resizing
            subprocess.run(
                ["xprop", "-id", pygame_window_id, "-f", "_NET_WM_STATE", "32a", "-set", "_NET_WM_STATE", "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ"],
                timeout=0.2, check=False, env=env
            )
            # Remove decorations one more time after setting properties
            window_mgr.remove_window_decorations(pygame_window_id)
            # CRITICAL: Ensure window has focus for controller input
            try:
                import subprocess
                env = os.environ.copy()
                env["DISPLAY"] = ":0"
                subprocess.run(
                    ["xdotool", "windowactivate", pygame_window_id],
                    timeout=0.5, check=False, env=env
                )
                subprocess.run(
                    ["xdotool", "windowfocus", pygame_window_id],
                    timeout=0.5, check=False, env=env
                )
                log.debug("Focused pygame window for controller input")
            except Exception:
                pass
            log.debug(f"Set window properties immediately after creation: {pygame_window_id}")
        except Exception as win_prop_exc:
            log.debug(f"Could not set initial window properties: {win_prop_exc}")
    
    # Initialize transition manager for seamless mpv <-> UI transitions
    screen_width, screen_height = target_resolution
    transition_mgr = TransitionManager(window_mgr, screen_width, screen_height)
    
    # Initialize bezel loader
    bezel_loader = BezelLoader(config.repo_root / "assets")
    
    # Initialize CRT effects manager
    crt_effects = CRTEffectsManager()
    crt_effects.load_settings(settings_store)
    
    # Load bezel if needed
    bezel = None
    bezel_overlay_file = None
    if display_mode == DisplayMode.MODERN_WITH_BEZEL:
        # Try to load bezel from file first
        bezel_style = settings_store.get("bezel_style", "retro_tv_1")
        bezel = bezel_loader.load_bezel(bezel_style, target_resolution)
        
        if bezel:
            log.info(f"Loaded bezel: {bezel_style}")
            # Prepare a pre-scaled bezel PNG for mpv overlay when video is playing
            try:
                from pathlib import Path as _P
                # Use /tmp on Linux to avoid permission issues with /run/magic when UI runs as non-root
                overlay_path = _P("/tmp/bezel_overlay.png")
                # Create overlay image with a transparent window matching the 4:3 content area
                overlay_surface = pygame.Surface(target_resolution, pygame.SRCALPHA)
                overlay_surface.fill((0, 0, 0, 0))
                # Draw bezel first
                overlay_surface.blit(bezel, (0, 0))
                # Punch a fully transparent hole in the content area so video shows through.
                # Use BLEND_RGBA_MIN with alpha=0 to force dest alpha to 0 in that rect.
                hole_rect = pygame.Rect(
                    display_mgr.content_rect.x,
                    display_mgr.content_rect.y,
                    display_mgr.content_rect.width,
                    display_mgr.content_rect.height,
                )
                overlay_surface.fill((255, 255, 255, 0), hole_rect, special_flags=pygame.BLEND_RGBA_MIN)
                # Save the overlay
                pygame.image.save(overlay_surface, str(overlay_path))
                bezel_overlay_file = str(overlay_path)
                log.info(f"Prepared bezel overlay file: {bezel_overlay_file}")
            except Exception as _exc:
                log.warning(f"Failed to prepare bezel overlay file: {_exc}")
        else:
            # Fallback to procedural bezel
            bezel = display_mgr.generate_bezel()
            log.info("Using procedural bezel (image not found)")

    clock = pygame.time.Clock()

    # Load playlists
    library = PlaylistLibrary(config.playlists_dir)
    all_playlists = library.load_playlists()
    
    # Separate video and game playlists
    video_playlists = [p for p in all_playlists if p.is_video_playlist()]
    game_playlists = [p for p in all_playlists if p.is_game_playlist()]
    
    playlists = video_playlists  # Main UI shows only video playlists
    selected_index = 0

    # mpv + controller (hardware-accelerated playback with v4l2m2m)
    mpv_socket = config.mpv_socket
    mpv = MpvClient(mpv_socket)

    # CRITICAL: Start MPV if not already running
    # This ensures MPV is available for intro video playback
    if not mpv._connect():
        log.info("MPV not running, starting MPV service...")
        try:
            import subprocess
            # Use the same arguments as the systemd service
            mpv_cmd = [
                "/usr/bin/mpv",
                "--idle=yes",
                "--no-osc",
                "--no-osd-bar",
                "--keep-open=yes",
                "--no-config",
                f"--input-ipc-server={mpv_socket}",
                "--input-vo-keyboard=no",
                "--input-default-bindings=no",
                "--vo=x11",
                "--hwdec=v4l2m2m",
                "--video-sync=desync",
                "--video-latency-hacks=no",
                "--vd-lavc-threads=4",
                "--vd-lavc-fast=yes",
                "--vd-lavc-skiploopfilter=all",
                "--vd-lavc-skipframe=nonref",
                "--vd-lavc-dr=yes",
                "--sws-fast=yes",
                "--cache=yes",
                "--cache-secs=30",
                "--demuxer-max-bytes=500M",
                "--demuxer-max-back-bytes=500M",
                "--force-window=yes",
                "--no-border",
                "--loop-playlist=no",
                "--loop-file=no",
                "--loop=no",
                f"--audio-device={config.audio_device}",
            ]

            # Set environment
            env = os.environ.copy()
            env["DISPLAY"] = ":0"

            # Start MPV in background
            log.info("Starting MPV with command: " + " ".join(mpv_cmd))
            subprocess.Popen(mpv_cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

            # Wait for MPV to start and create socket
            for attempt in range(10):
                time.sleep(0.5)
                if mpv._connect():
                    log.info(f"MPV started successfully after {attempt + 1} attempts")
                    break
            else:
                log.error("Failed to start MPV after 10 attempts")

        except Exception as exc:
            log.error(f"Failed to start MPV: {exc}")
    else:
        log.info("MPV already running, using existing instance")
    # Auto-select HDMI audio device on Linux if available
    if config.platform == "linux":
        try:
            devices = mpv.get_property("audio-device-list") or []
            chosen = None
            # Normalize names and build priority candidates
            def norm(s: str) -> str:
                return str(s or "").lower()
            hdmi_candidates = []
            default_candidates = []
            sysdefault_candidates = []
            generic_alsa = None
            for d in devices:
                name = norm(d.get("name", ""))
                desc = norm(d.get("description", ""))
                if name == "alsa":
                    generic_alsa = d.get("name")
                if name.startswith(("alsa:", "alsa/")):
                    if "hdmi" in name or "vc4hdmi" in name or "hdmi" in desc or "vc4hdmi" in desc:
                        hdmi_candidates.append(d.get("name"))
                    elif name.startswith(("alsa:default", "alsa/default")):
                        default_candidates.append(d.get("name"))
                    elif name.startswith(("alsa:sysdefault", "alsa/sysdefault")):
                        sysdefault_candidates.append(d.get("name"))
            # Prefer specific HDMI ports (vc4hdmi0 then vc4hdmi1), else any HDMI
            def prefer_port(names: list[str], port: str) -> str | None:
                for n in names:
                    if port in n:
                        return n
                return None
            chosen = prefer_port(hdmi_candidates, "vc4hdmi0") or prefer_port(hdmi_candidates, "vc4hdmi1") or (hdmi_candidates[0] if hdmi_candidates else None)
            # Fallback chain
            if chosen is None and default_candidates:
                chosen = default_candidates[0]
            if chosen is None and sysdefault_candidates:
                chosen = sysdefault_candidates[0]
            if chosen is None and generic_alsa:
                chosen = generic_alsa
            # Apply if found
            if chosen:
                mpv.set_property("audio-device", chosen)
                log.info(f"Using HDMI audio device: {chosen}")
            else:
                log.warning("No HDMI ALSA device found; using mpv default audio device")
        except Exception as _exc:
            log.warning(f"Audio device auto-select failed: {_exc}")
    
    # Configure mpv for hardware acceleration and performance
    # Batch property setting reduces IPC overhead significantly
    try:
        mpv_properties = {
            # Hardware/decoder settings (set first)
            "hwdec": "v4l2m2m",  # Force hardware decode via V4L2
            "vd-lavc-threads": 4,  # Use more decoder threads
            "vd-lavc-fast": True,  # Fast decoding
            "vd-lavc-dr": True,  # Direct rendering
            "vd-lavc-skiploopfilter": "all",  # Skip loop filter for speed
            "vd-lavc-skipframe": "nonref",  # Skip non-reference frames
            # Video output settings
            "vo": "x11",  # Use X11 output (more reliable than gpu on Pi)
            "x11-bypass-compositor": True,  # Bypass compositor for lower latency
            "sws-fast": True,  # Fast software scaling
            # Sync/playback settings
            "video-sync": "desync",  # Play at native speed, don't sync to display
            "video-latency-hacks": False,  # Disable latency hacks (can cause slowdown)
            "video-sync-max-audio-change": 0.125,  # Tighter audio sync tolerance
            "video-sync-max-factor": 2.0,  # Limit sync factor changes
            "interpolation": False,  # Disable interpolation for performance
            "tscale": "oversample",  # Fast temporal scaling
            "speed": 1.0,  # Ensure playback speed is 1.0x (normal speed)
            # Caching settings
            "cache": True,  # Enable caching
            "cache-secs": 30,  # Cache 30 seconds
            "demuxer-max-bytes": "500M",  # Large demuxer buffer
            "demuxer-max-back-bytes": "500M",  # Large backward buffer
        }
        mpv.set_properties_batch(mpv_properties)
        log.info("mpv configured for hardware-accelerated decoding with performance optimizations")
    except Exception as exc:
        log.warning(f"Failed to configure mpv optimizations: {exc}")
    
    controller = PlaybackController(
        mpv_client=mpv,
        settings_store=settings_store,
        assets_dir=config.repo_root / "assets"
    )
    
    # Sample mode manager
    sample_mode = SampleModeManager()
    
    # Enable 1-second audio fade-in for smooth transitions (macOS/dev only to avoid CPU on Pi)
    if config.platform != "linux":
        mpv.enable_audio_fade(fade_duration=1.0)
    # Ensure normal playback speed - set this AFTER loading files
    # This will be set again when files are loaded via controller

    # Preserve mpv's service-level scaling/filters as configured in systemd

    # Manage mpv bezel overlay (helpers placed before intro so intro can use them)
    mpv_bezel_overlay_active = False
    mpv_bezel_overlay_vf_active = False
    
    # -------- Audio device helpers (ensure HDMI binding on Pi) --------
    def _choose_hdmi_device(dev_list) -> str | None:
        def norm(s: str) -> str:
            return str(s or "").lower()
        hdmi = []
        default = []
        sysdef = []
        generic = None
        for d in (dev_list or []):
            name = norm(d.get("name", ""))
            desc = norm(d.get("description", ""))
            if name == "alsa":
                generic = d.get("name")
            if name.startswith(("alsa:", "alsa/")):
                if "hdmi" in name or "vc4hdmi" in name or "hdmi" in desc or "vc4hdmi" in desc:
                    hdmi.append(d.get("name"))
                elif name.startswith(("alsa:default", "alsa/default")):
                    default.append(d.get("name"))
                elif name.startswith(("alsa:sysdefault", "alsa/sysdefault")):
                    sysdef.append(d.get("name"))
        # Prefer vc4hdmi0 then vc4hdmi1, else first HDMI
        def prefer(names, token):
            for n in names:
                if token in str(n):
                    return n
            return None
        chosen = prefer(hdmi, "vc4hdmi0") or prefer(hdmi, "vc4hdmi1") or (hdmi[0] if hdmi else None)
        if chosen is None and default:
            chosen = default[0]
        if chosen is None and sysdef:
            chosen = sysdef[0]
        if chosen is None and generic:
            chosen = generic
        return chosen
    
    def _apply_audio_device(timeout_seconds: float = 2.0) -> None:
        """Bind mpv to HDMI device and ensure AO opens; safe no-op on non-Linux."""
        if config.platform != "linux":
            return
        try:
            # Try to get available audio devices
            audio_devices = mpv.get_property("audio-device-list")
            if not audio_devices:
                log.warning("Could not get audio device list")
                return
            
            # Find HDMI audio devices
            hdmi_candidates = []
            default_candidates = []
            sysdefault_candidates = []
            generic_alsa = None
            
            for d in audio_devices:
                if not isinstance(d, dict):
                    continue
                name = d.get("name", "")
                if "hdmi" in name.lower() or "vc4hdmi" in name.lower():
                    hdmi_candidates.append(d.get("name"))
                elif name.startswith(("alsa:default", "alsa/default")):
                    default_candidates.append(d.get("name"))
                elif name.startswith(("alsa:sysdefault", "alsa/sysdefault")):
                    sysdefault_candidates.append(d.get("name"))
                elif name.startswith("alsa/"):
                    generic_alsa = d.get("name")
            
            # Prefer specific HDMI ports (vc4hdmi0 then vc4hdmi1), else any HDMI
            def prefer_port(names: list[str], port: str) -> str | None:
                for n in names:
                    if port in n:
                        return n
                return None
            chosen = prefer_port(hdmi_candidates, "vc4hdmi0") or prefer_port(hdmi_candidates, "vc4hdmi1") or (hdmi_candidates[0] if hdmi_candidates else None)
            # Fallback chain
            if chosen is None and default_candidates:
                chosen = default_candidates[0]
            if chosen is None and sysdefault_candidates:
                chosen = sysdefault_candidates[0]
            if chosen is None and generic_alsa:
                chosen = generic_alsa
            
            # Apply if found
            if chosen:
                mpv.set_property("audio-device", chosen)
                log.info(f"Applied audio device for playlist playback: {chosen}")
            else:
                log.warning("No suitable audio device found; using mpv default")
        except Exception as exc:
            log.warning(f"Failed to apply audio device: {exc}")
    def _activate_mpv_bezel_overlay() -> None:
        nonlocal mpv_bezel_overlay_active
        if mpv_bezel_overlay_active:
            return
        if display_mode != DisplayMode.MODERN_WITH_BEZEL:
            return
        if not bezel_overlay_file:
            return
        try:
            # Minimal overlay graph: no scaling, overlay full-screen bezel PNG at (0,0)
            # VLC doesn't support lavfi-complex filters
            # Bezel overlay will be handled by pygame instead
            pass
            mpv_bezel_overlay_active = True
        except Exception as exc:
            logging.getLogger("magic.main").warning(f"mpv overlay enable failed: {exc}")

    def _deactivate_mpv_bezel_overlay() -> None:
        nonlocal mpv_bezel_overlay_active
        if not mpv_bezel_overlay_active:
            return
        try:
            pass  # VLC doesn't use lavfi-complex
        except Exception:
            pass
        mpv_bezel_overlay_active = False
    
    def _activate_mpv_bezel_overlay_vf() -> None:
        nonlocal mpv_bezel_overlay_vf_active
        if mpv_bezel_overlay_vf_active:
            return
        if display_mode != DisplayMode.MODERN_WITH_BEZEL:
            return
        if not bezel_overlay_file:
            return
        try:
            # VLC doesn't support vf filters like mpv
            # Bezel overlay will be handled by pygame instead
            pass
            mpv_bezel_overlay_vf_active = True
        except Exception as exc:
            logging.getLogger("magic.main").warning(f"mpv vf overlay enable failed: {exc}")
    
    def _deactivate_mpv_bezel_overlay_vf() -> None:
        nonlocal mpv_bezel_overlay_vf_active
        if not mpv_bezel_overlay_vf_active:
            return
        # VLC doesn't use vf filters - this is a no-op
        mpv_bezel_overlay_vf_active = False

    # mpv uses X11 output - keep pygame window behind mpv using window stacking
    # When video plays, pygame window stays behind mpv but remains findable for instant UI switching
    if config.platform == "linux":
        log.info("mpv configured for hardware-accelerated video output - will coordinate with pygame")

    # Get content surface for rendering
    content_surface = display_mgr.get_render_surface()
    
    # UI renderer
    renderer = UIRenderer(screen=content_surface, config=config)
    # Set bezel mode for proper text margins
    renderer.bezel_mode = (display_mode == DisplayMode.MODERN_WITH_BEZEL)
    
    # Optional intro video playback with CRT overlay (Linux embedded mpv)
    played_intro = False
    # Declare controller variables here so they can be re-initialized after intro
    js_provider = None
    evdev_js_provider = None
    try:
        log.info("Checking intro video conditions...")
        # CRITICAL: Check RetroArch lock file again before playing intro
        # This prevents intro from starting if RetroArch was launched after UI started
        retroarch_running = False
        if os.path.exists(retroarch_lock_file):
            try:
                with open(retroarch_lock_file, 'r') as f:
                    lock_pid = f.read().strip()
                if lock_pid and lock_pid != "starting":
                    import subprocess
                    try:
                        result = subprocess.run(
                            ["ps", "-p", lock_pid, "-o", "comm="],
                            capture_output=True,
                            text=True,
                            timeout=1.0
                        )
                        if result.returncode == 0 and result.stdout and "retroarch" in result.stdout.lower():
                            log.info(f"RetroArch is running (PID: {lock_pid}) - skipping intro video")
                            retroarch_running = True
                            played_intro = False  # Don't play intro if RetroArch is running
                        else:
                            # Lock file exists but process is dead - remove stale lock
                            log.info(f"Stale RetroArch lock file detected (PID: {lock_pid} not running) - removing")
                            try:
                                os.remove(retroarch_lock_file)
                            except Exception:
                                pass
                    except Exception as ps_exc:
                        # If ps command fails, assume process is dead and remove stale lock
                        log.info(f"Could not check RetroArch process (PID: {lock_pid}): {ps_exc} - removing stale lock")
                        try:
                            os.remove(retroarch_lock_file)
                        except Exception:
                            pass
                elif lock_pid == "starting":
                    # Still "starting" - likely stale, remove it
                    log.info("Stale RetroArch lock file detected (still 'starting') - removing")
                    try:
                        os.remove(retroarch_lock_file)
                    except Exception:
                        pass
                else:
                    # Empty lock file - remove it
                    log.info("Empty RetroArch lock file detected - removing")
                    try:
                        os.remove(retroarch_lock_file)
                    except Exception:
                        pass
            except (IOError, OSError, PermissionError) as e:
                # File permission or I/O error - try to remove lock file if we can
                log.warning(f"Error reading RetroArch lock file: {e} - attempting to remove")
                try:
                    os.remove(retroarch_lock_file)
                except Exception:
                    pass
            except Exception as exc:
                # Other errors - be defensive and remove lock file
                log.warning(f"Unexpected error checking RetroArch lock file: {exc} - attempting to remove")
                try:
                    os.remove(retroarch_lock_file)
                except Exception:
                    pass
        
        from pathlib import Path
        intro_setting = settings_store.get("intro_video", None)
        # Use intro.30fps.mp4 (pre-transcoded version for consistent performance)
        intro_30fps = config.media_dir / "intro.30fps.mp4"
        intro_path = None
        
        if isinstance(intro_setting, str) and intro_setting:
            p = Path(intro_setting).expanduser()
            # Use the explicitly set path
            if p.exists():
                intro_path = p
        else:
            # Use intro.30fps.mp4 (pre-transcoded version)
            if intro_30fps.exists():
                intro_path = intro_30fps
            else:
                log.warning(f"Intro video not found: {intro_30fps}")
        
        # Only play intro if RetroArch is NOT running
        log.info(f"Intro video check: intro_path={intro_path}, retroarch_running={retroarch_running}, lock_file_exists={os.path.exists(retroarch_lock_file)}")
        if intro_path is not None and not retroarch_running:
            # Use module-level time import (already imported at top of file)
            log.info(f"Playing ONLY intro video: {intro_path}")
            
            # CRITICAL: Hide the pygame window using multiple methods since xdotool doesn't work in systemd
            log.info("Preparing for intro video - hiding pygame window using multiple methods")

            try:
                pg_id = window_mgr.pygame_window_id or pygame_window_id
                log.info(f"Pygame window ID to hide: {pg_id}")

                if pg_id:
                    import subprocess
                    env = os.environ.copy()
                    env["DISPLAY"] = ":0"

                    # Method 0: Kill the window completely first (most aggressive)
                    try:
                        subprocess.run(
                            ["xdotool", "windowkill", pg_id],
                            timeout=0.5, check=False, env=env
                        )
                        log.info(f"Killed pygame window {pg_id} completely")
                        time.sleep(0.1)  # Give time for the kill to take effect
                    except Exception as kill_exc:
                        log.warning(f"xdotool windowkill failed: {kill_exc}")

                    # Method 1: Use xprop to set window to HIDDEN state (most reliable in systemd)
                    try:
                        subprocess.run([
                            "xprop", "-id", pg_id, "-f", "_NET_WM_STATE", "32a",
                            "-set", "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN"
                        ], timeout=1.0, check=False, env=env)
                        log.info(f"Set pygame window {pg_id} to HIDDEN state using xprop")
                    except Exception as xprop_exc:
                        log.warning(f"xprop HIDDEN failed: {xprop_exc}")

                    # Method 2: Try wmctrl (more reliable than xdotool in some contexts)
                    try:
                        # Check if wmctrl is available
                        wmctrl_check = subprocess.run(["which", "wmctrl"], capture_output=True, timeout=0.5)
                        if wmctrl_check.returncode == 0:
                            subprocess.run([
                                "wmctrl", "-i", "-r", str(pg_id), "-b", "add,hidden"
                            ], timeout=1.0, check=False, env=env)
                            log.info(f"Set pygame window {pg_id} to hidden using wmctrl")
                    except Exception as wmctrl_exc:
                        log.warning(f"wmctrl failed: {wmctrl_exc}")

                    # Method 3: Try xdotool windowunmap (sometimes works when others don't)
                    try:
                        subprocess.run(
                            ["xdotool", "windowunmap", pg_id],
                            timeout=1.0, check=False, env=env
                        )
                        log.info(f"Unmapped pygame window {pg_id} using xdotool")
                    except Exception as unmap_exc:
                        log.warning(f"xdotool unmap failed: {unmap_exc}")

                    # Method 4: Move off-screen as final fallback
                    try:
                        subprocess.run(
                            ["xdotool", "windowmove", pg_id, "-10000", "-10000"],
                            timeout=1.0, check=False, env=env
                        )
                        log.info(f"Moved pygame window {pg_id} off-screen as final fallback")
                    except Exception as move_exc:
                        log.warning(f"xdotool move failed: {move_exc}")

                    # Verify the window state
                    try:
                        result = subprocess.run(
                            ["xwininfo", "-id", pg_id],
                            capture_output=True, text=True, timeout=0.5, env=env
                        )
                        if result.returncode == 0:
                            for line in result.stdout.split('\n'):
                                if 'geometry' in line.lower() or 'map' in line.lower():
                                    log.info(f"Pygame window {pg_id} state: {line.strip()}")
                        else:
                            log.info(f"Pygame window {pg_id} may be successfully hidden (xwininfo failed)")
                    except Exception as verify_exc:
                        log.info(f"Could not verify pygame window {pg_id} state: {verify_exc}")

                    log.info(f"Completed all pygame window hiding attempts for {pg_id}")
                else:
                    log.error("CRITICAL: No pygame window ID found - cannot hide window!")

            except Exception as hide_exc:
                log.error(f"CRITICAL: Exception during pygame window hiding: {hide_exc}")
            
            # CRITICAL: Stop any existing playback and clear playlist MULTIPLE times to ensure only intro plays
            log.info("DEBUG: Checking MPV client connection...")
            mpv_connected = mpv._connect()
            log.info(f"DEBUG: MPV client connected: {mpv_connected}")

            for attempt in range(3):
                try:
                    mpv.stop()
                    log.info(f"DEBUG: Called mpv.stop() (attempt {attempt + 1})")
                    # Clear any playlist that might be queued
                    import json
                    import socket
                    sock = socket.socket(socket.AF_UNIX)
                    sock.connect(config.mpv_socket)
                    sock.sendall(json.dumps({"command": ["playlist-clear"]}).encode() + b"\n")
                    sock.close()
                    time.sleep(0.1)
                except Exception as e:
                    log.warning(f"DEBUG: MPV stop/clear failed (attempt {attempt + 1}): {e}")
            log.info("Cleared mpv playlist multiple times to ensure only intro video plays")
            # Ensure we don't loop the intro
            mpv.set_loop_file(False)
            # Ensure normal playback speed and desync mode before loading
            try:
                mpv.set_property("speed", 1.0)
                mpv.set_property("video-sync", "desync")  # Force desync mode for normal speed
            except Exception:
                pass
            # Load ONLY the intro video - no playlist, no other files
            # Use loadfile with "replace" mode to ensure it replaces any existing file
            # CRITICAL: Clear playlist FIRST, then load file
            try:
                import json
                import socket
                sock = socket.socket(socket.AF_UNIX)
                sock.connect(config.mpv_socket)
                sock.sendall(json.dumps({"command": ["playlist-clear"]}).encode() + b"\n")
                sock.close()
            except Exception:
                pass
            
            # CRITICAL: Set mpv properties BEFORE loading video to ensure correct display from start
            # Force 4:3 aspect ratio FIRST (before fullscreen)
            mpv.set_property("video-aspect", "4/3")  # Force 4:3 aspect ratio
            mpv.set_property("video-zoom", 0.0)  # Reset zoom
            mpv.set_property("panscan", 0.0)  # No pan/scan - show full video with margins
            # Set fullscreen BEFORE loading video
            mpv.set_fullscreen(True)
            # Wait for fullscreen to activate
            time.sleep(0.2)
            
            # Load ONLY the 30fps intro video - verify it's the right file
            if "30fps" not in str(intro_path):
                log.error(f"ERROR: Wrong intro file selected: {intro_path} (should be intro.30fps.mp4)")
            mpv.load_file(str(intro_path))
            log.info(f"Loaded ONLY intro video: {intro_path}")

            # CRITICAL: Ensure MPV window is visible and on TOP after loading video
            # This is essential for the intro video to be seen during boot
            log.info("Ensuring MPV window is visible and on top layer...")
            time.sleep(0.5)  # Give MPV time to create window

            try:
                import subprocess
                env = os.environ.copy()
                env["DISPLAY"] = ":0"

                # Multiple attempts to find and position MPV window
                mpv_window_id = None
                for attempt in range(5):
                    result = subprocess.run(
                        ["xdotool", "search", "--class", "mpv"],
                        capture_output=True, text=True, timeout=1.0, env=env
                    )
                    if result.returncode == 0 and result.stdout.strip():
                        mpv_window_id = result.stdout.strip().split('\n')[0]
                        log.info(f"Found MPV window: {mpv_window_id} (attempt {attempt + 1})")
                        break
                    time.sleep(0.2)

                if mpv_window_id:
                    log.info(f"MPV window {mpv_window_id} found, positioning aggressively...")

                    # Use xprop to set window properties first (more reliable than xdotool during boot)
                    try:
                        # Set window to be always on top and fullscreen
                        subprocess.run([
                            "xprop", "-id", mpv_window_id, "-f", "_NET_WM_STATE", "32a",
                            "-set", "_NET_WM_STATE",
                            "_NET_WM_STATE_ABOVE", "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ"
                        ], timeout=1.0, check=False, env=env)
                        log.info("Set MPV window properties: ABOVE, FULLSCREEN, MAXIMIZED")
                    except Exception as xprop_exc:
                        log.warning(f"xprop failed: {xprop_exc}")

                    # Use xdotool for positioning (with simpler, more reliable commands)
                    try:
                        # Move to top-left corner
                        subprocess.run(
                            ["xdotool", "windowmove", mpv_window_id, "0", "0"],
                            timeout=0.5, check=False, env=env
                        )
                        log.info("Moved MPV window to (0,0)")
                    except Exception as move_exc:
                        log.warning(f"windowmove failed: {move_exc}")

                    try:
                        # Resize to fullscreen
                        subprocess.run(
                            ["xdotool", "windowsize", mpv_window_id, str(config.screen_width), str(config.screen_height)],
                            timeout=0.5, check=False, env=env
                        )
                        log.info(f"Resized MPV window to {config.screen_width}x{config.screen_height}")
                    except Exception as size_exc:
                        log.warning(f"windowsize failed: {size_exc}")

                    # Simple activation (avoid --sync which times out)
                    try:
                        subprocess.run(
                            ["xdotool", "windowactivate", mpv_window_id],
                            timeout=0.5, check=False, env=env
                        )
                        log.info("Activated MPV window")
                    except Exception as activate_exc:
                        log.warning(f"windowactivate failed: {activate_exc}")

                    # Final verification and extra raise to ensure MPV is on top
                    try:
                        verify_result = subprocess.run(
                            ["xwininfo", "-id", mpv_window_id],
                            capture_output=True, text=True, timeout=0.5, env=env
                        )
                        if verify_result.returncode == 0:
                            # Parse the geometry from xwininfo output
                            for line in verify_result.stdout.split('\n'):
                                if 'geometry' in line.lower() or 'position' in line.lower():
                                    log.info(f"Window verification: {line.strip()}")
                        else:
                            log.warning("Could not verify window geometry")
                    except Exception as verify_exc:
                        log.warning(f"Window verification failed: {verify_exc}")

                    # EXTRA: Raise MPV window multiple times to ensure it's definitely on top
                    for raise_attempt in range(3):
                        try:
                            subprocess.run(
                                ["xdotool", "windowraise", mpv_window_id],
                                timeout=0.2, check=False, env=env
                            )
                        except Exception:
                            pass
                        time.sleep(0.05)
                    log.info(f"MPV window raised to top multiple times for {mpv_window_id}")
                    log.info(f"MPV window positioning complete for {mpv_window_id}")

                else:
                    log.error("CRITICAL: Could not find MPV window after 5 attempts - intro video will not be visible!")

            except Exception as win_exc:
                log.error(f"CRITICAL: Failed to position MPV window: {win_exc} - intro video may not be visible")
            
            # Wait for video to fully load and be ready before starting playback
            # Check multiple times to ensure video is actually ready
            video_ready = False
            max_wait_attempts = 20  # Wait up to 4 seconds (20 * 0.2)
            for attempt in range(max_wait_attempts):
                try:
                    # Check if video has dimensions (indicates it's loaded)
                    width = mpv.get_property("width")
                    height = mpv.get_property("height")
                    # Check if video is actually loaded (not just metadata)
                    eof = mpv.get_property("eof-reached")
                    # Video is ready if we have dimensions and it's not at EOF (EOF means not loaded yet)
                    if width and height and width > 0 and height > 0 and eof is not True:
                        video_ready = True
                        log.info(f"Video ready: {width}x{height}")
                        break
                except Exception:
                    pass
                time.sleep(0.2)  # Wait 200ms between checks
            
            if not video_ready:
                log.warning("Video may not be fully ready, but proceeding anyway")
            else:
                # Give mpv a bit more time to fully render the first frame
                time.sleep(0.3)
            # Use MPV client instead of manual socket operations
            try:
                log.info("DEBUG: Checking playlist with MPV client...")
                playlist = mpv.get_playlist()
                log.info(f"DEBUG: MPV client returned playlist: {playlist}")
                if len(playlist) > 1:
                    log.warning(f"WARNING: Playlist has {len(playlist)} files! Clearing with MPV client...")
                    mpv.playlist_clear()
                    time.sleep(0.1)
                    # Reload only the intro file
                    mpv.load_file(str(intro_path))
                    log.info(f"Reloaded ONLY intro video: {intro_path}")
                    # Check playlist again
                    playlist = mpv.get_playlist()
                    log.info(f"DEBUG: Playlist after reload: {playlist}")
                log.info(f"Playlist verified: {len(playlist)} file(s) - {intro_path.name}")
            except Exception as pl_exc:
                log.warning(f"Could not verify playlist with MPV client: {pl_exc}")
                # Fallback to manual socket if MPV client fails
                try:
                    import json
                    import socket
                    sock = socket.socket(socket.AF_UNIX)
                    sock.settimeout(0.5)
                    sock.connect(config.mpv_socket)
                    sock.sendall(json.dumps({"command": ["get_property", "playlist"]}).encode() + b"\n")
                    time.sleep(0.2)
                    resp = b""
                    try:
                        while True:
                            chunk = sock.recv(4096)
                            if not chunk:
                                break
                            resp += chunk
                            if b"\n" in resp:
                                break
                    except socket.timeout:
                        pass
                    sock.close()
                    if resp:
                        resp_str = resp.decode().split("\n")[0]
                        playlist_data = json.loads(resp_str)
                        if "data" in playlist_data:
                            playlist = playlist_data["data"]
                            log.info(f"FALLBACK: Playlist verified: {len(playlist)} file(s)")
                except Exception as fallback_exc:
                    log.error(f"FALLBACK playlist check also failed: {fallback_exc}")
            
            # CRITICAL: Ensure mpv doesn't auto-advance to next file
            try:
                mpv.set_property("loop-playlist", "no")
                mpv.set_property("loop-file", "no")
                mpv.set_property("loop", "no")
                mpv.set_property("playlist-pos", 0)
                log.info("Disabled auto-advance and looping")
            except Exception:
                pass
            # Ensure speed and sync are still correct after loading
            try:
                mpv.set_property("speed", 1.0)
                mpv.set_property("video-sync", "desync")  # Force desync mode again after load
                # Force 4:3 aspect ratio FIRST (before fullscreen)
                mpv.set_property("video-aspect", "4/3")  # Force 4:3 aspect ratio
                mpv.set_property("video-zoom", 0.0)  # Reset zoom
                mpv.set_property("panscan", 0.0)  # No pan/scan - show full video with margins
                # Set fullscreen
                mpv.set_fullscreen(True)
                # Wait for fullscreen to activate
                time.sleep(0.5)
                # Ensure window is actually fullscreen-sized using window-scale
                # window-scale > 1.0 makes window larger than screen, which forces fullscreen
                mpv.set_property("window-scale", 2.0)  # Force large scale to ensure fullscreen
                time.sleep(0.2)
                mpv.set_property("window-scale", 1.0)  # Reset to normal scale
                log.debug("Set mpv to fullscreen with 4:3 aspect ratio")
            except Exception as exc:
                log.warning(f"Failed to set mpv fullscreen/scaling: {exc}")
            # Ensure any overlay is off; we'll draw bezel via pygame for consistent sizing
            _deactivate_mpv_bezel_overlay_vf()
            # mpv audio device already configured
            _apply_audio_device(timeout_seconds=2.0)
            
            # CRITICAL: Do minimal window operations BEFORE starting playback
            # Avoid any operations that might disrupt mpv window state right before playback
            # The pygame window is already hidden above, so we just need to ensure mpv is ready
            # Wait a moment for mpv window to be fully created and configured
            time.sleep(0.3)
            
            # Start playback - no window operations after this to avoid interruptions
            mpv.resume()
            
            # Wait for video to actually start playing (check playback state)
            playback_started = False
            for attempt in range(10):  # Wait up to 1 second
                try:
                    is_playing = not mpv.get_property("pause")
                    if is_playing:
                        playback_started = True
                        break
                except Exception:
                    pass
                time.sleep(0.1)
            
            if playback_started:
                log.info("Intro video playback confirmed started")
            else:
                log.warning("Could not confirm playback started, but continuing")

            # CRITICAL: No window operations after playback starts - let video play uninterrupted
            # All window setup was done before loading the video

            # FOR TESTING: Skip the waiting loop entirely and transition immediately
            # This ensures the video is visible and we can test the window layering
            log.info("TESTING: Skipping intro video wait - transitioning immediately to verify video visibility")

            # Get video duration for logging
            try:
                actual_video_duration = mpv.get_property("duration")
                if actual_video_duration:
                    log.info(f"Intro video actual duration: {actual_video_duration:.3f}s")
            except Exception:
                log.debug("Could not get video duration from mpv")

            # Verify video is actually playing
            try:
                is_playing = not mpv.get_property("pause")
                log.info(f"Intro video playback started, paused={not is_playing}")
            except Exception:
                pass

            # Skip waiting - set complete immediately for testing
            video_complete = True
            log.info("TESTING: Intro video started successfully - proceeding to UI transition")
            # Fade transition from intro video to UI using transition manager
            log.info("Starting clean fade transition from intro to UI using transition manager")
            transition_success = transition_mgr.transition_to_ui()
            if transition_success:
                log.info("Successfully transitioned to UI")
            else:
                log.warning("Transition to UI failed, UI may not be properly visible")

            # Recreate display surfaces after transition (transition manager handles window sizing)
            try:
                # Update display manager with current screen
                display_mgr = DisplayManager(display_mode, target_resolution)
                content_surface = display_mgr.get_render_surface()
                renderer = UIRenderer(screen=content_surface, config=config)
                renderer.bezel_mode = (display_mode == DisplayMode.MODERN_WITH_BEZEL)
                log.info("Recreated display surfaces after transition")
            except Exception as recreate_exc:
                log.warning(f"Could not recreate display surfaces: {recreate_exc}")
            
            # Fade transition: fade out video, fade in UI
            fade_duration = 1.0  # 1 second fade
            fade_start = time.time()
            
            while True:
                elapsed = time.time() - fade_start
                fade_progress = min(1.0, elapsed / fade_duration)
                
                # Allow quit during fade
                for event in pygame.event.get():
                    if event.type == pygame.QUIT:
                        pygame.quit()
                        return
                
                # Render UI with alpha fade-in (0.0 -> 1.0)
                ui_alpha_fade = fade_progress
                
                # Ensure mpv is stopped during fade (prevent any auto-play)
                try:
                    if not mpv.get_property("pause"):
                        mpv.pause()
                except Exception:
                    pass
                
                # Render UI content with fade (use actual playlists so UI is complete when fade ends)
                renderer.render(playlists=playlists, selected_index=selected_index, controller=controller, 
                              show_overlay=False, ui_alpha=ui_alpha_fade, ui_hidden=False, 
                              has_playback=False, sample_mode=None)
                
                # Apply CRT effects
                crt_effects.apply_all(content_surface, time.time())
                
                # Composite to screen
                display_mgr.present(screen, bezel, preserve_video_area=False, skip_content_blit=False)
                pygame.display.flip()
                
                # When fade is complete, stop video and exit fullscreen
                if fade_progress >= 1.0:
                    # Stop mpv completely and ensure no video is playing
                    try:
                        # Stop playback and clear playlist
                mpv.stop()
                        # Clear playlist again to be absolutely sure
                import json
                        import socket
                        sock = socket.socket(socket.AF_UNIX)
                        sock.connect(config.mpv_socket)
                        sock.sendall(json.dumps({"command": ["playlist-clear"]}).encode() + b"\n")
                        sock.close()
                        mpv.set_fullscreen(False)  # Exit fullscreen
                        # Ensure mpv is paused/stopped
                        mpv.pause()
                        log.info("Stopped mpv and cleared playlist after intro fade")
                    except Exception as stop_exc:
                        log.warning(f"Error stopping mpv: {stop_exc}")
                    # Ensure pygame window is on top for UI display
                    try:
                        pg_id = window_mgr.pygame_window_id or pygame_window_id
                        if pg_id:
                            window_mgr.map_window(pg_id)
                            window_mgr.raise_window(pg_id)
                            # Ensure persistent undecorated state
                            window_mgr.set_window_undecorated_persistent(pg_id)
                    except Exception:
                        pass
                    break
                
                clock.tick(60)
            
            log.info("Intro complete - UI faded in")
            played_intro = True
            
            # Ensure pygame window is raised and visible after intro
            try:
                pg_id = window_mgr.pygame_window_id or pygame_window_id
                if pg_id:
                    # CRITICAL: Ensure window is fullscreen-sized and positioned
                    # Use retry logic for boot-time reliability
                    if config.platform == "linux":
                        def ensure_fullscreen_after_intro(pg_id, resolution, max_attempts=3):
                            """Ensure window is fullscreen after intro with retries."""
                            import subprocess
                            env = os.environ.copy()
                            env["DISPLAY"] = ":0"
                            for attempt in range(max_attempts):
                                try:
                                    result1 = subprocess.run(
                                        ["xdotool", "windowsize", pg_id, str(resolution[0]), str(resolution[1])],
                                        timeout=1.0, check=False, env=env, capture_output=True
                                    )
                                    result2 = subprocess.run(
                                        ["xdotool", "windowmove", pg_id, "0", "0"],
                                        timeout=1.0, check=False, env=env, capture_output=True
                                    )
                                    subprocess.run(
                                        ["xprop", "-id", pg_id, "-f", "_NET_WM_STATE", "32a", "-set", "_NET_WM_STATE", "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ"],
                                        timeout=0.2, check=False, env=env
                                    )
                                    if result1.returncode == 0 and result2.returncode == 0:
                                        log.info(f"Ensured pygame window {pg_id} is fullscreen: {resolution[0]}x{resolution[1]} at (0,0) (attempt {attempt + 1})")
                                        return True
                                    elif attempt < max_attempts - 1:
                                        time.sleep(0.2)
                                except Exception as exc:
                                    if attempt < max_attempts - 1:
                                        time.sleep(0.2)
                                    else:
                                        log.warning(f"Could not ensure fullscreen after intro: {exc}")
                            return False
                        
                        try:
                            ensure_fullscreen_after_intro(pg_id, modern_resolution, max_attempts=3)
                        except Exception as fullscreen_exc:
                            log.warning(f"Could not ensure fullscreen after intro: {fullscreen_exc}")
                    window_mgr.map_window(pg_id)
                    window_mgr.raise_window(pg_id)
                    # CRITICAL: Ensure window has focus for controller input
                    if config.platform == "linux":
                        try:
                            import subprocess
                            env = os.environ.copy()
                            env["DISPLAY"] = ":0"
                            subprocess.run(
                                ["xdotool", "windowactivate", pg_id],
                                timeout=0.5, check=False, env=env
                            )
                            subprocess.run(
                                ["xdotool", "windowfocus", pg_id],
                                timeout=0.5, check=False, env=env
                            )
                            log.debug("Focused pygame window for controller input")
                        except Exception:
                            pass
                    # Ensure persistent undecorated state after intro
                    window_mgr.set_window_undecorated_persistent(pg_id)
                    window_mgr.hide_cursor_aggressive()
                    log.debug("Raised pygame window after intro and set persistent undecorated state")
            except Exception:
                pass
            
            # CRITICAL: Re-initialize controllers after intro video completes
            # The pygame window was recreated, so controllers may need re-initialization
            # This ensures controllers work after service restart (not just full boot)
            log.info("Re-initializing controllers after intro video (pygame window was recreated)")
            # Note: js_provider and evdev_js_provider are in outer scope, so assignment will work
            try:
                # Re-initialize pygame joystick (window was recreated)
                pygame.joystick.quit()
                time.sleep(0.2)
                pygame.joystick.init()
                if pygame.joystick.get_count() > 0:
                    js = pygame.joystick.Joystick(0)
                    js.init()
                    js_provider = JoystickInputProvider(js)
                    log.info(f"Re-initialized joystick: {js.get_name()}")
                else:
                    log.warning("No joysticks found after re-initialization")
            except Exception as exc:
                log.warning(f"Failed to re-initialize joystick after intro: {exc}")
                import traceback
                log.debug(f"Traceback: {traceback.format_exc()}")
            
            # Re-initialize evdev providers (devices may not have been ready earlier)
            if config.platform == "linux" and EVDEV_AVAILABLE:
                try:
                    evdev_js_provider = EvdevJoystickInputProvider()
                    log.info("Re-initialized evdev joystick input after intro video")
                except Exception as exc:
                    log.warning(f"Could not re-initialize evdev joystick after intro: {exc}")
            
            # Verify js_provider is set correctly
            if js_provider is None:
                log.error("js_provider is None after re-initialization - controller will not work!")
            else:
                log.info(f"js_provider successfully set after intro video re-initialization")
            
            # Note: UI rendering is handled by the main loop, no need to render here
            # The fade transition already rendered the UI, and the main loop will continue rendering
    except Exception as exc:
        log.warning(f"Intro playback failed: {exc}")
    
    # Settings menu (pass settings_store for persistence)
    settings_menu = SettingsMenuManager(settings_store)
    settings_renderer = SettingsMenuRenderer(config.screen_width, config.screen_height)
    # Set bezel mode flag so settings menu can render appropriately
    settings_renderer.bezel_mode = (display_mode == DisplayMode.MODERN_WITH_BEZEL)
    
    # Startup animation
    startup_animation = StartupAnimation(screen=content_surface, font_title=renderer.theme.font_title, duration=5.5, theme=renderer.theme)
    startup_animation.start()
    showing_startup_animation = not played_intro

    # CRITICAL: Periodic fullscreen check for boot-time reliability
    # On boot, window manager may resize window after initial positioning
    # Check and fix window size periodically during main loop
    last_fullscreen_check = time.time()
    fullscreen_check_interval = 2.0  # Check every 2 seconds

    # Input providers
    # CRITICAL: Initialize controllers with retry logic to handle service restarts
    # When service restarts (not full boot), input devices may not be ready immediately
    kb_provider = KeyboardInputProvider()
    # js_provider and evdev_js_provider are declared before intro video section
    # so they can be re-initialized after intro completes
    js_provider = None  # Initialize to None, will be set below
    evdev_js_provider = None  # Initialize to None, will be set below
    
    # Check if we're restarting after RetroArch (controller may need re-initialization)
    # Only re-initialize if we detect RetroArch was recently running
    retroarch_just_exited = os.path.exists("/tmp/magic_retroarch_launch.log") and \
                           (time.time() - os.path.getmtime("/tmp/magic_retroarch_launch.log")) < 10
    
    if retroarch_just_exited:
        # RetroArch just exited - re-initialize controllers
        log.info("Detected RetroArch just exited - re-initializing controllers")
        try:
            pygame.joystick.quit()
        except Exception:
            pass
        time.sleep(0.5)  # Give devices time to be released
    
    # Wait a moment for input devices to be ready (important for service restarts)
    # On full boot, devices are ready; on service restart, they may need a moment
    if config.platform == "linux":
        time.sleep(0.5)  # Give input devices time to be ready after service restart
    
    # Initialize pygame joystick with retry logic
    js_init_attempts = 3
    for attempt in range(js_init_attempts):
        try:
            pygame.joystick.init()
            if pygame.joystick.get_count() > 0:
                js = pygame.joystick.Joystick(0)
                js.init()
                js_provider = JoystickInputProvider(js)
                log.info(f"Joystick connected: {js.get_name()} (buttons={js.get_numbuttons()}, hats={js.get_numhats()}, axes={js.get_numaxes()})")
                break
            elif attempt < js_init_attempts - 1:
                log.debug(f"Joystick not found, retrying ({attempt + 1}/{js_init_attempts})...")
                time.sleep(0.5)
        except Exception as exc:
            if attempt < js_init_attempts - 1:
                log.debug(f"Joystick init failed, retrying ({attempt + 1}/{js_init_attempts}): {exc}")
                time.sleep(0.5)
            else:
                log.warning(f"Joystick init failed after {js_init_attempts} attempts: {exc}")
    
    # Evdev providers (Linux only - work even when pygame doesn't have focus)
    # Use retry logic for evdev initialization as well
    # Note: evdev_js_provider may already be initialized after intro video, so check first
    evdev_kb_provider = None
    # Only initialize evdev_js_provider if it wasn't already initialized after intro video
    if config.platform == "linux" and EVDEV_AVAILABLE:
        # Retry evdev keyboard initialization
        for attempt in range(3):
            try:
                evdev_kb_provider = EvdevKeyboardInputProvider()
                log.info("Evdev keyboard input enabled")
                break
            except Exception as exc:
                if attempt < 2:
                    log.debug(f"Evdev keyboard init failed, retrying ({attempt + 1}/3): {exc}")
                    time.sleep(0.5)
                else:
                    log.warning(f"Evdev keyboard unavailable after 3 attempts: {exc}")
        
        # Retry evdev joystick initialization (only if not already initialized after intro)
        if evdev_js_provider is None:
            log.info("Initializing evdev joystick provider (not yet initialized)")
            for attempt in range(3):
                try:
                    evdev_js_provider = EvdevJoystickInputProvider()
                    log.info(f"Evdev joystick input enabled successfully (attempt {attempt + 1})")
                    # Verify it was actually set
                    if evdev_js_provider is None:
                        raise RuntimeError("EvdevJoystickInputProvider returned None")
                    break
                except Exception as exc:
                    import traceback
                    if attempt < 2:
                        log.warning(f"Evdev joystick init failed, retrying ({attempt + 1}/3): {exc}")
                        log.debug(f"Traceback: {traceback.format_exc()}")
                        time.sleep(0.5)
                    else:
                        log.error(f"Evdev joystick unavailable after 3 attempts: {exc}")
                        log.error(f"Final traceback: {traceback.format_exc()}")
        else:
            log.info("Evdev joystick already initialized after intro video - skipping re-init")
    
    gpio_provider = None
    if config.platform == "linux" and os.getenv("MAGIC_USE_GPIO", "0") == "1":
        try:
            # Default pins: encoder A/B on 17/18, select on 27
            gpio_provider = GPIOInputProvider(pin_a=17, pin_b=18, pin_select=27)
            log.info("GPIO input enabled")
        except Exception as exc:
            log.warning("GPIO input unavailable: %s", exc)

    overlay_last_interaction_ts = time.time()

    # Fade/hide state
    ui_alpha = 1.0
    ui_hidden = False
    has_playback = False  # Track if we've started playback
    transition_dir = 0  # -1 fade out, +1 fade in, 0 none
    transition_start = 0.0
    transition_duration = 1.0
    volume_menu = 75  # Volume when menu is visible
    volume_video = 100  # Volume when watching video
    
    # Track currently playing playlist and position for audio-only mode
    playing_playlist = None  # Reference to the currently playing playlist
    saved_position = None  # Saved playback position when UI is shown during playback

    def start_fade(direction: int) -> None:
        nonlocal transition_dir, transition_start
        transition_dir = direction
        transition_start = time.time()

    # Manage mpv bezel overlay during normal video playback (UI hidden)
    mpv_bezel_overlay_active = False
    def _activate_mpv_bezel_overlay() -> None:
        nonlocal mpv_bezel_overlay_active
        if mpv_bezel_overlay_active:
            return
        if display_mode != DisplayMode.MODERN_WITH_BEZEL:
            return
        if not bezel_overlay_file:
            return
        try:
            # Minimal overlay graph: no scaling, overlay full-screen bezel PNG at (0,0)
            # VLC doesn't support lavfi-complex filters
            # Bezel overlay will be handled by pygame instead
            pass
            mpv_bezel_overlay_active = True
        except Exception as exc:
            logging.getLogger("magic.main").warning(f"mpv overlay enable failed: {exc}")

    def _deactivate_mpv_bezel_overlay() -> None:
        nonlocal mpv_bezel_overlay_active
        if not mpv_bezel_overlay_active:
            return
        try:
            pass  # VLC doesn't use lavfi-complex
        except Exception:
            pass
        mpv_bezel_overlay_active = False

    # Playlist watcher (hot reload)
    playlists_dirty = threading.Event()

    def _mark_dirty() -> None:
        playlists_dirty.set()

    watcher = PlaylistWatcher(config.playlists_dir, on_change=_mark_dirty, interval_seconds=1.5)
    watcher.start()

    # Optional web admin
    if config.enable_web_admin:
        app = create_app(config.data_dir, config)
        admin_port = int(os.getenv("MAGIC_ADMIN_PORT", "8080"))
        threading.Thread(
            target=lambda: app.run(host="0.0.0.0", port=admin_port, debug=False, use_reloader=False),
            name="AdminServer",
            daemon=True,
        ).start()

    running = True
    
    # Handle SIGTERM for graceful shutdown (systemd stop)
    def _handle_sigterm(signum, frame) -> None:
        nonlocal running
        running = False
    
    signal.signal(signal.SIGTERM, _handle_sigterm)
    frame_count = 0  # Track frames for occasional window operations
    while running:
        # Periodic fullscreen check (important on boot when window manager may interfere)
        current_time = time.time()
        if current_time - last_fullscreen_check >= fullscreen_check_interval:
            if config.platform == "linux" and pygame_window_id:
                try:
                    import subprocess
                    env = os.environ.copy()
                    env["DISPLAY"] = ":0"
                    # Quick check and fix if window is not fullscreen
                    result = subprocess.run(
                        ["xdotool", "getwindowgeometry", pygame_window_id],
                        timeout=0.5, check=False, env=env, capture_output=True, text=True
                    )
                    if result.returncode == 0:
                        # Parse geometry output (format: "Geometry: 1280x720+0+0")
                        lines = result.stdout.split('\n')
                        for line in lines:
                            if 'Geometry:' in line:
                                # Extract size and position
                                import re
                                match = re.search(r'(\d+)x(\d+)\+(\d+)\+(\d+)', line)
                                if match:
                                    width, height, x, y = map(int, match.groups())
                                    expected_w, expected_h = modern_resolution
                                    # If window is not fullscreen or not at (0,0), fix it
                                    if width != expected_w or height != expected_h or x != 0 or y != 0:
                                        log.debug(f"Window not fullscreen ({width}x{height}+{x}+{y}), fixing...")
                                        subprocess.run(
                                            ["xdotool", "windowsize", pygame_window_id, str(expected_w), str(expected_h)],
                                            timeout=0.5, check=False, env=env
                                        )
                                        subprocess.run(
                                            ["xdotool", "windowmove", pygame_window_id, "0", "0"],
                                            timeout=0.5, check=False, env=env
                                        )
                                        subprocess.run(
                                            ["xprop", "-id", pygame_window_id, "-f", "_NET_WM_STATE", "32a", "-set", "_NET_WM_STATE", "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ"],
                                            timeout=0.2, check=False, env=env
                                        )
                                break
                except Exception:
                    pass  # Don't spam logs with periodic check failures
            last_fullscreen_check = current_time
        
        # Handle startup animation first
        if showing_startup_animation:
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    running = False
                    break
            
            if not running:
                break
                
            startup_animation.render()
            
            # Apply CRT effects to startup animation
            crt_effects.apply_all(content_surface, time.time())
            
            # Composite content to screen with display mode handling
            display_mgr.present(screen, bezel)
            
            pygame.display.flip()
            clock.tick(60)
            
            if startup_animation.is_complete():
                showing_startup_animation = False
            
            continue
        
        # Input handling
        events = []
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
                break
            mapped = kb_provider.translate(event)
            if mapped is not None:
                events.append(mapped)
            if js_provider is not None:
                mapped_js = js_provider.translate(event)
                if mapped_js is not None:
                    events.append(mapped_js)

        # Check for hold-to-seek keyboard events
        for ev in kb_provider.poll_seeking():
            events.append(ev)
        # Check joystick continuous inputs
        if js_provider is not None:
            for ev in js_provider.poll():
                events.append(ev)

        if gpio_provider is not None:
            for ev in gpio_provider.poll():  # type: ignore[attr-defined]
                events.append(ev)
        
        # Check evdev providers (Linux only - work even when pygame doesn't have focus)
        # Always use evdev - it works even when window doesn't have focus (important during video playback)
        # Evdev and pygame can coexist - evdev reads directly from device, pygame reads from SDL
        if evdev_kb_provider is not None:
            for ev in evdev_kb_provider.poll():
                events.append(ev)
        # Always use evdev joystick - it works during video playback when pygame window may not have focus
        # This ensures controller works at all times, not just when UI has focus
        if evdev_js_provider is not None:
            try:
                for ev in evdev_js_provider.poll():
                    events.append(ev)
            except Exception as exc:
                # Log error but don't crash - controller might have disconnected
                if frame_count % 300 == 0:  # Log every 5 seconds (60fps * 5)
                    log.debug(f"Error polling evdev joystick: {exc}")
        elif config.platform == "linux" and frame_count % 300 == 0:
            # Log warning if evdev joystick provider is not initialized (every 5 seconds)
            log.warning("evdev_js_provider is None - controller input may not work")

        for mapped in events:
            t = mapped.type
            if t == mapped.Type.QUIT:
                running = False
                break

            # Settings menu toggle (button 4/B button) - works even during video playback
            if t == mapped.Type.SETTINGS_MENU:
                if ui_hidden:
                    # During video playback: show UI and open menu
                    log.info("SETTINGS_MENU pressed during video - showing UI and opening menu")
                    # Pause video
                    try:
                        mpv.pause()
                        log.debug("Paused mpv before showing UI")
                    except Exception as pause_exc:
                        log.debug(f"Could not pause mpv: {pause_exc}")
                    # Show UI
                    ui_alpha = 1.0
                    ui_hidden = False
                    # Open settings menu
                    settings_menu.open()
                    overlay_last_interaction_ts = time.time()
                else:
                    # When UI is visible: toggle menu
                    settings_menu.toggle()
                    overlay_last_interaction_ts = time.time()
                continue

            # If settings menu is active, route inputs to menu
            if settings_menu.active:
                if t == mapped.Type.ROTATE:
                    # Get game count if viewing individual games
                    games_count = 0
                    if settings_menu.viewing_games_in_playlist and game_playlists:
                        if settings_menu.current_game_playlist_index < len(game_playlists):
                            games_count = len(game_playlists[settings_menu.current_game_playlist_index].items)
                    settings_menu.navigate(mapped.delta, len(game_playlists), games_count)
                    overlay_last_interaction_ts = time.time()
                elif t == mapped.Type.SELECT:
                    # Handle individual game selection (deepest level)
                    if settings_menu.viewing_games_in_playlist:
                        if game_playlists and settings_menu.current_game_playlist_index < len(game_playlists):
                            selected_playlist = game_playlists[settings_menu.current_game_playlist_index]
                            # Check if Back is selected (index == number of games)
                            if settings_menu.selected_game_in_playlist == len(selected_playlist.items):
                                # Back selected - return to systems list
                                settings_menu.exit_game_list()
                            elif settings_menu.selected_game_in_playlist < len(selected_playlist.items):
                                # Launch specific game
                                selected_game = selected_playlist.items[settings_menu.selected_game_in_playlist]
                                # Create a temporary single-item playlist for this game
                                from magic_dingus_box.library.models import Playlist
                                temp_playlist = Playlist(
                                    title=selected_game.title,
                                    curator="",
                                    items=[selected_game],
                                    source_path=selected_playlist.source_path
                                )
                                controller.load_playlist(temp_playlist)
                                controller.play_current()
                                has_playback = True
                                settings_menu.close()  # Close settings after launching game
                        overlay_last_interaction_ts = time.time()
                    # Handle game system/playlist selection
                    elif settings_menu.game_browser_active:
                        # Check if Back is selected (index == number of playlists)
                        if settings_menu.game_browser_selected == len(game_playlists):
                            # Back selected - return to Video Games submenu
                            settings_menu.exit_game_browser()
                            settings_menu.enter_submenu(MenuSection.VIDEO_GAMES)
                        elif game_playlists and settings_menu.game_browser_selected < len(game_playlists):
                            # Enter the individual game list for this system
                            settings_menu.enter_game_list(settings_menu.game_browser_selected)
                        overlay_last_interaction_ts = time.time()
                    else:
                        # Handle normal menu selection
                        section = settings_menu.select_current()
                        if section == MenuSection.BACK:
                            # Back button handles all backward navigation
                            if settings_menu.viewing_games_in_playlist:
                                # Viewing individual games -> back to systems list
                                settings_menu.exit_game_list()
                            elif settings_menu.game_browser_active:
                                # In game browser -> back to video games submenu
                                settings_menu.exit_game_browser()
                                settings_menu.enter_submenu(MenuSection.VIDEO_GAMES)
                            elif settings_menu.current_submenu:
                                # In submenu -> back to main menu
                                settings_menu.exit_submenu()
                            else:
                                # In main menu -> close settings
                                settings_menu.close()
                        elif section == MenuSection.VIDEO_GAMES:
                            settings_menu.enter_submenu(MenuSection.VIDEO_GAMES)
                        elif section == MenuSection.DISPLAY:
                            settings_menu.enter_submenu(MenuSection.DISPLAY)
                        elif section == MenuSection.AUDIO:
                            settings_menu.enter_submenu(MenuSection.AUDIO)
                        elif section == MenuSection.SYSTEM:
                            settings_menu.enter_submenu(MenuSection.SYSTEM)
                        elif section == MenuSection.BROWSE_GAMES:
                            settings_menu.enter_game_browser()
                        elif section == MenuSection.DOWNLOAD_CORES:
                            # Open RetroArch Core Downloader
                            log.info("Opening RetroArch Core Downloader")
                            
                            # Release controllers before launching RetroArch (on Linux)
                            if sys.platform.startswith("linux"):
                                log.info("Releasing controllers before RetroArch Core Downloader launch")
                                try:
                                    # Release pygame joystick
                                    pygame.joystick.quit()
                                    log.debug("Released pygame joystick")
                                except Exception:
                                    pass
                                
                                # Release evdev joystick if available
                                if evdev_js_provider is not None:
                                    try:
                                        # Release evdev device grabs
                                        for device in evdev_js_provider.devices:
                                            try:
                                                device.ungrab()
                                                log.debug(f"Released evdev device: {device.name}")
                                            except Exception:
                                                pass
                                    except Exception as evdev_exc:
                                        log.debug(f"Could not release evdev devices: {evdev_exc}")
                                
                                # CRITICAL: Hide/kill UI windows IMMEDIATELY for visual feedback
                                log.info("Hiding UI windows immediately for visual feedback")
                                try:
                                    pg_id = window_mgr.pygame_window_id or pygame_window_id
                                    if pg_id:
                                        # Hide pygame window immediately
                                        import subprocess
                                        # Note: os is already imported at module level
                                        env = os.environ.copy()
                                        env["DISPLAY"] = ":0"
                                        subprocess.run(
                                            ["xdotool", "windowunmap", pg_id],
                                            timeout=0.3, check=False, env=env
                                        )
                                        subprocess.run(
                                            ["xdotool", "windowkill", pg_id],
                                            timeout=0.3, check=False, env=env
                                        )
                                        log.debug("Hidden pygame window immediately")
                                except Exception as hide_exc:
                                    log.debug(f"Could not hide UI window: {hide_exc}")
                            
                            if controller.retroarch.open_core_downloader():
                                # On Pi, UI will exit when service stops
                                # On macOS/dev, RetroArch launches in background
                                if sys.platform.startswith("linux") and controller.retroarch.wrapper_script:
                                    # UI will exit when wrapper stops the service
                                    log.info("Core Downloader launched - UI will exit and restart after RetroArch closes")
                                    # Close menu
                                    settings_menu.close()
                                    # The wrapper script will stop the service, causing this process to exit
                                    # Give it a moment to stop the service
                                    time.sleep(1)
                                    # Process should exit when service stops, but if not, continue loop
                                    # The wrapper script handles everything
                                    overlay_last_interaction_ts = time.time()
                                    continue
                                else:
                                    # On macOS/dev, just close the menu
                                    settings_menu.close()
                                    log.info("RetroArch Core Downloader launched. Navigate to: Online Updater -> Core Downloader")
                            else:
                                log.error("Failed to launch RetroArch Core Downloader")
                        elif section == MenuSection.TOGGLE_DISPLAY_MODE:
                            # Cycle through display modes
                            current_mode = settings_store.get_display_mode()
                            modes = ["crt_native", "modern_clean", "modern_bezel"]
                            current_idx = modes.index(current_mode) if current_mode in modes else 0
                            next_mode = modes[(current_idx + 1) % len(modes)]
                            settings_store.set_display_mode(next_mode)
                            # Rebuild display submenu to reflect change (preserve cursor position)
                            settings_menu.rebuild_current_submenu()
                            log.info(f"Display mode changed to: {next_mode} (requires restart)")
                        elif section == MenuSection.TOGGLE_BEZEL:
                            # Toggle bezel on/off
                            current = settings_store.get_show_bezel()
                            settings_store.set_show_bezel(not current)
                            settings_store.set_display_mode("modern_bezel" if not current else "modern_clean")
                            # Rebuild display submenu (preserve cursor position)
                            settings_menu.rebuild_current_submenu()
                            log.info(f"CRT Bezel toggled (requires restart)")
                        elif section == MenuSection.CHANGE_RESOLUTION:
                            # Cycle through common resolutions
                            current_res = settings_store.get_modern_resolution()
                            resolutions = ["auto", "1920x1080", "2560x1440", "3840x2160"]
                            current_idx = resolutions.index(current_res) if current_res in resolutions else 0
                            next_res = resolutions[(current_idx + 1) % len(resolutions)]
                            settings_store.set_modern_resolution(next_res)
                            settings_menu.rebuild_current_submenu()
                            log.info(f"Resolution changed to: {next_res} (requires restart)")
                        elif section == MenuSection.CYCLE_BEZEL_STYLE:
                            # Cycle through available bezel styles
                            bezels = bezel_loader.list_available_bezels()
                            current_style = settings_store.get("bezel_style", "retro_tv_1")
                            bezel_ids = [b.id for b in bezels]
                            current_idx = bezel_ids.index(current_style) if current_style in bezel_ids else 0
                            next_style = bezel_ids[(current_idx + 1) % len(bezel_ids)]
                            settings_store.set("bezel_style", next_style)
                            settings_menu.rebuild_current_submenu()
                            log.info(f"Bezel style changed to: {next_style} (requires restart)")
                        elif section == MenuSection.CYCLE_SCANLINES:
                            # Cycle scanline intensity
                            modes = ["off", "light", "medium", "heavy"]
                            current = settings_store.get("scanlines_mode", "off")
                            current_idx = modes.index(current) if current in modes else 0
                            next_mode = modes[(current_idx + 1) % len(modes)]
                            settings_store.set("scanlines_mode", next_mode)
                            crt_effects.load_settings(settings_store)
                            settings_menu.rebuild_current_submenu()
                            log.info(f"Scanlines: {next_mode}")
                        elif section == MenuSection.CYCLE_WARMTH:
                            # Cycle color warmth
                            warmths = [0.0, 0.25, 0.5, 0.75]
                            current = settings_store.get("color_warmth", 0.0)
                            # Find closest
                            current_idx = min(range(len(warmths)), key=lambda i: abs(warmths[i] - current))
                            next_warmth = warmths[(current_idx + 1) % len(warmths)]
                            settings_store.set("color_warmth", next_warmth)
                            crt_effects.load_settings(settings_store)
                            settings_menu.rebuild_current_submenu()
                            log.info(f"Color warmth: {int(next_warmth*100)}%")
                        elif section == MenuSection.CYCLE_GLOW:
                            # Cycle phosphor glow intensity: OFF → Low → Medium → High → OFF
                            intensities = [0.0, 0.25, 0.5, 0.75]
                            current = settings_store.get("phosphor_glow", 0.0)
                            # Find closest intensity
                            current_idx = min(range(len(intensities)), key=lambda i: abs(intensities[i] - current))
                            next_intensity = intensities[(current_idx + 1) % len(intensities)]
                            settings_store.set("phosphor_glow", next_intensity)
                            crt_effects.load_settings(settings_store)
                            settings_menu.rebuild_current_submenu()
                            if next_intensity == 0.0:
                                log.info("Phosphor glow: OFF")
                            else:
                                log.info(f"Phosphor glow: {int(next_intensity*100)}%")
                        elif section == MenuSection.CYCLE_PHOSPHOR_MASK:
                            # Cycle RGB phosphor mask intensity
                            intensities = [0.0, 0.25, 0.5, 0.75]
                            current = settings_store.get("phosphor_mask", 0.0)
                            current_idx = min(range(len(intensities)), key=lambda i: abs(intensities[i] - current))
                            next_intensity = intensities[(current_idx + 1) % len(intensities)]
                            settings_store.set("phosphor_mask", next_intensity)
                            crt_effects.load_settings(settings_store)
                            settings_menu.rebuild_current_submenu()
                            if next_intensity == 0.0:
                                log.info("RGB mask: OFF")
                            else:
                                log.info(f"RGB mask: {int(next_intensity*100)}%")
                        elif section == MenuSection.CYCLE_BLOOM:
                            # Cycle screen bloom intensity
                            intensities = [0.0, 0.25, 0.5, 0.75]
                            current = settings_store.get("screen_bloom", 0.0)
                            current_idx = min(range(len(intensities)), key=lambda i: abs(intensities[i] - current))
                            next_intensity = intensities[(current_idx + 1) % len(intensities)]
                            settings_store.set("screen_bloom", next_intensity)
                            crt_effects.load_settings(settings_store)
                            settings_menu.rebuild_current_submenu()
                            if next_intensity == 0.0:
                                log.info("Screen bloom: OFF")
                            else:
                                log.info(f"Screen bloom: {int(next_intensity*100)}%")
                        elif section == MenuSection.CYCLE_INTERLACING:
                            # Cycle interlacing intensity
                            intensities = [0.0, 0.25, 0.5, 0.75]
                            current = settings_store.get("interlacing", 0.0)
                            current_idx = min(range(len(intensities)), key=lambda i: abs(intensities[i] - current))
                            next_intensity = intensities[(current_idx + 1) % len(intensities)]
                            settings_store.set("interlacing", next_intensity)
                            crt_effects.load_settings(settings_store)
                            settings_menu.rebuild_current_submenu()
                            if next_intensity == 0.0:
                                log.info("Interlacing: OFF")
                            else:
                                log.info(f"Interlacing: {int(next_intensity*100)}%")
                        elif section == MenuSection.CYCLE_FLICKER:
                            # Cycle screen flicker intensity
                            intensities = [0.0, 0.25, 0.5, 0.75]
                            current = settings_store.get("screen_flicker", 0.0)
                            current_idx = min(range(len(intensities)), key=lambda i: abs(intensities[i] - current))
                            next_intensity = intensities[(current_idx + 1) % len(intensities)]
                            settings_store.set("screen_flicker", next_intensity)
                            crt_effects.load_settings(settings_store)
                            settings_menu.rebuild_current_submenu()
                            if next_intensity == 0.0:
                                log.info("Screen flicker: OFF")
                            else:
                                log.info(f"Screen flicker: {int(next_intensity*100)}%")
                        overlay_last_interaction_ts = time.time()
                continue  # Don't process other inputs when menu is active

            # Context-sensitive input handling based on UI visibility
            if t == mapped.Type.ROTATE:
                if ui_hidden:
                    # Seek ±1 second when video playing
                    controller.seek_relative(mapped.delta)
                else:
                    # Navigate playlist when UI visible
                    selected_index = (selected_index + mapped.delta) % max(len(playlists), 1)
                    overlay_last_interaction_ts = time.time()

            elif t == mapped.Type.SELECT:
                if ui_hidden:
                    # User pressed SELECT while watching video - seamless transition to UI
                    log.info("SELECT pressed during video - transitioning to UI")
                    
                    # Update selected_index to show currently playing playlist (if any)
                    if playing_playlist is not None:
                        try:
                            selected_index = playlists.index(playing_playlist)
                            log.info(f"Highlighting playlist: {playing_playlist.title}")
                        except (ValueError, AttributeError):
                            pass
                    
                    # Pause mpv BEFORE transition to prevent any visual artifacts
                    try:
                        mpv.pause()
                        log.debug("Paused mpv before transition to UI")
                    except Exception as pause_exc:
                        log.debug(f"Could not pause mpv: {pause_exc}")
                    
                    # Show UI immediately
                    ui_alpha = 1.0
                    ui_hidden = False
                    
                    # Force refresh window IDs before transition
                    transition_mgr.invalidate_cache()
                    try:
                        wm_info = pygame.display.get_wm_info()
                        if "window" in wm_info:
                            pg_id = str(wm_info["window"])
                            transition_mgr.window_mgr.pygame_window_id = pg_id
                            log.debug(f"Refreshed pygame window ID: {pg_id}")
                    except Exception as wid_exc:
                        log.debug(f"Could not refresh pygame window ID: {wid_exc}")
                    
                    # Perform seamless transition: mpv -> UI
                    transition_success = transition_mgr.transition_to_ui()
                    if not transition_success:
                        log.warning("Transition to UI failed - trying fallback window operations")
                        # Fallback: direct window operations
                        try:
                            pg_id = transition_mgr.window_mgr.pygame_window_id or pygame_window_id
                            if pg_id:
                                window_mgr.map_window(pg_id)
                                window_mgr.remove_window_state(pg_id, "_NET_WM_STATE_HIDDEN")
                                window_mgr.remove_window_state(pg_id, "_NET_WM_STATE_ICONIC")
                                window_mgr.raise_window(pg_id)
                                log.info("Fallback: Manually raised pygame window")
                        except Exception as fallback_exc:
                            log.warning(f"Fallback window operations failed: {fallback_exc}")
                    
                    # Stop mpv (it's already hidden by transition, so no visual interruption)
                    try:
                        mpv.set_fullscreen(False)  # Exit fullscreen
                mpv.stop()
                        mpv.playlist_clear()
                        log.info("Stopped mpv and cleared playlist")
                    except Exception as stop_exc:
                        log.warning(f"Could not stop mpv: {stop_exc}")
                    
                    # Clear playback state
                    has_playback = False
                    playing_playlist = None
                    saved_position = None
                    
                    # Invalidate window cache since mpv window state changed
                    transition_mgr.invalidate_cache()
                    
                    log.info("Seamless transition complete: UI visible, mpv hidden and stopped")
                    
                else:
                    # UI is visible - user is selecting a playlist to start
                    if playlists and selected_index < len(playlists):
                        selected_playlist = playlists[selected_index]
                        log.info(f"Starting playlist: {selected_playlist.title}")
                        
                        # Check if this is a game (emulated_game) - games launch RetroArch which blocks
                        current_item = selected_playlist.items[0] if selected_playlist.items else None
                        is_game = current_item and current_item.source_type == "emulated_game"
                        
                        if is_game:
                            # For games: check if wrapper script is available (Pi deployment)
                            wrapper_available = controller.retroarch.wrapper_script is not None
                            
                            if wrapper_available:
                                # On Pi with wrapper script: process will exit when service stops
                                # Release controllers before launching RetroArch
                                log.info("Releasing controllers before RetroArch launch")
                                try:
                                    # Release pygame joystick
                                    pygame.joystick.quit()
                                    log.debug("Released pygame joystick")
                                except Exception:
                                    pass
                                
                                # Release evdev joystick if available
                                if evdev_js_provider is not None:
                                    try:
                                        # Release evdev device grabs
                                        for device in evdev_js_provider.devices:
                                            try:
                                                device.ungrab()
                                                log.debug(f"Released evdev device: {device.name}")
                                            except Exception:
                                                pass
                                    except Exception as evdev_exc:
                                        log.debug(f"Could not release evdev devices: {evdev_exc}")
                                
                                # Ensure HDMI audio device is set before exit
                                try:
                                    _apply_audio_device(timeout_seconds=2.0)
                                except Exception as audio_exc:
                                    log.warning(f"Could not set audio device before game launch: {audio_exc}")
                                
                                # Load playlist and launch game
                                # The wrapper script will stop UI service, run RetroArch, restart UI
                                log.info("Launching game - UI will exit and restart after game")
                                controller.load_playlist(selected_playlist)
                                controller.load_current(start_playback=False)
                                
                                # Process will exit when wrapper script stops the service
                                # The wrapper script handles restarting the UI
                                overlay_last_interaction_ts = time.time()
                                continue
                            else:
                                # On macOS/dev: hide pygame window before launch, restore after
                                try:
                                    pg_id = window_mgr.pygame_window_id or pygame_window_id
                                    if pg_id:
                                        window_mgr.hide_cursor_aggressive()
                                        # Hide pygame window so RetroArch can take over
                                        window_mgr.unmap_window(pg_id)
                                        log.debug("Hidden pygame window before game launch")
                                except Exception:
                                    pass
                        
                        # Ensure HDMI audio device is set
                        _apply_audio_device(timeout_seconds=2.0)
                        
                        # Step 1: Load playlist and file (but don't start playback yet)
                        controller.load_playlist(selected_playlist)
                        controller.load_current(start_playback=False)  # Load but keep paused
                        playing_playlist = selected_playlist
                        
                        # After game exits, restore pygame window (macOS/dev only)
                        if is_game:
                            try:
                                pg_id = window_mgr.pygame_window_id or pygame_window_id
                                if pg_id:
                                    # Restore pygame window after RetroArch exits
                                    window_mgr.map_window(pg_id)
                                    window_mgr.raise_window(pg_id)
                                    window_mgr.set_window_undecorated_persistent(pg_id)
                                    window_mgr.hide_cursor_aggressive()
                                    log.debug("Restored pygame window after game exit")
                            except Exception:
                                pass
                            # Games don't use mpv, so skip video transition logic
                            overlay_last_interaction_ts = time.time()
                            continue
                        
                        # Step 2: Set UI state for video playback (will be hidden by transition)
                        has_playback = True
                        ui_alpha = 0.0
                        ui_hidden = True
                        playback_start_time = time.time()
                        
                        # Step 3: Perform seamless transition: UI -> mpv
                        # This hides UI, prepares mpv window while hidden, then shows mpv
                        transition_success = transition_mgr.transition_to_video()
                        if not transition_success:
                            log.warning("Transition to video failed - trying fallback")
                            # Fallback: ensure mpv is visible
                            try:
                                window_mgr.ensure_mpv_visible(max_attempts=3)
                            except Exception as fallback_exc:
                                log.warning(f"Fallback mpv visibility failed: {fallback_exc}")
                        
                        # Step 4: Set mpv to fullscreen (window is already visible from transition)
                        try:
                            mpv.set_fullscreen(True)
                            log.debug("Set mpv fullscreen after transition")
                            # Ensure mpv window has persistent undecorated state after fullscreen
                            time.sleep(0.05)  # Brief wait for fullscreen to apply
                            mpv_id = transition_mgr._get_mpv_id(max_attempts=3)
                            if mpv_id:
                                window_mgr.set_window_undecorated_persistent(mpv_id)
                                window_mgr.hide_cursor_aggressive()
                        except Exception as vid_exc:
                            log.warning(f"Could not set mpv fullscreen: {vid_exc}")
                        
                        # Step 5: Resume playback (file was loaded paused, now start it)
                        try:
                            mpv.resume()
                            controller.paused = False
                            log.info("Resumed playback after transition")
                        except Exception as resume_exc:
                            log.warning(f"Could not resume playback: {resume_exc}")
                        
                        # Invalidate window cache since mpv window state changed
                        transition_mgr.invalidate_cache()
                
                overlay_last_interaction_ts = time.time()

            elif t == mapped.Type.NEXT:
                # Next track (triggered by quick press)
                controller.next_item()
                has_playback = True
                saved_position = None  # Clear saved position when changing tracks
                sample_mode.clear_markers()  # Clear markers when changing tracks
                overlay_last_interaction_ts = time.time()

            elif t == mapped.Type.PREV:
                # Previous track (triggered by quick press)
                controller.previous_item()
                has_playback = True
                saved_position = None  # Clear saved position when changing tracks
                sample_mode.clear_markers()  # Clear markers when changing tracks
                overlay_last_interaction_ts = time.time()

            elif t == mapped.Type.SEEK_LEFT:
                # Seek backward (triggered by hold or arrow keys)
                controller.seek_relative(-2)
                overlay_last_interaction_ts = time.time()

            elif t == mapped.Type.SEEK_RIGHT:
                # Seek forward (triggered by hold or arrow keys)
                controller.seek_relative(2)
                overlay_last_interaction_ts = time.time()

            elif t == mapped.Type.PLAY_PAUSE:
                controller.toggle_pause()
                overlay_last_interaction_ts = time.time()

            elif t == mapped.Type.TOGGLE_LOOP:
                controller.toggle_loop()
                overlay_last_interaction_ts = time.time()

            elif t == mapped.Type.ENTER_SAMPLE_MODE:
                # Enter sample mode
                sample_mode.toggle_active()
                kb_provider.sample_mode = sample_mode.active
                log.info("Sample mode activated")
                overlay_last_interaction_ts = time.time()

            elif t == mapped.Type.EXIT_SAMPLE_MODE:
                # Exit sample mode and clear all markers
                sample_mode.toggle_active()
                sample_mode.clear_markers()
                kb_provider.sample_mode = sample_mode.active
                log.info("Sample mode deactivated, markers cleared")
                overlay_last_interaction_ts = time.time()

            elif t == mapped.Type.MARKER_ACTION:
                # Set or jump to marker (delta contains slot 0-3)
                slot = mapped.delta
                elapsed, _ = controller.elapsed_and_duration()
                if elapsed is not None:
                    if sample_mode.is_marker_set(slot):
                        # Marker already set, jump to it
                        timestamp = sample_mode.get_marker(slot)
                        if timestamp is not None:
                            controller.seek_absolute(timestamp)
                            log.info("Jumped to marker %d at %.2fs", slot + 1, timestamp)
                    else:
                        # Marker not set, set it
                        sample_mode.set_marker(slot, elapsed)
                        log.info("Set marker %d at %.2fs", slot + 1, elapsed)
                overlay_last_interaction_ts = time.time()

            elif t == mapped.Type.UNDO_MARKER:
                # Undo last marker
                sample_mode.undo_last_marker()
                log.info("Undid last marker")
                overlay_last_interaction_ts = time.time()

        # Refresh playlists on change
        if playlists_dirty.is_set():
            try:
                all_playlists = library.load_playlists()
                video_playlists = [p for p in all_playlists if p.is_video_playlist()]
                game_playlists = [p for p in all_playlists if p.is_game_playlist()]
                playlists = video_playlists
                selected_index = min(selected_index, max(len(playlists) - 1, 0))
            except Exception as exc:
                logging.getLogger(__name__).warning("Playlist reload failed: %s", exc)
            finally:
                playlists_dirty.clear()

        # Update fade transition - simplified: fade UI out, then show video
        if transition_dir != 0:
            elapsed = time.time() - transition_start
            p = max(0.0, min(1.0, elapsed / transition_duration))
            if transition_dir < 0:
                # Fade out UI (video is already playing)
                ui_alpha = 1.0 - p
                # Fade volume from 75% (menu) to 100% (video)
                current_volume = int(volume_menu + (volume_video - volume_menu) * p)
                mpv.set_volume(current_volume)
            else:
                # Fade in UI
                ui_alpha = p
                # Fade volume from 100% (video) to 75% (menu)
                current_volume = int(volume_video - (volume_video - volume_menu) * p)
                mpv.set_volume(current_volume)
            
            if p >= 1.0:
                # Transition finished
                # Volume is now at target (100% for video, 75% for menu)
                transition_dir = 0

        # Auto-progression: advance to next track/playlist when current track ends
        if has_playback and ui_hidden and controller.is_at_end():
            # Current track has ended
            if controller.playlist and controller.playlist.items:
                next_index = (controller.index + 1) % len(controller.playlist.items)
                if next_index == 0:
                    # Reached end of playlist, advance to next playlist
                    current_playlist_idx = -1
                    for idx, pl in enumerate(playlists):
                        if pl == controller.playlist:
                            current_playlist_idx = idx
                            break
                    if current_playlist_idx >= 0:
                        next_playlist_idx = (current_playlist_idx + 1) % len(playlists)
                        selected_index = next_playlist_idx
                        controller.load_playlist(playlists[next_playlist_idx])
                        controller.play_current()
                        playing_playlist = playlists[next_playlist_idx]  # Update currently playing playlist
                        saved_position = None  # Clear saved position for new playlist
                        # Ensure pygame window stays behind mpv for next video
                        try:
                            transition_mgr.transition_to_video()
                            transition_mgr.invalidate_cache()
                        except Exception:
                            pass
                        log.info("Auto-advanced to next playlist: %s", playlists[next_playlist_idx].title)
                else:
                    # Just advance to next track in current playlist
                    controller.next_item()
                    saved_position = None  # Clear saved position for new track
                    sample_mode.clear_markers()  # Clear markers when auto-advancing tracks
                    log.info("Auto-advanced to next track")

        # If UI is visible, ensure mpv overlay is off and UI stays visible
        # Check decorations periodically (every 60 frames ~1 second) to prevent WM from reapplying them
        if not ui_hidden:
            _deactivate_mpv_bezel_overlay()
            # Ensure UI visibility and check decorations periodically
            if frame_count % 60 == 0:  # Every 60 frames (~1 second) for better performance
                try:
                    transition_mgr.ensure_ui_visible()
                    # Ensure persistent undecorated state (single call, cached)
                    pg_id = window_mgr.pygame_window_id or pygame_window_id
                    if pg_id:
                        window_mgr.set_window_undecorated_persistent(pg_id)
                except Exception:
                    pass

        # When UI is hidden and video is playing, skip UI rendering for performance
        # mpv is fullscreen, so it covers everything - no window management needed
        if ui_hidden and has_playback:
            clock.tick(60)
            continue
        
        # Ensure cursor stays hidden and windows have no decorations (check periodically)
        # Check every 60 frames (~1 second) to prevent WM from reapplying decorations
        if frame_count % 60 == 0:  # Reduced frequency for better performance
            pygame.mouse.set_visible(False)
            if config.platform == "linux":
                # Hide cursor periodically
                window_mgr.hide_cursor_aggressive()
                # Ensure pygame window has persistent undecorated state (single call, cached)
                try:
                    pg_id = window_mgr.pygame_window_id or pygame_window_id
                    if pg_id:
                        window_mgr.set_window_undecorated_persistent(pg_id)
                except Exception:
                    pass
                # Also ensure mpv window has persistent undecorated state
                try:
                    mpv_id = transition_mgr._get_mpv_id(max_attempts=1)
                    if mpv_id:
                        window_mgr.set_window_undecorated_persistent(mpv_id)
                except Exception:
                    pass
        
        # Render
        show_overlay = (time.time() - overlay_last_interaction_ts) < config.overlay_fade_seconds
        renderer.render(playlists=playlists, selected_index=selected_index, controller=controller, show_overlay=show_overlay, ui_alpha=ui_alpha, ui_hidden=ui_hidden, has_playback=has_playback, sample_mode=sample_mode)

        # Render settings menu overlay (drawn over main UI)
        settings_renderer.render(content_surface, settings_menu, renderer.theme, game_playlists)

        # Apply CRT effects only when UI/content will be shown
        if not ui_hidden:
            crt_effects.apply_all(content_surface, time.time())

        # Composite content to actual screen with display mode handling
        display_mgr.present(screen, bezel, preserve_video_area=False, skip_content_blit=False)

        pygame.display.flip()
        clock.tick(60)
        frame_count += 1

    try:
        watcher.stop()
    finally:
        pygame.quit()


if __name__ == "__main__":
    try:
        run()
    except KeyboardInterrupt:
        sys.exit(0)


