#!/bin/sh
set -eu

# build-iso.sh — Extract OCI container image into archiso profile and build ISO
#
# Usage:
#   sudo ./build-iso.sh [--image IMAGE_REF] [--calamares-pkg CALAMARES_PKG.tar.zst] [--out OUT_DIR]
#
# Requires: podman, mkarchiso (from archiso), and a pre-built Calamares package

IMAGE_REF="${IMAGE_REF:-ghcr.io/fiftydinar/xfce-aerolike:latest}"
CALAMARES_PKG="${CALAMARES_PKG:-}"
OUT_DIR="${OUT_DIR:-./out}"
PROFILE_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK_DIR="/tmp/archiso-work-$$"
AIROOTFS="${PROFILE_DIR}/airootfs"

usage() {
    echo "Usage: $0 [--image IMAGE_REF] [--calamares-pkg CALAMARES_PKG.tar.zst] [--out OUT_DIR]"
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --image) IMAGE_REF="$2"; shift 2 ;;
        --calamares-pkg) CALAMARES_PKG="$2"; shift 2 ;;
        --out) OUT_DIR="$2"; shift 2 ;;
        *) usage ;;
    esac
done

CALAMARES_DIR=""

if [ -f "$CALAMARES_PKG" ]; then
    case "$CALAMARES_PKG" in
        *.tar.gz|*.tgz) CALAMARES_DIR=$(mktemp -d); tar -xzf "$CALAMARES_PKG" -C "$CALAMARES_DIR" ;;
        *.pkg.tar.zst) ;;
        *)
            echo "ERROR: Calamares package must be .tar.gz (DESTDIR) or .pkg.tar.zst (pacman)"
            exit 1
            ;;
    esac
else
    echo "ERROR: Calamares package not found at '$CALAMARES_PKG'"
    echo "Build it first via GitHub Actions or download from Releases"
    exit 1
fi

cleanup() {
    rm -rf "${WORK_DIR}"
    if mountpoint -q "${AIROOTFS}/proc" 2>/dev/null; then umount "${AIROOTFS}/proc"; fi
    if mountpoint -q "${AIROOTFS}/sys" 2>/dev/null; then umount "${AIROOTFS}/sys"; fi
    if mountpoint -q "${AIROOTFS}/dev" 2>/dev/null; then umount -l "${AIROOTFS}/dev"; fi
}
trap cleanup EXIT

echo "=== Step 1: Pull container image ==="
echo "[build-iso] Image: ${IMAGE_REF}"
podman pull "${IMAGE_REF}"
echo "[build-iso] Image pulled successfully"

echo "=== Step 2: Export container to airootfs ==="
rm -rf "${AIROOTFS}"
mkdir -p "${AIROOTFS}"
echo "[build-iso] Creating container from image..."
CID=$(podman create "${IMAGE_REF}" 2>/dev/null)
echo "[build-iso] Container ID: ${CID}"
echo "[build-iso] Exporting container filesystem (this may take a while)..."
podman export "${CID}" | tar -xf- -C "${AIROOTFS}"
echo "[build-iso] Container exported, removing..."
podman rm "${CID}" >/dev/null 2>&1 || true
echo "[build-iso] Container removed"

echo "=== Step 3: Prepare live environment ==="
mkdir -p "${AIROOTFS}/proc" "${AIROOTFS}/sys" "${AIROOTFS}/dev" "${AIROOTFS}/run" "${AIROOTFS}/boot"

# Provide DNS config for pacman inside chroot
cp -L /etc/resolv.conf "${AIROOTFS}/etc/resolv.conf" 2>/dev/null || true
echo "[build-iso] Copied resolv.conf into chroot"

# Container removes /boot/ but mkarchiso needs kernel there for ISO boot
KVER=$(find "${AIROOTFS}/usr/lib/modules" -maxdepth 1 -type d ! -name "*.img" ! -name "." -printf "%f\n" | head -1)
if [ -n "$KVER" ]; then
    ln -sf "../usr/lib/modules/${KVER}/vmlinuz" "${AIROOTFS}/boot/vmlinuz-linux" 2>/dev/null || true
fi

sed -i 's/^root:.*/root::14871::::::/' "${AIROOTFS}/etc/shadow" 2>/dev/null || true

# Create live user for auto-login
chroot "${AIROOTFS}" useradd -m -G wheel live 2>/dev/null || true
chroot "${AIROOTFS}" passwd -d live 2>/dev/null || true

echo "=== Step 4: Save OCI archive for installer ==="
mkdir -p "${AIROOTFS}/run/install"
podman save "${IMAGE_REF}" | zstd -c > "${AIROOTFS}/run/install/xfce-aerolike.tar.zst"

echo "=== Step 5: Chroot and customize ==="
mount --bind /proc "${AIROOTFS}/proc"
echo "[build-iso] Mounted /proc"
mount --bind /sys "${AIROOTFS}/sys"
echo "[build-iso] Mounted /sys"
mount --bind /dev "${AIROOTFS}/dev"
echo "[build-iso] Mounted /dev"

# Disable bootc auto-update timer in live environment
chroot "${AIROOTFS}" systemctl mask bootc-fetch-apply-updates.timer 2>/dev/null || true
echo "[build-iso] Masked bootc auto-update timer"

# Install live ISO boot support (mkinitcpio + archiso hooks for SquashFS live boot)
# Also install bcachefs-tools for bcachefs support in Calamares partitioner
echo "[build-iso] Installing mkinitcpio, mkinitcpio-archiso, bcachefs-tools inside chroot..."
chroot "${AIROOTFS}" pacman -S --needed --noconfirm mkinitcpio mkinitcpio-archiso bcachefs-tools
echo "[build-iso] Package install complete"

# Configure mkinitcpio for live boot
printf '%s\n' 'HOOKS=(base udev archiso block filesystems keyboard)' \
    'MODULES=(loop overlay squashfs erofs)' \
    'COMPRESSION=(zstd)' \
    > "${AIROOTFS}/etc/mkinitcpio.conf.d/archiso.conf"
echo "[build-iso] Written mkinitcpio archiso.conf"

# Rebuild initramfs for live ISO boot
KVER=$(find "${AIROOTFS}/usr/lib/modules" -maxdepth 1 -type d ! -name "*.img" ! -name "." -printf "%f\n" | head -1)
echo "[build-iso] Kernel version: ${KVER}"
chroot "${AIROOTFS}" mkinitcpio -k "${KVER}" -g /boot/initramfs-linux.img
echo "[build-iso] Initramfs rebuilt"

echo "=== Step 6: Install Calamares ==="
if [ -n "$CALAMARES_DIR" ]; then
    echo "[build-iso] Installing Calamares from DESTDIR package..."
    cp -a "$CALAMARES_DIR/." "${AIROOTFS}/"
    rm -rf "$CALAMARES_DIR"
    echo "[build-iso] Calamares files copied"
else
    echo "[build-iso] Installing Calamares from pacman package..."
    cp "${CALAMARES_PKG}" "${AIROOTFS}/tmp/calamares.pkg.tar.zst"
    chroot "${AIROOTFS}" pacman -U --noconfirm /tmp/calamares.pkg.tar.zst
    rm -f "${AIROOTFS}/tmp/calamares.pkg.tar.zst"
    echo "[build-iso] Calamares package installed"
fi

# Ensure Calamares branding and module configs are in the right place
mkdir -p "${AIROOTFS}/etc/calamares"
echo "[build-iso] Calamares config directory created"
# (airootfs overlay from profile will be applied by mkarchiso)

echo "=== Step 7: Build ISO ==="
mkdir -p "${OUT_DIR}"
echo "[build-iso] Running mkarchiso (working dir: ${WORK_DIR}/iso-work, output: ${OUT_DIR})..."
mkarchiso -w "${WORK_DIR}/iso-work" -o "${OUT_DIR}" "${PROFILE_DIR}"
echo "[build-iso] mkarchiso completed"

echo "=== Done! ==="
ls -lh "${OUT_DIR}"/*.iso 2>/dev/null || echo "ISO built in ${OUT_DIR}"
