# CRT Shader & Realistic Bezel Options

## Your Questions

1. **Can we add CRT shaders like CRT-Royale for modern displays?**
   - **Answer**: Yes, but requires OpenGL integration with pygame

2. **Can we use RetroArch's realistic CRT bezels instead of our procedural one?**
   - **Answer**: YES! This is the easiest and best option

3. **Is RetroArch open source?**
   - **Answer**: Yes! GPL license, we can use their assets

## Option 1: Use RetroArch Bezel Images (RECOMMENDED - Easy)

### What This Gives You

- Professional, photorealistic CRT TV bezels
- Hundreds of pre-made designs (Sony Trinitron, RCA, etc.)
- Easy to implement (just load PNG files)
- Zero performance impact
- Works immediately with your current code

### How to Implement

**Step 1: Download RetroArch Bezels**

```bash
cd ~/Downloads
git clone https://github.com/libretro/overlay-borders.git

# Browse the collection
cd overlay-borders/16x9/
ls -la
# You'll see: crt-1.png, crt-2.png, trinitron-*.png, etc.
```

**Step 2: Add to Your Project**

```bash
cd /Users/alexanderchaney/Documents/Projects/magic_dingus_box
mkdir -p assets/bezels

# Copy your favorite bezels
cp ~/Downloads/overlay-borders/16x9/crt-*.png assets/bezels/
cp ~/Downloads/overlay-borders/4x3/*.png assets/bezels/
```

**Step 3: Update DisplayManager to Load Image Bezels**

Modify `display_manager.py`:

```python
def load_bezel_from_file(self, bezel_path: str) -> pygame.Surface:
    """Load a bezel image from file.
    
    Args:
        bezel_path: Path to PNG bezel file
        
    Returns:
        Bezel surface scaled to target resolution
    """
    try:
        bezel_img = pygame.image.load(bezel_path)
        # Scale to screen size
        scaled_bezel = pygame.transform.scale(
            bezel_img,
            self.target_resolution
        )
        return scaled_bezel
    except Exception as e:
        self._log.warning(f"Failed to load bezel {bezel_path}: {e}")
        # Fall back to procedural bezel
        return self.generate_bezel()
```

**Step 4: Update Config to Select Bezel**

```python
# In config.py
self.crt_bezel_image = os.getenv("MAGIC_BEZEL_IMAGE", "assets/bezels/crt-1.png")
```

**Step 5: Update Settings Menu**

Add bezel selection to Display Settings:
- "Bezel Style: CRT-1" → cycles through available bezel images
- Lists all .png files in assets/bezels/

### Comparison

**Our Procedural Bezel:**
```
┌──────────────────────┐
│ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓ │ Simple
│ ▓┌────────────────┐▓ │ Functional
│ ▓│   Content      │▓ │ But basic
│ ▓└────────────────┘▓ │
│ ▓ [O] MAGIC [O]   ▓ │
└──────────────────────┘
```

**RetroArch Bezel Images:**
```
╔══════════════════════╗
║░░░░░░░░░░░░░░░░░░░░░░║ Photo-realistic
║░┌──────────────────┐░║ Curved screen
║░│   Content        │░║ Screen glare
║░│   Reflections    │░║ Realistic plastic
║░└──────────────────┘░║ Brand logos
║░ ⚫ SONY ⚫ 📺      ░║ Authentic details
╚══════════════════════╝
```

### Pros of Using RetroArch Bezels

✅ Professional quality  
✅ Photorealistic  
✅ Many styles to choose from  
✅ Easy to implement (just load PNG)  
✅ Zero performance impact  
✅ Community-maintained  
✅ No legal issues (GPL/public domain)

### Cons

⚠️ Larger file sizes (bezels are ~1-5MB each)  
⚠️ Need to bundle with app or download separately  
⚠️ Less customizable than procedural

---

## Option 2: CRT Shader Effects (ADVANCED - Complex)

### What This Gives You

- Scanlines, shadow mask, phosphor glow
- Screen curvature distortion
- Color bleeding and bloom
- Authentic CRT look applied to actual content
- Same effects RetroArch uses

### Complexity Level: HIGH

This requires:
1. **OpenGL with pygame** (pygame-ce with OpenGL support)
2. **GLSL shader compilation** at runtime
3. **Render-to-texture pipeline**
4. **Shader uniforms/parameters**

### Implementation Approach

**Architecture:**
```
Content (720x480)
    ↓
Render to OpenGL texture
    ↓
Apply CRT shader (GLSL)
    ↓
Render to screen
```

**Required Changes:**

1. **Switch to OpenGL rendering**
```python
# Instead of normal pygame
screen = pygame.display.set_mode(resolution, pygame.OPENGL | pygame.DOUBLEBUF)

# Use ModernGL for shader management
import moderngl
ctx = moderngl.create_context()
```

2. **Load GLSL Shaders**
```python
# Port shaders from RetroArch slang-shaders repo
# Example: crt-royale shader
vertex_shader = """
#version 330
in vec2 in_vert;
in vec2 in_texcoord;
out vec2 v_texcoord;
void main() {
    v_texcoord = in_texcoord;
    gl_Position = vec4(in_vert, 0.0, 1.0);
}
"""

fragment_shader = """
#version 330
uniform sampler2D texture0;
in vec2 v_texcoord;
out vec4 f_color;

// CRT shader code here (hundreds of lines)
void main() {
    // Scanlines, curvature, phosphor glow, etc.
}
"""
```

3. **Render Pipeline**
```python
# Every frame:
# 1. Render UI to pygame surface
# 2. Upload surface to OpenGL texture
# 3. Apply shader
# 4. Render quad with shader to screen
```

### Pros of Shader Approach

✅ Authentic CRT effects  
✅ Applies to all content (videos + UI)  
✅ Can use RetroArch's exact shaders  
✅ Highly customizable parameters

### Cons

⚠️ **Very complex** (100+ hours of work)  
⚠️ Requires OpenGL expertise  
⚠️ pygame doesn't natively support this well  
⚠️ Performance overhead (5-15%)  
⚠️ Risk of breaking existing functionality  
⚠️ Harder to maintain  
⚠️ May not work on all Pi hardware configurations

---

## RECOMMENDED APPROACH: Hybrid Solution

Use the best of both worlds with minimal complexity:

### Phase 1: Use RetroArch Bezel Images (NOW)

Replace procedural bezel with real images:

1. **Download bezels** from RetroArch
2. **Add selection menu** in Display Settings
3. **Load and scale** PNG bezels
4. **10-20 minutes of work**, huge visual upgrade

### Phase 2: Simple Pygame CRT Effects (LATER)

Add pygame-based effects (no OpenGL needed):

1. **Enhanced scanlines** (already have basic version)
2. **Phosphor glow** (blur effect around bright pixels)
3. **Screen curvature** (pygame.transform.warp - available in pygame 2.0+)
4. **Color adjustments** (warmth, contrast)

These can be applied as **post-processing** to your content surface before scaling:

```python
def apply_crt_effects(surface, enable_scanlines, enable_glow, enable_curve):
    # Enhanced scanlines
    if enable_scanlines:
        apply_scanlines_advanced(surface)
    
    # Phosphor glow (blur bright areas)
    if enable_glow:
        apply_phosphor_glow(surface)
    
    # Gentle curvature (pygame.transform)
    if enable_curve:
        surface = apply_barrel_distortion(surface)
    
    return surface
```

**Effort**: 2-4 hours  
**Impact**: Good CRT feel without OpenGL complexity

### Phase 3: Optional OpenGL Shaders (FUTURE)

If you really want CRT-Royale level quality:
- Research pygame-ce with OpenGL
- Port specific shaders
- Create toggle in settings
- **100+ hours effort**, very advanced

---

## PRACTICAL IMPLEMENTATION PLAN

### Immediate (Next 30 Minutes): RetroArch Bezel Images

**1. Download Bezels**
```bash
# Clone the repo
cd ~/Downloads
git clone --depth 1 https://github.com/libretro/overlay-borders.git

# Copy CRT bezels to your project
cd /Users/alexanderchaney/Documents/Projects/magic_dingus_box
mkdir -p assets/bezels

# Get the good ones
cp ~/Downloads/overlay-borders/16x9/crt-1.png assets/bezels/
cp ~/Downloads/overlay-borders/16x9/crt-2.png assets/bezels/
cp ~/Downloads/overlay-borders/16x9/realistic-crt-1.png assets/bezels/
```

**2. Modify DisplayManager**

Add to `display_manager.py`:
```python
def load_bezel_image(self, bezel_path: Path) -> Optional[pygame.Surface]:
    """Load and scale a bezel image file."""
    if not bezel_path.exists():
        return None
    try:
        bezel = pygame.image.load(str(bezel_path))
        return pygame.transform.smoothscale(bezel, self.target_resolution)
    except:
        return None
```

**3. Update main.py**

```python
# Generate or load bezel
bezel = None
if display_mode == DisplayMode.MODERN_WITH_BEZEL:
    bezel_path = Path("assets/bezels/crt-1.png")
    if bezel_path.exists():
        bezel = display_mgr.load_bezel_image(bezel_path)
        log.info("Loaded bezel from assets")
    else:
        bezel = display_mgr.generate_bezel()
        log.info("Generated procedural bezel")
```

**4. Add Bezel Selector to Settings**

Update Display submenu:
```python
MenuItem("Bezel: CRT-1", sublabel="Style selection")
# Cycles through: CRT-1, CRT-2, Realistic, Procedural
```

### Near Term (Next Week): Enhanced Pygame Effects

Add these as toggleable options:

**1. Advanced Scanlines**
```python
def apply_enhanced_scanlines(surface, intensity=0.3):
    """Better scanlines with adjustable intensity."""
    w, h = surface.get_size()
    overlay = pygame.Surface((w, h), pygame.SRCALPHA)
    for y in range(0, h, 2):
        alpha = int(255 * intensity)
        pygame.draw.line(overlay, (0, 0, 0, alpha), (0, y), (w, y))
    surface.blit(overlay, (0, 0))
```

**2. Phosphor Glow** (simple version)
```python
def apply_phosphor_glow(surface):
    """Subtle glow around bright pixels."""
    # This is a simplified approximation
    # Real phosphor glow would need pixel analysis
    pass  # Placeholder for now
```

**3. Screen Curvature** (pygame 2.0+ has basic support)
```python
# Could use pygame.transform or manual pixel manipulation
# Creates gentle barrel distortion like CRT screens
```

### Long Term (If You Want): Full OpenGL Shaders

This would be a major rewrite but would give you CRT-Royale quality.

**Requirements:**
- Switch from pygame to pygame-ce with OpenGL
- Use ModernGL or PyOpenGL
- Port GLSL shaders from libretro/slang-shaders
- Significant testing needed

**Estimated effort**: 100-200 hours
**Realistic?**: Only if you want to make this a major feature

---

## MY RECOMMENDATION

### Step 1: Use RetroArch Bezel Images (DO THIS FIRST)

This gives you **80% of the visual quality** with **2% of the effort**:

```bash
# Takes 15 minutes total
1. Download RetroArch bezels
2. Add to assets/ folder
3. Update DisplayManager to load PNG instead of generate
4. Add bezel selector to settings menu
```

**Result**: Professional-looking CRT TV frames that look amazing!

### Step 2: Add Simple Pygame CRT Effects

Add toggleable effects in Display Settings:
- ✅ Enhanced scanlines (already partially done)
- ✅ Screen tint/warmth
- ✅ Vignette darkening (already done)
- ✅ Gentle blur for phosphor simulation

**Effort**: 2-4 hours  
**Result**: Convincing CRT feel without complexity

### Step 3: Skip Full Shader System (For Now)

Full GLSL shader integration is:
- Very complex
- High maintenance burden
- Overkill for this project
- Can always add later if needed

---

## IMMEDIATE ACTION: Let Me Implement RetroArch Bezels

I can implement professional bezel support right now if you want! Here's what I'll do:

**1. Download Sample Bezels** (requires network access)
- Get 3-4 high-quality CRT bezels from RetroArch
- Add to `assets/bezels/` directory

**2. Update DisplayManager**
- Add `load_bezel_image()` method
- Prefer image bezels over procedural

**3. Add Bezel Selector**
- Settings → Display → "Bezel Style: CRT-1"
- Cycles through available bezel images
- Falls back to procedural if images not found

**Time**: 20-30 minutes  
**Benefit**: Professional appearance immediately

### Would You Like Me To:

**Option A**: Implement RetroArch bezel images now (recommended)
**Option B**: Just document how you can do it yourself later
**Option C**: Research OpenGL shader integration more deeply first

For Option A, I'll need network access to download the bezel repository. Let me know!

---

## Technical Deep Dive: Why OpenGL is Complex

### Current Architecture (Simple)
```
pygame.Surface (CPU) → blit → Screen
                     ↓
             All rendering in software
             Easy to understand
             Works everywhere
```

### OpenGL Architecture (Complex)
```
pygame.Surface → Upload to GPU → Texture
                                    ↓
                            Apply GLSL shader
                                    ↓
                            Render to framebuffer
                                    ↓
                                Screen
```

**Challenges:**
- mpv embeds into window using `wid` - conflicts with OpenGL
- All UI code would need OpenGL rendering
- Shader debugging is difficult
- Performance tuning required
- Platform compatibility issues

### Hybrid Approach (Possible Middle Ground)

Keep pygame for UI, use OpenGL only for video layer:
```
1. Render UI to pygame surface (current approach)
2. Convert mpv video output to OpenGL texture
3. Apply CRT shader to video only
4. Composite UI over shaded video
```

**Effort**: 40-60 hours  
**Realistic?**: Maybe, but still quite complex

---

## RetroArch Resources

### Bezel Assets
- **Repository**: https://github.com/libretro/overlay-borders
- **License**: Various (check individual bezels, most are free to use)
- **Formats**: PNG with alpha channel
- **Resolutions**: Pre-made for 1080p, 4K, etc.

### Shader Assets
- **Repository**: https://github.com/libretro/slang-shaders
- **License**: Varies (GPL, public domain, MIT)
- **Format**: GLSL (would need significant porting)
- **Examples**: crt-royale, crt-geom, crt-easymode, etc.

### Documentation
- **RetroArch Docs**: https://docs.libretro.com/
- **Shader Guide**: https://docs.libretro.com/shader/
- **Bezel Guide**: https://docs.libretro.com/guides/libretro-overlays/

---

## Simplified CRT Effects (No OpenGL)

Here's what we CAN do easily with pure pygame:

### Effect 1: Enhanced Scanlines (EASY)
```python
# Adjustable intensity, RGB separation
for y in range(0, height, 2):
    # Vary intensity based on content brightness
    pygame.draw.line(surface, (0, 0, 0, 80), (0, y), (width, y))
```

### Effect 2: RGB Chromatic Aberration (MEDIUM)
```python
# Slight RGB color separation like CRT phosphors
# Split into R, G, B channels
# Offset each channel by 1-2 pixels
# Recombine
```

### Effect 3: Screen Bloom/Glow (MEDIUM)
```python
# Pygame doesn't have blur, but we can approximate:
# 1. Find bright pixels
# 2. Draw subtle colored halos around them
# 3. Simulates phosphor glow
```

### Effect 4: Color Temperature (EASY)
```python
# Apply color tint overlay
warm_tint = pygame.Surface(size, pygame.SRCALPHA)
warm_tint.fill((20, 10, 0, 30))  # Slight yellow/orange
surface.blit(warm_tint, (0, 0))
```

### Effect 5: Curvature (HARD but doable)
```python
# Barrel distortion using pixel array manipulation
# Or wait for pygame 2.5+ with better transform support
```

These combine to create a convincing CRT feel without OpenGL!

---

## BOTTOM LINE

### What I Recommend RIGHT NOW:

1. **✅ Use RetroArch bezel images** - Easy, huge visual upgrade
2. **✅ Add enhanced scanlines** - Already mostly done
3. **✅ Add color warmth toggle** - 5 minutes of work
4. **❌ Skip full shader system** - Not worth the complexity

### Next Steps:

If you want professional-looking bezels:
1. Switch to agent mode
2. I'll download RetroArch bezels
3. Update your code to use them
4. Add bezel selection to settings menu
5. **~30 minutes, massive improvement**

Let me know if you want me to implement the RetroArch bezel integration! 🎮✨

