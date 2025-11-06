"""Renderer for slide-in settings menu."""
from __future__ import annotations

import pygame
from typing import TYPE_CHECKING, List

if TYPE_CHECKING:
    from .settings_menu import SettingsMenuManager, MenuSection
    from .theme import Theme
    from ..library.models import Playlist


class SettingsMenuRenderer:
    """Renders the slide-in settings menu."""
    
    def __init__(self, screen_width: int, screen_height: int):
        self.screen_width = screen_width
        self.screen_height = screen_height
        self.menu_width_ratio = 0.5  # 1/2 of screen width (center of screen)
        self.menu_width = int(screen_width * self.menu_width_ratio)
        self.bezel_mode = False  # Set externally based on display mode
    
    def render(
        self,
        screen: pygame.Surface,
        menu: SettingsMenuManager,
        theme: Theme,
        game_playlists: List[Playlist] = None,
    ) -> None:
        """Render the settings menu with slide animation."""
        if not menu.active and not menu.is_closing:
            return
        
        # Restore slide animation (but keep no alpha fade)
        progress = menu.get_animation_progress()
        
        if game_playlists is None:
            game_playlists = []
        
        # Calculate slide-in position (no alpha fade)
        slide_offset = int(self.menu_width * (1 - progress))
        menu_x = self.screen_width - self.menu_width + slide_offset
        
        # Resolve section color: headings, outlines, and selection indicators
        section_color = self._get_section_color(menu, theme)
        
        # Create menu surface
        menu_surface = pygame.Surface((self.menu_width, self.screen_height), pygame.SRCALPHA)
        
        # Background - fully opaque panel using theme background
        bg_color = (theme.bg[0], theme.bg[1], theme.bg[2], 255)
        menu_surface.fill(bg_color)
        
        # Left border accent uses section color
        border_color = section_color
        pygame.draw.line(
            menu_surface,
            border_color,
            (0, 0),
            (0, self.screen_height),
            4
        )
        
        # Header
        if menu.viewing_games_in_playlist:
            # Show playlist name when viewing individual games (preserve original case)
            if game_playlists and menu.current_game_playlist_index < len(game_playlists):
                playlist = game_playlists[menu.current_game_playlist_index]
                header_text = playlist.title
            else:
                header_text = "Games"
        elif menu.game_browser_active:
            header_text = "Game libraries"
        elif menu.current_submenu:
            header_text = self._get_submenu_title(menu)
        else:
            header_text = "Settings"
        
        # Header (centered)
        header_y = 8
        header_surf = theme.font_heading.render(header_text, True, section_color)
        header_x = (self.menu_width - header_surf.get_width()) // 2
        menu_surface.blit(header_surf, (header_x, header_y))
        
        # Text-width decorative line under header (matches main UI style)
        underline_y = header_y + header_surf.get_height() + 4
        pygame.draw.line(
            menu_surface,
            section_color,
            (header_x, underline_y),
            (header_x + header_surf.get_width(), underline_y),
            2
        )
        
        # Render game list, game browser, or normal menu
        if menu.viewing_games_in_playlist:
            # Show individual games in selected playlist
            self._render_individual_games(menu_surface, game_playlists, menu.current_game_playlist_index,
                                         menu.selected_game_in_playlist, theme, section_color, underline_y + 30)
        elif menu.game_browser_active:
            self._render_game_browser(menu_surface, game_playlists, menu.game_browser_selected, 
                                     theme, section_color, underline_y + 30)
        else:
            # Menu items with scrolling
            items = menu.submenu_items if menu.current_submenu else menu.menu_items
            start_y = underline_y + 30
            item_height = 60
            max_visible_items = 7  # Maximum items visible at once
            
            # Calculate visible range
            scroll_offset = menu.scroll_offset
            visible_items = items[scroll_offset:scroll_offset + max_visible_items]
            
            # Show scroll indicators if needed
            total_items = len(items)
            can_scroll_up = scroll_offset > 0
            can_scroll_down = scroll_offset + max_visible_items < total_items
            
            # Render scroll up indicator
            if can_scroll_up:
                indicator_y = start_y - 15
                pygame.draw.polygon(
                    menu_surface,
                    theme.dim,
                    [(self.menu_width // 2 - 6, indicator_y),
                     (self.menu_width // 2 + 6, indicator_y),
                     (self.menu_width // 2, indicator_y - 8)]
                )
            
            y = start_y
            for visible_idx, item in enumerate(visible_items):
                actual_idx = scroll_offset + visible_idx
                is_selected = (actual_idx == menu.selected_index)
                
                # Selection highlight
                if is_selected:
                    highlight_rect = pygame.Rect(10, y - 5, self.menu_width - 20, item_height)
                    highlight_surf = pygame.Surface(
                        (highlight_rect.width, highlight_rect.height),
                        pygame.SRCALPHA
                    )
                    # Subtle section tint
                    highlight_surf.fill((section_color[0], section_color[1], section_color[2], 40))
                    menu_surface.blit(highlight_surf, (highlight_rect.x, highlight_rect.y))
                    
                    # Selection indicator (section color)
                    indicator_x = 15
                    cy = y + item_height // 2 - 5
                    size = 8
                    points = [
                        (indicator_x, cy - size),
                        (indicator_x, cy + size),
                        (indicator_x + int(size * 1.2), cy),
                    ]
                    pygame.draw.polygon(menu_surface, section_color, points)
                
                # Main label (simple rendering, truncate if too long)
                # Use wider left margin in bezel mode
                text_x = 45 if self.bezel_mode else 35
                
                font = theme.font_medium if is_selected else theme.font_small
                color = section_color if is_selected else theme.fg
                label_surf = font.render(item.label, True, color)
                menu_surface.blit(label_surf, (text_x, y))
                
                # Sublabel (if exists)
                if item.sublabel:
                    sublabel_surf = theme.font_small.render(item.sublabel, True, theme.dim)
                    menu_surface.blit(sublabel_surf, (text_x, y + label_surf.get_height() + 4))
                
                y += item_height
            
            # Render scroll down indicator
            if can_scroll_down:
                indicator_y = y + 5
                pygame.draw.polygon(
                    menu_surface,
                    theme.dim,
                    [(self.menu_width // 2 - 6, indicator_y),
                     (self.menu_width // 2 + 6, indicator_y),
                     (self.menu_width // 2, indicator_y + 8)]
                )
        
        # Footer hint (different for different views)
        if menu.game_browser_active or menu.viewing_games_in_playlist:
            hint_text = "SELECT to choose • Back option to return"
        else:
            hint_text = "SELECT to choose • Button 4 to close"
        
        hint_surf = theme.font_small.render(hint_text, True, theme.dim)
        hint_x = (self.menu_width - hint_surf.get_width()) // 2
        menu_surface.blit(hint_surf, (hint_x, self.screen_height - 30))
        
        # Blit to screen (no alpha fade, just slide)
        screen.blit(menu_surface, (menu_x, 0))
    
    def _render_individual_games(
        self,
        surface: pygame.Surface,
        game_playlists: List[Playlist],
        playlist_index: int,
        selected_game_index: int,
        theme: Theme,
        section_color: tuple,
        start_y: int,
    ) -> None:
        """Render individual games in a playlist."""
        if not game_playlists or playlist_index >= len(game_playlists):
            return
        
        playlist = game_playlists[playlist_index]
        games = playlist.items
        
        if not games:
            # No games message
            no_games_text = "No games in this collection"
            no_games_surf = theme.font_small.render(no_games_text, True, theme.dim)
            x = (self.menu_width - no_games_surf.get_width()) // 2
            surface.blit(no_games_surf, (x, start_y + 40))
        else:
            # Render individual games
            y = start_y
            item_height = 45
            
            for idx, game in enumerate(games[:10]):  # Show up to 10 games
                is_selected = (idx == selected_game_index)
                
                # Selection highlight
                if is_selected:
                    highlight_rect = pygame.Rect(10, y - 3, self.menu_width - 20, item_height)
                    highlight_surf = pygame.Surface(
                        (highlight_rect.width, highlight_rect.height),
                        pygame.SRCALPHA
                    )
                    highlight_surf.fill((section_color[0], section_color[1], section_color[2], 40))
                    surface.blit(highlight_surf, (highlight_rect.x, highlight_rect.y))
                    
                    # Selection indicator
                    indicator_x = 15
                    cy = y + item_height // 2
                    size = 7
                    points = [
                        (indicator_x, cy - size),
                        (indicator_x, cy + size),
                        (indicator_x + int(size * 1.2), cy),
                    ]
                    pygame.draw.polygon(surface, section_color, points)
                
                # Game title
                text_x = 45 if self.bezel_mode else 30
                font = theme.font_medium if is_selected else theme.font_small
                color = section_color if is_selected else theme.fg
                title_surf = font.render(game.title, True, color)
                surface.blit(title_surf, (text_x, y + 2))
                
                # System badge
                if game.emulator_system:
                    system_surf = theme.font_small.render(game.emulator_system, True, theme.dim)
                    surface.blit(system_surf, (text_x, y + title_surf.get_height() + 4))
                
                y += item_height
            
            # Add a Back option at the bottom
            y += 10  # Small gap
            back_label = "← Back to Systems"
            back_font = theme.font_small
            back_color = theme.fg
            
            # Check if Back is selected (it's after all games)
            if selected_game_index == len(games):
                # Back is selected
                back_color = section_color
                # Highlight
                highlight_rect = pygame.Rect(10, y - 3, self.menu_width - 20, 40)
                highlight_surf = pygame.Surface((highlight_rect.width, highlight_rect.height), pygame.SRCALPHA)
                highlight_surf.fill((section_color[0], section_color[1], section_color[2], 40))
                surface.blit(highlight_surf, (highlight_rect.x, highlight_rect.y))
                
                # Indicator
                indicator_x = 15
                cy = y + 20
                size = 7
                points = [(indicator_x, cy - size), (indicator_x, cy + size), (indicator_x + int(size * 1.2), cy)]
                pygame.draw.polygon(surface, section_color, points)
            
            text_x = 45 if self.bezel_mode else 30
            back_surf = back_font.render(back_label, True, back_color)
            surface.blit(back_surf, (text_x, y + 10))
    
    def _render_game_browser(
        self,
        surface: pygame.Surface,
        game_playlists: List[Playlist],
        selected_index: int,
        theme: Theme,
        section_color: tuple,
        start_y: int,
    ) -> None:
        """Render game playlists browser."""
        if not game_playlists:
            # No games message
            no_games_text = "No game playlists found"
            no_games_surf = theme.font_small.render(no_games_text, True, theme.dim)
            x = (self.menu_width - no_games_surf.get_width()) // 2
            surface.blit(no_games_surf, (x, start_y + 40))
            
            info_text = "Add games to playlists"
            info_surf = theme.font_small.render(info_text, True, theme.dim)
            x = (self.menu_width - info_surf.get_width()) // 2
            surface.blit(info_surf, (x, start_y + 60))
        else:
            # Render game playlists
            y = start_y
            item_height = 50
            
            for idx, playlist in enumerate(game_playlists[:8]):  # Show up to 8
                is_selected = (idx == selected_index)
                
                # Selection highlight
                if is_selected:
                    highlight_rect = pygame.Rect(10, y - 3, self.menu_width - 20, item_height)
                    highlight_surf = pygame.Surface(
                        (highlight_rect.width, highlight_rect.height),
                        pygame.SRCALPHA
                    )
                    highlight_surf.fill((section_color[0], section_color[1], section_color[2], 40))
                    surface.blit(highlight_surf, (highlight_rect.x, highlight_rect.y))
                    
                    # Selection indicator
                    indicator_x = 15
                    cy = y + item_height // 2
                    size = 7
                    points = [
                        (indicator_x, cy - size),
                        (indicator_x, cy + size),
                        (indicator_x + int(size * 1.2), cy),
                    ]
                    pygame.draw.polygon(surface, section_color, points)
                
                # Playlist title
                text_x = 45 if self.bezel_mode else 30
                font = theme.font_medium if is_selected else theme.font_small
                color = section_color if is_selected else theme.fg
                title_surf = font.render(playlist.title, True, color)
                surface.blit(title_surf, (text_x, y))
                
                # Game count
                game_count = len(playlist.items)
                count_text = f"{game_count} game{'s' if game_count != 1 else ''}"
                count_surf = theme.font_small.render(count_text, True, theme.dim)
                surface.blit(count_surf, (text_x, y + title_surf.get_height() + 2))
                
                y += item_height
            
            # Add a Back option at the bottom
            y += 10  # Small gap
            back_label = "← Back to Video Games"
            back_font = theme.font_small
            back_color = theme.fg
            
            # Check if Back is selected (it's after all playlists)
            if selected_index == len(game_playlists):
                # Back is selected
                back_color = section_color
                # Highlight
                highlight_rect = pygame.Rect(10, y - 3, self.menu_width - 20, 40)
                highlight_surf = pygame.Surface((highlight_rect.width, highlight_rect.height), pygame.SRCALPHA)
                highlight_surf.fill((section_color[0], section_color[1], section_color[2], 40))
                surface.blit(highlight_surf, (highlight_rect.x, highlight_rect.y))
                
                # Indicator
                indicator_x = 15
                cy = y + 20
                size = 7
                points = [(indicator_x, cy - size), (indicator_x, cy + size), (indicator_x + int(size * 1.2), cy)]
                pygame.draw.polygon(surface, section_color, points)
            
            text_x = 45 if self.bezel_mode else 30
            back_surf = back_font.render(back_label, True, back_color)
            surface.blit(back_surf, (text_x, y + 10))
    
    def _get_submenu_title(self, menu: SettingsMenuManager) -> str:
        """Get title for current submenu."""
        from .settings_menu import MenuSection
        
        titles = {
            MenuSection.VIDEO_GAMES: "Video games",
            MenuSection.DISPLAY: "Display",
            MenuSection.AUDIO: "Audio",
            MenuSection.SYSTEM: "System info",
        }
        return titles.get(menu.current_submenu, "Settings")

    def _get_section_color(self, menu: SettingsMenuManager, theme: Theme) -> tuple:
        """Choose a section color from the theme highlights/action.

        - Video Games views: highlight1 (green)
        - Display: highlight3 (gold)
        - Audio: action (blue)
        - System: highlight2 (red)
        - Default/main (root settings screen): highlight1 (green)
        """
        from .settings_menu import MenuSection

        # Game-specific views
        if menu.viewing_games_in_playlist or menu.game_browser_active:
            return theme.highlight1

        if menu.current_submenu == MenuSection.VIDEO_GAMES:
            return theme.highlight1
        if menu.current_submenu == MenuSection.DISPLAY:
            return theme.highlight3
        if menu.current_submenu == MenuSection.AUDIO:
            return theme.action
        if menu.current_submenu == MenuSection.SYSTEM:
            return theme.highlight2
        return theme.highlight1

