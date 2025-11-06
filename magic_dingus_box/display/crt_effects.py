"""Pygame-based CRT visual effects for authentic retro aesthetic."""
from __future__ import annotations

import math
import pygame


def apply_enhanced_scanlines(surface: pygame.Surface, intensity: float = 0.3) -> None:
    """Apply adjustable scanlines with subtle RGB offset.
    
    Args:
        surface: Surface to apply effect to (modified in-place)
        intensity: Scanline darkness (0.0 = none, 1.0 = maximum)
    """
    if intensity <= 0:
        return
    
    w, h = surface.get_size()
    scanline_surf = pygame.Surface((w, h), pygame.SRCALPHA)
    
    for y in range(0, h, 2):
        # Main scanline
        alpha = int(255 * min(1.0, intensity))
        pygame.draw.line(scanline_surf, (0, 0, 0, alpha), (0, y), (w, y))
        
        # Subtle RGB offset on alternating lines for phosphor simulation
        if y % 4 == 0 and intensity > 0.2:
            rgb_alpha = int(alpha * 0.3)
            pygame.draw.line(scanline_surf, (10, 0, 0, rgb_alpha), (0, y+1), (w, y+1))
    
    surface.blit(scanline_surf, (0, 0))


def apply_color_warmth(surface: pygame.Surface, warmth: float = 0.5) -> None:
    """Apply warm color tint (simulates CRT phosphor temperature).
    
    Args:
        surface: Surface to apply effect to (modified in-place)
        warmth: Warmth amount (0.0 = none, 1.0 = maximum warm)
    """
    if warmth <= 0:
        return
    
    w, h = surface.get_size()
    tint = pygame.Surface((w, h), pygame.SRCALPHA)
    
    # Warm orange/yellow tint
    r = int(20 * warmth)
    g = int(10 * warmth)
    b = 0
    alpha = int(30 * warmth)
    
    tint.fill((r, g, b, alpha))
    surface.blit(tint, (0, 0))


def apply_screen_bloom(surface: pygame.Surface, amount: float = 0.2) -> None:
    """Subtle bloom effect for bright areas (CRT light bleeding).
    
    Args:
        surface: Surface to apply effect to (modified in-place)
        amount: Bloom intensity (0.0 = none, 1.0 = maximum)
    """
    if amount <= 0:
        return
    
    # Simple bloom: very subtle white glow over entire image
    w, h = surface.get_size()
    bloom_overlay = pygame.Surface((w, h), pygame.SRCALPHA)
    
    # Much subtler than before - only 5-12 alpha
    alpha = int(5 + (7 * amount))
    bloom_overlay.fill((255, 255, 255, alpha))
    
    # Use normal blending, not BLEND_RGB_ADD
    surface.blit(bloom_overlay, (0, 0))


def apply_phosphor_glow(surface: pygame.Surface, intensity: float = 0.3) -> None:
    """Add subtle phosphor glow effect (simulates CRT phosphor persistence).
    
    Creates a smooth radial gradient from center to edges using surface-level
    opacity control for predictable, noticeable intensity.
    
    Args:
        surface: Surface to apply effect to (modified in-place)
        intensity: Glow intensity (0.0 = none, 1.0 = maximum, default 0.3 = 8-20% opacity)
    """
    if intensity <= 0:
        return
    
    # Create smooth radial gradient (many circles with tiny alphas)
    w, h = surface.get_size()
    glow_surf = pygame.Surface((w, h), pygame.SRCALPHA)
    
    center_x, center_y = w // 2, h // 2
    max_dist = math.sqrt(center_x**2 + center_y**2)
    
    # Draw many concentric circles for smooth gradient
    # Use FULL alpha range within surface, control opacity at surface level
    num_circles = 15
    for i in range(num_circles, 0, -1):
        # Radius grows from center to edges
        progress = i / num_circles
        radius = int(max_dist * progress * 1.1)
        
        # Build internal gradient using full alpha range (0-255)
        # Power curve for natural falloff (brighter center, fades to edges)
        alpha_for_gradient = int((progress ** 1.5) * 255)
        
        # Subtle green/cyan tint (classic CRT phosphor) with gradient alpha
        color = (4, 120, 100, alpha_for_gradient)
        pygame.draw.circle(glow_surf, color, (center_x, center_y), radius)
    
    # Control overall effect intensity with surface-level opacity
    # Range: 20-50 = 8-20% total opacity (noticeable but tasteful)
    overall_opacity = int(20 + (30 * intensity))
    glow_surf.set_alpha(overall_opacity)
    
    # Normal blit (surface alpha handles the blending)
    surface.blit(glow_surf, (0, 0))


def apply_rgb_phosphor_mask(surface: pygame.Surface, intensity: float = 0.3) -> None:
    """Apply RGB phosphor mask pattern (simulates CRT RGB sub-pixels).
    
    Creates subtle vertical RGB stripes like real CRT phosphors using surface-level
    opacity control for predictable, subtle intensity.
    
    Args:
        surface: Surface to apply effect to (modified in-place)
        intensity: Mask intensity (0.0 = none, 1.0 = maximum, default 0.3 = 4-10% opacity)
    """
    if intensity <= 0:
        return
    
    w, h = surface.get_size()
    mask_surf = pygame.Surface((w, h), pygame.SRCALPHA)
    
    # Draw vertical RGB stripes with FULL alpha (255)
    # We'll control opacity at surface level for predictable intensity
    # Every 4th row to balance visibility and performance
    for y in range(0, h, 4):
        for x in range(0, w, 3):  # R, G, B pattern (standard 3-pixel repeat)
            # Draw short vertical lines (2 pixels tall)
            line_height = 2
            
            # Red phosphor stripe (full alpha 255)
            if x < w:
                for dy in range(line_height):
                    if y + dy < h:
                        pygame.draw.line(mask_surf, (255, 0, 0, 255), (x, y+dy), (x, y+dy))
            
            # Green phosphor stripe (full alpha 255)
            if x + 1 < w:
                for dy in range(line_height):
                    if y + dy < h:
                        pygame.draw.line(mask_surf, (0, 255, 0, 255), (x+1, y+dy), (x+1, y+dy))
            
            # Blue phosphor stripe (full alpha 255)
            if x + 2 < w:
                for dy in range(line_height):
                    if y + dy < h:
                        pygame.draw.line(mask_surf, (0, 0, 255, 255), (x+2, y+dy), (x+2, y+dy))
    
    # Control overall effect intensity with surface-level opacity
    # Range: 10-25 = 4-10% total opacity (subtle but visible)
    overall_opacity = int(10 + (15 * intensity))
    mask_surf.set_alpha(overall_opacity)
    
    # Normal blit (surface alpha handles the blending)
    surface.blit(mask_surf, (0, 0))


def apply_interlacing(surface: pygame.Surface, frame_count: int, intensity: float = 0.3) -> None:
    """Apply interlacing effect (simulates CRT interlaced video).
    
    Args:
        surface: Surface to apply effect to (modified in-place)
        frame_count: Current frame number (for alternating lines)
        intensity: Interlacing darkness (0.0 = none, 1.0 = maximum)
    """
    if intensity <= 0:
        return
    
    w, h = surface.get_size()
    interlace_surf = pygame.Surface((w, h), pygame.SRCALPHA)
    
    # Alternate which lines are darkened based on frame
    alpha = int(60 * intensity)
    offset = frame_count % 2
    
    for y in range(offset, h, 2):
        pygame.draw.line(interlace_surf, (0, 0, 0, alpha), (0, y), (w, y))
    
    surface.blit(interlace_surf, (0, 0))


def apply_subtle_flicker(surface: pygame.Surface, time: float, intensity: float = 0.3) -> None:
    """Apply subtle screen flicker (simulates CRT instability).
    
    Args:
        surface: Surface to apply effect to (modified in-place)
        time: Current time in seconds (for sine wave)
        intensity: Flicker amount (0.0 = none, 1.0 = maximum)
    """
    if intensity <= 0:
        return
    
    # Very subtle brightness variation using sine wave
    # Multiple frequencies for natural look
    flicker = math.sin(time * 23.0) * 0.3 + math.sin(time * 7.5) * 0.7
    
    # Brightness adjustment (-2% to +2% at max intensity)
    brightness_delta = int(flicker * 5 * intensity)
    
    w, h = surface.get_size()
    flicker_surf = pygame.Surface((w, h), pygame.SRCALPHA)
    
    if brightness_delta > 0:
        # Brighten slightly
        flicker_surf.fill((brightness_delta, brightness_delta, brightness_delta, 255))
        surface.blit(flicker_surf, (0, 0), special_flags=pygame.BLEND_RGB_ADD)
    elif brightness_delta < 0:
        # Darken slightly
        alpha = abs(brightness_delta) * 3
        flicker_surf.fill((0, 0, 0, alpha))
        surface.blit(flicker_surf, (0, 0))


def apply_screen_curvature(surface: pygame.Surface, amount: float = 0.05) -> pygame.Surface:
    """Apply gentle barrel distortion (CRT screen curvature).
    
    NOTE: This is computationally expensive. Use sparingly.
    
    Args:
        surface: Surface to apply effect to
        amount: Curvature amount (0.0 = none, 0.1 = subtle curve)
        
    Returns:
        New surface with curvature applied
    """
    if amount <= 0:
        return surface
    
    # This would require pixel-level manipulation
    # For now, return unchanged (can implement later if performance allows)
    return surface


class CRTEffectsManager:
    """Manages and applies CRT visual effects."""
    
    def __init__(self):
        """Initialize CRT effects manager with default settings."""
        self.scanlines_enabled = False
        self.scanlines_intensity = 0.3  # 0.0 - 1.0
        self.warmth_enabled = False
        self.warmth_amount = 0.5  # 0.0 - 1.0
        self.bloom_enabled = False
        self.bloom_amount = 0.2  # 0.0 - 1.0
        self.glow_enabled = False
        self.glow_intensity = 0.05  # 0.0 - 1.0
        self.phosphor_mask_enabled = False
        self.phosphor_mask_intensity = 0.05  # 0.0 - 1.0
        self.interlacing_enabled = False
        self.interlacing_intensity = 0.3  # 0.0 - 1.0
        self.flicker_enabled = False
        self.flicker_intensity = 0.3  # 0.0 - 1.0
        
        # Frame counter for interlacing
        self.frame_count = 0
    
    def load_settings(self, settings_store) -> None:
        """Load effect settings from settings store.
        
        Args:
            settings_store: SettingsStore instance
        """
        # Scanlines
        scanlines_mode = settings_store.get("scanlines_mode", "off")
        if scanlines_mode == "off":
            self.scanlines_enabled = False
        else:
            self.scanlines_enabled = True
            intensity_map = {"light": 0.15, "medium": 0.3, "heavy": 0.5}
            self.scanlines_intensity = intensity_map.get(scanlines_mode, 0.3)
        
        # Color warmth
        self.warmth_amount = settings_store.get("color_warmth", 0.0)
        self.warmth_enabled = self.warmth_amount > 0
        
        # Phosphor Glow (intensity-based)
        self.glow_intensity = settings_store.get("phosphor_glow", 0.0)
        self.glow_enabled = self.glow_intensity > 0
        
        # RGB Phosphor Mask (intensity-based)
        self.phosphor_mask_intensity = settings_store.get("phosphor_mask", 0.0)
        self.phosphor_mask_enabled = self.phosphor_mask_intensity > 0
        
        # Screen Bloom (intensity-based)
        self.bloom_amount = settings_store.get("screen_bloom", 0.0)
        self.bloom_enabled = self.bloom_amount > 0
        
        # Interlacing (intensity-based)
        self.interlacing_intensity = settings_store.get("interlacing", 0.0)
        self.interlacing_enabled = self.interlacing_intensity > 0
        
        # Flicker (intensity-based)
        self.flicker_intensity = settings_store.get("screen_flicker", 0.0)
        self.flicker_enabled = self.flicker_intensity > 0
    
    def apply_all(self, surface: pygame.Surface, current_time: float = 0.0) -> None:
        """Apply all enabled CRT effects to surface.
        
        Args:
            surface: Surface to apply effects to (modified in-place)
            current_time: Current time in seconds (for time-based effects like flicker)
        """
        # Increment frame counter for interlacing
        self.frame_count += 1
        
        # Apply effects in order for best visual result
        if self.bloom_enabled:
            apply_screen_bloom(surface, self.bloom_amount)
        
        if self.warmth_enabled:
            apply_color_warmth(surface, self.warmth_amount)
        
        if self.glow_enabled:
            apply_phosphor_glow(surface, self.glow_intensity)
        
        if self.phosphor_mask_enabled:
            apply_rgb_phosphor_mask(surface, self.phosphor_mask_intensity)
        
        if self.interlacing_enabled:
            apply_interlacing(surface, self.frame_count, self.interlacing_intensity)
        
        if self.scanlines_enabled:
            apply_enhanced_scanlines(surface, self.scanlines_intensity)
        
        if self.flicker_enabled:
            apply_subtle_flicker(surface, current_time, self.flicker_intensity)
    
    def get_scanlines_label(self) -> str:
        """Get label for current scanlines setting."""
        if not self.scanlines_enabled:
            return "OFF"
        intensity_pct = int(self.scanlines_intensity * 100)
        if self.scanlines_intensity <= 0.2:
            return f"Light ({intensity_pct}%)"
        elif self.scanlines_intensity <= 0.4:
            return f"Medium ({intensity_pct}%)"
        else:
            return f"Heavy ({intensity_pct}%)"
    
    def get_warmth_label(self) -> str:
        """Get label for current warmth setting."""
        if not self.warmth_enabled or self.warmth_amount <= 0:
            return "OFF"
        pct = int(self.warmth_amount * 100)
        if self.warmth_amount < 0.35:
            return f"Cool ({pct}%)"
        elif self.warmth_amount < 0.65:
            return f"Neutral ({pct}%)"
        else:
            return f"Warm ({pct}%)"

