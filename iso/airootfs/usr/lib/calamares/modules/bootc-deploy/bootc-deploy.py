#!/usr/bin/env python3
import libcalamares
import subprocess
import os
import json
import re
import pty

_status = "Preparing..."

def pretty_name():
    return "Installing system via bootc"

def pretty_status_message():
    return _status

def run_bootc(cmd, start_progress, end_progress):
    """Run bootc install with streaming output via PTY (forces line-buffered TTY output)."""
    global _status
    master_fd, slave_fd = pty.openpty()
    proc = subprocess.Popen(cmd, stdout=slave_fd, stderr=slave_fd, close_fds=True)
    os.close(slave_fd)

    buf = ""
    while True:
        try:
            data = os.read(master_fd, 4096)
        except OSError:
            break
        if not data:
            break
        buf += data.decode("utf-8", errors="replace")
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            line = line.rstrip("\r")
            libcalamares.utils.debug(f"bootc: {line}")
            _status = line

            m = re.search(r"layers already present:\s*(\d+)\s*;\s*layers needed:\s*(\d+)", line)
            if m:
                present, needed = int(m.group(1)), int(m.group(2))
                total = present + needed
                if total > 0:
                    libcalamares.job.setprogress(
                        start_progress + (end_progress - start_progress) * (present / total))
                continue

            m = re.search(r"Pulling layer\s*(\d+)/(\d+)", line)
            if m:
                cur, total = int(m.group(1)), int(m.group(2))
                if total > 0:
                    libcalamares.job.setprogress(
                        start_progress + (end_progress - start_progress) * (cur / total))

            if "Deploying" in line or "Bootloader" in line:
                libcalamares.job.setprogress(
                    start_progress + (end_progress - start_progress) * 0.95)

    proc.wait()
    os.close(master_fd)
    return proc

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

        proc = run_bootc([
            "bootc", "install", "to-filesystem",
            "--source-imgref", f"docker://{image_name}",
            "--generic-image", "--skip-fetch-check", "--bootloader", "grub",
            "--enforce-container-sigpolicy",
            "--target-imgref", image_name, root
        ], 0.2, 0.85)
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

        proc = run_bootc([
            "bootc", "install", "to-filesystem",
            "--source-imgref", "oci:/mnt/oci-staging:latest",
            "--generic-image", "--skip-fetch-check", "--bootloader", "grub",
            "--enforce-container-sigpolicy",
            "--target-imgref", image_name, root
        ], 0.2, 0.85)

    if proc.returncode != 0:
        return ("bootc install failed", proc.stderr)

    libcalamares.job.setprogress(0.85)
    _status = "Installing EFI bootloader config..."

    # GRUB configs (EFI stub + BIOS fix) and fallback BOOTX64.EFI are handled
    # by bootupd via custom files shipped in the arch-bootc base image.
    # bootupd deploys them during install and on bootloader-update.service runs.

    _status = "Cleaning up OCI staging..."

    if mode == "offline":
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
