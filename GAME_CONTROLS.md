# Game Controls and Pause Menu

## How to Play and Control Games

When you launch a game from the Magic Dingus Box, RetroArch takes over the display completely. Your Magic Dingus Box UI is still running in the background and will automatically reappear when you quit the game.

### Visual Continuity

**If you're using Modern (Bezel) display mode:**
- Your selected bezel (e.g., NES TV, N64 TV) automatically appears in RetroArch!
- Same CRT TV frame in UI and gameplay
- Seamless visual experience throughout

See **[RETROARCH_BEZEL_INTEGRATION.md](RETROARCH_BEZEL_INTEGRATION.md)** for details.

### Default RetroArch Controls

#### Keyboard (for development/testing):
- **Arrow Keys**: D-Pad
- **Z**: A Button
- **X**: B Button
- **A**: X Button  
- **S**: Y Button
- **Enter**: Start
- **Right Shift**: Select
- **F1** or **F+1**: Open RetroArch Menu
- **Esc**: Quick Menu / Exit

#### USB Controller (Raspberry Pi):
RetroArch will auto-detect and configure most USB controllers. The button mapping follows standard layouts:
- **D-Pad**: Navigate
- **A/B/X/Y**: Game buttons
- **Start+Select** (simultaneously): Open RetroArch Menu

### Accessing the RetroArch Menu During Gameplay

**Option 1: Keyboard**
- Press **F1** (or Fn+F1 on Mac) to open the RetroArch menu
- This pauses the game

**Option 2: Controller** 
- Press **Start + Select** simultaneously
- This opens the Quick Menu

**Option 3: With GPIO Buttons (Raspberry Pi)**
- Configure one of your buttons to send the RetroArch hotkey combination

### RetroArch Menu Options

Once in the RetroArch menu, you can:

1. **Resume**: Return to game
2. **Save State**: Save your progress at current position
3. **Load State**: Load a previously saved state
4. **Reset**: Restart the game
5. **Close Content**: Exit game and return to Magic Dingus Box

### Returning to Magic Dingus Box

**Method 1: Close Content (Recommended)**
1. Press F1 or Start+Select to open menu
2. Navigate to "Close Content"
3. Select it - Game closes, Magic Dingus Box UI reappears

**Method 2: Quit RetroArch**
1. Press F1 or Start+Select
2. Navigate to "Quit RetroArch"
3. Confirm - Returns to Magic Dingus Box

**Method 3: Quick Exit**
- Press **Esc** twice quickly
- First Esc opens Quick Menu
- Second Esc exits (may need to confirm)

## Important Notes

### Why Can't I See Magic Dingus Box UI During Gameplay?

The Magic Dingus Box UI **cannot** appear on top of RetroArch for technical reasons:
- RetroArch runs in fullscreen and owns the entire display
- The UI runs in the background waiting for the game to exit
- This is by design for best gaming performance

### Save States vs. Game Saves

- **Save States** (F2 to save, F4 to load): Quick save/load at any point
- **In-Game Saves**: Some games have their own save systems (work normally)
- Save states are stored in `~/.config/retroarch/states/` (macOS) or `/home/pi/.config/retroarch/states/` (Pi)

### GPIO Button Integration (Raspberry Pi)

You can configure your physical buttons to work with RetroArch by editing RetroArch's config:

```bash
nano ~/.config/retroarch/retroarch.cfg
```

Add these lines to map GPIO buttons (example):
```
input_player1_a = "z"
input_player1_b = "x"
input_player1_start = "enter"
input_player1_select = "rshift"
input_menu_toggle = "f1"
```

## Troubleshooting

**Game doesn't quit properly:**
- Use "Close Content" from RetroArch menu instead of force-quitting
- This ensures clean return to Magic Dingus Box

**Controller not working:**
- First time: RetroArch will prompt you to configure buttons
- Or go to: Settings → Input → Port 1 Controls → Set All Controls

**Can't access RetroArch menu:**
- Keyboard: Try F1, Fn+F1, or configure in RetroArch settings
- Controller: Hold Start+Select together for 2 seconds

**Game won't launch on Raspberry Pi:**
- Check that cores are installed: `ls /usr/lib/*/libretro/`
- Install missing cores: `sudo apt install libretro-<corename>`

