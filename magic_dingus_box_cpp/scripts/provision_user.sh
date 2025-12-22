#!/bin/bash
set -e

# This script creates the standard 'magic' system user for the Magic Dingus Box.
# It should be run as root during image provisioning.

USERNAME="magic"
USER_UID=1000

echo "=== Provisioning System User: $USERNAME ==="

# Check if user exists
if id "$USERNAME" &>/dev/null; then
    echo "User $USERNAME already exists."
else
    echo "Creating user $USERNAME..."
    # Try creating with UID 1000, if fails, let system pick
    if ! useradd -m -s /bin/bash -u "$USER_UID" -G video,input,audio,render,sudo "$USERNAME" 2>/dev/null; then
        echo "UID $USER_UID in use, creating with next available UID..."
        useradd -m -s /bin/bash -G video,input,audio,render,sudo "$USERNAME"
    fi
    echo "User created."
fi

# Check UID
CURRENT_UID=$(id -u "$USERNAME")
if [ "$CURRENT_UID" -ne "$USER_UID" ]; then
    echo "Warning: $USERNAME has UID $CURRENT_UID (target: $USER_UID)."
    echo "Conflict resolution required manually after login."
fi

# Set a default password if needed (optional, for distribution usually passwordless or default)
# echo "magic:magic" | chpasswd

# Setup directories
echo "Setting up directories..."
mkdir -p "/home/$USERNAME/.config/retroarch/cores"
mkdir -p "/home/$USERNAME/.config/retroarch/system"
mkdir -p "/home/$USERNAME/.config/retroarch/autoconfig"

# Setup SSH keys
echo "Setting up SSH keys..."
mkdir -p "/home/$USERNAME/.ssh"
if [ -f "$HOME/.ssh/authorized_keys" ]; then
    cp "$HOME/.ssh/authorized_keys" "/home/$USERNAME/.ssh/authorized_keys"
    echo "Copied authorized_keys from $USER"
fi
chmod 700 "/home/$USERNAME/.ssh"
chmod 600 "/home/$USERNAME/.ssh/authorized_keys" 2>/dev/null || true
chown -R "$USERNAME:$USERNAME" "/home/$USERNAME/.ssh"

# Enable passwordless sudo for magic
echo "Enabling passwordless sudo for $USERNAME..."
echo "$USERNAME ALL=(ALL) NOPASSWD: ALL" > "/etc/sudoers.d/010_$USERNAME-nopasswd"
chmod 440 "/etc/sudoers.d/010_$USERNAME-nopasswd"

# Setup PulseAudio to force HDMI output
echo "Configuring PulseAudio for HDMI output..."
mkdir -p "/home/$USERNAME/.config/pulse"
echo ".include /etc/pulse/default.pa" > "/home/$USERNAME/.config/pulse/default.pa"
echo "set-default-sink alsa_output.platform-fef00700.hdmi.hdmi-stereo" >> "/home/$USERNAME/.config/pulse/default.pa"
chown -R "$USERNAME:$USERNAME" "/home/$USERNAME/.config/pulse"

# Fix permissions
echo "Fixing permissions..."
chown -R "$USERNAME:$USERNAME" "/home/$USERNAME"

echo "=== Provisioning Complete ==="
echo "User $USERNAME is ready."
