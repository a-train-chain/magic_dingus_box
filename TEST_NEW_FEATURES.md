# Test Your New Features - Quick Guide

## 🚀 Quick Test Sequence

### Step 1: Start the App

```bash
cd /Users/alexanderchaney/Documents/Projects/magic_dingus_box
source .venv/bin/activate
./scripts/run_dev.sh
```

### Step 2: Test Professional Bezels

1. App starts in **CRT Native mode** (720x480 window)
2. Press **4** (quick press) → Settings menu opens
3. Navigate to **"Display"** → Press Enter
4. Select **"Mode: CRT Native"** → Press Enter
5. Mode cycles to **"Modern (Clean)"**
6. **Quit** (press Q)
7. **Restart**: `./scripts/run_dev.sh`
8. Window now larger (auto-detected your screen size)
9. Content centered with black bars ✅

**Now try the bezel:**

10. Press **4** → Display
11. Select **"Bezel: OFF"** → Press Enter (toggles to ON)
12. Mode auto-switches to **"Modern (Bezel)"**
13. **Quit** and **restart**
14. **You should see a professional CRT TV frame!** 🖼️

### Step 3: Try Different Bezels

1. Press **4** → Display
2. Select **"Bezel Style: Retro TV 1"** → Press Enter
3. Cycles to **"NES TV"**
4. **Quit** and **restart**
5. See NES-themed TV bezel!
6. **Repeat** to try:
   - N64 TV
   - PlayStation TV
   - Vintage TV
   - Modern TV
   - Retro TV 2

### Step 4: Test CRT Effects (No Restart Needed!)

1. **Press 4** → Display
2. Select **"Scanlines: OFF"** → Press Enter
3. Cycles to **"Light (15%)"** → **Effect applies immediately!**
4. Press Enter again → **"Medium (30%)"**
5. Press Enter again → **"Heavy (50%)"**
6. See the scanlines get stronger each time!

**Try Color Warmth:**

7. Select **"Color Warmth: OFF"** → Press Enter
8. Cycles to **"Cool (25%)"** → **Warm tint appears!**
9. Keep cycling: Neutral (50%) → Warm (75%) → OFF

**Try Additional Effects:**

10. Toggle **"Screen Bloom: OFF"** → becomes **ON**
11. See subtle highlights brighten!
12. Toggle **"Phosphor Glow: OFF"** → becomes **ON**
13. See colored glow around edges!

### Step 5: Test Game Launching

1. **Press 4** → Video Games → Browse Games
2. Select **"NES Classics"** → Press Enter
3. See individual game list
4. Select **"Super Mario Bros. 3"** → Press Enter
5. Game launches in fullscreen! 🎮
6. **Play for a bit**
7. Press **F1** (or Start+Select) → RetroArch menu
8. Select **"Close Content"**
9. **Returns to Magic Dingus Box UI!** ✅

### Step 6: Test All Display Modes

**CRT Native:**
- Mode: CRT Native
- Effects: Try scanlines in this mode
- Window: 720x480

**Modern Clean:**
- Mode: Modern (Clean)
- Window: Your screen size
- Content: Centered with black bars
- No bezel

**Modern Bezel:**
- Mode: Modern (Bezel)
- Bezel Style: N64 TV (or any)
- Effects: Scanlines Medium, Warmth Neutral
- **Looks amazing!**

## 🎯 What to Verify

### ✅ Bezels
- [ ] Bezel images load and display correctly
- [ ] Content is visible through bezel (not blocked)
- [ ] Can cycle through all 7+ bezel styles
- [ ] Bezels scale to fit screen
- [ ] Falls back to procedural if image missing

### ✅ CRT Effects
- [ ] Scanlines visible and adjustable
- [ ] Color warmth creates warm tint
- [ ] Screen bloom brightens highlights
- [ ] Phosphor glow adds edge coloring
- [ ] Effects apply immediately (no restart)
- [ ] Effects work in all display modes

### ✅ Settings
- [ ] Display settings menu has 9 options
- [ ] Can toggle all effects
- [ ] Settings save automatically
- [ ] Settings load on restart
- [ ] Menu labels update to show current state

### ✅ Performance
- [ ] App runs at 60 FPS
- [ ] No lag or stuttering
- [ ] All effects enabled: still smooth
- [ ] Videos play smoothly
- [ ] Games launch quickly

### ✅ Navigation
- [ ] Button 4 opens/closes settings
- [ ] Back buttons work at all levels
- [ ] Can navigate entire menu structure
- [ ] Settings menu text fits (no overflow)
- [ ] Startup animation text visible

## 📊 Expected Results

### Startup
- Smooth "Magic Dingus Box" animation
- Text clearly visible (not blocked)
- Transitions to playlist menu

### Display Settings Menu (Modern Bezel Mode)
```
DISPLAY
├─ Mode: Modern (Bezel)
├─ Resolution: Auto (detected)
├─ Bezel: ON
├─ Bezel Style: Retro TV 1
├─ Scanlines: Medium (30%)
├─ Color Warmth: Neutral (50%)
├─ Screen Bloom: OFF
├─ Phosphor Glow: OFF
└─ Back
```

### Visual with All Effects ON
- Content in center with CRT TV frame
- Horizontal scanlines visible
- Warm orange tint overall
- Bright areas have subtle glow
- Colored glow around screen edges
- Looks like viewing through vintage CRT TV!

## 🐛 Known Issues / Limitations

### Bezel Changes Require Restart
- Changing bezel style needs app restart
- This is intentional (bezel loaded at startup)
- CRT effects apply immediately though!

### Settings Menu Text
- Long labels are truncated if too wide
- Most should fit in 1/3 screen width
- Shortened labels used where needed

### RetroArch Integration
- Can't overlay Magic Dingus Box UI during gameplay
- Use RetroArch's menu (F1 or Start+Select)
- This is a technical limitation, not a bug

## 💡 Tips

### Best Look for Modern Display
1. Mode: Modern (Bezel)
2. Bezel: Match your content (NES TV for NES games, etc.)
3. Scanlines: Medium
4. Warmth: Neutral
5. Bloom: ON
6. Glow: OFF

### Best Performance
1. Mode: CRT Native or Modern (Clean)
2. Scanlines: Light or OFF
3. All other effects: OFF

### For Demos/Screenshots
1. Mode: Modern (Bezel)
2. Bezel: Retro TV 1 or NES TV
3. All effects: ON
4. Looks incredible in screenshots!

## 🎊 Enjoy!

You now have a fully-featured retro media and gaming kiosk with professional-grade visuals!

**Next**: Deploy to your Raspberry Pi and enjoy on a real CRT TV! 🎮📺✨

