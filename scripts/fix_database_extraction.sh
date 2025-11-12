#!/bin/bash
# Fix RetroArch database extraction error (rdb.zip)
# This script fixes the database extraction issue

set -e

CONFIG_DIR="$HOME/.config/retroarch"
ASSETS_DIR="$CONFIG_DIR/assets"
DATABASE_DIR="$ASSETS_DIR/database"
LOG_FILE="/tmp/retroarch_database_fix.log"

echo "Fixing RetroArch database extraction..."

# Create necessary directories
mkdir -p "$ASSETS_DIR"
mkdir -p "$DATABASE_DIR"
chmod 755 "$ASSETS_DIR"
chmod 755 "$DATABASE_DIR"

# Check if unzip is available
if ! command -v unzip >/dev/null 2>&1; then
    echo "Installing unzip..."
    sudo apt-get update -qq && sudo apt-get install -y unzip
fi

# Download database files manually
echo "Downloading RetroArch databases..."

DATABASES=(
    "rdb.zip"
    "database-rdb.zip"
)

for DB in "${DATABASES[@]}"; do
    URLS=(
        "https://buildbot.libretro.com/assets/frontend/${DB}"
        "https://buildbot.libretro.com/assets/${DB}"
    )
    
    for URL in "${URLS[@]}"; do
        echo "Trying: $URL"
        if curl -L -f --connect-timeout 30 --max-time 120 \
            "$URL" -o "/tmp/${DB}" 2>/dev/null; then
            if [ -f "/tmp/${DB}" ] && [ -s "/tmp/${DB}" ]; then
                FILE_SIZE=$(stat -f%z "/tmp/${DB}" 2>/dev/null || stat -c%s "/tmp/${DB}" 2>/dev/null)
                if [ "$FILE_SIZE" -gt 1000 ]; then
                    echo "Downloaded $DB ($FILE_SIZE bytes)"
                    if unzip -o "/tmp/${DB}" -d "$DATABASE_DIR" 2>/dev/null; then
                        echo "✓ Extracted $DB successfully"
                        rm -f "/tmp/${DB}"
                        break
                    else
                        echo "Failed to extract $DB"
                        rm -f "/tmp/${DB}"
                    fi
                fi
            fi
        fi
    done
done

# Update RetroArch config
CONFIG_FILE="$CONFIG_DIR/retroarch.cfg"
if [ -f "$CONFIG_FILE" ]; then
    sed -i '/^assets_directory/d' "$CONFIG_FILE"
    echo "assets_directory = \"$ASSETS_DIR\"" >> "$CONFIG_FILE"
    
    sed -i '/^database_directory/d' "$CONFIG_FILE"
    echo "database_directory = \"$DATABASE_DIR\"" >> "$CONFIG_FILE"
fi

echo "Database fix complete!"
echo "Assets directory: $ASSETS_DIR"
echo "Database directory: $DATABASE_DIR"

