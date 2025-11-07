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
    
    # Determine display mode (settings override env vars)
    display_mode_str = settings_store.get_display_mode()
    if display_mode_str == "modern_clean":
        display_mode = DisplayMode.MODERN_CLEAN
    elif display_mode_str == "modern_bezel":
        display_mode = DisplayMode.MODERN_WITH_BEZEL
    else:
        display_mode = DisplayMode.CRT_NATIVE

    # Init pygame
    pygame.init()
    
    # Auto-detect screen resolution for modern modes
    if display_mode != DisplayMode.CRT_NATIVE:
        display_info = pygame.display.Info()
        # Use current display size, or user's configured size if set
        modern_res_str = settings_store.get_modern_resolution()
        if modern_res_str == "auto" or modern_res_str == "1920x1080":
            # Auto-detect actual screen size
            modern_resolution = (display_info.current_w, display_info.current_h)
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
    if display_mode == DisplayMode.MODERN_WITH_BEZEL:
        # Try to load bezel from file first
        bezel_style = settings_store.get("bezel_style", "retro_tv_1")
        bezel = bezel_loader.load_bezel(bezel_style, target_resolution)
        
        if bezel:
            log.info(f"Loaded bezel: {bezel_style}")
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

    # mpv + controller
    mpv = MpvClient(config.mpv_socket)
    # Ensure keys go to pygame/app, not mpv, and set audio device
    try:
        mpv.set_property("input-vo-keyboard", "no")
        mpv.set_property("input-default-bindings", "no")
        mpv.set_property("audio-device", config.audio_device)
    except Exception:
        pass
    controller = PlaybackController(
        mpv_client=mpv,
        settings_store=settings_store,
        assets_dir=config.repo_root / "assets"
    )
    
    # Sample mode manager
    sample_mode = SampleModeManager()
    
    # Enable 1-second audio fade-in for smooth transitions
    mpv.enable_audio_fade(fade_duration=1.0)

    # Embed mpv into pygame window (X11/Linux only)
    if config.platform == "linux":
        try:
            wm_info = pygame.display.get_wm_info()
            if "window" in wm_info:
                wid = wm_info["window"]
                # Give mpv a moment to connect, then set wid
                time.sleep(0.2)
                mpv.set_property("wid", int(wid))
                log.info("mpv embedded into pygame window with wid=%s", wid)
        except Exception as exc:
            log.warning("Failed to embed mpv: %s", exc)

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
        default_intro = config.media_dir / "intro.mp4"
        intro_path = None
        if isinstance(intro_setting, str) and intro_setting:
            p = Path(intro_setting).expanduser()
            if p.exists():
                intro_path = p
            else:
                # Auto-clear invalid cross-OS path and fall back to default if present
                settings_store.set("intro_video", "")
                if default_intro.exists():
                    intro_path = default_intro
        elif default_intro.exists():
            intro_path = default_intro
        
        if intro_path is not None:
            log.info(f"Playing intro video: {intro_path}")
            # If in bezel mode and we have a bezel image, overlay it via mpv to guarantee stacking
            bezel_overlay_active = False
            if display_mode == DisplayMode.MODERN_WITH_BEZEL:
                try:
                    bezel_image_path = bezel_loader.get_bezel_image_path(settings_store.get("bezel_style", "retro_tv_1"))
                except Exception:
                    bezel_image_path = None
                if bezel_image_path is not None:
                    # Build lavfi-complex graph to scale bezel to video and overlay at (0,0)
                    # Use scale2ref so bezel matches the current video size automatically.
                    lavfi = (
                        f"movie='{str(bezel_image_path)}',format=rgba[bz];"
                        f"[vid1]format=rgba[v];"
                        f"[bz][v]scale2ref=w=iw:h=ih[bzs][vs];"
                        f"[vs][bzs]overlay=x=0:y=0:format=auto"
                    )
                    try:
                        mpv.set_property("lavfi-complex", lavfi)
                        bezel_overlay_active = True
                        log.info("Applied mpv bezel overlay for intro")
                    except Exception as exc:
                        log.warning(f"Failed to apply mpv overlay: {exc}")
            # Ensure we don't loop the intro
            mpv.set_loop_file(False)
            mpv.load_file(str(intro_path))
            mpv.resume()
            
            intro_duration = float(settings_store.get("intro_duration", 10.0))
            # Optional: apply CRT effects during intro (default off for performance)
            intro_effects_enabled = bool(settings_store.get("intro_crt_effects", False))
            intro_start = time.time()
            
            while True:
                # Allow quit during intro
                for event in pygame.event.get():
                    if event.type == pygame.QUIT:
                        pygame.quit()
                        return
                
                # Clear overlay surface to fully transparent, then optionally apply CRT effects
                content_surface.fill((0, 0, 0, 0))
                if intro_effects_enabled:
                    crt_effects.apply_all(content_surface, time.time())
                # Present via display manager; when mpv overlay is active, bezel is handled by mpv
                display_mgr.present(screen, None if bezel_overlay_active else bezel)
                pygame.display.flip()
                
                # End conditions: duration elapsed or mpv signaled EOF
                if (time.time() - intro_start) >= intro_duration:
                    break
                try:
                    if mpv.get_property("eof-reached") is True:
                        break
                except Exception:
                    pass
                
                clock.tick(60)
            
            # Stop intro playback to return mpv to idle
            mpv.stop()
            # Clear any temporary overlay
            try:
                if bezel_overlay_active:
                    mpv.set_property("lavfi-complex", "")
                    log.info("Cleared mpv bezel overlay after intro")
            except Exception:
                pass
            log.info("Intro complete")
            played_intro = True
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
    pending_playlist_index = 0
    volume_menu = 75  # Volume when menu is visible
    volume_video = 100  # Volume when watching video

    def start_fade(direction: int) -> None:
        nonlocal transition_dir, transition_start
        transition_dir = direction
        transition_start = time.time()

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

        # Check for hold-to-seek keyboard events
        for ev in kb_provider.poll_seeking():
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
                    # Bring UI back (volume will fade during transition)
                    ui_hidden = False
                    start_fade(+1)
                else:
                    # Select playlist and fade out (volume will fade during transition)
                    if playlists and transition_dir == 0:
                        pending_playlist_index = selected_index
                        start_fade(-1)
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

        # Update fade transition
        if transition_dir != 0:
            elapsed = time.time() - transition_start
            p = max(0.0, min(1.0, elapsed / transition_duration))
            if transition_dir < 0:
                ui_alpha = 1.0 - p
                # Fade volume from 75% (menu) to 100% (video)
                current_volume = int(volume_menu + (volume_video - volume_menu) * p)
                mpv.set_volume(current_volume)
            else:
                ui_alpha = p
                # Fade volume from 100% (video) to 75% (menu)
                current_volume = int(volume_video - (volume_video - volume_menu) * p)
                mpv.set_volume(current_volume)
            
            if p >= 1.0:
                # Transition finished
                if transition_dir < 0 and not ui_hidden:
                    # Fade-out complete: load and play the selected playlist
                    if playlists and pending_playlist_index < len(playlists):
                        selected_playlist = playlists[pending_playlist_index]
                        # Only restart if switching to a different playlist
                        if controller.playlist is None or controller.playlist != selected_playlist:
                            controller.load_playlist(selected_playlist)
                            controller.play_current()
                        has_playback = True
                    # Hide UI (volume already at 100% from fade)
                    ui_hidden = True
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
                        log.info("Auto-advanced to next playlist: %s", playlists[next_playlist_idx].title)
                else:
                    # Just advance to next track in current playlist
                    controller.next_item()
                    sample_mode.clear_markers()  # Clear markers when auto-advancing tracks
                    log.info("Auto-advanced to next track")

        # Render
        show_overlay = (time.time() - overlay_last_interaction_ts) < config.overlay_fade_seconds
        renderer.render(playlists=playlists, selected_index=selected_index, controller=controller, show_overlay=show_overlay, ui_alpha=ui_alpha, ui_hidden=ui_hidden, has_playback=has_playback, sample_mode=sample_mode)

        # Render settings menu overlay (drawn over main UI)
        settings_renderer.render(content_surface, settings_menu, renderer.theme, game_playlists)

        # Apply CRT effects to the entire composed frame (over everything)
        crt_effects.apply_all(content_surface, time.time())

        # Composite content to actual screen with display mode handling
        display_mgr.present(screen, bezel)

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

