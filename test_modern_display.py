#!/usr/bin/env python3
"""
Quick proof-of-concept for modern display mode with CRT bezel.
Run this to see how the Magic Dingus Box would look on a modern display.
"""
import pygame
import sys

# Display modes
CRT_NATIVE = "crt_native"
MODERN_CLEAN = "modern_clean"
MODERN_BEZEL = "modern_bezel"


def generate_crt_bezel(screen_size, content_rect):
    """Generate a retro CRT TV bezel that frames the content."""
    w, h = screen_size
    surface = pygame.Surface(screen_size, pygame.SRCALPHA)
    surface.fill((0, 0, 0, 0))  # Start fully transparent
    
    # Define bezel area (everything EXCEPT content_rect)
    bezel_thickness = 60
    
    # Outer wooden frame (only outside content area)
    frame_color = (101, 67, 33)  # Dark wood
    
    # Top bar
    top_rect = pygame.Rect(0, 0, w, content_rect.top - bezel_thickness)
    if top_rect.height > 0:
        pygame.draw.rect(surface, frame_color, top_rect)
    
    # Bottom bar
    bottom_rect = pygame.Rect(0, content_rect.bottom + bezel_thickness, w, h)
    if bottom_rect.height > 0:
        pygame.draw.rect(surface, frame_color, bottom_rect)
    
    # Left bar
    left_rect = pygame.Rect(0, 0, content_rect.left - bezel_thickness, h)
    if left_rect.width > 0:
        pygame.draw.rect(surface, frame_color, left_rect)
    
    # Right bar
    right_rect = pygame.Rect(content_rect.right + bezel_thickness, 0, w, h)
    if right_rect.width > 0:
        pygame.draw.rect(surface, frame_color, right_rect)
    
    # Inner plastic bezel (dark frame around screen)
    bezel_rect = content_rect.inflate(bezel_thickness*2, bezel_thickness*2)
    pygame.draw.rect(surface, (30, 30, 30), bezel_rect, bezel_thickness)
    
    # Glass edge highlight
    glass_highlight = content_rect.inflate(4, 4)
    pygame.draw.rect(surface, (60, 60, 60), glass_highlight, 2)
    
    # Brand name (below screen if there's room)
    if content_rect.bottom + bezel_thickness + 60 < h:
        font = pygame.font.SysFont("Arial", 24, bold=True)
        brand = font.render("MAGICVISION", True, (200, 180, 140))
        brand_x = (w - brand.get_width()) // 2
        brand_y = content_rect.bottom + bezel_thickness + 15
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
    
    return surface


def calculate_content_rect(screen_size, content_size):
    """Calculate where 4:3 content fits in screen."""
    screen_w, screen_h = screen_size
    content_w, content_h = content_size
    
    # Maintain 4:3 aspect
    aspect = content_w / content_h
    
    scaled_h = screen_h - 200  # Leave room for bezel decorations
    scaled_w = int(scaled_h * aspect)
    
    if scaled_w > screen_w - 100:
        scaled_w = screen_w - 100
        scaled_h = int(scaled_w / aspect)
    
    x = (screen_w - scaled_w) // 2
    y = (screen_h - scaled_h) // 2 - 50  # Slightly higher for knobs below
    
    return pygame.Rect(x, y, scaled_w, scaled_h)


def main():
    pygame.init()
    
    # Get current display resolution
    display_info = pygame.display.Info()
    screen_w = min(display_info.current_w, 1920)  # Cap at 1080p for demo
    screen_h = min(display_info.current_h, 1080)
    
    print(f"Demo resolution: {screen_w}x{screen_h}")
    print("\nControls:")
    print("  1 = CRT Native Mode (720x480 fullscreen)")
    print("  2 = Modern Clean Mode (pillarboxed)")
    print("  3 = Modern with CRT Bezel")
    print("  Q = Quit")
    
    # Start in windowed mode for demo
    screen = pygame.display.set_mode((screen_w, screen_h))
    pygame.display.set_caption("Magic Dingus Box - Modern Display Demo")
    
    # Content surface (always 4:3)
    content_size = (720, 480)
    content_surface = pygame.Surface(content_size)
    
    # Calculate layout
    content_rect = calculate_content_rect((screen_w, screen_h), content_size)
    
    # Generate bezel
    bezel = generate_crt_bezel((screen_w, screen_h), content_rect)
    
    # Demo content
    font_large = pygame.font.SysFont("DejaVu Sans Mono", 32, bold=True)
    font_small = pygame.font.SysFont("DejaVu Sans Mono", 18)
    
    mode = MODERN_BEZEL
    clock = pygame.time.Clock()
    running = True
    
    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_q:
                    running = False
                elif event.key == pygame.K_1:
                    mode = CRT_NATIVE
                    screen = pygame.display.set_mode(content_size)
                    print("Mode: CRT Native (720x480)")
                elif event.key == pygame.K_2:
                    mode = MODERN_CLEAN
                    screen = pygame.display.set_mode((screen_w, screen_h))
                    print("Mode: Modern Clean")
                elif event.key == pygame.K_3:
                    mode = MODERN_BEZEL
                    screen = pygame.display.set_mode((screen_w, screen_h))
                    print("Mode: Modern with CRT Bezel")
        
        # Render content (this is your actual UI)
        content_surface.fill((8, 8, 12))  # Your dark background
        
        # Draw demo UI
        title = font_large.render("MAGIC DINGUS BOX", True, (255, 64, 160))
        content_surface.blit(title, (
            (content_size[0] - title.get_width()) // 2, 100
        ))
        
        info_lines = [
            "This is your 4:3 content",
            "Always rendered at 720x480",
            "Properly centered on modern displays",
            f"Current mode: {mode}",
        ]
        
        y = 200
        for line in info_lines:
            text = font_small.render(line, True, (235, 235, 220))
            content_surface.blit(text, (
                (content_size[0] - text.get_width()) // 2, y
            ))
            y += 30
        
        # Scanlines effect
        for y in range(0, content_size[1], 2):
            pygame.draw.line(content_surface, (0, 0, 0, 48), 
                           (0, y), (content_size[0], y))
        
        # Composite to screen based on mode
        if mode == CRT_NATIVE:
            screen.blit(content_surface, (0, 0))
        else:
            # Fill with black (pillarboxing)
            screen.fill((0, 0, 0))
            
            # Add bezel FIRST if requested (it has transparent center)
            if mode == MODERN_BEZEL:
                screen.blit(bezel, (0, 0))
            
            # Scale and center content ON TOP of bezel
            scaled_content = pygame.transform.scale(
                content_surface,
                (content_rect.width, content_rect.height)
            )
            screen.blit(scaled_content, content_rect)
        
        pygame.display.flip()
        clock.tick(60)
    
    pygame.quit()


if __name__ == "__main__":
    main()

