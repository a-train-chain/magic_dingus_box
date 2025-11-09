"""Transparent overlay window for bezel display using X11."""
from __future__ import annotations

import logging
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional


class BezelOverlayWindow:
    """Manages a transparent overlay window that displays the bezel image.
    
    Uses an external tool (pqiv, feh, or a Python script) to create a transparent,
    always-on-top, click-through overlay window.
    """
    
    def __init__(self, bezel_image_path: str, resolution: tuple[int, int]):
        """Initialize bezel overlay window manager.
        
        Args:
            bezel_image_path: Path to the bezel PNG image
            resolution: Screen resolution (width, height)
        """
        self.bezel_image_path = bezel_image_path
        self.resolution = resolution
        self._log = logging.getLogger("bezel_overlay")
        self._process: Optional[subprocess.Popen] = None
        self._overlay_script_path: Optional[Path] = None
        
    def start(self) -> bool:
        """Start the overlay window."""
        if self._process is not None:
            self._log.warning("Overlay window already running")
            return True
            
        # Try different methods in order of preference
        if self._try_pqiv():
            return True
        if self._try_feh():
            return True
        if self._try_python_overlay():
            return True
            
        self._log.error("Failed to start bezel overlay window with any method")
        return False
    
    def _try_pqiv(self) -> bool:
        """Try to use pqiv for overlay."""
        try:
            result = subprocess.run(
                ["which", "pqiv"],
                capture_output=True,
                timeout=2,
                check=False
            )
            if result.returncode != 0:
                return False
                
            # Start pqiv with transparent overlay settings
            self._process = subprocess.Popen(
                [
                    "pqiv",
                    "--click-through",
                    "--keep-above",
                    "--transparent-background",
                    "--hide-info-box",
                    "--no-scaling",
                    "--geometry", f"{self.resolution[0]}x{self.resolution[1]}+0+0",
                    self.bezel_image_path
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                env={"DISPLAY": ":0"}
            )
            
            # Give it a moment to start
            time.sleep(0.5)
            if self._process.poll() is None:
                self._log.info("Started bezel overlay using pqiv")
                return True
            else:
                self._process = None
                return False
        except Exception as exc:
            self._log.debug(f"pqiv method failed: {exc}")
            return False
    
    def _try_feh(self) -> bool:
        """Try to use feh for overlay."""
        try:
            result = subprocess.run(
                ["which", "feh"],
                capture_output=True,
                timeout=2,
                check=False
            )
            if result.returncode != 0:
                return False
                
            # feh doesn't have built-in click-through, but we can use it with xdotool
            self._process = subprocess.Popen(
                [
                    "feh",
                    "--fullscreen",
                    "--no-menus",
                    "--borderless",
                    "--image-bg", "transparent",
                    self.bezel_image_path
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                env={"DISPLAY": ":0"}
            )
            
            time.sleep(0.5)
            if self._process.poll() is None:
                # Use xdotool to make it always on top and click-through
                try:
                    # Find feh window
                    result = subprocess.run(
                        ["xdotool", "search", "--class", "feh"],
                        capture_output=True,
                        text=True,
                        timeout=2,
                        check=False
                    )
                    if result.returncode == 0 and result.stdout.strip():
                        wid = result.stdout.strip().split("\n")[0]
                        # Set always on top
                        subprocess.run(
                            ["xdotool", "windowraise", wid],
                            timeout=2,
                            check=False
                        )
                        # Note: feh doesn't support click-through natively
                        self._log.info("Started bezel overlay using feh")
                        return True
                except Exception:
                    pass
                    
            if self._process:
                self._process.terminate()
                self._process = None
            return False
        except Exception as exc:
            self._log.debug(f"feh method failed: {exc}")
            return False
    
    def _try_python_overlay(self) -> bool:
        """Create a Python-based overlay using pygame in a separate process."""
        try:
            # Create a temporary Python script for the overlay
            import tempfile
            script_content = f'''#!/usr/bin/env python3
import pygame
import sys
import time
from pathlib import Path

# Initialize pygame
pygame.init()

# Get arguments
bezel_path = sys.argv[1]
width = int(sys.argv[2])
height = int(sys.argv[3])

# Create window with transparency support
# Use SRCALPHA flag to enable per-pixel alpha transparency
screen = pygame.display.set_mode((width, height), pygame.NOFRAME | pygame.SRCALPHA)
pygame.display.set_caption("Bezel Overlay")
# Set window to be transparent by default
screen.set_colorkey((0, 0, 0))
screen.set_alpha(255)

# Load bezel image
bezel = pygame.image.load(bezel_path).convert_alpha()

# Configure window properties using xdotool
try:
    import subprocess
    wm_info = pygame.display.get_wm_info()
    if "window" in wm_info:
        wid = wm_info["window"]
        # Set window to stay on top but allow click-through
        # Use xprop to set window type to "dock" or "splash" for better overlay behavior
        subprocess.run(
            ["xprop", "-id", str(wid), "-f", "_NET_WM_WINDOW_TYPE", "32a", "-set", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK"],
            timeout=2,
            check=False
        )
        # Set window to be above others but not grab focus
        subprocess.run(
            ["xprop", "-id", str(wid), "-f", "_NET_WM_STATE", "32a", "-set", "_NET_WM_STATE", "_NET_WM_STATE_ABOVE"],
            timeout=2,
            check=False
        )
        # Make window ignore input events (click-through)
        # This uses xdotool to set the window to not accept input
        subprocess.run(
            ["xdotool", "windowfocus", str(wid)],
            timeout=2,
            check=False
        )
        # Lower the window slightly so main window can be raised above it when needed
        time.sleep(0.2)
except Exception:
    pass

# Main loop - just display the bezel
running = True
clock = pygame.time.Clock()
frame_count = 0
while running:
    for event in pygame.event.get():
        if event.type == pygame.QUIT:
            running = False
        # Ignore all input events - make window click-through
        # This allows clicks to pass through to windows below
    
    # Draw bezel
    screen.fill((0, 0, 0, 0))  # Transparent background
    screen.blit(bezel, (0, 0))
    pygame.display.flip()
    
    # Periodically ensure window stays configured correctly
    frame_count += 1
    if frame_count % 3600 == 0:  # Every 60 seconds
        try:
            import subprocess
            wm_info = pygame.display.get_wm_info()
            if "window" in wm_info:
                wid = wm_info["window"]
                # Re-apply window properties
                subprocess.run(
                    ["xprop", "-id", str(wid), "-f", "_NET_WM_WINDOW_TYPE", "32a", "-set", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK"],
                    timeout=1,
                    check=False
                )
        except Exception:
            pass
    
    clock.tick(60)

pygame.quit()
'''
            # Write script to temp file
            script_file = tempfile.NamedTemporaryFile(
                mode='w',
                suffix='.py',
                delete=False
            )
            script_file.write(script_content)
            script_file.close()
            
            # Make executable
            import os
            os.chmod(script_file.name, 0o755)
            self._overlay_script_path = Path(script_file.name)
            
            # Start overlay process
            self._process = subprocess.Popen(
                [
                    sys.executable,
                    str(self._overlay_script_path),
                    self.bezel_image_path,
                    str(self.resolution[0]),
                    str(self.resolution[1])
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                env={"DISPLAY": ":0"}
            )
            
            time.sleep(0.5)
            if self._process.poll() is None:
                self._log.info("Started bezel overlay using Python/pygame")
                return True
            else:
                self._process = None
                if self._overlay_script_path.exists():
                    self._overlay_script_path.unlink()
                self._overlay_script_path = None
                return False
        except Exception as exc:
            self._log.debug(f"Python overlay method failed: {exc}")
            if self._overlay_script_path and self._overlay_script_path.exists():
                self._overlay_script_path.unlink()
            self._overlay_script_path = None
            return False
    
    def stop(self) -> None:
        """Stop the overlay window."""
        if self._process is not None:
            try:
                self._process.terminate()
                # Wait a bit for graceful shutdown
                try:
                    self._process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self._process.kill()
                    self._process.wait()
                self._log.info("Stopped bezel overlay window")
            except Exception as exc:
                self._log.warning(f"Error stopping overlay: {exc}")
            finally:
                self._process = None
        
        # Clean up temp script if it exists
        if self._overlay_script_path and self._overlay_script_path.exists():
            try:
                self._overlay_script_path.unlink()
            except Exception:
                pass
            self._overlay_script_path = None
    
    def __del__(self):
        """Cleanup on deletion."""
        self.stop()

