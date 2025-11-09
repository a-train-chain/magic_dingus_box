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
from .web.admin import create_app
from .display.display_manager import DisplayManager, DisplayMode
from .display.bezel_loader import BezelLoader
from .display.crt_effects import CRTEffectsManager


def run() -> None:
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
            # Auto-detect actual screen size but clamp to 1080p for performance
            detected = (display_info.current_w, display_info.current_h)
            if detected[0] > 1920 or detected[1] > 1080:
                modern_resolution = (1920, 1080)
                log.info(f"Auto-detected {detected[0]}x{detected[1]} -> clamped to {modern_resolution[0]}x{modern_resolution[1]} for performance")
            else:
                modern_resolution = detected
                log.info(f"Auto-detected display resolution: {modern_resolution[0]}x{modern_resolution[1]}")
        else:
            modern_resolution = config._parse_resolution(modern_res_str)
    else:
        modern_resolution = (config.screen_width, config.screen_height)
    
    pygame.display.set_caption("Magic Dingus Box")
    flags = pygame.FULLSCREEN if config.fullscreen else 0
    
    # Create screen with appropriate resolution
    if display_mode == DisplayMode.CRT_NATIVE:
        screen = pygame.display.set_mode((config.screen_width, config.screen_height), flags)
        target_resolution = (config.screen_width, config.screen_height)
    else:
        screen = pygame.display.set_mode(modern_resolution, flags)
        target_resolution = modern_resolution
    
    pygame.mouse.set_visible(False)
    
    # Initialize display manager
    display_mgr = DisplayManager(display_mode, target_resolution)
    
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
    try:
        mpv.set_property("hwdec", "v4l2m2m")  # Force hardware decode via V4L2
        # Use x11 output - simpler and more reliable than gpu on Pi
        mpv.set_property("vo", "x11")  # Use X11 output (more reliable than gpu on Pi)
        # Use desync mode - play at native speed without syncing to display (prevents slowdown)
        mpv.set_property("video-sync", "desync")  # Play at native speed, don't sync to display
        mpv.set_property("video-latency-hacks", False)  # Disable latency hacks (can cause slowdown)
        mpv.set_property("interpolation", False)  # Disable interpolation for performance
        mpv.set_property("tscale", "oversample")  # Fast temporal scaling
        mpv.set_property("speed", 1.0)  # Ensure playback speed is 1.0x (normal speed)
        mpv.set_property("vd-lavc-threads", 4)  # Use more decoder threads
        mpv.set_property("vd-lavc-fast", True)  # Fast decoding
        mpv.set_property("vd-lavc-dr", True)  # Direct rendering
        mpv.set_property("vd-lavc-skiploopfilter", "all")  # Skip loop filter for speed
        mpv.set_property("vd-lavc-skipframe", "nonref")  # Skip non-reference frames
        mpv.set_property("sws-fast", True)  # Fast software scaling
        mpv.set_property("cache", True)  # Enable caching
        mpv.set_property("cache-secs", 30)  # Cache 30 seconds
        mpv.set_property("demuxer-max-bytes", "500M")  # Large demuxer buffer
        mpv.set_property("demuxer-max-back-bytes", "500M")  # Large backward buffer
        log.info("mpv configured for hardware-accelerated decoding and GPU rendering")
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

    # mpv uses X11 output - minimize pygame to allow mpv to render fullscreen
    # When video plays, we'll minimize pygame window to let mpv take over display
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
    try:
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
        
        if intro_path is not None:
            log.info(f"Playing ONLY intro video: {intro_path}")
            # CRITICAL: Stop any existing playback and clear playlist MULTIPLE times to ensure only intro plays
            for attempt in range(3):
                try:
                    mpv.stop()
                    # Clear any playlist that might be queued
                    import json
                    import socket
                    sock = socket.socket(socket.AF_UNIX)
                    sock.connect(config.mpv_socket)
                    sock.sendall(json.dumps({"command": ["playlist-clear"]}).encode() + b"\n")
                    sock.close()
                    time.sleep(0.1)
                except Exception:
                    pass
            log.info("Cleared mpv playlist multiple times to ensure only intro video plays")
            # Ensure we don't loop the intro
            mpv.set_loop_file(False)
            # Ensure normal playback speed and desync mode before loading
            try:
                mpv.set_property("speed", 1.0)
                mpv.set_property("video-sync", "desync")  # Force desync mode for normal speed
            except Exception:
                pass
            # mpv hardware decoding enabled via v4l2m2m
            # Minimize pygame window to let mpv take over fullscreen
            try:
                pygame.display.iconify()
                log.info("Minimized pygame window for intro video playback")
            except Exception as icon_exc:
                log.warning(f"Could not minimize window: {icon_exc}")
            
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
            
            # Load ONLY the 30fps intro video - verify it's the right file
            if "30fps" not in str(intro_path):
                log.error(f"ERROR: Wrong intro file selected: {intro_path} (should be intro.30fps.mp4)")
            mpv.load_file(str(intro_path))
            log.info(f"Loaded ONLY intro video: {intro_path}")
            
            # Wait a moment, then verify playlist has only one file
            time.sleep(0.3)
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
                    # Parse only the first JSON object (mpv may send multiple)
                    resp_str = resp.decode().split("\n")[0]
                    playlist_data = json.loads(resp_str)
                    if "data" in playlist_data:
                        playlist = playlist_data["data"]
                        if len(playlist) > 1:
                            log.warning(f"WARNING: Playlist has {len(playlist)} files! Clearing again...")
                            sock = socket.socket(socket.AF_UNIX)
                            sock.connect(config.mpv_socket)
                            sock.sendall(json.dumps({"command": ["playlist-clear"]}).encode() + b"\n")
                            sock.close()
                            time.sleep(0.1)
                            # Reload only the intro file
                            mpv.load_file(str(intro_path))
                            log.info(f"Reloaded ONLY intro video: {intro_path}")
                        else:
                            log.info(f"Playlist verified: {len(playlist)} file(s) - {intro_path.name}")
            except Exception as pl_exc:
                log.warning(f"Could not verify playlist: {pl_exc}")
            
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
                # Set video scaling to fill screen height with margins (letterboxing/pillarboxing)
                mpv.set_property("video-zoom", 0.0)  # Reset zoom
                mpv.set_property("panscan", 0.0)  # No pan/scan - show full video with margins
                mpv.set_property("video-aspect", -1)  # Use video's native aspect ratio
                # Set fullscreen for video playback
                mpv.set_fullscreen(True)
                # Wait a moment for fullscreen to activate, then ensure proper scaling
                time.sleep(0.3)
                # Force window to fill screen and center
                try:
                    mpv.set_property("window-scale", 1.0)  # Scale to window size
                except Exception:
                    pass
            except Exception:
                pass
            # Ensure any overlay is off; we'll draw bezel via pygame for consistent sizing
            _deactivate_mpv_bezel_overlay_vf()
            # mpv audio device already configured
            _apply_audio_device(timeout_seconds=2.0)
            
            # Start playback
            mpv.resume()
            
            # CRITICAL: Raise mpv window to front so it's visible
            # Wait a bit longer to ensure mpv has created the window and started playback
            time.sleep(0.5)
            try:
                import subprocess
                # Find and raise the mpv window - try multiple times to ensure it works
                for attempt in range(3):
                    result = subprocess.run(["xdotool", "search", "--class", "mpv", "windowmap", "windowraise"], 
                                          capture_output=True, timeout=2, check=False)
                    if result.returncode == 0:
                        log.info("Raised mpv window to front for intro video")
                        break
                    time.sleep(0.2)
                else:
                    log.warning("Could not raise mpv window after multiple attempts")
            except Exception as raise_exc:
                log.warning(f"Could not raise mpv window: {raise_exc}")
            
            intro_duration = float(settings_store.get("intro_duration", 10.0))
            # Optional: apply CRT effects during intro (default off for performance)
            intro_effects_enabled = bool(settings_store.get("intro_crt_effects", False))
            intro_start = time.time()
            
            # Verify video is actually playing
            try:
                is_playing = not mpv.get_property("pause")
                log.info(f"Intro video playback started, paused={not is_playing}")
            except Exception:
                pass
            
            while True:
                # Allow quit during intro
                for event in pygame.event.get():
                    if event.type == pygame.QUIT:
                        pygame.quit()
                        return
                
                # Don't render pygame during intro - mpv is handling the display
                # Just check for end conditions
                
                # End conditions: duration elapsed or mpv signaled EOF
                if (time.time() - intro_start) >= intro_duration:
                    break
                try:
                    if mpv.get_property("eof-reached") is True:
                        break
                except Exception:
                    pass
                
                clock.tick(60)
            
            # Fade transition from intro video to UI
            log.info("Starting fade transition from intro to UI")
            
            # Restore pygame window first (but keep it transparent initially)
            try:
                flags = pygame.FULLSCREEN if config.fullscreen else 0
                if display_mode == DisplayMode.CRT_NATIVE:
                    screen = pygame.display.set_mode((config.screen_width, config.screen_height), flags)
                else:
                    screen = pygame.display.set_mode(modern_resolution, flags)
                pygame.mouse.set_visible(False)
                # Update display manager with new screen
                display_mgr = DisplayManager(display_mode, target_resolution)
                content_surface = display_mgr.get_render_surface()
                renderer = UIRenderer(screen=content_surface, config=config)
                renderer.bezel_mode = (display_mode == DisplayMode.MODERN_WITH_BEZEL)
                log.info("Recreated pygame display surface for UI")
            except Exception as recreate_exc:
                log.warning(f"Could not recreate display: {recreate_exc}")
            
            # Raise pygame window so it's ready to fade in
            try:
                import subprocess
                subprocess.run(["xdotool", "search", "--name", "Magic Dingus Box", "windowmap", "windowraise"], 
                              capture_output=True, timeout=2, check=False)
            except Exception:
                pass
            
            # Fade transition: fade out video, fade in UI
            fade_duration = 1.0  # 1 second fade
            fade_start = time.time()
            
            # Initialize empty playlists for fade (will be loaded after fade completes)
            playlists_for_fade = []
            selected_index_for_fade = 0
            
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
                
                # Render UI content with fade (use empty playlists during fade)
                renderer.render(playlists=playlists_for_fade, selected_index=selected_index_for_fade, controller=controller, 
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
                    # Minimize mpv window
                    try:
                        import subprocess
                        subprocess.run(["xdotool", "search", "--class", "mpv", "windowminimize"], 
                                      capture_output=True, timeout=2, check=False)
                    except Exception:
                        pass
                    break
                
                clock.tick(60)
            
            log.info("Intro complete - UI faded in")
            played_intro = True
            
            # Ensure pygame window is raised and visible after intro
            try:
                import subprocess
                subprocess.run(["xdotool", "search", "--name", "Magic Dingus Box", "windowmap", "windowraise"], 
                              capture_output=True, timeout=2, check=False)
                log.info("Raised pygame window after intro")
            except Exception as raise_exc:
                log.warning(f"Could not raise pygame window: {raise_exc}")
            
            # Ensure UI is rendered once with actual playlists after intro fade
            # This ensures the UI is visible before entering the main loop
            try:
                renderer.render(playlists=playlists, selected_index=selected_index, controller=controller, 
                              show_overlay=False, ui_alpha=1.0, ui_hidden=False, 
                              has_playback=False, sample_mode=None)
                crt_effects.apply_all(content_surface, time.time())
                display_mgr.present(screen, bezel, preserve_video_area=False, skip_content_blit=False)
                pygame.display.flip()
                log.info("Rendered UI with playlists after intro")
            except Exception as render_exc:
                log.warning(f"Failed to render UI after intro: {render_exc}")
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

    # Input providers
    kb_provider = KeyboardInputProvider()
    js_provider = None
    try:
        pygame.joystick.init()
        if pygame.joystick.get_count() > 0:
            js = pygame.joystick.Joystick(0)
            js_provider = JoystickInputProvider(js)
            log.info(f"Joystick connected: {js.get_name()} (buttons={js.get_numbuttons()}, hats={js.get_numhats()}, axes={js.get_numaxes()})")
    except Exception as exc:
        log.warning(f"Joystick init failed: {exc}")
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
    
    # Track current playing playlist and position for resume functionality
    current_playing_playlist = None  # Playlist object currently playing
    saved_playback_position = None  # Saved position in seconds when navigating back to UI

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
    while running:
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

        for mapped in events:
            t = mapped.type
            if t == mapped.Type.QUIT:
                running = False
                break

            # Settings menu toggle (button 4 quick press when UI visible)
            if t == mapped.Type.SETTINGS_MENU:
                if not ui_hidden:  # Only open menu when UI is visible
                    # Button 4 only toggles the entire settings menu open/close
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
                    # Navigate back to UI while keeping audio playing
                    try:
                        # Disable video track but keep audio playing
                        mpv.set_property("video", "no")
                        # Save current playlist and position for resume
                        if controller.playlist:
                            current_playing_playlist = controller.playlist
                            elapsed, _ = controller.elapsed_and_duration()
                            saved_playback_position = elapsed if elapsed is not None else None
                            log.info(f"Saved playback position: {saved_playback_position}s for playlist: {current_playing_playlist.title}")
                            # Update selected_index to match the currently playing playlist
                            # Compare by source_path for reliability
                            if current_playing_playlist.source_path:
                                for idx, pl in enumerate(playlists):
                                    if (pl.source_path == current_playing_playlist.source_path and
                                        pl.source_path is not None):
                                        selected_index = idx
                                        break
                        # Exit fullscreen and restore pygame window
                        mpv.set_fullscreen(False)
                        # Restore pygame window
                        try:
                            import subprocess
                            subprocess.run(["xdotool", "search", "--name", "Magic Dingus Box", "windowmap", "windowraise"], 
                                          capture_output=True, timeout=2, check=False)
                            # Minimize mpv window
                            subprocess.run(["xdotool", "search", "--class", "mpv", "windowminimize"], 
                                          capture_output=True, timeout=2, check=False)
                        except Exception:
                            pass
                        # Show UI with fade-in
                        start_fade(1)
                        ui_hidden = False
                        log.info("Navigated back to UI, audio continues playing")
                    except Exception as nav_exc:
                        log.warning(f"Could not navigate back to UI: {nav_exc}")
                else:
                    # UI is visible - handle playlist selection
                    if playlists and selected_index < len(playlists):
                        selected_playlist = playlists[selected_index]
                        
                        # Check if this is the same playlist that's currently playing
                        # Compare by source_path for reliability (playlists may be reloaded)
                        is_same_playlist = (current_playing_playlist is not None and 
                                           has_playback and
                                           current_playing_playlist.source_path == selected_playlist.source_path and
                                           current_playing_playlist.source_path is not None)
                        
                        if is_same_playlist:
                            # Same playlist - resume from saved position
                            try:
                                # Re-enable video track
                                mpv.set_property("video", "auto")
                                mpv.set_fullscreen(True)
                                # Seek to saved position if available
                                if saved_playback_position is not None:
                                    controller.seek_absolute(saved_playback_position)
                                    log.info(f"Resumed playlist from position: {saved_playback_position}s")
                                # Ensure mpv window is visible
                                import subprocess
                                time.sleep(0.3)
                                subprocess.run(["xdotool", "search", "--class", "mpv", "windowmap", "windowraise"], 
                                              capture_output=True, timeout=2, check=False)
                                pygame.display.iconify()
                                # Start fade-out transition
                                start_fade(-1)
                                ui_hidden = True
                                log.info("Resumed same playlist with video")
                            except Exception as resume_exc:
                                log.warning(f"Could not resume playlist: {resume_exc}")
                        else:
                            # Different playlist (or no playlist playing) - switch to new playlist
                            # Stop current playback if different playlist
                            if has_playback and controller.playlist:
                                # Compare by source_path for reliability
                                if (controller.playlist.source_path != selected_playlist.source_path or
                                    controller.playlist.source_path is None):
                                    try:
                                        mpv.stop()
                                    except Exception:
                                        pass
                            
                            # Set volume to menu level (75%) before starting playback
                            try:
                                mpv.set_volume(volume_menu)
                            except Exception:
                                pass
                            
                            # Load new playlist and start playback
                            controller.load_playlist(selected_playlist)
                            controller.play_current()
                            current_playing_playlist = selected_playlist
                            saved_playback_position = None  # Reset saved position for new playlist
                            
                            # Ensure HDMI audio is bound for playlist playback
                            _apply_audio_device(timeout_seconds=2.0)
                            
                            # Ensure mpv is fullscreen and visible
                            try:
                                mpv.set_fullscreen(True)
                                import subprocess
                                time.sleep(0.3)
                                subprocess.run(["xdotool", "search", "--class", "mpv", "windowmap", "windowraise"], 
                                              capture_output=True, timeout=2, check=False)
                                pygame.display.iconify()
                                log.info("Video playback started, pygame window minimized, mpv raised")
                            except Exception as vid_exc:
                                log.warning(f"Could not start video playback: {vid_exc}")
                            
                            has_playback = True
                            # Start fade-out transition (volume will fade from 75% to 100%)
                            start_fade(-1)
                            # Hide UI immediately (don't wait for fade)
                            ui_hidden = True
                            log.info(f"Switched to new playlist: {selected_playlist.title}")
                overlay_last_interaction_ts = time.time()

            elif t == mapped.Type.NEXT:
                # Next track (triggered by quick press)
                controller.next_item()
                has_playback = True
                sample_mode.clear_markers()  # Clear markers when changing tracks
                overlay_last_interaction_ts = time.time()

            elif t == mapped.Type.PREV:
                # Previous track (triggered by quick press)
                controller.previous_item()
                has_playback = True
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
                        next_playlist = playlists[next_playlist_idx]
                        controller.load_playlist(next_playlist)
                        controller.play_current()
                        current_playing_playlist = next_playlist
                        saved_playback_position = None  # Reset saved position for new playlist
                        # Ensure pygame window stays minimized for next video
                        try:
                            pygame.display.iconify()
                        except Exception:
                            pass
                        log.info("Auto-advanced to next playlist: %s", next_playlist.title)
                else:
                    # Just advance to next track in current playlist
                    controller.next_item()
                    sample_mode.clear_markers()  # Clear markers when auto-advancing tracks
                    log.info("Auto-advanced to next track")

        # If UI became visible, ensure mpv overlay is off so pygame bezel shows
        if not ui_hidden:
            _deactivate_mpv_bezel_overlay()

        # When UI is hidden and video is playing, skip all rendering
        if ui_hidden and has_playback and transition_dir == 0:
            # Video is playing - don't render pygame, just tick clock
            clock.tick(60)
            continue
        
        # Render
        show_overlay = (time.time() - overlay_last_interaction_ts) < config.overlay_fade_seconds
        renderer.render(playlists=playlists, selected_index=selected_index, controller=controller, show_overlay=show_overlay, ui_alpha=ui_alpha, ui_hidden=ui_hidden, has_playback=has_playback, sample_mode=sample_mode)

        # Render settings menu overlay (drawn over main UI)
        settings_renderer.render(content_surface, settings_menu, renderer.theme, game_playlists)

        # Apply CRT effects only when UI/content will be shown (and during fade-out)
        if not ui_hidden or (transition_dir < 0 and ui_alpha > 0):
            crt_effects.apply_all(content_surface, time.time())

        # Composite content to actual screen with display mode handling
        # When UI is hidden (video playing), do not blit UI/content; preserve video area and draw bezel only
        # During fade-out (transition_dir < 0), still render UI with alpha fade so it fades out smoothly
        # Only skip content blit when UI is fully hidden (not during fade)
        skip_content = ui_hidden and (transition_dir == 0 or ui_alpha <= 0)
        display_mgr.present(screen, bezel, preserve_video_area=ui_hidden, skip_content_blit=skip_content)

        pygame.display.flip()
        clock.tick(60)

    try:
        watcher.stop()
    finally:
        pygame.quit()


if __name__ == "__main__":
    try:
        run()
    except KeyboardInterrupt:
        sys.exit(0)

