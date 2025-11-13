from __future__ import annotations

import logging
import os
import subprocess
import time
from typing import Optional
import sys


class WindowManager:
    """Helper class for managing X11 windows with debouncing to reduce overhead.
    
    Caches last operation time per action type and skips redundant calls within
    a debounce window (200ms). No-ops on non-Linux platforms.
    """
    
    def __init__(self, debounce_ms: float = 200.0, pygame_window_id: Optional[str] = None) -> None:
        """Initialize WindowManager.
        
        Args:
            debounce_ms: Minimum milliseconds between same operation type
            pygame_window_id: X11 window ID of pygame window (if known, avoids searching)
        """
        self._log = logging.getLogger("window_manager")
        self.debounce_ms = debounce_ms / 1000.0  # Convert to seconds
        self._last_ops: dict[str, float] = {}  # operation_type -> timestamp
        # Treat Linux as supported regardless of current DISPLAY env; we inject DISPLAY=:0 for calls
        self.platform = "linux" if sys.platform.startswith("linux") else "other"
        self.pygame_window_id = pygame_window_id
        
        if self.platform != "linux":
            self._log.debug("WindowManager: Not on Linux/X11, window operations will be no-ops")
        elif pygame_window_id:
            self._log.debug(f"WindowManager: Using pygame window ID: {pygame_window_id}")
    
    def _should_skip(self, op_type: str) -> bool:
        """Check if operation should be skipped due to debouncing."""
        if self.platform != "linux":
            return True  # Skip on non-Linux
        
        now = time.time()
        last_time = self._last_ops.get(op_type, 0)
        if (now - last_time) < self.debounce_ms:
            return True
        
        self._last_ops[op_type] = now
        return False
    
    def _run_xdotool(self, args: list[str], timeout: float = 1.0) -> Optional[str]:
        """Run xdotool command and return stdout if successful."""
        if self.platform != "linux":
            return None
        
        try:
            env = os.environ.copy()
            env["DISPLAY"] = ":0"
            result = subprocess.run(
                ["xdotool"] + args,
                capture_output=True,
                timeout=timeout,
                check=False,
                env=env
            )
            if result.returncode == 0 and result.stdout.strip():
                return result.stdout.decode().strip()
        except Exception as exc:
            self._log.debug(f"xdotool command failed: {exc}")
        return None
    
    def _run_xdotool_batch(self, commands: list[list[str]], timeout: float = 2.0) -> bool:
        """Run multiple xdotool commands chained together for better performance.
        
        Args:
            commands: List of command argument lists (e.g., [["windowunmap", "123"], ["windowmove", "123", "0", "0"]])
            timeout: Total timeout for all commands
            
        Returns:
            True if all commands succeeded
        """
        if self.platform != "linux" or not commands:
            return False
        
        try:
            env = os.environ.copy()
            env["DISPLAY"] = ":0"
            # Chain commands: xdotool cmd1 cmd2 cmd3 ...
            # xdotool supports chaining multiple commands in a single invocation
            args = []
            for cmd in commands:
                args.extend(cmd)
            
            result = subprocess.run(
                ["xdotool"] + args,
                capture_output=True,
                timeout=timeout,
                check=False,
                env=env
            )
            return result.returncode == 0
        except Exception as exc:
            self._log.debug(f"xdotool batch command failed: {exc}")
        return False
    
    def _run_xprop(self, args: list[str], timeout: float = 1.0) -> bool:
        """Run xprop command and return True if successful."""
        if self.platform != "linux":
            return False
        
        try:
            result = subprocess.run(
                ["xprop"] + args,
                capture_output=True,
                timeout=timeout,
                check=False
            )
            return result.returncode == 0
        except Exception as exc:
            self._log.debug(f"xprop command failed: {exc}")
        return False
    
    def find_window_by_class(self, class_name: str) -> Optional[str]:
        """Find window ID by class name.
        
        Args:
            class_name: X11 window class (e.g., "mpv")
            
        Returns:
            Window ID string or None if not found
        """
        if self._should_skip(f"find_class_{class_name}"):
            return None
        
        return self._run_xdotool(["search", "--class", class_name], timeout=2.0)
    
    def find_window_by_name(self, name: str) -> Optional[str]:
        """Find window ID by window name.
        
        Args:
            name: X11 window name (e.g., "Magic Dingus Box")
            
        Returns:
            Window ID string or None if not found
        """
        if self._should_skip(f"find_name_{name}"):
            return None
        
        return self._run_xdotool(["search", "--name", name], timeout=2.0)
    
    def raise_window(self, window_id: str) -> bool:
        """Raise window to front.
        
        Args:
            window_id: X11 window ID
            
        Returns:
            True if successful
        """
        if self._should_skip(f"raise_{window_id}"):
            return False
        
        result = self._run_xdotool(["windowraise", window_id], timeout=1.0)
        return result is not None
    
    def map_window(self, window_id: str) -> bool:
        """Map (show) window.
        
        Args:
            window_id: X11 window ID
            
        Returns:
            True if successful
        """
        if self._should_skip(f"map_{window_id}"):
            return False
        
        result = self._run_xdotool(["windowmap", window_id], timeout=1.0)
        return result is not None
    
    def minimize_window(self, window_id: str) -> bool:
        """Minimize window.
        
        Args:
            window_id: X11 window ID
            
        Returns:
            True if successful
        """
        if self._should_skip(f"minimize_{window_id}"):
            return False
        
        result = self._run_xdotool(["windowminimize", window_id], timeout=1.0)
        return result is not None
    
    def move_window_offscreen(self, window_id: str) -> bool:
        """Move window off-screen (seamless, no animation).
        
        Moves window to negative coordinates so it's invisible but still rendering.
        This is better than minimizing for seamless transitions.
        
        Args:
            window_id: X11 window ID
            
        Returns:
            True if successful
        """
        if self._should_skip(f"move_offscreen_{window_id}"):
            return False
        
        # Move window to -2000,-2000 (off-screen, but still exists)
        result = self._run_xdotool(["windowmove", window_id, "-2000", "-2000"], timeout=1.0)
        return result is not None
    
    def restore_window_position(self, window_id: str, x: int = 0, y: int = 0) -> bool:
        """Restore window to on-screen position.
        
        Args:
            window_id: X11 window ID
            x: X position (default 0)
            y: Y position (default 0)
            
        Returns:
            True if successful
        """
        if self._should_skip(f"restore_pos_{window_id}"):
            return False
        
        result = self._run_xdotool(["windowmove", window_id, str(x), str(y)], timeout=1.0)
        return result is not None
    
    def remove_window_state(self, window_id: str, state: str) -> bool:
        """Remove a window state property (e.g., HIDDEN, ICONIC).
        
        Args:
            window_id: X11 window ID
            state: State name (e.g., "_NET_WM_STATE_HIDDEN")
            
        Returns:
            True if successful
        """
        if self._should_skip(f"remove_state_{window_id}_{state}"):
            return False
        
        return self._run_xprop([
            "-display", ":0",
            "-id", window_id,
            "-f", "_NET_WM_STATE", "32a",
            "-remove", "_NET_WM_STATE", state
        ], timeout=1.0)
    
    def add_window_state(self, window_id: str, state: str) -> bool:
        """Add a window state property (e.g., ABOVE, BELOW).
        
        Args:
            window_id: X11 window ID
            state: State name (e.g., "_NET_WM_STATE_ABOVE")
            
        Returns:
            True if successful
        """
        if self._should_skip(f"add_state_{window_id}_{state}"):
            return False
        
        return self._run_xprop([
            "-display", ":0",
            "-id", window_id,
            "-f", "_NET_WM_STATE", "32a",
            "-add", "_NET_WM_STATE", state
        ], timeout=1.0)
    
    def remove_window_decorations(self, window_id: str) -> bool:
        """Remove window decorations (borders, title bar) using _MOTIF_WM_HINTS.
        
        This sets the window to be borderless/undecorated for seamless transitions.
        Uses multiple methods to ensure decorations are removed.
        
        Args:
            window_id: X11 window ID
            
        Returns:
            True if successful
        """
        if self.platform != "linux":
            return False
        
        try:
            env = os.environ.copy()
            env["DISPLAY"] = ":0"
            
            # Method 1: _MOTIF_WM_HINTS format: [flags, functions, decorations, input_mode, status]
            # flags=2 means decorations hint is set
            # decorations = 0 means no decorations
            # This is the standard way to remove decorations
            result1 = subprocess.run(
                ["xprop", "-id", window_id, "-f", "_MOTIF_WM_HINTS", "32c", "-set", "_MOTIF_WM_HINTS", "2", "0", "0", "0", "0"],
                capture_output=True,
                timeout=1.0,
                check=False,
                env=env
            )
            
            # Method 2: Also try setting it with all zeros (some WMs prefer this)
            result2 = subprocess.run(
                ["xprop", "-id", window_id, "-f", "_MOTIF_WM_HINTS", "32c", "-set", "_MOTIF_WM_HINTS", "0", "0", "0", "0", "0"],
                capture_output=True,
                timeout=1.0,
                check=False,
                env=env
            )
            
            # Method 3: Set window type to SPLASH (some WMs don't decorate splash windows)
            subprocess.run(
                ["xprop", "-id", window_id, "-f", "_NET_WM_WINDOW_TYPE", "32a", "-set", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_SPLASH"],
                capture_output=True,
                timeout=1.0,
                check=False,
                env=env
            )
            
            # Method 4: Also try setting to DOCK type (undecorated by default)
            subprocess.run(
                ["xprop", "-id", window_id, "-f", "_NET_WM_WINDOW_TYPE", "32a", "-set", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK"],
                capture_output=True,
                timeout=1.0,
                check=False,
                env=env
            )
            
            # Method 5: Set back to NORMAL but with no decorations hint
            subprocess.run(
                ["xprop", "-id", window_id, "-f", "_NET_WM_WINDOW_TYPE", "32a", "-set", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_NORMAL"],
                capture_output=True,
                timeout=1.0,
                check=False,
                env=env
            )
            
            if result1.returncode == 0 or result2.returncode == 0:
                self._log.debug(f"Removed decorations from window {window_id}")
                return True
        except Exception as exc:
            self._log.debug(f"Failed to remove decorations: {exc}")
        return False
    
    def set_window_undecorated_persistent(self, window_id: str) -> bool:
        """Persistently remove window decorations with multiple attempts.
        
        This method aggressively removes decorations and sets window properties
        to prevent the window manager from reapplying them. Should be called
        periodically to maintain undecorated state.
        
        Args:
            window_id: X11 window ID
            
        Returns:
            True if at least one operation succeeded
        """
        if self.platform != "linux":
            return False
        
        success = False
        try:
            env = os.environ.copy()
            env["DISPLAY"] = ":0"
            
            # Remove decorations multiple times to ensure it sticks
            for attempt in range(3):
                if self.remove_window_decorations(window_id):
                    success = True
            
            # Also set window type to prevent decorations
            subprocess.run(
                ["xprop", "-id", window_id, "-f", "_NET_WM_WINDOW_TYPE", "32a", "-set", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_SPLASH"],
                capture_output=True,
                timeout=0.5,
                check=False,
                env=env
            )
            
            # Set maximized hints to prevent resizing/decorations
            subprocess.run(
                ["xprop", "-id", window_id, "-f", "_NET_WM_STATE", "32a", "-set", "_NET_WM_STATE", "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ"],
                capture_output=True,
                timeout=0.5,
                check=False,
                env=env
            )
            
        except Exception as exc:
            self._log.debug(f"Failed to set persistent undecorated state: {exc}")
        
        return success
    
    def hide_cursor_aggressive(self) -> bool:
        """Aggressively hide cursor using multiple methods.
        
        Returns:
            True if at least one method succeeded
        """
        if self.platform != "linux":
            return False
        
        success = False
        try:
            env = os.environ.copy()
            env["DISPLAY"] = ":0"
            
            # Method 1: xsetroot (sets root cursor to blank)
            result = subprocess.run(
                ["xsetroot", "-cursor_name", "none"],
                timeout=0.5, check=False, env=env
            )
            if result.returncode == 0:
                success = True
            
            # Method 2: Move cursor way off-screen
            subprocess.run(
                ["xdotool", "mousemove", "10000", "10000"],
                timeout=0.5, check=False, env=env
            )
            
            # Method 3: Use xdotool to hide cursor on all windows
            subprocess.run(
                ["xdotool", "search", "--class", ".*", "mousemove", "--window", "%1", "10000", "10000"],
                timeout=0.5, check=False, env=env
            )
            
        except Exception:
            pass
        
        return success
    
    def ensure_mpv_visible(self, max_attempts: int = 3) -> bool:
        """Ensure mpv window is visible and raised (common operation).
        
        This batches multiple operations together for efficiency.
        
        Args:
            max_attempts: Maximum number of attempts to find window
            
        Returns:
            True if window was found and made visible
        """
        if self.platform != "linux":
            return False
        
        # Find mpv window
        window_id = None
        for attempt in range(max_attempts):
            window_id = self.find_window_by_class("mpv")
            if window_id:
                break
            time.sleep(0.2)
        
        if not window_id:
            return False
        
        # Batch operations: map, remove states, raise
        self.map_window(window_id)
        self.remove_window_state(window_id, "_NET_WM_STATE_HIDDEN")
        self.remove_window_state(window_id, "_NET_WM_STATE_ICONIC")
        self.raise_window(window_id)
        
        return True
    
    def ensure_pygame_fullscreen(self, width: int, height: int) -> bool:
        """Ensure pygame window is fullscreen-sized and positioned before showing.
        
        Sets window size and position to (0,0) with fullscreen dimensions,
        then makes it visible. This prevents visible resizing when showing the UI.
        
        Args:
            width: Screen width
            height: Screen height
            
        Returns:
            True if successful
        """
        if self.platform != "linux":
            return False
        
        window_id = self.pygame_window_id or self.find_window_by_name("Magic Dingus Box")
        if not window_id:
            return False
        
        # Set window size and position FIRST (while hidden) to prevent visible transition
        self._run_xdotool(["windowsize", window_id, str(width), str(height)], timeout=1.0)
        self._run_xdotool(["windowmove", window_id, "0", "0"], timeout=1.0)
        
        # Now make it visible - it's already the right size
        self.map_window(window_id)
        self.remove_window_state(window_id, "_NET_WM_STATE_HIDDEN")
        self.remove_window_state(window_id, "_NET_WM_STATE_ICONIC")
        self.raise_window(window_id)
        
        return True
    
    def ensure_pygame_visible(self) -> bool:
        """Ensure pygame window is visible and raised.
        
        Returns:
            True if successful
        """
        if self.platform != "linux":
            return False
        
        window_id = self.pygame_window_id or self.find_window_by_name("Magic Dingus Box")
        if not window_id:
            return False
        
        # Batch operations: unhide, map, and raise
        self.remove_window_state(window_id, "_NET_WM_STATE_HIDDEN")
        self.remove_window_state(window_id, "_NET_WM_STATE_ICONIC")
        self.map_window(window_id)
        # Try to activate focus to unminimize in some WMs
        self._run_xdotool(["windowactivate", window_id], timeout=1.0)
        self.raise_window(window_id)
        return True
    
    def ensure_pygame_above(self) -> bool:
        """Ensure pygame window is visible and above other windows."""
        if self.platform != "linux":
            return False
        
        window_id = self.pygame_window_id or self.find_window_by_name("Magic Dingus Box")
        if not window_id:
            return False
        
        # Map, set ABOVE, and raise
        self.map_window(window_id)
        self.add_window_state(window_id, "_NET_WM_STATE_ABOVE")
        self.raise_window(window_id)
        return True
    
    def ensure_mpv_above(self, max_attempts: int = 3) -> bool:
        """Ensure mpv window is above pygame (for returning to video)."""
        if self.platform != "linux":
            return False
        
        window_id = None
        for attempt in range(max_attempts):
            window_id = self.find_window_by_class("mpv")
            if window_id:
                break
            time.sleep(0.2)
        if not window_id:
            return False
        
        # Remove ABOVE from pygame if present, then raise mpv
        pygame_id = self.pygame_window_id or self.find_window_by_name("Magic Dingus Box")
        if pygame_id:
            self.remove_window_state(pygame_id, "_NET_WM_STATE_ABOVE")
        # Remove BELOW from mpv if present
        self.remove_window_state(window_id, "_NET_WM_STATE_BELOW")
        self.map_window(window_id)
        self.raise_window(window_id)
        return True
    
    def ensure_ui_over_video(self, max_attempts: int = 3) -> bool:
        """Force pygame above mpv by setting stacking hints on both."""
        if self.platform != "linux":
            return False
        
        mpv_id = None
        for attempt in range(max_attempts):
            mpv_id = self.find_window_by_class("mpv")
            if mpv_id:
                break
            time.sleep(0.2)
        # Use stored window ID if available, otherwise search by name
        pygame_id = self.pygame_window_id or self.find_window_by_name("Magic Dingus Box")
        if not pygame_id:
            self._log.warning("ensure_ui_over_video: Could not find pygame window (ID={}, searched by name='Magic Dingus Box')".format(self.pygame_window_id or "not set"))
            return False
        self._log.debug(f"ensure_ui_over_video: Using pygame_id={pygame_id}, mpv_id={mpv_id}")
        
        # Aggressively ensure pygame is visible and on top
        # Remove all hidden/minimized states
        self.remove_window_state(pygame_id, "_NET_WM_STATE_HIDDEN")
        self.remove_window_state(pygame_id, "_NET_WM_STATE_ICONIC")
        # Map window (make it visible)
        self.map_window(pygame_id)
        # Activate window (brings to front in some WMs)
        self._run_xdotool(["windowactivate", pygame_id], timeout=1.0)
        # Set stacking hints: pygame ABOVE, mpv BELOW
        self.add_window_state(pygame_id, "_NET_WM_STATE_ABOVE")
        if mpv_id:
            self.remove_window_state(mpv_id, "_NET_WM_STATE_ABOVE")
            self.add_window_state(mpv_id, "_NET_WM_STATE_BELOW")
        # Multiple raise attempts to ensure it sticks
        self.raise_window(pygame_id)
        time.sleep(0.05)  # Brief delay for WM to process
        self.raise_window(pygame_id)  # Raise again
        # Also use windowfocus as a backup
        self._run_xdotool(["windowfocus", pygame_id], timeout=1.0)
        self._log.info(f"ensure_ui_over_video: Aggressively raised pygame window {pygame_id} above mpv {mpv_id}")
        return True
    
    def layout_mpv_background(self, width: int, height: int, max_attempts: int = 3) -> bool:
        """Size and position mpv to cover the screen without using fullscreen.
        
        Places the mpv window at (0,0) with given size and sets it BELOW.
        """
        if self.platform != "linux":
            return False
        
        mpv_id = None
        for attempt in range(max_attempts):
            mpv_id = self.find_window_by_class("mpv")
            if mpv_id:
                break
            time.sleep(0.2)
        if not mpv_id:
            return False
        
        # Ensure window is visible and not hidden/iconic
        self.remove_window_state(mpv_id, "_NET_WM_STATE_HIDDEN")
        self.remove_window_state(mpv_id, "_NET_WM_STATE_ICONIC")
        self.map_window(mpv_id)
        # Remove ABOVE if present, add BELOW to keep it behind UI
        self.remove_window_state(mpv_id, "_NET_WM_STATE_ABOVE")
        self.add_window_state(mpv_id, "_NET_WM_STATE_BELOW")
        # Move and resize to fill screen
        self._run_xdotool(["windowmove", mpv_id, "0", "0"], timeout=1.0)
        self._run_xdotool(["windowsize", mpv_id, str(width), str(height)], timeout=1.0)
        return True

