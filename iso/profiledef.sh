#!/usr/bin/env bash
# shellcheck disable=SC2034

iso_name="xfce-aerolike"
iso_label="XFCELK_$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y%m)"
iso_publisher="fiftydinar <https://github.com/fiftydinar>"
iso_application="xfce-aerolike Live/Installer DVD"
iso_version="$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y%m%d)${ISO_VERSION_SUFFIX:-}"
install_dir="arch"
buildmodes=('iso')
bootmodes=('bios.syslinux' 'uefi.grub')
pacman_conf="pacman.conf"
airootfs_image_type="squashfs"
# -no-duplicates: workaround for a data race in mksquashfs 4.7.x, fixed only in
# the unreleased 4.7.6 ("Fix data race when reading fragments in duplicate
# checking (caused by elimination of 'fragment block stall' in 4.7)").
# Duplicate checking is the only thing that reads compressed fragments back out
# of the in-memory fragment cache, and with multi-threaded xz on a 4 GiB image
# that read-back intermittently fails with:
#   FATAL ERROR: xz uncompress failed with error code 9
# (LZMA_DATA_ERROR), aborting the build ~20 minutes into squashing.
# Dropping cross-file deduplication costs a little image size and removes the
# failure mode entirely. Revert once squashfs-tools 4.7.6 ships.
airootfs_image_tool_options=('-comp' 'xz' '-Xbcj' 'x86' '-b' '1M' '-Xdict-size' '1M' '-no-duplicates')
bootstrap_tarball_compression=('zstd' '-c' '-T0' '--auto-threads=logical' '--long' '-19')
file_permissions=(
  ["/etc/shadow"]="0:0:400"
  ["/root"]="0:0:750"
  ["/opt/install/xfce-aerolike.tar.zst"]="0:0:644"

)
