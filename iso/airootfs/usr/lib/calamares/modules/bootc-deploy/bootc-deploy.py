#!/usr/bin/env python3
import libcalamares
import subprocess
import os

_status = "Preparing..."

def pretty_name():
    return "Install system via bootc"

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
