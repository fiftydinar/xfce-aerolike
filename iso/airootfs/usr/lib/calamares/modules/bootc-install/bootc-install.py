#!/usr/bin/env python3
import libcalamares
import subprocess
import os
import json
import re

_status = "Preparing..."

def pretty_name():
    return "Install xfce-aerolike system"

def pretty_status_message():
    return _status

def _stream_output(proc, phase_start, phase_end, prefix=""):
    """Read stdout line by line and update progress/status."""
    global _status
    blob_re = re.compile(r"^(Copying (blob|config) sha256:[a-f0-9]+)")
    total_blobs = 0
    completed_blobs = 0
    lines = []
    for line in iter(proc.stdout.readline, ""):
        line = line.rstrip()
        lines.append(line)
        libcalamares.utils.debug(f"{prefix}{line}")
        m = blob_re.match(line)
        if m:
            if "blob" in line:
                total_blobs += 1
            _status = line
        elif line.startswith("Copying config"):
            _status = "Copying config..."
        elif line.startswith("Writing manifest"):
            _status = "Writing manifest..."
            libcalamares.job.setprogress(phase_start + (phase_end - phase_start) * 0.95)
        # Count "done" or completed blob lines for progress
        if "done" in line and "blob" in m.group() if m else False:
            completed_blobs += 1
            if total_blobs > 0:
                libcalamares.job.setprogress(
                    phase_start + (phase_end - phase_start) * min(completed_blobs / max(total_blobs, 1), 1.0)
                )
    proc.wait()
    return "".join(lines)

def _run_bootc(base_args, root, progress_start, progress_end):
    global _status
    bootc_args = base_args + ["--generic-image", "--skip-fetch-check", "--bootloader", "grub", root]
    proc = subprocess.Popen(bootc_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    stdout, stderr = proc.communicate()
    libcalamares.utils.debug(f"bootc stdout:\n{stdout}")
    libcalamares.utils.debug(f"bootc stderr:\n{stderr}")
    return stdout, stderr, proc.returncode

def run():
    libcalamares.job.setprogress(0.0)
    global _status
    _status = "Starting installation..."
    libcalamares.utils.debug("Starting install-xfce-aerolike...")

    root = libcalamares.globalstorage.value("rootMountPoint")
    if not root:
        return ("No root mount point", "GlobalStorage rootMountPoint is not set")

    archive = "/opt/install/xfce-aerolike.tar.zst"
    ref_file = "/opt/install/image-ref"
    default_image = "ghcr.io/fiftydinar/xfce-aerolike:latest"

    mode = "offline"
    if os.path.exists("/opt/install/install-mode"):
        with open("/opt/install/install-mode") as f:
            mode = f.read().strip()

    image_name = default_image
    if os.path.exists(ref_file):
        with open(ref_file) as f:
            image_name = f.read().strip()

    empty_overlay = "/tmp/.empty-overlay"
    os.makedirs(empty_overlay, exist_ok=True)

    libcalamares.job.setprogress(0.05)
    _status = "Preparing temporary storage..."

    os.makedirs(os.path.join(root, "tmp"), exist_ok=True)
    os.makedirs("/mnt/target-tmp", exist_ok=True)
    subprocess.run(["mount", "--bind", os.path.join(root, "tmp"), "/mnt/target-tmp"], capture_output=True)
    subprocess.run(["mount", "--bind", empty_overlay, os.path.join(root, "tmp")], capture_output=True)
    os.environ["TMPDIR"] = "/mnt/target-tmp"

    libcalamares.job.setprogress(0.1)

    if mode == "offline" and os.path.exists(archive):
        oci_dir = os.path.join(root, ".oci-image")
        os.makedirs(oci_dir, exist_ok=True)

        libcalamares.job.setprogress(0.15)
        _status = "Decompressing archive..."

        skopeo_tmp = os.path.join(root, ".skopeo-tmp")
        image_tar = os.path.join(skopeo_tmp, "image.tar")
        os.makedirs(skopeo_tmp, exist_ok=True)
        subprocess.run(["zstd", "-d", archive, "-o", image_tar], capture_output=True, check=True)

        libcalamares.job.setprogress(0.2)
        _status = "Converting to OCI layout..."

        skopeo_proc = subprocess.Popen(
            ["skopeo", "copy", f"docker-archive:{image_tar}", f"oci:{oci_dir}:latest"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
        )
        _stream_output(skopeo_proc, 0.2, 0.35, "skopeo: ")
        if skopeo_proc.returncode != 0:
            return ("skopeo copy failed", "See debug log for details")

        libcalamares.job.setprogress(0.35)
        _status = "OCI image ready, mounting..."

        subprocess.run(["rm", "-f", image_tar], capture_output=True)
        subprocess.run(["rm", "-rf", skopeo_tmp], capture_output=True, check=True)

        os.makedirs("/mnt/oci-staging", exist_ok=True)
        subprocess.run(["mount", "--bind", oci_dir, "/mnt/oci-staging"], capture_output=True)
        subprocess.run(["mount", "--make-private", "/mnt/oci-staging"], capture_output=True)
        subprocess.run(["mount", "--bind", empty_overlay, oci_dir], capture_output=True)

        libcalamares.job.setprogress(0.4)

        stdout, stderr, rc = _run_bootc(
            ["bootc", "install", "to-filesystem",
             "--source-imgref", "oci:/mnt/oci-staging:latest",
             "--target-imgref", image_name],
            root, 0.4, 0.85
        )
        if rc != 0:
            return ("bootc install failed", stderr)

        libcalamares.job.setprogress(0.85)
        subprocess.run(["umount", oci_dir], capture_output=True)
        subprocess.run(["umount", "/mnt/oci-staging"], capture_output=True)
        subprocess.run(["rm", "-rf", oci_dir, "/mnt/oci-staging"], capture_output=True)

    elif mode == "online":
        libcalamares.utils.debug("Online mode: resolving latest image...")
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

        libcalamares.job.setprogress(0.3)

        stdout, stderr, rc = _run_bootc(
            ["bootc", "install", "to-filesystem",
             "--source-imgref", f"docker://{image_name}",
             "--target-imgref", image_name],
            root, 0.3, 0.85
        )
        if rc != 0:
            return ("bootc install failed", stderr)

        libcalamares.job.setprogress(0.85)
        _status = "Bootc install complete, cleaning up..."

    libcalamares.job.setprogress(0.9)
    _status = "Final cleanup..."

    subprocess.run(["umount", os.path.join(root, "tmp")], capture_output=True)
    subprocess.run(["umount", "/mnt/target-tmp"], capture_output=True)
    subprocess.run(["rm", "-rf", os.path.join(root, "tmp"), empty_overlay], capture_output=True)

    libcalamares.job.setprogress(1.0)
    _status = "Container image deployed"
    libcalamares.utils.debug("Container image deployed")
    return None
