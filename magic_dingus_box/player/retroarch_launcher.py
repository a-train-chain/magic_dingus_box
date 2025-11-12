"""RetroArch emulator launcher for seamless game integration."""
from __future__ import annotations

import logging
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional


class RetroArchLauncher:
    """Launches RetroArch emulator with specified cores and ROMs."""
    
    def __init__(self):
        self._log = logging.getLogger("retroarch")
        # Common RetroArch executable paths (prefer RetroPie)
        self.retroarch_paths = [
            "/opt/retropie/emulators/retroarch/bin/retroarch",  # RetroPie (preferred)
            "/usr/bin/retroarch",           # Linux standard
            "/Applications/RetroArch.app/Contents/MacOS/RetroArch",  # macOS
        ]
        self.retroarch_bin = self._find_retroarch()
        # Path to wrapper script (for full process isolation on Pi)
        self.wrapper_script = self._find_wrapper_script()
        
    def _find_retroarch(self) -> Optional[str]:
        """Find RetroArch executable."""
        for path in self.retroarch_paths:
            if Path(path).exists():
                self._log.info(f"Found RetroArch at: {path}")
                return path
        self._log.warning("RetroArch not found. Install with: sudo apt install retroarch (Linux) or brew install --cask retroarch (macOS)")
        return None
    
    def _find_wrapper_script(self) -> Optional[str]:
        """Find the RetroArch wrapper script for full process isolation."""
        # Look for wrapper script in scripts directory relative to repo root
        repo_root = Path(__file__).parent.parent.parent
        wrapper = repo_root / "scripts" / "launch_retroarch.sh"
        if wrapper.exists():
            return str(wrapper)
        return None
    
    def launch_game(self, rom_path: str, core: str, overlay_path: str = None) -> bool:
        """Launch a game with RetroArch.
        
        On Raspberry Pi (Linux), uses wrapper script to stop UI service,
        launch RetroArch as the only process, then restart UI service.
        
        On macOS/dev, launches RetroArch directly as subprocess.
        
        Args:
            rom_path: Path to the ROM file
            core: RetroArch core name (e.g., 'snes9x_libretro', 'mupen64plus_next_libretro')
            overlay_path: Optional path to overlay/bezel PNG file
        
        Returns:
            True if game launched and exited cleanly, False otherwise
        """
        if not self.retroarch_bin:
            self._log.error("RetroArch not installed")
            return False
        
        rom = Path(rom_path).expanduser().resolve()
        if not rom.exists():
            self._log.error(f"ROM not found: {rom}")
            return False
        
        # Release controllers before launching RetroArch (on Linux)
        if sys.platform.startswith("linux"):
            self._release_controllers()
        
        # On Raspberry Pi, use wrapper script for full process isolation
        if sys.platform.startswith("linux") and self.wrapper_script:
            return self._launch_via_wrapper(rom_path, core, overlay_path)
        
        # On macOS/dev, launch directly (original behavior)
        return self._launch_direct(rom_path, core, overlay_path)
    
    def _launch_via_wrapper(self, rom_path: str, core: str, overlay_path: str = None) -> bool:
        """Launch RetroArch via wrapper script (stops UI, runs RetroArch, restarts UI)."""
        self._log.info("Launching RetroArch via wrapper script (full process isolation)")
        self._log.info("DEBUG: Received core parameter: %s", repr(core))
        
        # Determine systemd service name
        service_name = os.getenv("MAGIC_UI_SERVICE", "magic-ui.service")
        
        cmd = [
            "/bin/bash",
            self.wrapper_script,
            str(rom_path),
            core,
            overlay_path or "",
            service_name
        ]
        
        self._log.info(f"Executing wrapper: {' '.join(cmd)}")
        self._log.info("DEBUG: Core being passed to wrapper: %s", repr(core))
        
        try:
            # CRITICAL: Launch wrapper script in a way that survives service shutdown
            # Use the same method as core downloader - systemd-run or setsid
            try:
                # Use systemd-run with --scope to create an independent scope that won't be killed
                # when the UI service stops
                systemd_cmd = [
                    "systemd-run",
                    "--user",
                    "--scope",
                    "--unit=magic-retroarch-launcher",
                    "--no-block",
                    "/bin/bash",
                    self.wrapper_script,
                    str(rom_path),
                    core,
                    overlay_path or "",
                    service_name
                ]
                process = subprocess.Popen(
                    systemd_cmd,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    stdin=subprocess.DEVNULL,
                )
                self._log.info(f"Game wrapper launched via systemd-run --scope with PID: {process.pid}")
            except (FileNotFoundError, OSError) as e:
                self._log.warning(f"systemd-run failed: {e}, trying fallback")
                # Fallback: use nohup and setsid to detach from parent
                process = subprocess.Popen(
                    cmd,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    stdin=subprocess.DEVNULL,
                    preexec_fn=os.setsid,  # Create new session, detach from parent
                    start_new_session=True,  # Also set this for extra safety
                )
                self._log.info(f"Game wrapper launched with PID: {process.pid} (detached via setsid)")
            
            # Give wrapper time to stop the service (which will cause this process to exit)
            time.sleep(2)
            
            # If we reach here, the service didn't stop (shouldn't happen)
            self._log.warning("Wrapper launched but service didn't stop - process may still be running")
            return True
            
        except Exception as e:
            self._log.error(f"Failed to launch RetroArch wrapper: {e}")
            return False
    
    def _launch_direct(self, rom_path: str, core: str, overlay_path: str = None) -> bool:
        """Launch RetroArch directly as subprocess (macOS/dev mode)."""
        # On Linux, cores don't have the _libretro suffix in the -L argument
        core_name = core.replace("_libretro", "") if sys.platform.startswith("linux") else core
        
        # Create a temporary RetroArch config to enforce ultra-low internal resolution (96x72).
        # Include overlay settings if provided.
        temp_config_path = None
        try:
            import tempfile
            temp_config = tempfile.NamedTemporaryFile(mode='w', suffix='.cfg', delete=False)
            temp_config_path = temp_config.name
            # Base ultra-low resolution settings (96x72), windowed fullscreen
            temp_config.write("# Temporary RetroArch config for Magic Dingus Box (mac/dev)\n")
            temp_config.write("video_fullscreen = \"false\"\n")
            temp_config.write("video_windowed_fullscreen = \"true\"\n")
            temp_config.write("video_custom_viewport_width = \"96\"\n")
            temp_config.write("video_custom_viewport_height = \"72\"\n")
            temp_config.write("video_custom_viewport_enable = \"true\"\n")
            temp_config.write("video_custom_viewport_x = \"0\"\n")
            temp_config.write("video_custom_viewport_y = \"0\"\n")
            temp_config.write("aspect_ratio_index = \"22\"\n")
            temp_config.write("video_aspect_ratio = \"1.333\"\n")
            temp_config.write("video_force_aspect = \"false\"\n")
            temp_config.write("video_scale_integer = \"true\"\n")
            temp_config.write("video_scale = \"1.0\"\n")
            # Disable expensive effects
            temp_config.write("video_shader_enable = \"false\"\n")
            temp_config.write("video_filter = \"\"\n")
            temp_config.write("video_smooth = \"false\"\n")
            # If an overlay is provided and a matching .cfg exists, include it
            if overlay_path:
                overlay_png = Path(overlay_path)
                overlay_cfg = overlay_png.with_suffix('.cfg')
                if overlay_cfg.exists():
                    temp_config.write("input_overlay_enable = \"true\"\n")
                    temp_config.write(f"input_overlay = \"{str(overlay_cfg.resolve())}\"\n")
                    temp_config.write("input_overlay_opacity = \"1.0\"\n")
                    temp_config.write("input_overlay_scale = \"1.0\"\n")
            temp_config.close()
            self._log.info("Created temp RetroArch config enforcing 96x72 internal resolution")
        except Exception as exc:
            self._log.warning(f"Failed to create temp config: {exc}")
            temp_config_path = None
        
        # RetroArch command line arguments
        cmd = [
            self.retroarch_bin,
            "-L", core_name,         # Load specific libretro core
            str(rom_path),           # ROM path
            "--verbose",             # Enable logging
        ]
        
        # Use temporary config if overlay is enabled
        if temp_config_path:
            cmd.extend(["--config", temp_config_path])
        
        self._log.info(f"Launching: {' '.join(cmd)}")
        
        try:
            # Launch RetroArch asynchronously so we can find and modify its window
            process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )
            
            # Wait a moment for RetroArch window to appear, then remove decorations
            if sys.platform.startswith("linux"):
                self._remove_retroarch_decorations()
            
            # Wait for RetroArch to exit
            stdout, stderr = process.communicate()
            
            if process.returncode == 0:
                self._log.info("Game exited normally")
                return True
            else:
                self._log.warning(f"RetroArch exited with code {process.returncode}")
                if stderr:
                    self._log.debug(f"stderr: {stderr}")
                return False
                
        except Exception as e:
            self._log.error(f"Failed to launch RetroArch: {e}")
            return False
        finally:
            # Clean up temporary config file
            if temp_config_path:
                try:
                    os.unlink(temp_config_path)
                    self._log.debug("Cleaned up temp config")
                except:
                    pass
    
    def _release_controllers(self) -> None:
        """Release controller devices before RetroArch launches.
        
        This ensures RetroArch can grab controllers cleanly without conflicts.
        """
        if not sys.platform.startswith("linux"):
            return
        
        self._log.info("Releasing controller devices before RetroArch launch")
        
        try:
            # Release pygame joystick if initialized
            try:
                import pygame
                pygame.joystick.quit()
                self._log.debug("Released pygame joystick")
            except Exception:
                pass
            
            # Release evdev devices if available
            try:
                from ..inputs.evdev_joystick import EvdevJoystickInputProvider
                # Note: We can't access the instance directly, but we can trigger udev reset
                # The wrapper script will handle the actual device release
                self._log.debug("Controller release initiated - wrapper script will handle device reset")
            except Exception:
                pass
            
            # Trigger udev to reset controller devices
            import subprocess
            import os
            env = os.environ.copy()
            env["DISPLAY"] = ":0"
            
            # Reset joystick devices
            for js_device in ["/dev/input/js0", "/dev/input/js1"]:
                if Path(js_device).exists():
                    try:
                        subprocess.run(
                            ["udevadm", "trigger", "--action=change", "--subsystem-match=input", js_device],
                            timeout=1.0,
                            check=False,
                            env=env,
                            capture_output=True
                        )
                        self._log.debug(f"Triggered udev reset for {js_device}")
                    except Exception:
                        pass
            
            # Small delay to allow devices to be released
            time.sleep(0.2)
            
        except Exception as exc:
            self._log.warning(f"Error releasing controllers: {exc}")
    
    def _remove_retroarch_decorations(self) -> None:
        """Find RetroArch window and remove its decorations for seamless display."""
        if not sys.platform.startswith("linux"):
            return
        
        try:
            env = os.environ.copy()
            env["DISPLAY"] = ":0"
            
            # Wait for RetroArch window to appear (up to 2 seconds)
            retroarch_window_id = None
            for attempt in range(20):  # 20 attempts * 0.1s = 2 seconds max
                time.sleep(0.1)
                try:
                    # Try to find RetroArch window by class name or window title
                    result = subprocess.run(
                        ["xdotool", "search", "--class", "retroarch"],
                        capture_output=True,
                        timeout=0.5,
                        check=False,
                        env=env
                    )
                    if result.returncode == 0 and result.stdout.strip():
                        retroarch_window_id = result.stdout.decode().strip().split("\n")[0]
                        self._log.info(f"Found RetroArch window: {retroarch_window_id}")
                        break
                except Exception:
                    pass
            
            if retroarch_window_id:
                # Remove window decorations using _MOTIF_WM_HINTS
                subprocess.run(
                    ["xprop", "-id", retroarch_window_id, "-f", "_MOTIF_WM_HINTS", "32c", 
                     "-set", "_MOTIF_WM_HINTS", "2", "0", "0", "0", "0"],
                    timeout=1.0,
                    check=False,
                    env=env
                )
                
                # Hide cursor aggressively
                subprocess.run(
                    ["xsetroot", "-cursor_name", "none"],
                    timeout=0.5,
                    check=False,
                    env=env
                )
                subprocess.run(
                    ["xdotool", "mousemove", "10000", "10000"],
                    timeout=0.5,
                    check=False,
                    env=env
                )
                
                self._log.info("Removed RetroArch window decorations and hid cursor")
        except Exception as exc:
            self._log.debug(f"Could not remove RetroArch decorations: {exc}")
    
    def open_core_downloader(self) -> bool:
        """Open RetroArch Core Downloader menu.
        
        Launches RetroArch with menu open, then navigates to Core Downloader.
        On Raspberry Pi, uses wrapper script to stop UI service first.
        
        Returns:
            True if RetroArch launched successfully, False otherwise
        """
        if not self.retroarch_bin:
            self._log.error("RetroArch not installed")
            return False
        
        # Release controllers before launching RetroArch (on Linux)
        if sys.platform.startswith("linux"):
            self._release_controllers()
        
        # On Raspberry Pi, use wrapper script for full process isolation
        if sys.platform.startswith("linux") and self.wrapper_script:
            return self._open_core_downloader_via_wrapper()
        
        # On macOS/dev, launch directly
        return self._open_core_downloader_direct()
    
    def _open_core_downloader_via_wrapper(self) -> bool:
        """Open Core Downloader via wrapper script (stops UI, runs RetroArch, restarts UI).
        
        This will cause the UI process to exit when the service stops.
        The wrapper script handles stopping the service, launching RetroArch, and restarting the service.
        """
        self._log.info("Opening RetroArch Core Downloader via wrapper script")
        
        # Use the existing launch_retroarch.sh script with "menu" as the core
        # This reuses the proven wrapper script that works for games
        import os  # Import os at function level to ensure it's available
        service_name = os.getenv("MAGIC_UI_SERVICE", "magic-ui.service")
        
        if not self.wrapper_script:
            self._log.error("Wrapper script not found")
            return False
        
        # Use the existing wrapper script with "menu" parameter to launch Core Downloader
        cmd = [
            "/bin/bash",
            self.wrapper_script,
            "menu",  # ROM_PATH (not used for menu)
            "menu",  # CORE (triggers Core Downloader mode)
            "",      # OVERLAY_PATH (not needed for menu)
            service_name
        ]
        
        self._log.info(f"Executing Core Downloader via wrapper: {' '.join(cmd)}")
        
        try:
            # CRITICAL: Launch wrapper script in a way that survives service shutdown
            # Use systemd-run with --scope to create an independent scope that won't be killed
            # when the UI service stops
            try:
                # Use systemd-run with --scope and --no-block to create independent process
                # The --scope flag ensures it's not tied to the current service
                systemd_cmd = [
                    "systemd-run",
                    "--user",
                    "--scope",
                    "--unit=magic-retroarch-launcher",
                    "--no-block",
                    "/bin/bash",
                    self.wrapper_script,
                    "menu",  # ROM_PATH
                    "menu",  # CORE
                    "",      # OVERLAY_PATH
                    service_name
                ]
                process = subprocess.Popen(
                    systemd_cmd,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    stdin=subprocess.DEVNULL,
                )
                self._log.info(f"Core Downloader wrapper launched via systemd-run --scope with PID: {process.pid}")
            except (FileNotFoundError, OSError) as e:
                self._log.warning(f"systemd-run failed: {e}, trying fallback")
                # Fallback: use nohup and setsid to detach from parent
                # os is already imported above
                process = subprocess.Popen(
                    cmd,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    stdin=subprocess.DEVNULL,
                    preexec_fn=os.setsid,  # Create new session, detach from parent
                    start_new_session=True,  # Also set this for extra safety
                )
                self._log.info(f"Core Downloader wrapper launched with PID: {process.pid} (detached via setsid)")
            
            # Give wrapper time to stop the service (which will cause this process to exit)
            time.sleep(2)
            
            # If we reach here, the service didn't stop (shouldn't happen)
            self._log.warning("Wrapper launched but service didn't stop - process may still be running")
            return True
            
        except Exception as e:
            self._log.error(f"Failed to launch Core Downloader wrapper: {e}")
            return False
    
    def _open_core_downloader_direct(self) -> bool:
        """Open Core Downloader directly (macOS/dev mode)."""
        self._log.info("Opening RetroArch Core Downloader")
        
        # Launch RetroArch with menu
        cmd = [
            self.retroarch_bin,
            "--menu",  # Open RetroArch menu
        ]
        
        self._log.info(f"Launching: {' '.join(cmd)}")
        
        try:
            # Launch RetroArch - user will navigate to Online Updater -> Core Downloader
            process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            
            # Note: On macOS/dev, we don't stop the UI, so user can navigate manually
            # In RetroArch menu: Online Updater -> Core Downloader
            self._log.info("RetroArch launched. Navigate to: Online Updater -> Core Downloader")
            return True
            
        except Exception as e:
            self._log.error(f"Failed to launch RetroArch: {e}")
            return False


# Core mappings for common systems (RetroPie cores)
RETROARCH_CORES = {
    "NES": "nestopia_libretro",         # NES/Famicom (64-bit, works on aarch64)
    "SNES": "snes9x_libretro",          # Super Nintendo (64-bit, works on aarch64)
    "N64": "mupen64plus-next_libretro",  # Nintendo 64 (mupen64plus-next - OpenGLES compatible)
    "PS1": "pcsx_rearmed_libretro",     # PlayStation 1 (lr-pcsx-rearmed) - needs 64-bit build
    "GBA": "mgba_libretro",             # Game Boy Advance
    "GB": "gambatte_libretro",          # Game Boy
    "GBC": "gambatte_libretro",         # Game Boy Color
    "Genesis": "genesis_plus_gx_libretro",  # Sega Genesis/Mega Drive
}

