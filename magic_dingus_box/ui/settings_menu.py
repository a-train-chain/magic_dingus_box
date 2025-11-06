"""Settings menu with slide-in animation."""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto
from typing import List, Optional, Callable
import time


class MenuSection(Enum):
    """Main menu sections."""
    VIDEO_GAMES = auto()
    DISPLAY = auto()
    AUDIO = auto()
    SYSTEM = auto()
    BACK = auto()
    BROWSE_GAMES = auto()  # Game playlist browser
    TOGGLE_DISPLAY_MODE = auto()  # Cycle display modes
    TOGGLE_BEZEL = auto()  # Toggle CRT bezel on/off
    CHANGE_RESOLUTION = auto()  # Change modern display resolution
    CYCLE_BEZEL_STYLE = auto()  # Cycle through bezel images
    CYCLE_SCANLINES = auto()  # Cycle scanline intensity
    CYCLE_WARMTH = auto()  # Cycle color warmth
    CYCLE_BLOOM = auto()  # Cycle screen bloom intensity
    CYCLE_GLOW = auto()  # Cycle phosphor glow intensity
    CYCLE_PHOSPHOR_MASK = auto()  # Cycle RGB phosphor mask intensity
    CYCLE_INTERLACING = auto()  # Cycle interlacing intensity
    CYCLE_FLICKER = auto()  # Cycle screen flicker intensity


@dataclass
class MenuItem:
    """A single menu item."""
    label: str
    section: Optional[MenuSection] = None
    action: Optional[Callable] = None
    sublabel: Optional[str] = None  # Secondary info line


class SettingsMenuManager:
    """Manages settings menu state and navigation."""
    
    def __init__(self, settings_store=None):
        self.active = False
        self.selected_index = 0
        self.animation_start = 0.0
        self.animation_duration = 0.3  # seconds
        self.is_opening = False
        self.is_closing = False
        
        # Scrolling state
        self.scroll_offset = 0  # Index of first visible item
        
        # Settings store for persistence
        self.settings_store = settings_store
        
        # Main menu items
        self.menu_items: List[MenuItem] = [
            MenuItem("Video Games", MenuSection.VIDEO_GAMES, sublabel="Emulated games"),
            MenuItem("Display", MenuSection.DISPLAY, sublabel="Screen settings"),
            MenuItem("Audio", MenuSection.AUDIO, sublabel="Volume"),
            MenuItem("System", MenuSection.SYSTEM, sublabel="Info"),
            MenuItem("Back", MenuSection.BACK),
        ]
        
        # Current submenu (None = main menu)
        self.current_submenu: Optional[MenuSection] = None
        self.submenu_items: List[MenuItem] = []
        
        # Game browser state
        self.game_browser_active = False
        self.game_browser_selected = 0
        
        # Individual game selection state
        self.viewing_games_in_playlist = False
        self.current_game_playlist_index = 0
        self.selected_game_in_playlist = 0
    
    def toggle(self) -> None:
        """Toggle menu open/closed."""
        if self.active or self.is_opening:
            self.close()
        else:
            self.open()
    
    def open(self) -> None:
        """Start opening animation."""
        if not self.active and not self.is_opening:
            self.active = True
            self.is_opening = True
            self.is_closing = False
            # Start animation in the "past" so first frame is immediately visible
            self.animation_start = time.time() - 0.05
            self.selected_index = 0
            # Reset scroll so titles are visible on reopen
            self.scroll_offset = 0
            self.current_submenu = None
    
    def close(self) -> None:
        """Start closing animation."""
        if self.active and not self.is_closing:
            self.is_closing = True
            self.is_opening = False
            self.animation_start = time.time()
            # Reset scroll so next open starts at top
            self.scroll_offset = 0
    
    def get_animation_progress(self) -> float:
        """Get current animation progress (0.0 to 1.0)."""
        if not (self.is_opening or self.is_closing):
            return 1.0 if self.active else 0.0
        
        # Capture phase at start to avoid one-frame inversion when we flip flags below
        opening = self.is_opening
        closing = self.is_closing
        
        elapsed = time.time() - self.animation_start
        progress = min(1.0, elapsed / self.animation_duration)
        
        # Easing function (ease-out cubic)
        eased = 1 - pow(1 - progress, 3)
        
        # Mark animation completion
        if progress >= 1.0:
            if self.is_closing:
                self.active = False
                self.is_closing = False
            elif self.is_opening:
                self.is_opening = False
        
        # Use the captured phase for consistent output within this frame
        return eased if opening else (1.0 - eased)
    
    def navigate(self, delta: int, game_playlists_count: int = 0, games_in_current_playlist: int = 0) -> None:
        """Navigate menu items, game playlists, or individual games."""
        if not self.active:
            return
        
        # Viewing individual games in a playlist (includes Back option)
        if self.viewing_games_in_playlist:
            total = games_in_current_playlist + 1  # +1 for Back option
            if total > 0:
                self.selected_game_in_playlist = (self.selected_game_in_playlist + delta) % total
            return
        
        # Game browser navigation (includes Back option)
        if self.game_browser_active:
            total = game_playlists_count + 1  # +1 for Back option
            if total > 0:
                self.game_browser_selected = (self.game_browser_selected + delta) % total
            return
        
        # Normal menu navigation
        items = self.submenu_items if self.current_submenu else self.menu_items
        self.selected_index = (self.selected_index + delta) % len(items)
        
        # Adjust scroll offset to keep selected item visible
        # Assumes max 7 items visible at once (configurable in renderer)
        max_visible_items = 7
        if self.selected_index < self.scroll_offset:
            # Scrolling up - selected item went above visible area
            self.scroll_offset = self.selected_index
        elif self.selected_index >= self.scroll_offset + max_visible_items:
            # Scrolling down - selected item went below visible area
            self.scroll_offset = self.selected_index - max_visible_items + 1
    
    def select_current(self) -> Optional[MenuSection]:
        """Select current menu item, returns the section if action should be taken."""
        if not self.active:
            return None
        
        items = self.submenu_items if self.current_submenu else self.menu_items
        if 0 <= self.selected_index < len(items):
            item = items[self.selected_index]
            
            if item.action:
                item.action()
            
            return item.section
        
        return None
    
    def enter_submenu(self, section: MenuSection) -> None:
        """Enter a submenu."""
        self.current_submenu = section
        self.selected_index = 0
        self.scroll_offset = 0  # Reset scroll when entering submenu
        
        # Load submenu items based on section
        if section == MenuSection.VIDEO_GAMES:
            self.submenu_items = self._build_games_submenu()
        elif section == MenuSection.DISPLAY:
            self.submenu_items = self._build_display_submenu()
        elif section == MenuSection.AUDIO:
            self.submenu_items = self._build_audio_submenu()
        elif section == MenuSection.SYSTEM:
            self.submenu_items = self._build_system_submenu()
    
    def rebuild_current_submenu(self) -> None:
        """Rebuild current submenu items while preserving cursor position."""
        if not self.current_submenu:
            return
        
        # Save current position
        old_index = self.selected_index
        
        # Rebuild items
        if self.current_submenu == MenuSection.VIDEO_GAMES:
            self.submenu_items = self._build_games_submenu()
        elif self.current_submenu == MenuSection.DISPLAY:
            self.submenu_items = self._build_display_submenu()
        elif self.current_submenu == MenuSection.AUDIO:
            self.submenu_items = self._build_audio_submenu()
        elif self.current_submenu == MenuSection.SYSTEM:
            self.submenu_items = self._build_system_submenu()
        
        # Restore position (clamp to valid range)
        self.selected_index = min(old_index, len(self.submenu_items) - 1)
    
    def exit_submenu(self) -> None:
        """Return to main menu."""
        self.current_submenu = None
        self.selected_index = 0
        self.scroll_offset = 0  # Reset scroll when exiting submenu
        self.submenu_items = []
    
    def enter_game_browser(self) -> None:
        """Enter game browser mode."""
        self.game_browser_active = True
        self.game_browser_selected = 0
    
    def exit_game_browser(self) -> None:
        """Exit game browser mode back to video games submenu."""
        self.game_browser_active = False
        self.game_browser_selected = 0
        self.viewing_games_in_playlist = False
        self.current_game_playlist_index = 0
        self.selected_game_in_playlist = 0
    
    def enter_game_list(self, playlist_index: int) -> None:
        """Enter individual game list for a specific playlist."""
        self.viewing_games_in_playlist = True
        self.current_game_playlist_index = playlist_index
        self.selected_game_in_playlist = 0
    
    def exit_game_list(self) -> None:
        """Exit individual game list back to system browser."""
        self.viewing_games_in_playlist = False
        self.selected_game_in_playlist = 0
    
    def _build_games_submenu(self) -> List[MenuItem]:
        """Build video games submenu."""
        return [
            MenuItem("Browse Games", MenuSection.BROWSE_GAMES, sublabel="Game libraries"),
            MenuItem("Emulators", sublabel="RetroArch"),
            MenuItem("Controllers", sublabel="Button map"),
            MenuItem("Back", MenuSection.BACK),
        ]
    
    def _intensity_to_label(self, intensity: float) -> str:
        """Convert intensity float to label (OFF/Low/Medium/High)."""
        if intensity <= 0.0:
            return "OFF"
        elif intensity <= 0.35:
            return f"Low ({int(intensity*100)}%)"
        elif intensity <= 0.6:
            return f"Medium ({int(intensity*100)}%)"
        else:
            return f"High ({int(intensity*100)}%)"
    
    def _build_display_submenu(self) -> List[MenuItem]:
        """Build display settings submenu."""
        # Get current settings
        if self.settings_store:
            mode = self.settings_store.get_display_mode()
            resolution = self.settings_store.get_modern_resolution()
            show_bezel = self.settings_store.get_show_bezel()
            bezel_style = self.settings_store.get("bezel_style", "retro_tv_1")
            scanlines_mode = self.settings_store.get("scanlines_mode", "off")
            warmth = self.settings_store.get("color_warmth", 0.0)
            # Get intensity values (float 0.0-1.0) instead of booleans
            bloom = self.settings_store.get("screen_bloom", 0.0)
            glow = self.settings_store.get("phosphor_glow", 0.0)
            phosphor_mask = self.settings_store.get("phosphor_mask", 0.0)
            interlacing = self.settings_store.get("interlacing", 0.0)
            flicker = self.settings_store.get("screen_flicker", 0.0)
        else:
            mode = "crt_native"
            resolution = "auto"
            show_bezel = False
            bezel_style = "retro_tv_1"
            scanlines_mode = "off"
            warmth = 0.0
            bloom = 0.0
            glow = 0.0
            phosphor_mask = 0.0
            interlacing = 0.0
            flicker = 0.0
        
        # Display mode label
        mode_labels = {
            "crt_native": "CRT Native",
            "modern_clean": "Modern (Clean)",
            "modern_bezel": "Modern (Bezel)",
        }
        mode_label = mode_labels.get(mode, "CRT Native")
        
        # Bezel style name
        bezel_names = {
            "procedural": "Simple",
            "nes_tv": "NES TV",
            "n64_tv": "N64 TV",
            "ps1_tv": "PlayStation TV",
            "retro_tv_1": "Retro TV 1",
            "retro_tv_2": "Retro TV 2",
            "tv_retro_1": "Vintage",
            "tv_modern_1": "Modern",
        }
        bezel_name = bezel_names.get(bezel_style, bezel_style)
        
        # Scanlines label
        scanlines_labels = {"off": "OFF", "light": "Light (15%)", "medium": "Medium (30%)", "heavy": "Heavy (50%)"}
        scanlines_label = scanlines_labels.get(scanlines_mode, "OFF")
        
        # Warmth label
        if warmth <= 0:
            warmth_label = "OFF"
        elif warmth < 0.35:
            warmth_label = f"Cool ({int(warmth*100)}%)"
        elif warmth < 0.65:
            warmth_label = f"Neutral ({int(warmth*100)}%)"
        else:
            warmth_label = f"Warm ({int(warmth*100)}%)"
        
        # Build menu items
        items = [
            MenuItem(f"Mode: {mode_label}", MenuSection.TOGGLE_DISPLAY_MODE, sublabel="Cycle modes"),
        ]
        
        # Show resolution and bezel options only for modern modes
        if mode != "crt_native":
            res_display = "Auto" if resolution == "auto" else resolution
            items.append(MenuItem(f"Resolution: {res_display}", MenuSection.CHANGE_RESOLUTION, sublabel="Screen size"))
            items.append(MenuItem(f"Bezel: {'ON' if show_bezel else 'OFF'}", MenuSection.TOGGLE_BEZEL, sublabel="CRT frame"))
            
            # Bezel style selector (only when bezel is ON)
            if show_bezel or mode == "modern_bezel":
                items.append(MenuItem(f"Bezel Style: {bezel_name}", MenuSection.CYCLE_BEZEL_STYLE, sublabel="TV design"))
        
        # CRT effects (available in all modes) - all with intensity control
        items.extend([
            MenuItem(f"Scanlines: {scanlines_label}", MenuSection.CYCLE_SCANLINES, sublabel="CRT lines"),
            MenuItem(f"Color Warmth: {warmth_label}", MenuSection.CYCLE_WARMTH, sublabel="Temperature"),
            MenuItem(f"Phosphor Glow: {self._intensity_to_label(glow)}", MenuSection.CYCLE_GLOW, sublabel="Radial glow"),
            MenuItem(f"RGB Mask: {self._intensity_to_label(phosphor_mask)}", MenuSection.CYCLE_PHOSPHOR_MASK, sublabel="RGB stripes"),
            MenuItem(f"Screen Bloom: {self._intensity_to_label(bloom)}", MenuSection.CYCLE_BLOOM, sublabel="Bright glow"),
            MenuItem(f"Interlacing: {self._intensity_to_label(interlacing)}", MenuSection.CYCLE_INTERLACING, sublabel="Video lines"),
            MenuItem(f"Flicker: {self._intensity_to_label(flicker)}", MenuSection.CYCLE_FLICKER, sublabel="Subtle pulse"),
            MenuItem("Back", MenuSection.BACK),
        ])
        
        return items
    
    def _build_audio_submenu(self) -> List[MenuItem]:
        """Build audio settings submenu."""
        return [
            MenuItem("Menu Vol: 75%", sublabel="Browsing"),
            MenuItem("Video Vol: 100%", sublabel="Playback"),
            MenuItem("Fade: 1.0s", sublabel="Transitions"),
            MenuItem("Back", MenuSection.BACK),
        ]
    
    def _build_system_submenu(self) -> List[MenuItem]:
        """Build system info submenu."""
        return [
            MenuItem("Version: 1.0.0", sublabel="Magic Dingus Box"),
            MenuItem("Platform: Pi", sublabel="Hardware"),
            MenuItem("Uptime: 2h 34m", sublabel="Runtime"),
            MenuItem("Back", MenuSection.BACK),
        ]

