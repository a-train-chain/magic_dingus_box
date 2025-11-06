# Testing Display Modes

## Quick Test Plan

### Test 1: CRT Native Mode (Default)

**Setup:**
```bash
# Delete settings to start fresh
rm dev_data/settings.json

# Start app
./scripts/run_dev.sh
```

**Expected Behavior:**
- ✅ Window opens at 720x480
- ✅ Content fills entire window
- ✅ No borders, no framing
- ✅ Playlists visible and navigation works
- ✅ Videos play correctly
- ✅ Games launch correctly

**Pass/Fail Criteria:**
- Window size is exactly 720x480
- No black bars visible
- Everything works as it did before

---

### Test 2: Switch to Modern Clean Mode

**Steps:**
1. With app running in CRT Native mode
2. Press **Button 4** (quick press)
3. Navigate to "Display Settings"
4. Press Enter
5. Select "Display Mode: CRT Native"
6. Press Enter (cycles to "Modern (Clean)")
7. Quit app (press Q)
8. Restart: `./scripts/run_dev.sh`

**Expected Behavior:**
- ✅ Window opens at larger resolution (1920x1080 by default)
- ✅ 720x480 content centered in window
- ✅ Black bars on left and right (pillarboxing)
- ✅ No CRT frame/bezel visible
- ✅ Content maintains 4:3 aspect (not stretched)
- ✅ All UI elements visible and functional

**Verification:**
```bash
# Check settings file was created
cat dev_data/settings.json
# Should show: {"display_mode": "modern_clean", ...}
```

**Pass/Fail Criteria:**
- Content is centered
- Black bars present on sides
- No stretching or distortion
- 4:3 aspect ratio maintained

---

### Test 3: Modern Mode with CRT Bezel

**Steps:**
1. In Modern Clean mode
2. Press **Button 4**
3. Navigate to Display Settings
4. Select "Display Mode: Modern (Clean)"
5. Press Enter (cycles to "Modern (Bezel)")
6. Restart app

**Expected Behavior:**
- ✅ Window opens at 1920x1080 (or your configured resolution)
- ✅ 720x480 content centered
- ✅ Wood-grain CRT TV frame around content
- ✅ Dark plastic bezel visible
- ✅ "MAGICVISION" brand name below screen
- ✅ Decorative control knobs visible
- ✅ Labels under knobs (VOLUME, CHANNEL, BRIGHTNESS)
- ✅ Content visible and not blocked by bezel

**Pass/Fail Criteria:**
- Bezel renders correctly
- Content is clearly visible through bezel
- Bezel doesn't block any UI elements
- Looks like viewing through a vintage CRT TV

---

### Test 4: Resolution Cycling

**Steps:**
1. In Modern mode (Clean or Bezel)
2. Open Display Settings
3. Select "Resolution: 1920x1080"
4. Press Enter multiple times to cycle:
   - 1920x1080 → 2560x1440 → 3840x2160 → (back to 1920x1080)
5. Restart after each change

**Expected Behavior:**
- ✅ Window size changes to selected resolution
- ✅ Content always stays 4:3
- ✅ Content scales proportionally
- ✅ Pillarboxing adjusts to fit
- ✅ Bezel (if enabled) scales to fit new resolution

**Pass/Fail Criteria:**
- Each resolution produces correct window size
- Content never stretches
- Always centered
- Always 4:3 aspect ratio

---

### Test 5: Bezel Toggle

**Steps:**
1. In Modern mode
2. Open Display Settings
3. Toggle "CRT Bezel: ON/OFF"
4. Restart

**Expected Behavior:**
- ✅ ON: Shows bezel frame
- ✅ OFF: No bezel, clean pillarbox
- ✅ Mode automatically switches between modern_clean and modern_bezel
- ✅ Setting persists

---

### Test 6: All Features in All Modes

Run through this checklist in **each display mode**:

**CRT Native:**
- [ ] Videos play correctly
- [ ] Settings menu opens and closes
- [ ] Game browser works
- [ ] Games launch and return correctly
- [ ] Sample mode works
- [ ] Volume transitions work
- [ ] UI fade animations work

**Modern Clean:**
- [ ] Videos play correctly
- [ ] Settings menu visible and functional
- [ ] Game browser works
- [ ] Games launch and return correctly
- [ ] Sample mode works
- [ ] All UI elements visible

**Modern Bezel:**
- [ ] Videos play correctly
- [ ] Settings menu not blocked by bezel
- [ ] Game browser works
- [ ] Games launch correctly
- [ ] Bezel doesn't interfere with any functionality
- [ ] Content clearly visible

---

### Test 7: Raspberry Pi Deployment

**On Pi with CRT (Composite):**
```bash
ssh pi@your-pi
cat /data/settings.json  # Should not exist or show crt_native
sudo systemctl status magic-ui
# Should be running normally
```

**On Pi with HDMI TV:**
```bash
ssh pi@your-pi
echo '{"display_mode": "modern_bezel", "modern_resolution": "1920x1080"}' > /data/settings.json
sudo systemctl restart magic-ui
# Connect to Pi's display
# Should see content centered with bezel
```

---

## Common Issues and Solutions

### Issue: "Bezel blocks content"
**Solution:** This was the initial bug - fixed by rendering bezel first, then content on top

### Issue: "Content is stretched"
**Solution:** Check DisplayManager.calculate_layout() - should always maintain 4:3 aspect

### Issue: "Settings don't save"
**Solution:** 
- Check write permissions on `dev_data/` or `/data/`
- Check logs for save errors
- Verify SettingsStore is initialized

### Issue: "Window wrong size after restart"
**Solution:**
- Check `settings.json` contents
- Delete settings file to reset
- Check environment variables

### Issue: "Bezel doesn't regenerate when resolution changes"
**Solution:** Bezel is generated once at startup - restart app after changing resolution

---

## Performance Testing

### Frame Rate Check

All modes should maintain 60 FPS:

```bash
# Add FPS counter to main loop (temporary)
# In main.py, add after pygame.display.flip():
# print(f"FPS: {clock.get_fps():.1f}")
```

**Expected:**
- CRT Native: 60.0 FPS (no overhead)
- Modern Clean: 59-60 FPS (one scale operation)
- Modern Bezel: 59-60 FPS (one scale + one blit)

If FPS drops significantly, there's an issue.

---

## Acceptance Criteria

All tests must pass:
- ✅ CRT Native mode works identically to before
- ✅ Modern Clean mode centers content properly
- ✅ Modern Bezel mode shows frame without blocking content
- ✅ Settings persist across restarts
- ✅ All features work in all modes
- ✅ No performance degradation
- ✅ Works on both Mac and Pi
- ✅ No linter errors
- ✅ No runtime errors

## Final Verification

Run this complete test sequence:

1. Fresh start (delete settings)
2. Verify CRT Native works
3. Switch to Modern Clean
4. Verify content centered
5. Switch to Modern Bezel
6. Verify bezel visible and content not blocked
7. Change resolution to 2560x1440
8. Verify content scales correctly
9. Launch a video
10. Launch a game
11. Use sample mode
12. Open settings menu
13. All features work? ✅ PASS

If all checks pass, the implementation is complete! 🎉

