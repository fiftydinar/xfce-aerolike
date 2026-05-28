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

# Disable auto-updates in live environment
systemctl disable bootc-fetch-apply-updates.timer || :
systemctl disable bootc-fetch-apply-updates.service || :

# Ensure dracut has all modules (including dmsquash-live)
pacman -Sy --noconfirm dracut squashfs-tools

# Locate dmsquash-live module directory
MOD_DIR=$(find /usr/lib/dracut/modules.d -maxdepth 1 -type d -name '*dmsquash-live*' | head -1)
test -n "$MOD_DIR" || {
  echo "ERROR: dmsquash-live module not found. Available modules:" >&2
  ls /usr/lib/dracut/modules.d/ >&2
  exit 1
}
echo "Found dmsquash-live at: $MOD_DIR"

# Regenerate initramfs with dmsquash-live modules
KVER=$(basename "$(find /usr/lib/modules -maxdepth 1 -type d | grep -v "\.img$" | tail -n1)")
echo "Kernel version: $KVER"
test -n "$KVER" || { echo "ERROR: no kernel found" >&2; exit 1; }
test -f "/usr/lib/modules/$KVER/vmlinuz" || { echo "ERROR: vmlinuz not found for $KVER" >&2; exit 1; }

dracut --force --no-hostonly --reproducible --zstd \
  --kver "$KVER" \
  --add "dmsquash-live dmsquash-live-autooverlay" \
  /app/initramfs.img

# Verify initramfs was created
test -f /app/initramfs.img || { echo "ERROR: initramfs not created" >&2; exit 1; }
ls -lh /app/initramfs.img
