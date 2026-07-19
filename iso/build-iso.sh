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
AIROOTFS="${WORK_DIR}/iso-work/x86_64/airootfs"

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
    if mountpoint -q "${AIROOTFS}/dev" 2>/dev/null; then umount -l "${AIROOTFS}/dev"; fi
    if mountpoint -q "${AIROOTFS}/sys" 2>/dev/null; then umount "${AIROOTFS}/sys"; fi
    if mountpoint -q "${AIROOTFS}/proc" 2>/dev/null; then umount "${AIROOTFS}/proc"; fi
    rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

# Workaround for newer pacman which validates DBPath at startup
mkdir -p /usr/lib/sysimage/lib/pacman 2>/dev/null || true

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
mkdir -p "${AIROOTFS}/root" 2>/dev/null || true

# Provide DNS config for pacman inside chroot
cp -L /etc/resolv.conf "${AIROOTFS}/etc/resolv.conf" 2>/dev/null || true
echo "[build-iso] Copied resolv.conf into chroot"

# mkarchiso needs kernel at /boot/vmlinuz-linux for ISO boot.
# The container may lack a kernel or have it in an unexpected location.
# We handle this in Step 5 by installing the linux package inside the chroot.

sed -i 's|^root:[^:]*:|root::|' "${AIROOTFS}/etc/shadow" 2>/dev/null || true

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

# Ensure /var/tmp exists (dracut uses it as default TMPDIR)
mkdir -p "${AIROOTFS}/var/tmp"

# Disable bootc auto-update timer in live environment
chroot "${AIROOTFS}" systemctl mask bootc-fetch-apply-updates.timer 2>/dev/null || true
echo "[build-iso] Masked bootc auto-update timer"

# Install dracut + our custom archiso module for live ISO boot
# Also install bcachefs-tools for bcachefs support in Calamares partitioner
echo "[build-iso] Installing dracut, bcachefs-tools inside chroot..."
# Fix dracut's lib hang on finding root device due to dash's hex escape
sed -i 's/echo "\$hook"/printf '\''%s\\n'\'' "$hook"/g' \
    "${AIROOTFS}/usr/lib/dracut/modules.d/80base/dracut-lib.sh" 2>/dev/null || true
echo "[build-iso] Fixed dracut-lib.sh dash hex escape bug"
chroot "${AIROOTFS}" pacman -S --needed --noconfirm dracut bcachefs-tools

# Remove mkinitcpio if present to avoid conflicts
chroot "${AIROOTFS}" pacman -Rdd --noconfirm mkinitcpio mkinitcpio-archiso 2>/dev/null || true
echo "[build-iso] Package install complete"

# Install the custom archiso dracut module
DRACUT_MODDIR="${AIROOTFS}/usr/lib/dracut/modules.d/95archiso"
mkdir -p "$DRACUT_MODDIR"
cat > "$DRACUT_MODDIR/module-setup.sh" << 'DRACUTEOF'
#!/bin/sh
check() { return 255; }
depends() { echo dm overlayfs img-lib; }
installkernel() { hostonly='' instmods squashfs erofs loop iso9660 overlay; }
install() {
    inst_multiple losetup blkid blockdev mount umount mkdir rmdir rm ln cp truncate mountpoint
    inst_multiple lsblk grep sed awk sleep readlink realpath find head
    inst_multiple -o sha512sum gpg openssl pv
    inst_hook cmdline 30 "$moddir/parse-archiso.sh"
    inst_hook pre-udev 30 "$moddir/archiso-genrules.sh"
    inst_script "$moddir/archiso-root.sh" "/sbin/archiso-root"
    if dracut_module_included "systemd"; then
        inst_script "$moddir/archiso-generator.sh" "$systemdutildir"/system-generators/dracut-archiso-generator
        inst "$moddir/archiso-root.service" "$systemdsystemunitdir"/archiso-root.service
        ln_r "$systemdsystemunitdir/archiso-root.service" "/etc/systemd/system/initrd.target.wants/archiso-root.service"
    fi
}
DRACUTEOF

cat > "$DRACUT_MODDIR/parse-archiso.sh" << 'DRACUTEOF'
#!/bin/sh
command -v getarg > /dev/null || . /lib/dracut-lib.sh
[ -z "$root" ] && root=$(getarg root=)
archisodevice=$(getarg archisodevice=)
archisolabel=$(getarg archisolabel=)
archisosearchuuid=$(getarg archisosearchuuid=)
archisobasedir=$(getarg archisobasedir=); [ -z "$archisobasedir" ] && archisobasedir="arch"
arch=$(getarg arch=); [ -z "$arch" ] && arch=$(uname -m)
img_dev=$(getarg img_dev=)
img_loop=$(getarg img_loop=)
[ -n "$archisodevice" ] || [ -n "$archisolabel" ] || [ -n "$archisosearchuuid" ] || [ -n "$img_loop" ] || return 1
modprobe -q loop
[ -n "$archisolabel" ] && archisodevice="/dev/disk/by-label/${archisolabel}"
mkdir -p /run/archiso
[ -n "$archisodevice" ] && echo "$archisodevice" > /run/archiso/archisodevice
if [ -n "$archisosearchuuid" ]; then
    f=$(getarg archisosearchfilename=); [ -z "$f" ] && f="/boot/${archisosearchuuid}.uuid"
    echo "$f" > /run/archiso/archisosearchfilename
    echo "$archisosearchuuid" > /run/archiso/archisosearchuuid
fi
if [ -n "$img_dev" ]; then
    echo "$img_dev" > /run/archiso/img_dev
    echo "$img_loop" > /run/archiso/img_loop
fi
root=archiso; rootok=1
wait_for_dev -n /dev/root
return 0
DRACUTEOF

cat > "$DRACUT_MODDIR/archiso-genrules.sh" << 'DRACUTEOF'
#!/bin/sh
command -v getarg > /dev/null || . /lib/dracut-lib.sh
if [ -e /run/archiso/archisodevice ]; then
    d=$(cat /run/archiso/archisodevice)
    case "$d" in /dev/*)
        dev="${d#/dev/}"
        printf 'KERNEL=="%s", RUN+="/sbin/initqueue --settled --onetime --unique /sbin/archiso-root %s"\n' "$dev" "$d" >> /etc/udev/rules.d/99-archiso.rules
        printf 'SYMLINK=="%s", RUN+="/sbin/initqueue --settled --onetime --unique /sbin/archiso-root %s"\n' "$dev" "$d" >> /etc/udev/rules.d/99-archiso.rules
        wait_for_dev -n "$d"
    esac
fi
DRACUTEOF

cat > "$DRACUT_MODDIR/archiso-root.sh" << 'DRACUTEOF'
#!/bin/sh
command -v getarg > /dev/null || . /lib/dracut-lib.sh
command -v det_fs > /dev/null || . /lib/fs-lib.sh
command -v get_rd_overlay > /dev/null || . /lib/overlayfs-lib.sh
PATH=/usr/sbin:/usr/bin:/sbin:/bin

mnt_dev() {
    local dev="$1" mnt="$2" flags="$3" opts="$4" rootdelay
    rootdelay=$(getarg rootdelay 30)
    while ! [ -b "$dev" ]; do
        sleep 1; rootdelay=$((rootdelay - 1)); [ "$rootdelay" -le 0 ] && break
    done
    [ -b "$dev" ] || return 1
    mount --mkdir -o "x-initrd.mount,${opts}" "$flags" "$dev" "$mnt"
}

search_device() {
    local searchfile dev f rootdelay
    searchfile=$(cat /run/archiso/archisosearchfilename 2>/dev/null)
    [ -z "$searchfile" ] && return 1
    rootdelay=$(getarg rootdelay=); [ -z "$rootdelay" ] && rootdelay=30
    mkdir -p /archisosearch
    while [ "$rootdelay" -gt 0 ]; do
        for dev in $(lsblk -o PATH -n -l 2>/dev/null); do
            [ -b "$dev" ] || continue
            mount -o ro "$dev" /archisosearch 2>/dev/null || continue
            if [ -e "/archisosearch${searchfile}" ]; then
                echo "$dev" > /run/archiso/archisodevice
                umount /archisosearch 2>/dev/null; rmdir /archisosearch 2>/dev/null
                return 0
            fi
            umount /archisosearch 2>/dev/null
        done
        sleep 1; rootdelay=$((rootdelay - 1))
    done
    rmdir /archisosearch 2>/dev/null
    return 1
}

main() {
    local mode archisodevice archisobasedir arch cow_spacesize cow_device cow_persistent cow_directory cow_flags fs_img copytoram copytoram_img loopdev img_name loopimg_dev loopimg_path
    mode="${1:-}"
    mkdir -p /run/archiso
    # Auto-detect mode when called without arguments (from systemd service)
    if [ -z "$mode" ]; then
        if [ -f /run/archiso/archisodevice ]; then
            mode=$(cat /run/archiso/archisodevice)
        elif [ -f /run/archiso/archisosearchfilename ]; then
            mode="--search"
        elif [ -f /run/archiso/img_dev ]; then
            mode="--loopimg"
        fi
    fi
    case "$mode" in
        --search) search_device || { warn "archiso: device not found"; exit 1; } ;;
        --loopimg)
            loopimg_dev=$(cat /run/archiso/img_dev 2>/dev/null)
            loopimg_path=$(cat /run/archiso/img_loop 2>/dev/null)
            if [ -n "$loopimg_dev" ] && [ -n "$loopimg_path" ]; then
                mnt_dev "$loopimg_dev" "/run/archiso/img_dev" "-r" "defaults" || exit 1
                loopdev=$(losetup --find --show --read-only "/run/archiso/img_dev/$loopimg_path") || exit 1
                echo "$loopdev" > /run/archiso/archisodevice
            else
                warn "archiso: loopimg missing device or path"; exit 1
            fi
            ;;
        --resume) exit 0 ;;
        /dev/* | UUID=* | LABEL=* | PARTUUID=* | PARTLABEL=*)
            echo "$mode" > /run/archiso/archisodevice
    esac
    archisodevice=$(cat /run/archiso/archisodevice 2>/dev/null)
    [ -z "$archisodevice" ] && exit 1
    archisobasedir=$(getarg archisobasedir=); [ -z "$archisobasedir" ] && archisobasedir="arch"
    arch=$(getarg arch=); [ -z "$arch" ] && arch=$(uname -m)
    cow_spacesize=$(getarg cow_spacesize=); [ -z "$cow_spacesize" ] && cow_spacesize="256M"
    cow_device=$(getarg cow_device=)
    cow_label=$(getarg cow_label=)
    if [ -n "$cow_label" ]; then cow_device="/dev/disk/by-label/${cow_label}"; cow_persistent="P"
    elif [ -n "$cow_device" ]; then cow_persistent="P"
    else cow_persistent="N"; fi
    cow_flags=$(getarg cow_flags=); [ -z "$cow_flags" ] && cow_flags="defaults"
    archisolabel=$(getarg archisolabel=)
    cow_directory=$(getarg cow_directory=)
    if [ -z "$cow_directory" ]; then
        if [ -n "$archisolabel" ]; then cow_directory="persistent_${archisolabel}/${arch}"
        else cow_directory="persistent/${arch}"; fi
    fi
    copytoram=$(getarg copytoram=)

    mountpoint -q /run/archiso/bootmnt || mnt_dev "$archisodevice" "/run/archiso/bootmnt" "-r" "defaults" || exit 1

    if [ -f "/run/archiso/bootmnt/${archisobasedir}/${arch}/airootfs.sfs" ]; then
        fs_img="/run/archiso/bootmnt/${archisobasedir}/${arch}/airootfs.sfs"
    elif [ -f "/run/archiso/bootmnt/${archisobasedir}/${arch}/airootfs.erofs" ]; then
        fs_img="/run/archiso/bootmnt/${archisobasedir}/${arch}/airootfs.erofs"
    else
        warn "archiso: no airootfs image found"; exit 1
    fi

    if [ "$copytoram" = "y" ]; then
        img_name="${fs_img##*/}"; mkdir -p /run/archiso/copytoram
        cp "$fs_img" "/run/archiso/copytoram/${img_name}"
        fs_img="/run/archiso/copytoram/${img_name}"
    elif [ -z "$copytoram" ]; then
        avail_kb=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
        if [ -n "$avail_kb" ] && [ "$avail_kb" -ge 8388608 ]; then
            info "archiso: auto-enabling copytoram (avail=${avail_kb}kB >= 8GB)"
            img_name="${fs_img##*/}"; mkdir -p /run/archiso/copytoram
            cp "$fs_img" "/run/archiso/copytoram/${img_name}"
            fs_img="/run/archiso/copytoram/${img_name}"
        fi
    fi

    if ! mountpoint -q /run/archiso/cowspace 2>/dev/null; then
        if [ -n "$cow_device" ]; then
            mnt_dev "$cow_device" "/run/archiso/cowspace" "-r" "$cow_flags" || exit 1
            mount -o remount,rw /run/archiso/cowspace 2>/dev/null || true
        else
            mount --mkdir -t tmpfs -o "size=${cow_spacesize},mode=0755" cowspace /run/archiso/cowspace
        fi
    fi
    mkdir -p "/run/archiso/cowspace/${cow_directory}" && chmod 0700 "/run/archiso/cowspace/${cow_directory}"

    if mountpoint -q /sysroot 2>/dev/null; then
        info "archiso: root already ready"
        exit 0
    fi

    mkdir -p /run/archiso/airootfs
    if ! mountpoint -q /run/archiso/airootfs 2>/dev/null; then
        loopdev=$(losetup --find --show --read-only "$fs_img")
        mount -n -o ro "$loopdev" /run/archiso/airootfs
    fi

    mkdir -p "/run/archiso/cowspace/${cow_directory}/upperdir"
    mkdir -p "/run/archiso/cowspace/${cow_directory}/workdir"
    mkdir -m 0755 -p /sysroot
    mount -t overlay -o "lowerdir=/run/archiso/airootfs,upperdir=/run/archiso/cowspace/${cow_directory}/upperdir,workdir=/run/archiso/cowspace/${cow_directory}/workdir" overlay /sysroot
    ln -sf /sysroot /dev/root
    need_shutdown
    info "archiso: root ready"
    exit 0
}
main "$@"
DRACUTEOF

cat > "$DRACUT_MODDIR/archiso-generator.sh" << 'DRACUTEOF'
#!/bin/sh
command -v getarg > /dev/null || . /lib/dracut-lib.sh
[ "${root}" != "archiso" ] && exit 0
# archiso-root.sh mounts the overlay directly at /sysroot
exit 0
DRACUTEOF

cat > "$DRACUT_MODDIR/archiso-root.service" << 'DRACUTEOF'
[Unit]
Description=archiso root setup
DefaultDependencies=false
After=systemd-udevd.service systemd-udev-settle.service
Before=dracut-mount.service initrd.target

[Service]
Type=oneshot
ExecStart=/sbin/archiso-root
RemainAfterExit=yes

[Install]
WantedBy=initrd.target
DRACUTEOF

chmod -R +x "$DRACUT_MODDIR"

# Configure dracut to include the archiso module
mkdir -p "${AIROOTFS}/etc/dracut.conf.d"
cat > "${AIROOTFS}/etc/dracut.conf.d/50-archiso.conf" << 'CONFEOF'
add_dracutmodules+=" archiso "
omit_dracutmodules+=" dmsquash-live dmsquash-live-autooverlay livenet "
hostonly="no"
compress="zstd"
CONFEOF

cat > "${AIROOTFS}/etc/dracut.conf.d/99-initramfs-root.conf" << 'CONFEOF'
install_items+=" /etc/passwd /etc/shadow "
CONFEOF

# Find kernel version and regenerate initramfs with dracut
KVER=$(find "${AIROOTFS}/usr/lib/modules" -maxdepth 1 -type d -name "[0-9]*" -printf "%f\n" | sort -V | tail -1)
echo "[build-iso] Kernel version: ${KVER}"

if [ -n "$KVER" ]; then
    # Ensure /boot/vmlinuz-linux exists (kernel binary from container)
    if [ ! -f "${AIROOTFS}/boot/vmlinuz-linux" ] && [ -f "${AIROOTFS}/usr/lib/modules/${KVER}/vmlinuz" ]; then
        cp "${AIROOTFS}/usr/lib/modules/${KVER}/vmlinuz" "${AIROOTFS}/boot/vmlinuz-linux"
    fi
    chroot "${AIROOTFS}" env TMPDIR=/tmp dracut --force --add archiso --install "/etc/passwd /etc/shadow" /boot/initramfs-linux.img "${KVER}"
    echo "[build-iso] dracut initramfs built"
else
    echo "[build-iso] WARNING: No kernel found, skipping initramfs build"
fi

# Unmount chroot filesystems before mkarchiso processes the rootfs
umount -l "${AIROOTFS}/dev" 2>/dev/null || true
umount "${AIROOTFS}/sys" 2>/dev/null || true
umount "${AIROOTFS}/proc" 2>/dev/null || true

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
echo "[build-iso] Creating marker to skip mkarchiso pacstrap (rootfs already prepared)..."
mkdir -p "${WORK_DIR}/iso-work"
# _build_iso_base uses run_once_mode="base", so marker is <work_dir>/base._make_packages
touch "${WORK_DIR}/iso-work/base._make_packages"

# Copy syslinux files from host into the rootfs (mkarchiso reads them from
# the rootfs, not the host, even though they're only needed on the ISO)
if [ -d /usr/lib/syslinux/bios ]; then
    mkdir -p "${AIROOTFS}/usr/lib/syslinux"
    cp -r /usr/lib/syslinux/bios "${AIROOTFS}/usr/lib/syslinux/"
fi

echo "[build-iso] Running mkarchiso (working dir: ${WORK_DIR}/iso-work, output: ${OUT_DIR})..."
env -u TMPDIR mkarchiso -w "${WORK_DIR}/iso-work" -o "${OUT_DIR}" "${PROFILE_DIR}"
echo "[build-iso] mkarchiso completed"

echo "=== Done! ==="
ls -lh "${OUT_DIR}"/*.iso 2>/dev/null || echo "ISO built in ${OUT_DIR}"
