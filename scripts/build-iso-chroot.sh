#!/bin/bash
set -xeuo pipefail

# Live user
useradd -m -G wheel liveuser
echo "liveuser:liveuser" | chpasswd
echo "liveuser ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers

# LightDM autologin
mkdir -p /etc/lightdm/lightdm.conf.d
cat > /etc/lightdm/lightdm.conf.d/50-live-autologin.conf << 'LIGHTDM'
[Seat:*]
autologin-user=liveuser
autologin-user-timeout=0
LIGHTDM

# Disable services inappropriate for a live environment
systemctl disable bootc-fetch-apply-updates.timer
systemctl disable bootc-fetch-apply-updates.service
systemctl disable memory-tweaks-aerolike.service
systemctl disable dirty-centisecs-aerolike.service

# Install dracut for live initramfs
pacman -Sy --noconfirm --needed dracut

# Regenerate initramfs with dmsquash-live modules
KVER=$(basename "$(find /usr/lib/modules -maxdepth 1 -type d | grep -v "\.img$" | tail -n1)")
dracut --force --no-hostonly --reproducible --zstd \
  --kver "$KVER" \
  --add "dmsquash-live dmsquash-live-autooverlay" \
  /app/initramfs.img

# Clean up
pacman -Scc --noconfirm
