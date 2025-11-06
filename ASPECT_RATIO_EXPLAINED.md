# Aspect Ratio: 720x480 vs True 4:3

## Your Question: Is 720x480 True 4:3?

**Answer: NO, but it's correct for NTSC video!**

### The Math

**720x480:**
- Ratio: 720 ÷ 480 = 1.5 = **3:2**
- This is NOT 4:3

**True 4:3:**
- Ratio: 4 ÷ 3 = 1.333...
- For 480 height: 480 × (4/3) = **640 pixels wide**
- Standard resolutions: 640x480, 800x600, 1024x768

### Why 720x480 Then?

**NTSC Video Standard:**
- NTSC (North American TV standard) uses 720x480
- BUT it's **not square pixels**!
- NTSC uses **rectangular pixels** with aspect ratio correction
- When displayed on a CRT TV, these pixels are stretched to appear 4:3

**The Secret:**
- 720x480 pixels with **0.9 pixel aspect ratio** = appears as 4:3 on screen
- 720 × 0.9 = 648 (close to 640, which is true 4:3)
- This is called "anamorphic" or "non-square pixel" format

## What This Means for Your Project

### For CRT TV (Composite Output)
- **Use 720x480** ✅ Correct for NTSC
- Pixels are automatically stretched by CRT
- Appears as proper 4:3 on screen
- This is what you're doing now - **CORRECT**!

### For Modern Displays (Square Pixels)
- **720x480 displayed directly = looks horizontally stretched**
- Need to apply 0.9x horizontal scaling
- OR render at true 4:3 resolution

### Current Behavior

**Your app renders 720x480 and:**
- CRT mode: Outputs 720x480 to composite → CRT stretches it → looks 4:3 ✅
- Modern mode: Scales 720x480 to fit screen → might look slightly wide

## Should We Fix This?

### Option 1: Keep 720x480 (Current)
**Pros:**
- Correct for NTSC composite output
- Matches your target hardware (CRT TV)
- No changes needed

**Cons:**
- Slightly wrong aspect on modern displays (1.5:1 instead of 1.333:1)
- Content looks a bit wide

### Option 2: Use True 4:3 (640x480)
**Pros:**
- Perfect 4:3 on all displays
- Mathematically correct

**Cons:**
- Slightly wrong for NTSC composite
- Less horizontal resolution (720 → 640)

### Option 3: Apply Aspect Correction in Modern Mode
**Pros:**
- 720x480 for CRT (correct for NTSC)
- Apply 0.9x horizontal scale in modern modes
- Best of both worlds

**Cons:**
- Slight complexity
- Need to adjust display_manager

## Recommended Solution

**For Your Use Case:**

Since your PRIMARY target is **Raspberry Pi with CRT TV via composite**, stick with **720x480**.

For modern displays, apply aspect correction:

```python
# In display_manager.py calculate_layout()
# Instead of using content_resolution directly, apply NTSC correction

ntsc_pixel_aspect = 0.9  # NTSC pixels are 10% narrower
effective_width = int(self.content_resolution[0] * ntsc_pixel_aspect)
content_aspect = effective_width / self.content_resolution[1]
# This gives us ~1.35, much closer to 4:3 (1.333)
```

This makes modern display output more accurate while keeping CRT output correct.

## Visual Comparison

### Current (720x480 as-is)
```
Aspect: 1.5:1 (3:2)
Slightly wide on modern displays
Perfect on CRT (pixels stretched by TV)
```

### With NTSC Correction
```
Aspect: ~1.35:1 (close to 4:3)
Looks correct on modern displays
Still works on CRT
```

### True 4:3 (640x480)
```
Aspect: 1.333:1 (exactly 4:3)
Perfect on modern displays
Slightly wrong for NTSC composite
Less horizontal resolution
```

## What Do You Want?

**Option A**: Keep current (720x480, optimized for CRT)
- No changes needed
- Slightly wide on modern displays
- Perfect for your primary use case

**Option B**: Apply NTSC pixel aspect correction
- I can implement in 5 minutes
- Better aspect on modern displays
- Still works on CRT

**Option C**: Switch to true 4:3 (640x480)
- Change content_resolution throughout
- Perfect 4:3 everywhere
- Slightly compromises NTSC output

I recommend **Option B** - it's the best of both worlds!

Want me to implement it?

