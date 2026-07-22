#!/usr/bin/env python3
import libcalamares
import subprocess
import os
import json
import threading
import select

def pretty_name():
    return "Install xfce-aerolike system"

def _read_progress(r_fd, total_phase_start, total_phase_end):
    """Read bootc JSON progress events from a pipe fd and update Calamares progress."""
    try:
        with os.fdopen(r_fd, "r") as f:
            while True:
                line = f.readline()
                if not line:
                    break
                line = line.strip()
                if not line:
                    continue
                try:
                    event = json.loads(line)
                except json.JSONDecodeError:
                    continue
                evtype = event.get("type")
                if evtype == "ProgressBytes":
                    task = event.get("task", "")
                    bytes_val = event.get("bytes", 0)
                    total = event.get("bytesTotal", 0)
                    desc = event.get("description", task)
                    # Map the progress within the phase
                    if total > 0:
                        phase_progress = bytes_val / total
                        overall = total_phase_start + (total_phase_end - total_phase_start) * min(phase_progress, 1.0)
                        libcalamares.job.setprogress(overall)
    except Exception as e:
        libcalamares.utils.debug(f"Progress reader error: {e}")

def run():
    libcalamares.job.setprogress(0.0)
    libcalamares.utils.debug("Starting install-xfce-aerolike...")

    root = libcalamares.globalstorage.value("rootMountPoint")
    if not root:
        return ("No root mount point", "GlobalStorage rootMountPoint is not set")

    libcalamares.utils.debug(f"Root: {root}")

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

    os.makedirs(os.path.join(root, "tmp"), exist_ok=True)
    os.makedirs("/mnt/target-tmp", exist_ok=True)
    subprocess.run(["mount", "--bind", os.path.join(root, "tmp"), "/mnt/target-tmp"],
                   capture_output=True)
    subprocess.run(["mount", "--bind", empty_overlay, os.path.join(root, "tmp")],
                   capture_output=True)
    os.environ["TMPDIR"] = "/mnt/target-tmp"

    libcalamares.job.setprogress(0.1)

    if mode == "offline" and os.path.exists(archive):
        libcalamares.utils.debug("Offline mode: extracting image...")

        skopeo_tmp = os.path.join(root, ".skopeo-tmp")
        image_tar = os.path.join(skopeo_tmp, "image.tar")
        oci_dir = os.path.join(root, ".oci-image")
        os.makedirs(skopeo_tmp, exist_ok=True)
        os.makedirs(oci_dir, exist_ok=True)

        libcalamares.job.setprogress(0.15)

        subprocess.run(["zstd", "-d", archive, "-o", image_tar], check=True)
        subprocess.run(["mount", "--bind", skopeo_tmp, "/var/tmp"], capture_output=True)

        libcalamares.job.setprogress(0.2)

        subprocess.run(
            ["skopeo", "copy", f"docker-archive:{image_tar}", f"oci:{oci_dir}:latest"],
            check=True
        )

        subprocess.run(["umount", "/var/tmp"], capture_output=True)
        os.remove(image_tar)
        os.rmdir(skopeo_tmp)

        libcalamares.job.setprogress(0.35)

        os.makedirs("/mnt/oci-staging", exist_ok=True)
        subprocess.run(["mount", "--bind", oci_dir, "/mnt/oci-staging"], capture_output=True)
        subprocess.run(["mount", "--make-private", "/mnt/oci-staging"], capture_output=True)
        subprocess.run(["mount", "--bind", empty_overlay, oci_dir], capture_output=True)

        libcalamares.job.setprogress(0.4)

        # Run bootc with progress-fd for JSON progress
        r_fd, w_fd = os.pipe()
        thread = threading.Thread(
            target=_read_progress,
            args=(r_fd, 0.4, 0.85),
            daemon=True
        )
        thread.start()

        libcalamares.utils.debug(f"Running bootc install (offline) with image {image_name}")
        proc = subprocess.Popen([
            "bootc", "install", "to-filesystem",
            "--source-imgref", "oci:/mnt/oci-staging:latest",
            "--generic-image",
            "--skip-fetch-check",
            "--progress-fd", str(w_fd),
            "--bootloader", "grub",
            "--target-imgref", image_name,
            root
        ], pass_fds=(w_fd,), capture_output=True, text=True)

        os.close(w_fd)
        stdout, stderr = proc.communicate()
        thread.join(timeout=5)

        libcalamares.utils.debug(f"bootc stdout: {stdout}")
        libcalamares.utils.debug(f"bootc stderr: {stderr}")

        if proc.returncode != 0:
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
                        matching = sorted([t for t in tags if t.startswith(tag_prefix)],
                                         reverse=True)
                        if matching:
                            image_name = f"ghcr.io/fiftydinar/xfce-aerolike:{matching[0]}"

        libcalamares.job.setprogress(0.3)

        r_fd, w_fd = os.pipe()
        thread = threading.Thread(
            target=_read_progress,
            args=(r_fd, 0.3, 0.85),
            daemon=True
        )
        thread.start()

        libcalamares.utils.debug(f"Running bootc install (online) with image {image_name}")
        proc = subprocess.Popen([
            "bootc", "install", "to-filesystem",
            "--source-imgref", f"docker://{image_name}",
            "--skip-fetch-check",
            "--progress-fd", str(w_fd),
            "--bootloader", "grub",
            "--target-imgref", image_name,
            root
        ], pass_fds=(w_fd,), capture_output=True, text=True)

        os.close(w_fd)
        stdout, stderr = proc.communicate()
        thread.join(timeout=5)

        libcalamares.utils.debug(f"bootc stdout: {stdout}")
        libcalamares.utils.debug(f"bootc stderr: {stderr}")

        if proc.returncode != 0:
            return ("bootc install failed", stderr)

        libcalamares.job.setprogress(0.85)

    libcalamares.job.setprogress(0.9)

    subprocess.run(["umount", os.path.join(root, "tmp")], capture_output=True)
    subprocess.run(["umount", "/mnt/target-tmp"], capture_output=True)
    subprocess.run(["rm", "-rf", os.path.join(root, "tmp"), empty_overlay],
                   capture_output=True)

    libcalamares.job.setprogress(1.0)
    libcalamares.utils.debug("Installation complete!")
    return None
