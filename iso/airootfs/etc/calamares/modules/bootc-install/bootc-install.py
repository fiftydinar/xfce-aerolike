#!/usr/bin/env python3
import libcalamares
import subprocess
import os
import shlex

def pretty_name():
    return "Install xfce-aerolike system"

def run():
    libcalamares.job.setprogress(0.0)
    libcalamares.utils.debug("Starting install-xfce-aerolike...")

    gs = libcalamares.globalstorage
    root = gs.value("rootMountPoint")
    if not root:
        return ("No root mount point", "GlobalStorage rootMountPoint is not set")

    libcalamares.utils.debug(f"Root: {root}")

    archive = "/opt/install/xfce-aerolike.tar.zst"
    ref_file = "/opt/install/image-ref"
    default_image = "ghcr.io/fiftydinar/xfce-aerolike:latest"

    mode_file = "/opt/install/install-mode"
    mode = "offline"
    if os.path.exists(mode_file):
        with open(mode_file) as f:
            mode = f.read().strip()

    podman_root = os.path.join(root, "var/lib/containers-staging")
    os.makedirs(podman_root, exist_ok=True)

    tmpdir = os.path.join(root, "tmp")
    os.makedirs(tmpdir, exist_ok=True)
    os.environ["TMPDIR"] = tmpdir

    image_name = default_image

    # Step 1: Load or pull image (0% - 30%)
    libcalamares.job.setprogress(0.1)
    if mode == "offline" and os.path.exists(archive):
        libcalamares.utils.debug("Offline mode: loading image from archive...")
        with open(archive) as f:
            proc = subprocess.run(
                ["zstd", "-d", "-c"],
                stdin=f, capture_output=True,
                preexec_fn=lambda: None  # avoid inheriting signals
            )
        proc = subprocess.run(
            ["podman", "--root", podman_root, "load"],
            input=proc.stdout, capture_output=True, text=True
        )
        if proc.returncode != 0:
            return ("Failed to load image", proc.stderr)
        libcalamares.utils.debug(f"Load output: {proc.stdout}")

        if os.path.exists(ref_file):
            with open(ref_file) as f:
                image_name = f.read().strip()
    else:
        libcalamares.utils.debug("Online mode: pulling image...")
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
                        import json
                        tags = json.loads(result.stdout).get("Tags", [])
                        matching = sorted([t for t in tags if t.startswith(tag_prefix)], reverse=True)
                        if matching:
                            image_name = f"ghcr.io/fiftydinar/xfce-aerolike:{matching[0]}"

        proc = subprocess.run(
            ["podman", "--root", podman_root, "pull", image_name],
            capture_output=True, text=True
        )
        if proc.returncode != 0:
            return ("Failed to pull image", proc.stderr)

    libcalamares.job.setprogress(0.3)

    # Step 2: Create storage.conf for inner container
    os.makedirs(os.path.join(root, "etc/containers"), exist_ok=True)
    with open(os.path.join(root, "etc/containers/storage.conf"), "w") as f:
        f.write("[storage]\ndriver = \"overlay\"\n\n[storage.options]\nmount_program = \"/usr/bin/fuse-overlayfs\"\n")

    # Fix newuidmap
    subprocess.run(["chmod", "u+s", "/usr/bin/newuidmap", "/usr/bin/newgidmap"],
                   capture_output=True)

    libcalamares.job.setprogress(0.4)

    # Step 3: Bind-mount storage to default path (for bootc's host-ns podman inspect)
    os.makedirs("/var/lib/containers/storage", exist_ok=True)
    subprocess.run(["mount", "--bind", podman_root, "/var/lib/containers/storage"],
                   capture_output=True)

    libcalamares.job.setprogress(0.5)

    # Step 4: Switch tmpdir to host tmpfs (bootc rejects empty regular dirs)
    os.environ["TMPDIR"] = "/tmp/install-tmp"
    os.makedirs("/tmp/install-tmp", exist_ok=True)
    subprocess.run(["rm", "-rf", os.path.join(root, "tmp")], capture_output=True)

    libcalamares.job.setprogress(0.6)

    # Step 5: Run bootc install
    libcalamares.utils.debug(f"Running bootc install with image {image_name}...")
    cmd = [
        "podman", "--root", podman_root, "run", "--rm",
        "--privileged", "--pid=host", "--ipc=host",
        "-e", "CONTAINERS_STORAGE_CONF=/etc/containers/storage.conf",
        "-v", "/dev:/dev",
        "-v", "/dev/fuse:/dev/fuse",
        "-v", f"{root}:/target",
        "-v", "/var/lib/containers/storage:/var/lib/containers/storage",
        "-v", f"{root}/etc/containers/storage.conf:/etc/containers/storage.conf",
        "-v", "/usr/bin/fuse-overlayfs:/usr/bin/fuse-overlayfs:ro",
        "-v", "/usr/bin/fusermount3:/usr/bin/fusermount3:ro",
        "-v", "/usr/lib/libfuse3.so.4:/usr/lib/libfuse3.so.4:ro",
        image_name,
        "bootc", "install", "to-filesystem",
        "--bootloader", "grub",
        "--target-imgref", image_name,
        "/target"
    ]

    proc = subprocess.run(cmd, capture_output=True, text=True)
    libcalamares.utils.debug(f"bootc stdout: {proc.stdout}")
    libcalamares.utils.debug(f"bootc stderr: {proc.stderr}")

    if proc.returncode != 0:
        return ("bootc install failed", proc.stderr)

    libcalamares.job.setprogress(0.9)

    # Step 6: Cleanup
    subprocess.run(["umount", "/var/lib/containers/storage"], capture_output=True)
    subprocess.run(["rm", "-rf", podman_root, "/tmp/install-tmp"], capture_output=True)

    libcalamares.job.setprogress(1.0)
    libcalamares.utils.debug("Installation complete!")
    return None
