"""Unified transition manager for smooth mpv <-> UI transitions.

This module provides atomic, seamless transitions between video playback (mpv)
and the UI (pygame), ensuring no visible artifacts or animations.
"""

from __future__ import annotations

import logging
import time
from typing import Optional

from .window_manager import WindowManager


class TransitionManager:
    """Manages seamless transitions between mpv video and pygame UI.
    
    Provides atomic operations that handle all window management in the correct
    order to prevent visible transitions, resizing, or minimizing animations.
    """
    
    def __init__(self, window_mgr: WindowManager, screen_width: int, screen_height: int) -> None:
        """Initialize TransitionManager.
        
        Args:
            window_mgr: WindowManager instance for X11 operations
            screen_width: Screen width in pixels
            screen_height: Screen height in pixels
        """
        self._log = logging.getLogger("transition_manager")
        self.window_mgr = window_mgr
        self.screen_width = screen_width
        self.screen_height = screen_height
        
        # Cache window IDs to avoid repeated lookups
        self._mpv_id: Optional[str] = None
        self._pygame_id: Optional[str] = None
        self._last_mpv_lookup = 0.0
        self._last_pygame_lookup = 0.0
        self._lookup_cache_ttl = 5.0  # Cache window IDs for 5 seconds
    
    def _get_mpv_id(self, force_refresh: bool = False, max_attempts: int = 1, retry_delay: float = 0.2) -> Optional[str]:
        """Get mpv window ID, with caching and optional retry logic.
        
        Args:
            force_refresh: If True, force a fresh lookup
            max_attempts: Maximum number of attempts to find window (default: 1 for cached lookups)
            retry_delay: Delay between retry attempts in seconds
            
        Returns:
            mpv window ID or None if not found
        """
        now = time.time()
        if force_refresh or not self._mpv_id or (now - self._last_mpv_lookup) > self._lookup_cache_ttl:
            # Retry logic for finding window (useful when window is being created)
            for attempt in range(max_attempts):
                self._mpv_id = self.window_mgr.find_window_by_class("mpv")
                if self._mpv_id:
                    break
                if attempt < max_attempts - 1:
                    time.sleep(retry_delay)
            self._last_mpv_lookup = now
        return self._mpv_id
    
    def _get_pygame_id(self, force_refresh: bool = False) -> Optional[str]:
        """Get pygame window ID, with caching.
        
        Args:
            force_refresh: If True, force a fresh lookup
            
        Returns:
            pygame window ID or None if not found
        """
        now = time.time()
        if force_refresh or not self._pygame_id or (now - self._last_pygame_lookup) > self._lookup_cache_ttl:
            # Try stored ID first
            self._pygame_id = self.window_mgr.pygame_window_id
            if not self._pygame_id:
                # Fallback to search
                self._pygame_id = self.window_mgr.find_window_by_name("Magic Dingus Box")
            self._last_pygame_lookup = now
        return self._pygame_id
    
    def transition_to_ui(self) -> bool:
        """Seamlessly transition from mpv video to pygame UI.
        
        Order of operations:
        1. Hide cursor (X11 level)
        2. Hide mpv window FIRST (before touching pygame)
        3. Hide pygame window completely (move off-screen + unmap + HIDDEN state)
        4. Prepare pygame window (size/position) while completely hidden/unmapped
        5. Show pygame window instantly (already correct size/position)
        
        Returns:
            True if transition was successful
        """
        self._log.debug("Starting transition: mpv -> UI")
        
        pygame_id = self._get_pygame_id(force_refresh=True)
        mpv_id = self._get_mpv_id(force_refresh=True)
        
        if not pygame_id:
            self._log.warning("transition_to_ui: Could not find pygame window")
            return False
        
        # Step 0: Hide cursor aggressively (X11 level) - do this FIRST
        self.window_mgr.hide_cursor_aggressive()
        
        # Step 1: Hide mpv window FIRST (before touching pygame) to prevent any overlap
        # Use batched operations for better performance
        if mpv_id:
            try:
                # Batch: unmap + move off-screen in single xdotool call
                self.window_mgr._run_xdotool_batch([
                    ["windowunmap", mpv_id],
                    ["windowmove", mpv_id, "-10000", "-10000"]
                ], timeout=0.2)
                # Set HIDDEN state (xprop can't be batched with xdotool)
                import subprocess
                subprocess.run(
                    ["xprop", "-id", mpv_id, "-f", "_NET_WM_STATE", "32a", "-set", "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN"],
                    timeout=0.1, check=False
                )
                # Remove stacking hints from mpv
                self.window_mgr.remove_window_state(mpv_id, "_NET_WM_STATE_ABOVE")
                self.window_mgr.add_window_state(mpv_id, "_NET_WM_STATE_BELOW")
                self._log.debug(f"Hid mpv window {mpv_id} first")
            except Exception as hide_exc:
                self._log.debug(f"Failed to hide mpv window: {hide_exc}")
        
        # Step 2: Hide pygame window completely BEFORE any operations
        # Use batched operations for better performance
        try:
            # Batch: unmap + move off-screen in single xdotool call
            self.window_mgr._run_xdotool_batch([
                ["windowunmap", pygame_id],
                ["windowmove", pygame_id, "-10000", "-10000"]
            ], timeout=0.3)
            # Set HIDDEN state (xprop can't be batched with xdotool)
            import subprocess
            subprocess.run(
                ["xprop", "-id", pygame_id, "-f", "_NET_WM_STATE", "32a", "-set", "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN"],
                timeout=0.3, check=False
            )
        except Exception as hide_exc:
            self._log.debug(f"Failed to hide pygame window: {hide_exc}")
        
        # Step 3: Prepare pygame window WITHOUT resizing (to prevent visible resize animation)
        # Instead, check current size and only resize if absolutely necessary
        # Most importantly: set all properties BEFORE making window visible
        try:
            import subprocess
            import os
            env = os.environ.copy()
            env["DISPLAY"] = ":0"
            
            # CRITICAL: Set window properties FIRST (before any operations that might make it visible)
            # Set window type to SPLASH first (prevents decorations from being added)
            subprocess.run(
                ["xprop", "-id", pygame_id, "-f", "_NET_WM_WINDOW_TYPE", "32a", "-set", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_SPLASH"],
                timeout=0.2, check=False, env=env
            )
            
            # Remove decorations MULTIPLE times while window is still unmapped
            # Do this very aggressively - Openbox can be stubborn
            for _ in range(10):
                self.window_mgr.remove_window_decorations(pygame_id)
                time.sleep(0.01)  # Small delay between attempts
            
            # Check current window size - only resize if it's wrong
            # This prevents unnecessary resize operations that cause visible animations
            try:
                import subprocess as sp
                result = sp.run(
                    ["xdotool", "getwindowgeometry", pygame_id],
                    capture_output=True, timeout=0.2, check=False, env=env
                )
                current_size_ok = False
                if result.returncode == 0:
                    output = result.stdout.decode()
                    # Parse geometry output to check size
                    for line in output.split('\n'):
                        if 'Geometry:' in line:
                            # Extract size (e.g., "Geometry: 1920x1080")
                            parts = line.split()
                            if len(parts) >= 2:
                                size_part = parts[1]
                                if 'x' in size_part:
                                    w, h = map(int, size_part.split('x'))
                                    if w == self.screen_width and h == self.screen_height:
                                        current_size_ok = True
                                        self._log.debug(f"Window already correct size: {w}x{h}")
                                        break
                
                # Only resize if size is wrong
                if not current_size_ok:
                    self._log.debug(f"Resizing window to {self.screen_width}x{self.screen_height}")
                    # Batch: resize + move in single xdotool call (while unmapped)
                    self.window_mgr._run_xdotool_batch([
                        ["windowsize", pygame_id, str(self.screen_width), str(self.screen_height)],
                        ["windowmove", pygame_id, "0", "0"]
                    ], timeout=0.3)
                    # Remove decorations after resize
                    for _ in range(3):
                        self.window_mgr.remove_window_decorations(pygame_id)
                else:
                    # Just ensure position is correct
                    self.window_mgr._run_xdotool(["windowmove", pygame_id, "0", "0"], timeout=0.2)
            except Exception as size_check_exc:
                # If we can't check size, resize anyway (safe fallback)
                self._log.debug(f"Could not check window size, resizing anyway: {size_check_exc}")
                self.window_mgr._run_xdotool_batch([
                    ["windowsize", pygame_id, str(self.screen_width), str(self.screen_height)],
                    ["windowmove", pygame_id, "0", "0"]
                ], timeout=0.3)
                for _ in range(3):
                    self.window_mgr.remove_window_decorations(pygame_id)
            
            # Set maximized state hints to prevent window manager from changing size
            subprocess.run(
                ["xprop", "-id", pygame_id, "-f", "_NET_WM_STATE", "32a", "-set", "_NET_WM_STATE", "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ"],
                timeout=0.2, check=False, env=env
            )
            
            self._log.debug(f"Prepared pygame window {pygame_id} while unmapped")
        except Exception as prep_exc:
            self._log.debug(f"Window prep failed: {prep_exc}")
        
        # Longer delay to ensure all operations complete and window manager processes everything
        time.sleep(0.15)
        
        # Step 4: Show pygame window instantly (it's already the right size/position, no decorations)
        try:
            import subprocess
            import os
            env = os.environ.copy()
            env["DISPLAY"] = ":0"
            # Hide cursor again before showing window
            self.window_mgr.hide_cursor_aggressive()
            # CRITICAL: Remove decorations ONE MORE TIME before removing HIDDEN state
            # This ensures decorations are gone before window becomes visible
            for _ in range(3):
                self.window_mgr.remove_window_decorations(pygame_id)
            # Remove HIDDEN state
            subprocess.run(
                ["xprop", "-id", pygame_id, "-f", "_NET_WM_STATE", "32a", "-remove", "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN"],
                timeout=0.2, check=False, env=env
            )
            # Small delay to ensure state removal completes
            time.sleep(0.05)
            # Set stacking hint BEFORE mapping (prevents visible movement)
            self.window_mgr.add_window_state(pygame_id, "_NET_WM_STATE_ABOVE")
            # Remove decorations AGAIN right before mapping (last chance)
            # Do this multiple times with slight delays to ensure it sticks
            for _ in range(3):
                self.window_mgr.remove_window_decorations(pygame_id)
                time.sleep(0.01)
            
            # CRITICAL: Use a black overlay technique to mask any brief flash
            # Create a temporary black window that covers the screen, then remove it after pygame is shown
            try:
                # Create a black fullscreen window using xdotool/xwininfo to mask transition
                black_overlay_cmd = [
                    "xdotool", "search", "--name", "black_overlay_temp"
                ]
                # Try to find existing overlay, create if needed
                overlay_result = subprocess.run(black_overlay_cmd, capture_output=True, timeout=0.1, check=False, env=env)
                # For now, skip overlay - focus on making transition instant
            except Exception:
                pass
            
            # CRITICAL: Set maximized state BEFORE mapping (prevents WM from resizing/adding decorations)
            subprocess.run(
                ["xprop", "-id", pygame_id, "-f", "_NET_WM_STATE", "32a", "-set", "_NET_WM_STATE", "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ"],
                timeout=0.2, check=False, env=env
            )
            # Remove decorations one more time before mapping
            self.window_mgr.remove_window_decorations(pygame_id)
            
            # Map window (make visible) - do this atomically
            self.window_mgr.map_window(pygame_id)
            self.window_mgr.remove_window_state(pygame_id, "_NET_WM_STATE_ICONIC")
            
            # IMMEDIATELY remove decorations after mapping (before WM can add them)
            # Use a very tight loop with no delays for maximum speed
            for _ in range(10):  # Even more attempts
                self.window_mgr.remove_window_decorations(pygame_id)
            
            # Minimal delay - just enough for WM to process mapping
            time.sleep(0.01)
            
            # Raise window
            self.window_mgr.raise_window(pygame_id)
            
            # Remove decorations again after raising (WMs may reapply them)
            # Do this very aggressively
            for _ in range(5):
                self.window_mgr.remove_window_decorations(pygame_id)
                time.sleep(0.005)  # Very fast removal
            
            # Hide cursor one more time after window is shown
            self.window_mgr.hide_cursor_aggressive()
            
            # Activate for immediate focus
            subprocess.run(
                ["xdotool", "windowactivate", pygame_id],
                timeout=0.3, check=False, env=env
            )
            
            # Final decoration removal after activation (some WMs add them on focus)
            for _ in range(3):
                self.window_mgr.remove_window_decorations(pygame_id)
                time.sleep(0.005)
            self._log.debug(f"Showed pygame window {pygame_id} instantly (borderless)")
        except Exception as show_exc:
            self._log.warning(f"Failed to show pygame window: {show_exc}")
            return False
        
        # Validate that pygame window is actually visible
        time.sleep(0.1)  # Give window manager time to process
        final_pygame_id = self._get_pygame_id(force_refresh=True)
        if not final_pygame_id or final_pygame_id != pygame_id:
            self._log.warning("Transition validation failed: pygame window not found after show")
            return False
        
        self._log.info("Transition complete: mpv -> UI")
        return True
    
    def transition_to_video(self) -> bool:
        """Seamlessly transition from pygame UI to mpv video.
        
        Order of operations:
        1. Hide cursor (X11 level)
        2. Wait for mpv window to exist (with retry)
        3. Hide mpv window completely (move off-screen + unmap + HIDDEN state)
        4. Prepare mpv window (restore position, size) while hidden/unmapped
        5. Hide pygame window (move off-screen + unmap)
        6. Small delay to ensure operations complete
        7. Show mpv window instantly (already correct size/position)
        
        Returns:
            True if transition was successful
        """
        self._log.debug("Starting transition: UI -> mpv")
        
        pygame_id = self._get_pygame_id(force_refresh=True)
        # Retry finding mpv window (up to 1 second) since it might be creating
        mpv_id = self._get_mpv_id(force_refresh=True, max_attempts=5, retry_delay=0.2)
        
        if not mpv_id:
            self._log.warning("transition_to_video: Could not find mpv window after retries")
            return False
        
        # Step 0: Hide cursor aggressively (X11 level) - do this FIRST
        self.window_mgr.hide_cursor_aggressive()
        
        # Step 0.5: Ensure mpv window has no decorations (do this before hiding)
        # Some window managers add decorations when window is mapped, so remove them immediately
        self.window_mgr.remove_window_decorations(mpv_id)
        
        # Step 1: Hide mpv window completely BEFORE any operations
        # Use batched operations for better performance
        try:
            # Batch: unmap + move off-screen in single xdotool call
            self.window_mgr._run_xdotool_batch([
                ["windowunmap", mpv_id],
                ["windowmove", mpv_id, "-10000", "-10000"]
            ], timeout=0.2)
            # Set HIDDEN state (xprop can't be batched with xdotool)
            import subprocess
            subprocess.run(
                ["xprop", "-id", mpv_id, "-f", "_NET_WM_STATE", "32a", "-set", "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN"],
                timeout=0.1, check=False
            )
        except Exception as hide_exc:
            self._log.debug(f"Failed to hide mpv window: {hide_exc}")
        
        # Step 2: Prepare mpv window (restore position, size) while completely hidden
        # Use batched operations for better performance
        try:
            # Batch: resize + move in single xdotool call
            self.window_mgr._run_xdotool_batch([
                ["windowsize", mpv_id, str(self.screen_width), str(self.screen_height)],
                ["windowmove", mpv_id, "0", "0"]
            ], timeout=0.5)
            # Remove decorations while window is still unmapped (user won't see this)
            self.window_mgr.remove_window_decorations(mpv_id)
            # Remove BELOW state, prepare for ABOVE
            self.window_mgr.remove_window_state(mpv_id, "_NET_WM_STATE_BELOW")
            self._log.debug(f"Prepared mpv window {mpv_id} position/size while hidden")
        except Exception as prep_exc:
            self._log.debug(f"Window prep failed: {prep_exc}")
        
        # Step 3: Hide pygame window (move off-screen to prevent any visual artifacts)
        # Use batched operations for better performance
        if pygame_id:
            try:
                # Remove stacking hints from pygame first
                self.window_mgr.remove_window_state(pygame_id, "_NET_WM_STATE_ABOVE")
                # Batch: unmap + move off-screen in single xdotool call
                self.window_mgr._run_xdotool_batch([
                    ["windowunmap", pygame_id],
                    ["windowmove", pygame_id, "-10000", "-10000"]
                ], timeout=0.2)
                self._log.debug(f"Hid pygame window {pygame_id} (off-screen)")
            except Exception as hide_exc:
                self._log.debug(f"Failed to hide pygame window: {hide_exc}")
        
        # Small delay to ensure all hide operations complete before showing mpv
        time.sleep(0.1)
        
        # Step 4: Show mpv window instantly (it's already the right size/position, no decorations)
        try:
            import subprocess
            import os
            env = os.environ.copy()
            env["DISPLAY"] = ":0"
            # Hide cursor again before showing window
            self.window_mgr.hide_cursor_aggressive()
            # Remove HIDDEN state
            subprocess.run(
                ["xprop", "-id", mpv_id, "-f", "_NET_WM_STATE", "32a", "-remove", "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN"],
                timeout=0.2, check=False
            )
            # Small delay to ensure state removal completes
            time.sleep(0.05)
            # Set stacking hint BEFORE mapping (prevents visible movement)
            self.window_mgr.add_window_state(mpv_id, "_NET_WM_STATE_ABOVE")
            # Map window (make visible) - do this atomically
            self.window_mgr.map_window(mpv_id)
            self.window_mgr.remove_window_state(mpv_id, "_NET_WM_STATE_ICONIC")
            # Ensure decorations are still removed after mapping (some WMs reapply them)
            # Remove decorations multiple times to ensure it sticks
            for _ in range(3):
                self.window_mgr.remove_window_decorations(mpv_id)
                time.sleep(0.03)
            # Small delay to ensure mapping completes
            time.sleep(0.05)
            # Raise window
            self.window_mgr.raise_window(mpv_id)
            # Remove decorations again after raising (WMs may reapply them)
            for _ in range(2):
                self.window_mgr.remove_window_decorations(mpv_id)
                time.sleep(0.02)
            # Hide cursor one more time after window is shown
            self.window_mgr.hide_cursor_aggressive()
            # Activate for immediate focus
            subprocess.run(
                ["xdotool", "windowactivate", mpv_id],
                timeout=0.3, check=False, env=env
            )
            self._log.debug(f"Showed mpv window {mpv_id} instantly (borderless)")
        except Exception as show_exc:
            self._log.warning(f"Failed to show mpv window: {show_exc}")
            return False
        
        # Validate that mpv window is actually visible
        time.sleep(0.1)  # Give window manager time to process
        final_mpv_id = self._get_mpv_id(force_refresh=True, max_attempts=1)
        if not final_mpv_id or final_mpv_id != mpv_id:
            self._log.warning("Transition validation failed: mpv window not found after show")
            return False
        
        self._log.info("Transition complete: UI -> mpv")
        return True
    
    def ensure_ui_visible(self) -> bool:
        """Ensure UI is visible and on top (maintenance operation).
        
        Use this periodically to ensure UI stays visible when it should be.
        Does not perform a full transition - just ensures visibility.
        
        Returns:
            True if successful
        """
        pygame_id = self._get_pygame_id()
        if not pygame_id:
            return False
        
        try:
            self.window_mgr.map_window(pygame_id)
            self.window_mgr.remove_window_state(pygame_id, "_NET_WM_STATE_HIDDEN")
            self.window_mgr.remove_window_state(pygame_id, "_NET_WM_STATE_ICONIC")
            self.window_mgr.raise_window(pygame_id)
            return True
        except Exception:
            return False
    
    def invalidate_cache(self) -> None:
        """Invalidate cached window IDs (call when windows might have changed)."""
        self._mpv_id = None
        self._pygame_id = None
        self._last_mpv_lookup = 0.0
        self._last_pygame_lookup = 0.0
        self._log.debug("Invalidated window ID cache")

