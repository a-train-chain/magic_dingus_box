# Modern Display Support Design

## Feature Overview

Add support for modern displays while maintaining authentic CRT experience.

### Display Modes

#### 1. CRT Native (Default)
- **Resolution**: 720x480 (4:3)
- **Fullscreen**: Yes
- **Framing**: None
- **Use Case**: Raspberry Pi with NTSC composite output to CRT TV

#### 2. Modern Display with CRT Bezel
- **Content Resolution**: 720x480 (4:3) - rendered to offscreen surface
- **Window Resolution**: Matches display (1920x1080, etc.)
- **Pillarboxing**: Black bars on sides to maintain 4:3
- **CRT Bezel**: Optional decorative frame around content
- **Use Case**: Development on laptop, or Pi connected to HDMI TV

#### 3. Modern Display without Bezel
- **Content Resolution**: 720x480 (4:3)
- **Window Resolution**: Matches display
- **Pillarboxing**: Black bars on sides
- **CRT Bezel**: None (clean look)
- **Use Case**: Minimal modern presentation

## Implementation Plan

### 1. Display Manager Module

Create `magic_dingus_box/display/display_manager.py`:

```python
from enum import Enum
import pygame

class DisplayMode(Enum):
    CRT_NATIVE = "crt_native"           # 720x480 fullscreen, no bezel
    MODERN_WITH_BEZEL = "modern_bezel"  # Pillarboxed with CRT frame
    MODERN_CLEAN = "modern_clean"       # Pillarboxed, no frame

class DisplayManager:
    def __init__(self, mode, target_resolution=(1920, 1080)):
        self.mode = mode
        self.content_resolution = (720, 480)  # Always 4:3
        self.target_resolution = target_resolution
        
        # Create offscreen surface for 4:3 content
        self.content_surface = pygame.Surface(self.content_resolution)
        
        # Calculate pillarbox dimensions
        self.calculate_layout()
        
    def calculate_layout(self):
        """Calculate where 4:3 content fits in modern display."""
        target_w, target_h = self.target_resolution
        content_w, content_h = self.content_resolution
        
        # Maintain 4:3 aspect ratio
        content_aspect = content_w / content_h
        
        # Fit height, calculate width
        scaled_h = target_h
        scaled_w = int(scaled_h * content_aspect)
        
        if scaled_w > target_w:
            # Width constrained instead
            scaled_w = target_w
            scaled_h = int(scaled_w / content_aspect)
        
        # Center the content
        self.content_rect = pygame.Rect(
            (target_w - scaled_w) // 2,
            (target_h - scaled_h) // 2,
            scaled_w,
            scaled_h
        )
        
        self.pillarbox_left = pygame.Rect(0, 0, self.content_rect.x, target_h)
        self.pillarbox_right = pygame.Rect(
            self.content_rect.right, 0,
            target_w - self.content_rect.right, target_h
        )
    
    def get_render_surface(self):
        """Get the surface to render content to."""
        return self.content_surface
    
    def present(self, screen, bezel_image=None):
        """Composite content to screen with proper framing."""
        if self.mode == DisplayMode.CRT_NATIVE:
            # Direct blit, no processing
            screen.blit(self.content_surface, (0, 0))
        else:
            # Fill screen with black (pillarboxing)
            screen.fill((0, 0, 0))
            
            # Scale and blit 4:3 content to center
            scaled_content = pygame.transform.scale(
                self.content_surface,
                (self.content_rect.width, self.content_rect.height)
            )
            screen.blit(scaled_content, self.content_rect)
            
            # Optionally draw CRT bezel
            if self.mode == DisplayMode.MODERN_WITH_BEZEL and bezel_image:
                self._draw_bezel(screen, bezel_image)
    
    def _draw_bezel(self, screen, bezel_image):
        """Draw CRT TV bezel around content."""
        # Scale bezel to fit screen
        scaled_bezel = pygame.transform.scale(
            bezel_image,
            screen.get_size()
        )
        # Bezel has transparent center, content shows through
        screen.blit(scaled_bezel, (0, 0))
```

### 2. CRT Bezel Asset

Two options for the bezel:

#### Option A: Pre-made Image
- Create/download a PNG image of a CRT TV frame
- Transparent center (where content shows)
- Decorative surround (wood grain, buttons, etc.)
- File: `assets/crt_bezel_1920x1080.png`

#### Option B: Procedurally Generated
```python
def generate_crt_bezel(screen_size, content_rect):
    """Generate a simple CRT bezel programmatically."""
    w, h = screen_size
    surface = pygame.Surface(screen_size, pygame.SRCALPHA)
    
    # Outer frame (wood grain brown)
    pygame.draw.rect(surface, (101, 67, 33), (0, 0, w, h))
    
    # Inner bezel (dark gray)
    bezel_thickness = 40
    inner = content_rect.inflate(bezel_thickness*2, bezel_thickness*2)
    pygame.draw.rect(surface, (40, 40, 40), inner)
    
    # Glass reflection effect
    glass = content_rect.inflate(10, 10)
    glass_surf = pygame.Surface(glass.size, pygame.SRCALPHA)
    glass_surf.fill((255, 255, 255, 30))  # Subtle white overlay
    surface.blit(glass_surf, glass.topleft)
    
    # Brand name "MAGICVISION" 
    # Control knobs
    # etc.
    
    return surface
```

### 3. Config Updates

Add to `config.py`:

```python
class AppConfig:
    def __init__(self):
        # ... existing config ...
        
        # Display mode settings
        self.display_mode = os.getenv("MAGIC_DISPLAY_MODE", "crt_native")
        self.show_crt_bezel = os.getenv("MAGIC_SHOW_BEZEL", "0") == "1"
        
        # Modern display settings
        self.modern_resolution = self._parse_resolution(
            os.getenv("MAGIC_MODERN_RES", "1920x1080")
        )
        
    def _parse_resolution(self, res_string):
        """Parse '1920x1080' to (1920, 1080)."""
        parts = res_string.split('x')
        return (int(parts[0]), int(parts[1]))
```

### 4. Settings Menu Integration

Update `settings_menu.py` to add Display submenu options:

```python
def _build_display_submenu(self):
    """Build display settings submenu."""
    return [
        MenuItem("Display Mode: CRT Native", sublabel="720x480 fullscreen"),
        MenuItem("Display Mode: Modern (Bezel)", sublabel="Centered with CRT frame"),
        MenuItem("Display Mode: Modern (Clean)", sublabel="Centered, minimal"),
        MenuItem("Resolution: 1920x1080", sublabel="For modern displays"),
        MenuItem("Scanlines: ON", sublabel="Toggle CRT scanline effect"),
        MenuItem("Back to Main Menu", MenuSection.BACK),
    ]
```

### 5. Main Loop Updates

Modify `main.py`:

```python
from .display.display_manager import DisplayManager, DisplayMode

def run():
    config = AppConfig()
    
    # Determine display mode
    if config.display_mode == "crt_native":
        mode = DisplayMode.CRT_NATIVE
        screen = pygame.display.set_mode(
            (config.screen_width, config.screen_height),
            pygame.FULLSCREEN if config.fullscreen else 0
        )
    else:
        mode = (DisplayMode.MODERN_WITH_BEZEL if config.show_crt_bezel 
                else DisplayMode.MODERN_CLEAN)
        screen = pygame.display.set_mode(
            config.modern_resolution,
            pygame.FULLSCREEN if config.fullscreen else 0
        )
    
    # Create display manager
    display_mgr = DisplayManager(mode, screen.get_size())
    
    # Load bezel if needed
    bezel = None
    if mode == DisplayMode.MODERN_WITH_BEZEL:
        try:
            bezel = pygame.image.load("assets/crt_bezel.png")
        except:
            bezel = generate_crt_bezel(screen.get_size(), display_mgr.content_rect)
    
    # Main loop
    while running:
        # ... event handling ...
        
        # Get the surface to render to (either content or screen directly)
        render_target = display_mgr.get_render_surface()
        
        # All rendering goes to render_target instead of screen
        renderer.render(..., render_target)
        settings_renderer.render(render_target, ...)
        
        # Composite to actual screen with framing
        display_mgr.present(screen, bezel)
        
        pygame.display.flip()
```

## Visual Examples

### CRT Native Mode (Default)
```
┌────────────────────────────────────┐
│                                    │
│    Full 720x480 content            │
│    No borders, no bezel            │
│    Direct to CRT                   │
│                                    │
└────────────────────────────────────┘
```

### Modern Display with Bezel
```
┌──────────────────────────────────────────┐
│ ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │ Black
│ ░░┌──────────────────────────────┐░░░░░ │ Pillarbox
│ ░░│ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓ │░░░░░ │
│ ░░│ ▓┌──────────────────────┐▓▓▓ │░░░░░ │ CRT Bezel
│ ░░│ ▓│   720x480 Content    │▓▓▓ │░░░░░ │ (Wood/Plastic)
│ ░░│ ▓│   Your UI here        │▓▓▓ │░░░░░ │
│ ░░│ ▓│   Maintains 4:3       │▓▓▓ │░░░░░ │
│ ░░│ ▓└──────────────────────┘▓▓▓ │░░░░░ │
│ ░░│ ▓▓▓ [O] [O] Brand ▓▓▓▓▓▓▓▓▓▓ │░░░░░ │
│ ░░└──────────────────────────────┘░░░░░ │
│ ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │
└──────────────────────────────────────────┘
```

### Modern Display Clean
```
┌──────────────────────────────────────────┐
│ ████████████████████████████████████████ │ Black
│ ████┌──────────────────────┐████████████ │ Pillarbox
│ ████│   720x480 Content    │████████████ │
│ ████│   Your UI here        │████████████ │
│ ████│   Maintains 4:3       │████████████ │
│ ████│   No decorative frame │████████████ │
│ ████└──────────────────────┘████████████ │
│ ████████████████████████████████████████ │
└──────────────────────────────────────────┘
```

## Benefits

### For CRT Users
- ✅ No change - works exactly as before
- ✅ Full 720x480 utilization
- ✅ Authentic retro experience

### For Modern Display Users
- ✅ Proper 4:3 aspect ratio maintained
- ✅ No stretching/distortion
- ✅ Optional retro aesthetic with bezel
- ✅ Works on any resolution (1080p, 4K, etc.)

### For Development
- ✅ Test on laptop without CRT TV
- ✅ Screenshots look professional
- ✅ Easier to demo the project

## Implementation Priority

### Phase 1: Core Infrastructure
1. Create DisplayManager class
2. Modify main.py to render to offscreen surface
3. Add pillarboxing support
4. Test with CRT_NATIVE and MODERN_CLEAN modes

### Phase 2: Bezel Graphics
1. Design/find CRT bezel image
2. Implement bezel rendering
3. Add MODERN_WITH_BEZEL mode

### Phase 3: Settings Integration
1. Add display settings to settings menu
2. Add resolution selector
3. Add bezel toggle
4. Save/load display preferences

### Phase 4: Polish
1. Add scanline effect that works in both modes
2. Add CRT screen curvature shader (optional)
3. Add color bleeding/phosphor glow effects (optional)

## Testing

### Test Matrix

| Display Type | Resolution | Mode | Expected Result |
|--------------|-----------|------|-----------------|
| CRT TV | 720x480 | CRT_NATIVE | Fullscreen, no borders |
| Laptop | 1920x1080 | MODERN_CLEAN | Centered, black bars |
| Laptop | 1920x1080 | MODERN_BEZEL | Centered, CRT frame |
| 4K TV | 3840x2160 | MODERN_CLEAN | Centered, black bars |
| 4K TV | 3840x2160 | MODERN_BEZEL | Centered, CRT frame |

## Future Enhancements

- Multiple bezel designs (Sony Trinitron, RCA, etc.)
- Dynamic bezel selection based on "TV brand" setting
- Animated CRT power-on effect
- Screen burn-in simulation
- Authentic color temperature controls

