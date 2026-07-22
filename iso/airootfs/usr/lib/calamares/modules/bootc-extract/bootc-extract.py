#!/usr/bin/env python3
import libcalamares
import subprocess
import os

_status = "Preparing..."

def pretty_name():
    return "Extracting container image"

def pretty_status_message():
    return _status

def run():
    global _status
    _status = "Starting extraction..."
    libcalamares.job.setprogress(0)

    root = libcalamares.globalstorage.value("rootMountPoint")
    if not root:
        return ("No root mount point", "GlobalStorage rootMountPoint is not set")

    archive = "/opt/install/xfce-aerolike.tar.zst"
    if not os.path.exists(archive):
        return ("Archive not found", f"{archive} missing")

    libcalamares.job.setprogress(0.05)
    _status = "Preparing temporary storage..."

    empty_overlay = "/tmp/.empty-overlay"
    os.makedirs(empty_overlay, exist_ok=True)

    os.makedirs(os.path.join(root, "tmp"), exist_ok=True)
    os.makedirs("/mnt/target-tmp", exist_ok=True)
    subprocess.run(["mount", "--bind", os.path.join(root, "tmp"), "/mnt/target-tmp"], capture_output=True)
    subprocess.run(["mount", "--bind", empty_overlay, os.path.join(root, "tmp")], capture_output=True)
    os.environ["TMPDIR"] = "/mnt/target-tmp"

    oci_dir = os.path.join(root, ".oci-image")
    os.makedirs(oci_dir, exist_ok=True)

    libcalamares.job.setprogress(0.2)
    _status = "Decompressing archive..."

    skopeo_tmp = os.path.join(root, ".skopeo-tmp")
    image_tar = os.path.join(skopeo_tmp, "image.tar")
    os.makedirs(skopeo_tmp, exist_ok=True)

    proc = subprocess.run(["zstd", "-d", archive, "-o", image_tar], capture_output=True, text=True)
    if proc.returncode != 0:
        return ("zstd decompress failed", proc.stderr)

    libcalamares.job.setprogress(0.5)
    _status = "Converting to OCI layout..."

    skopeo_proc = subprocess.Popen(
        ["skopeo", "copy", f"docker-archive:{image_tar}", f"oci:{oci_dir}:latest"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    for line in iter(skopeo_proc.stdout.readline, ""):
        line = line.rstrip()
        libcalamares.utils.debug(f"skopeo: {line}")
    skopeo_proc.wait()
    if skopeo_proc.returncode != 0:
        return ("skopeo copy failed", "See debug log for details")

    libcalamares.job.setprogress(0.9)
    _status = "Cleaning up temp files..."

    subprocess.run(["rm", "-f", image_tar], capture_output=True)
    subprocess.run(["rm", "-rf", skopeo_tmp], capture_output=True)

    libcalamares.job.setprogress(1.0)
    _status = "Extraction complete"
    libcalamares.utils.debug("Extraction complete")
    return None
