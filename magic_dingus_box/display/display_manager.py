"""Display manager for handling multiple screen modes and resolutions."""
from __future__ import annotations

import logging
from enum import Enum
from pathlib import Path
from typing import Tuple, Optional
import pygame


class DisplayMode(Enum):
    """Display mode options."""
    CRT_NATIVE = "crt_native"           # 720x480 fullscreen, no bezel
    MODERN_CLEAN = "modern_clean"       # Pillarboxed, no bezel
    MODERN_WITH_BEZEL = "modern_bezel"  # Pillarboxed with CRT frame


class DisplayManager:
    """Manages rendering to different display modes and resolutions."""
    
    def __init__(
        self,
        mode: DisplayMode,
        target_resolution: Tuple[int, int],
        content_resolution: Tuple[int, int] = (720, 480)
    ):
        """Initialize display manager.
        
        Args:
            mode: Display mode (CRT_NATIVE, MODERN_CLEAN, or MODERN_WITH_BEZEL)
            target_resolution: Actual screen resolution (e.g., 1920x1080)
            content_resolution: Internal rendering resolution (always 720x480 for 4:3)
        """
        self.mode = mode
        self.target_resolution = target_resolution
        self.content_resolution = content_resolution
        self._log = logging.getLogger("display_mgr")
        
        # Create offscreen surface for 4:3 content rendering (with alpha support)
        self.content_surface = pygame.Surface(content_resolution, pygame.SRCALPHA)
        
        # Calculate layout for modern modes
        self.content_rect = pygame.Rect(0, 0, 0, 0)
        if mode != DisplayMode.CRT_NATIVE:
            self.calculate_layout()
    
    def calculate_layout(self) -> None:
        """Calculate where 4:3 content fits in target resolution."""
        target_w, target_h = self.target_resolution
        content_w, content_h = self.content_resolution
        
        # Apply NTSC pixel aspect ratio correction
        # 720x480 uses non-square pixels (0.9 aspect) that appear as 4:3 on CRT
        # For modern displays with square pixels, apply correction
        ntsc_pixel_aspect = 0.9
        effective_width = int(content_w * ntsc_pixel_aspect)
        content_aspect = effective_width / content_h  # ~1.35, closer to true 4:3 (1.333)
        
        # Use full screen space - let content fill entire height
        # Bezel will naturally overlay on top to create the frame effect
        usable_h = target_h
        usable_w = target_w
        
        # Fit to height first (maximize content size)
        scaled_h = usable_h
        scaled_w = int(scaled_h * content_aspect)
        
        # If width doesn't fit, scale by width instead
        if scaled_w > usable_w:
            scaled_w = usable_w
            scaled_h = int(scaled_w / content_aspect)
        
        # Center the content perfectly
        x = (target_w - scaled_w) // 2
        y = (target_h - scaled_h) // 2
        
        self.content_rect = pygame.Rect(x, y, scaled_w, scaled_h)
    
    def get_render_surface(self) -> pygame.Surface:
        """Get the surface that all UI components should render to."""
        return self.content_surface
    
    def present(self, screen: pygame.Surface, bezel: Optional[pygame.Surface] = None, preserve_video_area: bool = False) -> None:
        """Composite the content surface to the actual screen.
        
        Args:
            screen: The actual pygame display surface
            bezel: Optional pre-generated bezel surface
            preserve_video_area: If True, do not overpaint the 4:3 content area so embedded
                mpv video remains visible underneath; fill only outside that area.
        """
        if self.mode == DisplayMode.CRT_NATIVE:
            # Fill background then blit content (ensures no artifacts with alpha)
            screen.fill((0, 0, 0))
            screen.blit(self.content_surface, (0, 0))
        else:
            if preserve_video_area:
                # Fill only outside the content_rect so the mpv-rendered video in the content area
                # remains visible underneath our overlays.
                w, h = self.target_resolution
                
                # Top bar
                if self.content_rect.top > 0:
                    screen.fill((0, 0, 0), pygame.Rect(0, 0, w, self.content_rect.top))
                # Bottom bar
                if self.content_rect.bottom < h:
                    screen.fill((0, 0, 0), pygame.Rect(0, self.content_rect.bottom, w, h - self.content_rect.bottom))
                # Left bar
                if self.content_rect.left > 0:
                    screen.fill((0, 0, 0), pygame.Rect(0, self.content_rect.top, self.content_rect.left, self.content_rect.height))
                # Right bar
                if self.content_rect.right < w:
                    screen.fill((0, 0, 0), pygame.Rect(self.content_rect.right, self.content_rect.top, w - self.content_rect.right, self.content_rect.height))
            else:
                # Fill screen with black (pillarboxing/letterboxing)
                screen.fill((0, 0, 0))
            
            # Scale and blit content to center FIRST
            scaled_content = pygame.transform.scale(
                self.content_surface,
                (self.content_rect.width, self.content_rect.height)
            )
            screen.blit(scaled_content, self.content_rect)
            
            # Draw bezel ON TOP so frame overlays content edges
            if self.mode == DisplayMode.MODERN_WITH_BEZEL and bezel:
                screen.blit(bezel, (0, 0))
    
    def load_bezel_image(self, bezel_path: Path) -> Optional[pygame.Surface]:
        """Load and scale a bezel image from file.
        
        Args:
            bezel_path: Path to PNG bezel file
            
        Returns:
            Scaled bezel surface, or None if loading fails
        """
        if not bezel_path.exists():
            self._log.warning(f"Bezel file not found: {bezel_path}")
            return None
        
        try:
            bezel_img = pygame.image.load(str(bezel_path))
            # Scale to target resolution with smooth scaling
            scaled_bezel = pygame.transform.smoothscale(bezel_img, self.target_resolution)
            self._log.info(f"Loaded bezel from: {bezel_path.name}")
            return scaled_bezel
        except Exception as e:
            self._log.error(f"Failed to load bezel {bezel_path}: {e}")
            return None
    
    def generate_bezel(self) -> pygame.Surface:
        """Generate a CRT TV bezel frame for modern display mode."""
        w, h = self.target_resolution
        surface = pygame.Surface((w, h), pygame.SRCALPHA)
        surface.fill((0, 0, 0, 0))  # Start fully transparent
        
        bezel_thickness = 60
        frame_color = (101, 67, 33)  # Dark wood
        
        # Outer wooden frame (only outside content area)
        # Top bar
        top_rect = pygame.Rect(0, 0, w, self.content_rect.top - bezel_thickness)
        if top_rect.height > 0:
            pygame.draw.rect(surface, frame_color, top_rect)
        
        # Bottom bar
        bottom_rect = pygame.Rect(0, self.content_rect.bottom + bezel_thickness, w, h - (self.content_rect.bottom + bezel_thickness))
        if bottom_rect.height > 0:
            pygame.draw.rect(surface, frame_color, bottom_rect)
        
        # Left bar
        left_rect = pygame.Rect(0, 0, self.content_rect.left - bezel_thickness, h)
        if left_rect.width > 0:
            pygame.draw.rect(surface, frame_color, left_rect)
        
        # Right bar
        right_rect = pygame.Rect(self.content_rect.right + bezel_thickness, 0, w - (self.content_rect.right + bezel_thickness), h)
        if right_rect.width > 0:
            pygame.draw.rect(surface, frame_color, right_rect)
        
        # Inner plastic bezel (dark frame around screen)
        bezel_rect = self.content_rect.inflate(bezel_thickness*2, bezel_thickness*2)
        pygame.draw.rect(surface, (30, 30, 30), bezel_rect, bezel_thickness)
        
        # Glass edge highlight
        glass_highlight = self.content_rect.inflate(4, 4)
        pygame.draw.rect(surface, (60, 60, 60), glass_highlight, 2)
        
        # Brand name (below screen if there's room)
        if self.content_rect.bottom + bezel_thickness + 60 < h:
            try:
                font = pygame.font.SysFont("Arial", 24, bold=True)
                brand = font.render("MAGICVISION", True, (200, 180, 140))
                brand_x = (w - brand.get_width()) // 2
                brand_y = self.content_rect.bottom + bezel_thickness + 15
                surface.blit(brand, (brand_x, brand_y))
                
                # Control knobs (decorative circles)
                knob_y = brand_y + 35
                knob_size = 20
                knob_spacing = 50
                num_knobs = 3
                total_width = num_knobs * knob_size + (num_knobs - 1) * knob_spacing
                start_x = (w - total_width) // 2
                
                for i in range(num_knobs):
                    knob_x = start_x + i * (knob_size + knob_spacing) + knob_size//2
                    # Outer ring
                    pygame.draw.circle(surface, (50, 50, 50), (knob_x, knob_y), knob_size//2)
                    # Inner circle
                    pygame.draw.circle(surface, (70, 70, 70), (knob_x, knob_y), knob_size//2 - 3)
                    # Center indicator
                    pygame.draw.circle(surface, (30, 30, 30), (knob_x, knob_y), 3)
                
                # Labels under knobs
                label_font = pygame.font.SysFont("Arial", 9)
                labels = ["VOLUME", "CHANNEL", "BRIGHTNESS"]
                for i, label in enumerate(labels):
                    label_surf = label_font.render(label, True, (200, 180, 140))
                    label_x = start_x + i * (knob_size + knob_spacing) + (knob_size - label_surf.get_width()) // 2
                    surface.blit(label_surf, (label_x, knob_y + 25))
            except Exception:
                # Font rendering might fail in some environments, skip decorations
                pass
        
        return surface

