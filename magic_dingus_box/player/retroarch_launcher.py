"""RetroArch emulator launcher for seamless game integration."""
from __future__ import annotations

import logging
import subprocess
import sys
from pathlib import Path
from typing import Optional


class RetroArchLauncher:
    """Launches RetroArch emulator with specified cores and ROMs."""
    
    def __init__(self):
        self._log = logging.getLogger("retroarch")
        # Common RetroArch executable paths
        self.retroarch_paths = [
            "/usr/bin/retroarch",           # Linux standard
            "/opt/retropie/emulators/retroarch/bin/retroarch",  # RetroPie
            "/Applications/RetroArch.app/Contents/MacOS/RetroArch",  # macOS
        ]
        self.retroarch_bin = self._find_retroarch()
        
    def _find_retroarch(self) -> Optional[str]:
        """Find RetroArch executable."""
        for path in self.retroarch_paths:
            if Path(path).exists():
                self._log.info(f"Found RetroArch at: {path}")
                return path
        self._log.warning("RetroArch not found. Install with: sudo apt install retroarch (Linux) or brew install --cask retroarch (macOS)")
        return None
    
    def launch_game(self, rom_path: str, core: str, overlay_path: str = None) -> bool:
        """Launch a game with RetroArch.
        
        Args:
            rom_path: Path to the ROM file
            core: RetroArch core name (e.g., 'snes9x_libretro', 'mupen64plus_next_libretro')
            overlay_path: Optional path to overlay/bezel PNG file
        
        Returns:
            True if game launched and exited cleanly, False otherwise
        """
        if not self.retroarch_bin:
            self._log.error("RetroArch not installed")
            return False
        
        rom = Path(rom_path).expanduser().resolve()
        if not rom.exists():
            self._log.error(f"ROM not found: {rom}")
            return False
        
        # On Linux, cores don't have the _libretro suffix in the -L argument
        # The system will find them in /usr/lib/libretro/
        core_name = core.replace("_libretro", "") if sys.platform.startswith("linux") else core
        
        # Create temporary RetroArch config with overlay if provided
        temp_config_path = None
        if overlay_path:
            import tempfile
            import os
            
            overlay_png = Path(overlay_path)
            overlay_cfg = overlay_png.with_suffix('.cfg')
            
            if overlay_cfg.exists():
                # Create temporary RetroArch config that includes overlay settings
                temp_config = tempfile.NamedTemporaryFile(mode='w', suffix='.cfg', delete=False)
                temp_config_path = temp_config.name
                
                # Write minimal RetroArch config with overlay enabled
                temp_config.write(f"# Temporary RetroArch config for Magic Dingus Box\n")
                temp_config.write(f"input_overlay_enable = \"true\"\n")
                temp_config.write(f"input_overlay = \"{str(overlay_cfg.resolve())}\"\n")
                temp_config.write(f"input_overlay_opacity = \"1.0\"\n")
                temp_config.write(f"input_overlay_scale = \"1.0\"\n")
                temp_config.write(f"video_fullscreen = \"true\"\n")
                temp_config.close()
                
                self._log.info(f"Created temp config with overlay: {overlay_png.name}")
        
        # RetroArch command line arguments
        cmd = [
            self.retroarch_bin,
            "-L", core_name,         # Load specific libretro core
            str(rom),                # ROM path
            "--fullscreen",          # Start in fullscreen
            "--verbose",             # Enable logging
        ]
        
        # Use temporary config if overlay is enabled
        if temp_config_path:
            cmd.extend(["--config", temp_config_path])
        
        self._log.info(f"Launching: {' '.join(cmd)}")
        
        try:
            # Run RetroArch and wait for it to exit
            result = subprocess.run(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )
            
            if result.returncode == 0:
                self._log.info("Game exited normally")
                return True
            else:
                self._log.warning(f"RetroArch exited with code {result.returncode}")
                if result.stderr:
                    self._log.debug(f"stderr: {result.stderr}")
                return False
                
        except Exception as e:
            self._log.error(f"Failed to launch RetroArch: {e}")
            return False
        finally:
            # Clean up temporary config file
            if temp_config_path:
                try:
                    import os
                    os.unlink(temp_config_path)
                    self._log.debug("Cleaned up temp config")
                except:
                    pass


# Core mappings for common systems
RETROARCH_CORES = {
    "NES": "fceumm_libretro",           # NES/Famicom
    "SNES": "snes9x_libretro",          # Super Nintendo
    "N64": "mupen64plus_next_libretro", # Nintendo 64
    "PS1": "pcsx_rearmed_libretro",     # PlayStation 1
    "GBA": "mgba_libretro",             # Game Boy Advance
    "GB": "gambatte_libretro",          # Game Boy
    "GBC": "gambatte_libretro",         # Game Boy Color
    "Genesis": "genesis_plus_gx_libretro",  # Sega Genesis/Mega Drive
}

