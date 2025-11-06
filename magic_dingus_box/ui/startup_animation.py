from __future__ import annotations

import math
import pygame
from typing import Tuple


class StartupAnimation:
    """Elegant sliding word animation with tasteful glow effects."""
    
    def __init__(self, screen: pygame.Surface, font_title: pygame.font.Font, duration: float = 5.5, theme=None) -> None:
        self.screen = screen
        self.font_title = font_title
        self.duration = duration
        self.theme = theme
        self.start_time: float | None = None
        self.width = screen.get_width()
        self.height = screen.get_height()
        self.center_x = self.width // 2
        self.center_y = self.height // 2
        
        # Create larger font for animation
        self.anim_font = pygame.font.SysFont("DejaVu Sans Mono", 52, bold=True)
        
        # Colors from theme (fallback to legacy if theme not provided)
        if self.theme is not None:
            primary = getattr(self.theme, "accent", (255, 64, 160))
            action = getattr(self.theme, "accent2", (64, 255, 200))
            self.bg = getattr(self.theme, "bg", (0, 0, 0))
        else:
            primary = (255, 64, 160)
            action = (64, 255, 200)
            self.bg = (0, 0, 0)
        
        # Word configuration: (text, color, slide_from_left)
        self.words = [
            ("Magic", primary, True),
            ("Dingus", action, False),
            ("Box", primary, True),
        ]
        
    def start(self) -> None:
        """Begin the animation."""
        import time
        self.start_time = time.time()
    
    def is_complete(self) -> bool:
        """Check if animation has finished."""
        if self.start_time is None:
            return False
        import time
        return (time.time() - self.start_time) >= self.duration
    
    def render(self) -> None:
        """Render one frame of the animation."""
        if self.start_time is None:
            return
        
        import time
        elapsed = time.time() - self.start_time
        progress = min(1.0, elapsed / self.duration)
        
        # Background
        self.screen.fill(self.bg)
        
        # Draw sliding words (ensure background doesn't occlude text)
        self._draw_sliding_words(elapsed, progress)
        
    def _draw_sliding_words(self, elapsed: float, progress: float) -> None:
        """Draw three words sliding in from alternating sides with tasteful glow."""
        # Timing: Each word gets ~1.2 seconds to slide in, then 1 second hold at end
        word_duration = 1.2
        hold_start = 4.5  # When all words are in place
        fade_start = 5.0  # Start fading out
        
        # Vertical spacing between words
        line_height = 70
        start_y = self.center_y - line_height
        
        for idx, (word, color, from_left) in enumerate(self.words):
            word_start_time = idx * word_duration
            
            # Calculate word progress (0.0 = offscreen, 1.0 = in position)
            if elapsed < word_start_time:
                word_progress = 0.0
            elif elapsed < word_start_time + word_duration:
                # Ease-out curve for smooth deceleration
                t = (elapsed - word_start_time) / word_duration
                word_progress = 1.0 - (1.0 - t) ** 3  # Cubic ease-out
            else:
                word_progress = 1.0
            
            # Calculate position
            y_pos = start_y + idx * line_height
            
            if from_left:
                # Slide from left
                start_x = -200
                x_pos = start_x + (self.center_x - start_x) * word_progress
            else:
                # Slide from right
                start_x = self.width + 200
                x_pos = start_x - (start_x - self.center_x) * word_progress
            
            # Calculate opacity (fade out at the end)
            if elapsed > fade_start:
                fade_progress = (elapsed - fade_start) / (self.duration - fade_start)
                alpha = int(255 * (1.0 - fade_progress))
            else:
                alpha = 255
            
            # Draw word with glow
            self._draw_word_with_glow(word, int(x_pos), int(y_pos), color, alpha)
    
    def _draw_word_with_glow(self, word: str, x: int, y: int, color: tuple, alpha: int) -> None:
        """Draw a single word with retro 70s/80s styling - bold outline and drop shadow."""
        # Render readable startup text with minimal effects to avoid occlusion artifacts
        # Subtle shadow only
        shadow_offset = 2
        shadow = self.anim_font.render(word, True, (0, 0, 0))
        shadow_rect = shadow.get_rect(center=(x + shadow_offset, y + shadow_offset))
        if alpha < 255:
            shadow.set_alpha(int(alpha * 0.6))
        self.screen.blit(shadow, shadow_rect)

        # Main text
        text = self.anim_font.render(word, True, color)
        text_rect = text.get_rect(center=(x, y))
        if alpha < 255:
            text.set_alpha(alpha)
        self.screen.blit(text, text_rect)

