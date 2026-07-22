#!/usr/bin/env python3
import libcalamares
import subprocess
import os
import json

def pretty_name():
    return "Install xfce-aerolike system"

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

    libcalamares.job.setprogress(0.1)

    # Bootc temp dir (target disk) — mount an empty overlay over ROOT/tmp,
    # and bind-mount the real tmp dir elsewhere so bootc doesn't see regular dirs.
    os.makedirs(os.path.join(root, "tmp"), exist_ok=True)
    os.makedirs("/mnt/target-tmp", exist_ok=True)
    subprocess.run(["mount", "--bind", os.path.join(root, "tmp"), "/mnt/target-tmp"],
                   capture_output=True)
    subprocess.run(["mount", "--bind", empty_overlay, os.path.join(root, "tmp")],
                   capture_output=True)
    os.environ["TMPDIR"] = "/mnt/target-tmp"

    libcalamares.job.setprogress(0.2)

    if mode == "offline" and os.path.exists(archive):
        libcalamares.utils.debug("Offline mode: extracting image...")

        skopeo_tmp = os.path.join(root, ".skopeo-tmp")
        image_tar = os.path.join(skopeo_tmp, "image.tar")
        oci_dir = os.path.join(root, ".oci-image")
        os.makedirs(skopeo_tmp, exist_ok=True)
        os.makedirs(oci_dir, exist_ok=True)

        libcalamares.job.setprogress(0.3)

        # Decompress archive
        subprocess.run(["zstd", "-d", archive, "-o", image_tar], check=True)

        # Bind-mount skopeo tmp over /var/tmp (space on target disk)
        subprocess.run(["mount", "--bind", skopeo_tmp, "/var/tmp"], capture_output=True)

        libcalamares.job.setprogress(0.4)

        # Copy to OCI layout on target partition
        subprocess.run(
            ["skopeo", "copy", f"docker-archive:{image_tar}", f"oci:{oci_dir}:latest"],
            check=True
        )

        subprocess.run(["umount", "/var/tmp"], capture_output=True)
        os.remove(image_tar)
        os.rmdir(skopeo_tmp)

        libcalamares.job.setprogress(0.6)

        # Bind-mount OCI dir to host path, make private
        os.makedirs("/mnt/oci-staging", exist_ok=True)
        subprocess.run(["mount", "--bind", oci_dir, "/mnt/oci-staging"], capture_output=True)
        subprocess.run(["mount", "--make-private", "/mnt/oci-staging"], capture_output=True)
        subprocess.run(["mount", "--bind", empty_overlay, oci_dir], capture_output=True)

        libcalamares.job.setprogress(0.7)

        libcalamares.utils.debug(f"Running bootc install (offline) with image {image_name}")
        result = subprocess.run([
            "bootc", "install", "to-filesystem",
            "--source-imgref", "oci:/mnt/oci-staging:latest",
            "--generic-image",
            "--skip-fetch-check",
            "--bootloader", "grub",
            "--target-imgref", image_name,
            root
        ], capture_output=True, text=True)
        libcalamares.utils.debug(f"bootc stdout: {result.stdout}")
        libcalamares.utils.debug(f"bootc stderr: {result.stderr}")

        if result.returncode != 0:
            return ("bootc install failed", result.stderr)

        libcalamares.job.setprogress(0.85)

        # Cleanup OCI staging
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

        libcalamares.job.setprogress(0.4)

        libcalamares.utils.debug(f"Running bootc install (online) with image {image_name}")
        result = subprocess.run([
            "bootc", "install", "to-filesystem",
            "--source-imgref", f"docker://{image_name}",
            "--skip-fetch-check",
            "--bootloader", "grub",
            "--target-imgref", image_name,
            root
        ], capture_output=True, text=True)
        libcalamares.utils.debug(f"bootc stdout: {result.stdout}")
        libcalamares.utils.debug(f"bootc stderr: {result.stderr}")

        if result.returncode != 0:
            return ("bootc install failed", result.stderr)

        libcalamares.job.setprogress(0.85)

    libcalamares.job.setprogress(0.9)

    # Final cleanup
    subprocess.run(["umount", os.path.join(root, "tmp")], capture_output=True)
    subprocess.run(["umount", "/mnt/target-tmp"], capture_output=True)
    subprocess.run(["rm", "-rf", os.path.join(root, "tmp"), empty_overlay],
                   capture_output=True)

    libcalamares.job.setprogress(1.0)
    libcalamares.utils.debug("Installation complete!")
    return None
