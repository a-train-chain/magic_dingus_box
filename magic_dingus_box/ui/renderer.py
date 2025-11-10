from __future__ import annotations

import pygame
from typing import List, Optional

from ..config import AppConfig
from ..library.models import Playlist
from ..player.controller import PlaybackController
from ..player.sample_mode import SampleModeManager
from .theme import Theme


class UIRenderer:
    def __init__(self, screen: pygame.Surface, config: AppConfig) -> None:
        self.screen = screen
        self.config = config
        self.theme = Theme()
        self.bezel_mode = False  # Set externally if bezel is active
        self.safe_margin_x = 16  # Default margin

    def render(self, playlists: List[Playlist], selected_index: int, controller: PlaybackController, show_overlay: bool, ui_alpha: float = 1.0, ui_hidden: bool = False, has_playback: bool = False, sample_mode: Optional[SampleModeManager] = None) -> None:
        t = self.theme
        # Use larger margins in bezel mode to avoid text being cut off by frame
        x_margin = 60 if self.bezel_mode else 16

        # If UI is hidden (video playing), solid black with persistent track info overlay
        if ui_hidden:
            # Solid black background (for now - true transparency requires special window setup)
            self.screen.fill((0, 0, 0))
            
            # MTV-style track info in bottom-right (no background bar)
            if sample_mode and sample_mode.active:
                status = "Sample Mode Engaged"
            else:
                status = controller.status_text()
            elapsed, duration = controller.elapsed_and_duration()
            time_text = _format_time(elapsed) + (f" / {_format_time(duration)}" if duration else "") if elapsed else ""
            
            # Position in bottom-right area
            footer_y = self.config.screen_height - 60
            
            # Render status text (title/artist)
            if status:
                status_surf = t.font_small.render(status, True, t.fg)
                status_x = self.config.screen_width - status_surf.get_width() - 80
                self.screen.blit(status_surf, (status_x, footer_y))
            
            # Render time below status
            if time_text:
                time_surf = t.font_small.render(time_text, True, t.fg)
                time_x = self.config.screen_width - time_surf.get_width() - 80
                self.screen.blit(time_surf, (time_x, footer_y + 18))
            
            # Draw marker indicators if in sample mode
            if sample_mode and sample_mode.active:
                self._draw_marker_indicators(sample_mode, footer_y - 32)
            
            return

        # Draw menu UI to offscreen surface
        menu_surf = pygame.Surface(self.screen.get_size(), pygame.SRCALPHA)
        
        # Initial state (no playback): solid background
        if not has_playback:
            menu_surf.fill(t.bg)
        else:
            # Menu over video: semi-transparent dark background so UI is visible
            # Use 80% opacity black background so UI elements are clearly visible
            menu_surf.fill((0, 0, 0, 204))  # 204/255 = 80% opacity
        
        # Draw all text and UI elements on the offscreen surface (100% opaque)
        # Product title header with underline (centered)
        title_surf = t.font_title.render(self.config.product_title, True, t.accent)
        title_x = (self.config.screen_width - title_surf.get_width()) // 2
        title_y = 8
        menu_surf.blit(title_surf, (title_x, title_y))
        # Centered underline
        underline_y = title_y + title_surf.get_height() + 2
        pygame.draw.line(menu_surf, t.accent2, (title_x, underline_y), (title_x + title_surf.get_width(), underline_y), 2)

        # PLAYLISTS section header with simple underline
        y = 8 + title_surf.get_height() + 24
        playlists_header = "Playlists"
        header_surf = t.font_heading.render(playlists_header, True, t.accent2)
        menu_surf.blit(header_surf, (x_margin, y))
        
        # Simple decorative line under header
        header_line_y = y + header_surf.get_height() + 4
        pygame.draw.line(menu_surf, t.accent2, (x_margin, header_line_y), (x_margin + header_surf.get_width(), header_line_y), 2)
        
        # Playlist list with subtle channel-style numbering
        y = header_line_y + 20
        for idx, pl in enumerate(playlists[:12]):
            # Channel number (01., 02., etc.) - more subtle
            channel_num = f"{idx + 1:02d}."
            num_surf = t.font_small.render(channel_num, True, t.dim)
            menu_surf.blit(num_surf, (x_margin, y))
            
            # Playlist title and curator
            font = t.font_large if idx == selected_index else t.font_medium
            # Action color for selected row; otherwise normal foreground
            color = t.accent2 if idx == selected_index else t.fg
            text = f"{pl.title} — {pl.curator}" if pl.curator else pl.title
            surf = font.render(text, True, color)
            menu_surf.blit(surf, (x_margin + 36, y))
            
            # Blinking indicator to the right of the selected title (pointing toward text)
            if idx == selected_index:
                if (pygame.time.get_ticks() // 500) % 2 == 0:
                    indicator_x = x_margin + 36 + surf.get_width() + 16
                    cy = y + surf.get_height() // 2
                    size = 6
                    points = [
                        (indicator_x, cy - size),
                        (indicator_x, cy + size),
                        (indicator_x - int(size * 1.2), cy),
                    ]
                    pygame.draw.polygon(menu_surf, t.accent2, points)
                
                # Very subtle highlight bar for selected item (only when no video playing)
                if not has_playback:
                    highlight_rect = pygame.Rect(
                        x_margin + 32,
                        y - 2,
                        self.config.screen_width - x_margin * 2 - 32,
                        surf.get_height() + 4,
                    )
                    highlight_surf = pygame.Surface((highlight_rect.width, highlight_rect.height), pygame.SRCALPHA)
                    # Subtle action-color tint
                    highlight_surf.fill((t.accent2[0], t.accent2[1], t.accent2[2], 40))
                    menu_surf.blit(highlight_surf, (highlight_rect.x, highlight_rect.y))
            
            y += 36
        
        # Composite entire menu surface with fade
        # Apply alpha fade for smooth transitions when selecting/returning from playlists
        self.screen.fill((0, 0, 0))  # Black base
        menu_surf.set_alpha(int(max(0.0, min(1.0, ui_alpha)) * 255))
        self.screen.blit(menu_surf, (0, 0))

        # Track info overlay: MTV-style bottom-right position
        if sample_mode and sample_mode.active:
            status = "Sample Mode Engaged"
        else:
            status = controller.status_text()
        elapsed, duration = controller.elapsed_and_duration()
        time_text = _format_time(elapsed) + (f" / {_format_time(duration)}" if duration else "") if elapsed else ""
        
        # Position in bottom-right area, MTV-style (visible but not obtrusive)
        footer_y = self.config.screen_height - 60
        
        # Render status text (title/artist)
        if status:
            status_surf = t.font_small.render(status, True, t.fg)
            status_x = self.config.screen_width - status_surf.get_width() - 80
            self.screen.blit(status_surf, (status_x, footer_y))
        
        # Render time below status
        if time_text:
            time_surf = t.font_small.render(time_text, True, t.fg)
            time_x = self.config.screen_width - time_surf.get_width() - 80
            self.screen.blit(time_surf, (time_x, footer_y + 18))
        
        # Draw marker indicators if in sample mode
        if sample_mode and sample_mode.active:
            self._draw_marker_indicators(sample_mode, footer_y - 32)

        if self.config.enable_scanlines:
            t.draw_scanlines(self.screen)
        # Vignette disabled - creates visible rectangle in center
        # t.draw_vignette(self.screen)

    def _draw_marker_indicators(self, sample_mode: SampleModeManager, y_pos: int) -> None:
        """Draw marker indicator circles showing which markers are set.
        
        Args:
            sample_mode: The sample mode manager with marker state
            y_pos: Y position to draw indicators
        """
        t = self.theme
        circle_radius = 8
        circle_spacing = 24
        start_x = 16
        
        marker_states = sample_mode.get_marker_states()
        
        for i, is_set in enumerate(marker_states):
            cx = start_x + (i * circle_spacing) + circle_radius
            cy = y_pos + circle_radius
            
            if is_set:
                # Filled circle for set marker uses action color
                pygame.draw.circle(self.screen, t.accent2, (cx, cy), circle_radius)
            else:
                # Open circle for unset marker
                pygame.draw.circle(self.screen, t.accent2, (cx, cy), circle_radius, 2)


def _format_time(seconds):
    if seconds is None:
        return "--:--"
    total = int(seconds)
    m, s = divmod(total, 60)
    return f"{m:02d}:{s:02d}"

