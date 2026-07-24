#!/usr/bin/env python3
import libcalamares
import subprocess
import os
import json

_status = "Preparing..."

def pretty_name():
    return "Installing system via bootc"

def pretty_status_message():
    return _status

def run():
    global _status
    _status = "Starting..."
    libcalamares.job.setprogress(0)

    root = libcalamares.globalstorage.value("rootMountPoint")
    if not root:
        return ("No root mount point", "GlobalStorage rootMountPoint is not set")

    default_image = "ghcr.io/fiftydinar/xfce-aerolike:latest"
    image_name = default_image

    ref_file = "/opt/install/image-ref"
    if os.path.exists(ref_file):
        with open(ref_file) as f:
            image_name = f.read().strip()

    # Check install mode
    mode = "offline"
    if os.path.exists("/opt/install/install-mode"):
        with open("/opt/install/install-mode") as f:
            mode = f.read().strip()

    if mode == "online":
        libcalamares.utils.debug("Online mode: resolving latest image...")
        _status = "Resolving latest image..."
        if os.path.exists(ref_file):
            with open(ref_file) as f:
                tag = f.read().strip().split(":")[-1]
                tag_prefix = tag.rstrip("0123456789")
                if tag_prefix:
                    result = subprocess.run(
                        ["skopeo", "list-tags", "docker://ghcr.io/fiftydinar/xfce-aerolike"],
                        capture_output=True, text=True
                    )
                    if result.returncode == 0:
                        tags = json.loads(result.stdout).get("Tags", [])
                        matching = sorted([t for t in tags if t.startswith(tag_prefix)], reverse=True)
                        if matching:
                            image_name = f"ghcr.io/fiftydinar/xfce-aerolike:{matching[0]}"

        libcalamares.job.setprogress(0.2)
        _status = "Pulling and installing from registry..."
        proc = subprocess.run([
            "bootc", "install", "to-filesystem",
            "--source-imgref", f"docker://{image_name}",
            "--skip-fetch-check", "--bootloader", "grub",
            "--target-imgref", image_name, root
        ], capture_output=True, text=True)
    else:
        empty_overlay = "/tmp/.empty-overlay"
        oci_dir = os.path.join(root, ".oci-image")
        os.makedirs(empty_overlay, exist_ok=True)

        libcalamares.job.setprogress(0.1)
        _status = "Mounting OCI staging..."

        os.makedirs("/mnt/oci-staging", exist_ok=True)
        subprocess.run(["mount", "--bind", oci_dir, "/mnt/oci-staging"], capture_output=True)
        subprocess.run(["mount", "--make-private", "/mnt/oci-staging"], capture_output=True)
        subprocess.run(["mount", "--bind", empty_overlay, oci_dir], capture_output=True)

        libcalamares.job.setprogress(0.2)

        _status = "Running bootc install..."
        proc = subprocess.run([
            "bootc", "install", "to-filesystem",
            "--source-imgref", "oci:/mnt/oci-staging:latest",
            "--generic-image", "--skip-fetch-check", "--bootloader", "grub",
            "--target-imgref", image_name, root
        ], capture_output=True, text=True)

    libcalamares.utils.debug(f"bootc stdout:\n{proc.stdout}")
    libcalamares.utils.debug(f"bootc stderr:\n{proc.stderr}")

    if proc.returncode != 0:
        return ("bootc install failed", proc.stderr)

    libcalamares.job.setprogress(0.85)
    _status = "Installing EFI bootloader config..."

    # bootupd's grub-static-efi.cfg has RAID detection that incorrectly
    # triggers on single-disk systems (mdraid module auto-loaded).
    # Keep everything else from bootupd's config but drop the RAID check.
    esp_arch = os.path.join(root, "boot/efi/EFI/arch")
    esp_boot = os.path.join(root, "boot/efi/EFI/BOOT")
    os.makedirs(esp_arch, exist_ok=True)
    os.makedirs(esp_boot, exist_ok=True)

    import glob

    # Read BLS entries from target root and generate manual menuentry blocks as fallback
    bls_dir = os.path.join(root, "boot/loader/entries")
    menu_entries = []
    if os.path.isdir(bls_dir):
        for conf in sorted(glob.glob(os.path.join(bls_dir, "*.conf"))):
            entry = {}
            with open(conf) as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#"):
                        continue
                    idx = line.find(" ")
                    if idx > 0:
                        key = line[:idx]
                        val = line[idx+1:].strip()
                        entry[key] = val
            linux_path = entry.get("linux", "")
            initrd_path = entry.get("initrd", "")
            options = entry.get("options", "")
            title = entry.get("title", "Arch Linux")
            if linux_path:
                initrd_line = f"  initrd {initrd_path}\n" if initrd_path else ""
                menu_entries.append(f"""menuentry "{title}" {{
  linux {linux_path} {options}
{initrd_line}}}""")

    bls_entries = "\n\n".join(menu_entries) if menu_entries else ""

    grub_cfg_content = f"""# Generated by bootc-deploy
search --file /boot/grub/grub.cfg --set boot1 --no-floppy
if [ -n "$boot1" ]; then
  set root=$boot1
  set prefix=($boot1)/boot/grub
  . $prefix/grub.cfg
  blscfg --path /boot/loader/entries --enable-fallback
  {bls_entries}
else
  search --file /grub/grub.cfg --set boot0 --no-floppy
  if [ -n "$boot0" ]; then
    set root=$boot0
    set prefix=($boot0)/grub
    . $prefix/grub.cfg
    blscfg --enable-fallback
    {bls_entries}
  fi
fi
boot
"""
    grub_cfg_path = os.path.join(esp_arch, "grub.cfg")
    with open(grub_cfg_path, "w") as f:
        f.write(grub_cfg_content)
    libcalamares.utils.debug(f"Installed {grub_cfg_path}")

    # Install fallback bootloader
    grub_src = os.path.join(esp_arch, "grubx64.efi")
    grub_dst = os.path.join(esp_boot, "BOOTX64.EFI")
    if os.path.exists(grub_src):
        subprocess.run(["cp", grub_src, grub_dst], capture_output=True)
        libcalamares.utils.debug("Installed EFI/BOOT/BOOTX64.EFI fallback")

    _status = "Cleaning up OCI staging..."

    subprocess.run(["umount", oci_dir], capture_output=True)
    subprocess.run(["umount", "/mnt/oci-staging"], capture_output=True)
    subprocess.run(["rm", "-rf", oci_dir, "/mnt/oci-staging"], capture_output=True)

    libcalamares.job.setprogress(0.95)
    _status = "Cleaning up temporary mounts..."

    subprocess.run(["umount", os.path.join(root, "tmp")], capture_output=True)
    subprocess.run(["umount", "/mnt/target-tmp"], capture_output=True)
    subprocess.run(["rm", "-rf", os.path.join(root, "tmp"), empty_overlay], capture_output=True)

    libcalamares.job.setprogress(1.0)
    _status = "Install complete"
    libcalamares.utils.debug("bootc deploy complete")
    return None
