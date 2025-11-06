from __future__ import annotations

import pygame
from pathlib import Path


class Theme:
    def __init__(self) -> None:
        # New palette (user-provided)
        # Background: #1F191F, Text: #F2E4D9
        # Highlight 1-3: #66DD7A, #EA3A27, #F5BF42  (used sparingly)
        # Action: #5884B1 (selection, borders, indicators)
        self.bg = (31, 25, 31)
        self.fg = (242, 228, 217)

        # Role-based colors
        self.highlight1 = (102, 221, 122)  # green
        self.highlight2 = (234, 58, 39)    # red/orange
        self.highlight3 = (245, 191, 66)   # gold
        self.action = (88, 132, 177)       # steel blue

        # Back-compat aliases used throughout renderers
        # accent = primary decorative highlight, accent2 = action/border
        self.accent = self.highlight3
        self.accent2 = self.action

        # Dim text derived from fg (approximately 60%)
        self.dim = (150, 140, 135)

        # Fonts
        # Title prefers bundled Zen Dots TTF if present, then system, then fallbacks
        self.font_title = self._load_zen_dots_or_fallback(32, bold=True)
        # Slightly smaller than the main title for section headings
        self.font_heading = self._load_zen_dots_or_fallback(22)
        self.font_large = self._sysfont_fallback([
            "Share Tech Mono", "IBM Plex Mono", "DejaVu Sans Mono"
        ], 24)
        self.font_medium = self._sysfont_fallback([
            "Share Tech Mono", "IBM Plex Mono", "DejaVu Sans Mono"
        ], 18)
        self.font_small = self._sysfont_fallback([
            "Share Tech Mono", "IBM Plex Mono", "DejaVu Sans Mono"
        ], 14)

    def draw_scanlines(self, surface: pygame.Surface) -> None:
        w, h = surface.get_size()
        surf = pygame.Surface((w, h), pygame.SRCALPHA)
        scan_color = (0, 0, 0, 48)
        for y in range(0, h, 2):
            pygame.draw.line(surf, scan_color, (0, y), (w, y))
        surface.blit(surf, (0, 0))

    def draw_vignette(self, surface: pygame.Surface) -> None:
        # Simple radial vignette using alpha gradient bands
        w, h = surface.get_size()
        overlay = pygame.Surface((w, h), pygame.SRCALPHA)
        thickness = 20
        for i in range(12):
            a = 6 + i * 3
            pygame.draw.rect(
                overlay,
                (0, 0, 0, a),
                pygame.Rect(i * thickness // 2, i * thickness // 2, w - i * thickness, h - i * thickness),
                width=thickness,
            )
        surface.blit(overlay, (0, 0))
    
    def draw_box_border(self, surface: pygame.Surface, rect: pygame.Rect, color: tuple, thickness: int = 2) -> None:
        """Draw a retro-style box border with double lines."""
        x, y, w, h = rect.x, rect.y, rect.width, rect.height
        
        # Outer border
        pygame.draw.rect(surface, color, rect, thickness)
        
        # Inner border (for double-line effect)
        inner_offset = thickness + 2
        inner_rect = pygame.Rect(
            x + inner_offset,
            y + inner_offset,
            w - inner_offset * 2,
            h - inner_offset * 2
        )
        pygame.draw.rect(surface, color, inner_rect, 1)
    
    def draw_corner_brackets(self, surface: pygame.Surface, rect: pygame.Rect, color: tuple, bracket_size: int = 12) -> None:
        """Draw decorative corner brackets (like old TV frames)."""
        x, y, w, h = rect.x, rect.y, rect.width, rect.height
        thickness = 2
        
        # Top-left corner
        pygame.draw.line(surface, color, (x, y + bracket_size), (x, y), thickness)
        pygame.draw.line(surface, color, (x, y), (x + bracket_size, y), thickness)
        
        # Top-right corner
        pygame.draw.line(surface, color, (x + w - bracket_size, y), (x + w, y), thickness)
        pygame.draw.line(surface, color, (x + w, y), (x + w, y + bracket_size), thickness)
        
        # Bottom-left corner
        pygame.draw.line(surface, color, (x, y + h - bracket_size), (x, y + h), thickness)
        pygame.draw.line(surface, color, (x, y + h), (x + bracket_size, y + h), thickness)
        
        # Bottom-right corner
        pygame.draw.line(surface, color, (x + w - bracket_size, y + h), (x + w, y + h), thickness)
        pygame.draw.line(surface, color, (x + w, y + h), (x + w, y + h - bracket_size), thickness)

    def _sysfont_fallback(self, names: list[str], size: int, bold: bool = False) -> pygame.font.Font:
        """Attempt to load the first available system font from names.

        Falls back to pygame default mono if none are present.
        """
        for name in names:
            try:
                f = pygame.font.SysFont(name, size, bold=bold)
                # SysFont returns a default even if name isn't present; verify name match where possible
                if f is not None:
                    return f
            except Exception:
                continue
        return pygame.font.SysFont("DejaVu Sans Mono", size, bold=bold)

    def _load_zen_dots_or_fallback(self, size: int, bold: bool = False) -> pygame.font.Font:
        """Try to load Zen Dots from repository assets, then system, then fall back."""
        try:
            repo_root = Path(__file__).resolve().parents[2]
            ttf = repo_root / "assets" / "fonts" / "ZenDots-Regular.ttf"
            if ttf.exists():
                return pygame.font.Font(str(ttf), size)
        except Exception:
            pass
        return self._sysfont_fallback(["Zen Dots", "Orbitron", "Audiowide", "DejaVu Sans Mono"], size, bold=bold)

