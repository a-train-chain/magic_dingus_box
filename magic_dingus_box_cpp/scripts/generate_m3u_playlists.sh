#!/bin/bash
#
# generate_m3u_playlists.sh
# Automatically creates .m3u playlist files for multi-disc PS1 games
#
# Usage: ./generate_m3u_playlists.sh [rom_directory]
#
# This script scans the PS1 ROM directory for multi-disc games
# (identified by patterns like "Disc 1", "Disc 2", etc.) and creates
# .m3u playlist files that allow RetroArch to handle disc swapping.
#

set -euo pipefail

# Default ROM directory
ROM_DIR="${1:-/opt/magic_dingus_box/magic_dingus_box_cpp/data/roms/ps1}"

echo "=== M3U Playlist Generator for Multi-Disc Games ==="
echo "Scanning: $ROM_DIR"
echo ""

# Check if directory exists
if [ ! -d "$ROM_DIR" ]; then
    echo "Error: Directory $ROM_DIR does not exist"
    exit 1
fi

cd "$ROM_DIR"

# Track stats
created=0
updated=0
skipped=0

# Find all disc files and extract base game names
# Patterns: (Disc 1), (Disc 2), (Disk 1), (Disk 2), (CD 1), (CD 2)
declare -A games

# Enable nullglob so unmatched patterns expand to nothing
shopt -s nullglob

for file in *.chd *.cue *.bin *.iso; do
    # Check if this is a multi-disc file
    if [[ "$file" =~ \((Disc|Disk|CD)[[:space:]]*([0-9]+)\) ]]; then
        # Extract the base game name (everything before the disc indicator)
        base_name=$(echo "$file" | sed -E 's/[[:space:]]*\((Disc|Disk|CD)[[:space:]]*[0-9]+\).*$//')
        # Clean up any trailing spaces
        base_name=$(echo "$base_name" | sed 's/[[:space:]]*$//')
        
        # Add to our games associative array
        if [ -z "${games[$base_name]:-}" ]; then
            games[$base_name]="$file"
        else
            games[$base_name]="${games[$base_name]}"$'\n'"$file"
        fi
    fi
done

shopt -u nullglob

# Create .m3u files for each multi-disc game
for base_name in "${!games[@]}"; do
    disc_files="${games[$base_name]}"
    
    # Count how many discs
    disc_count=$(echo "$disc_files" | wc -l)
    
    if [ "$disc_count" -lt 2 ]; then
        # Skip single-disc games (shouldn't happen but just in case)
        continue
    fi
    
    # Sort the disc files numerically
    sorted_files=$(echo "$disc_files" | sort -V)
    
    # Create .m3u filename
    m3u_file="${base_name}.m3u"
    
    # Check if .m3u already exists and matches
    if [ -f "$m3u_file" ]; then
        existing_content=$(cat "$m3u_file")
        if [ "$existing_content" = "$sorted_files" ]; then
            echo "  ⊘ Skipping (unchanged): $m3u_file"
            ((skipped++)) || true
            continue
        else
            echo "  ↻ Updating: $m3u_file ($disc_count discs)"
            ((updated++)) || true
        fi
    else
        echo "  + Creating: $m3u_file ($disc_count discs)"
        ((created++)) || true
    fi
    
    # Write the .m3u file
    echo "$sorted_files" > "$m3u_file"
done

echo ""
echo "=== Summary ==="
echo "  Created: $created"
echo "  Updated: $updated"
echo "  Skipped: $skipped"
echo ""

# List all .m3u files
m3u_count=$(ls -1 *.m3u 2>/dev/null | wc -l || echo 0)
if [ "$m3u_count" -gt 0 ]; then
    echo "Current .m3u playlists:"
    for m3u in *.m3u; do
        disc_count=$(wc -l < "$m3u")
        echo "  📀 $m3u ($disc_count discs)"
    done
else
    echo "No multi-disc games found."
fi
