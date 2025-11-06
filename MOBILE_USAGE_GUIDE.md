# Mobile Usage Guide - Magic Dingus Box Web Interface

## Touch Controls Overview

The web interface now features full touch support for mobile devices! You can build and manage playlists entirely from your phone or tablet.

## Getting Started

1. **Connect to your device** at `http://10.0.0.196:8080` (or your device's IP)
2. **Select your Magic Dingus Box** from the device list
3. Start managing content with touch gestures!

## Touch Gestures

### Adding Content to Playlists

**Long Press & Drag:**

1. **Navigate to Playlists tab**
2. **Switch between Videos/ROMs** using the filter tabs
3. **Long press (hold for 0.3 seconds)** on any video or ROM
4. You'll feel a **haptic vibration** (if your phone supports it)
5. The item becomes **translucent** and a **floating clone** appears
6. **Drag your finger** to the "Playlist Items" panel on the right
7. The panel **highlights in teal** when you're over it
8. **Release** to drop the item into your playlist
9. You'll feel **three quick vibrations** to confirm!

### Reordering Playlist Items

**Long Press & Reorder:**

1. **Long press (0.3 seconds)** on any item in your playlist
2. Item becomes translucent with a **teal shadow**
3. **Drag up or down** to reorder
4. Other items show a **teal line** indicating where it will drop
5. **Release** to place in new position
6. **Triple vibration** confirms the reorder!

### Removing Items

**Quick Tap:**
- Tap the **"✕ Remove"** button on any playlist item
- No long press needed for removal

## Visual Feedback

### While Dragging Content Items:
- **Original item**: 50% opacity, scaled up 5%
- **Floating clone**: 80% opacity, scaled up 10%, pink glow shadow
- **Drop zone**: Background changes, teal border appears

### While Reordering:
- **Dragged item**: 50% opacity, "dragging" class applied
- **Potential drop position**: 3px teal border on top
- **Floating clone**: 90% opacity, teal shadow

### Haptic Feedback:
- **Single vibration**: Drag started (50ms)
- **Triple vibration**: Item dropped successfully (50ms-50ms-50ms pattern)

## Tips for Best Experience

### Touch Targets
- All draggable items are **minimum 48px tall** on touch devices
- Extra padding added automatically on phones
- Remove buttons are larger and easier to tap

### Scrolling
- **Momentum scrolling** enabled for content lists
- Scroll normally by swiping quickly
- Long press to initiate drag

### Landscape Mode
- Use **landscape orientation** for better two-panel view
- Portrait mode stacks panels vertically (still works great!)

### Long Press Timing
- Hold for **300 milliseconds** (about 1/3 second)
- Moving your finger too early cancels the long press
- You'll know it started when you feel the vibration

## Troubleshooting

### "Nothing happens when I touch items"
- Make sure you're holding for the full 0.3 seconds
- Try not to move your finger during the initial press
- Check if haptic feedback is working (test with keyboard vibration)

### "Items snap back instead of dropping"
- Make sure you drag over the playlist panel (right side)
- Look for the teal highlight on the panel
- Release while still over the panel

### "Can't reorder playlist items"
- Don't tap the Remove button - tap the item text
- Hold for 0.3 seconds before dragging
- Make sure you're in the Playlists tab

### "Scrolling triggers drag"
- Quick swipes should scroll normally
- Only long press activates drag mode
- If sensitivity is too high, scroll with two fingers

## Feature Comparison

| Feature | Desktop | Mobile Touch |
|---------|---------|--------------|
| Add to playlist | Click & drag | Long press & drag |
| Reorder items | Click & drag | Long press & drag |
| Remove items | Click button | Tap button |
| Visual feedback | Hover effects | Active states + shadows |
| Confirmation | Visual only | Visual + haptic |

## Advanced Tips

### Quick Playlist Building
1. Long press first item → drag to playlist
2. While still in playlist panel, tap back to content
3. Long press next item → drag to playlist
4. Repeat for fast building!

### Precise Reordering
1. Long press item you want to move
2. Drag slowly to see teal drop indicator
3. Position precisely between items
4. Release when indicator is in perfect spot

### Bulk Operations
- Unfortunately, multi-touch drag isn't supported yet
- Add items one at a time for now
- Future update may add multi-select!

## Browser Compatibility

### Fully Tested:
- ✅ Safari on iOS 14+
- ✅ Chrome on Android 10+
- ✅ Firefox Mobile
- ✅ Edge Mobile

### Should Work:
- Samsung Internet Browser
- Opera Mobile
- Brave Mobile

### Known Issues:
- Some older Android browsers (< 2020) may not support touch events
- If drag-and-drop doesn't work, try Chrome or Firefox

## Accessibility

### For Users with Motor Impairments:
- Long press duration can be adjusted in code if needed
- Larger touch targets on mobile devices
- High contrast borders for drop zones

### For Users with Visual Impairments:
- Screen reader support for playlist structure
- High contrast neon colors
- Large, readable monospace font

## Performance Tips

### On Older Phones:
- Close other apps before building large playlists
- Use WiFi (not cellular data) for uploads
- Refresh page if it becomes sluggish

### For Large Playlists:
- Playlist items render quickly up to ~50 items
- Beyond 100 items, consider splitting into multiple playlists
- Scrolling remains smooth with any playlist size

## Security Note

The web interface is designed for **local network use only**. Don't expose it to the public internet without adding authentication!

## Future Enhancements

Coming soon:
- Multi-select items for bulk operations
- Swipe to remove items
- Pinch to zoom playlist view
- Voice control for hands-free management
- Customizable long-press duration

## Questions?

The touch implementation uses:
- Native touch events (`touchstart`, `touchmove`, `touchend`)
- Passive event listeners for performance
- CSS `touch-action: none` to prevent scrolling during drag
- Visual clones that follow your finger
- Collision detection for drop zones

Check the console (F12) for debug messages if something isn't working!

---

**Enjoy building playlists on your phone!** 📱🎵🎮

