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

        # Check if bootc supports --progress-fd
        progress_check = subprocess.run(
            ["bootc", "install", "--help"], capture_output=True, text=True
        )
        has_progress_fd = "--progress-fd" in progress_check.stdout

        libcalamares.utils.debug(f"Running bootc install (offline) with image {image_name}")
        if has_progress_fd:
            r_fd, w_fd = os.pipe()
            thread = threading.Thread(
                target=_read_progress,
                args=(r_fd, 0.4, 0.85),
                daemon=True
            )
            thread.start()
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
        else:
            proc = subprocess.run([
                "bootc", "install", "to-filesystem",
                "--source-imgref", "oci:/mnt/oci-staging:latest",
                "--generic-image",
                "--skip-fetch-check",
                "--bootloader", "grub",
                "--target-imgref", image_name,
                root
            ], capture_output=True, text=True)
            stdout, stderr = proc.stdout, proc.stderr

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

        libcalamares.utils.debug(f"Running bootc install (online) with image {image_name}")
        progress_check = subprocess.run(
            ["bootc", "install", "--help"], capture_output=True, text=True
        )
        has_progress_fd = "--progress-fd" in progress_check.stdout
        if has_progress_fd:
            r_fd, w_fd = os.pipe()
            thread = threading.Thread(
                target=_read_progress,
                args=(r_fd, 0.3, 0.85),
                daemon=True
            )
            thread.start()
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
        else:
            proc = subprocess.run([
                "bootc", "install", "to-filesystem",
                "--source-imgref", f"docker://{image_name}",
                "--skip-fetch-check",
                "--bootloader", "grub",
                "--target-imgref", image_name,
                root
            ], capture_output=True, text=True)
            stdout, stderr = proc.stdout, proc.stderr

        libcalamares.utils.debug(f"bootc stdout: {stdout}")
        libcalamares.utils.debug(f"bootc stderr: {stderr}")

        if proc.returncode != 0:
            return ("bootc install failed", stderr)

        libcalamares.job.setprogress(0.85)

    # === Post-install configuration ===
    libcalamares.job.setprogress(0.87)
    gs = libcalamares.globalstorage
    etc = os.path.join(root, "etc")

    # Hostname
    hostname = gs.value("hostname")
    if hostname:
        try:
            with open(os.path.join(etc, "hostname"), "w") as f:
                f.write(hostname.strip() + "\n")
        except OSError as e:
            libcalamares.utils.warning(f"Failed to set hostname: {e}")

    # Timezone
    region = gs.value("locationRegion")
    zone = gs.value("locationZone")
    if region and zone:
        tz_path = f"/usr/share/zoneinfo/{region}/{zone}"
        target_tz = os.path.join(etc, "localtime")
        try:
            if os.path.exists(target_tz) or os.path.islink(target_tz):
                os.remove(target_tz)
            os.symlink(tz_path, target_tz)
        except OSError as e:
            libcalamares.utils.warning(f"Failed to set timezone: {e}")

    # Keyboard
    layout = gs.value("xkbLayout")
    if layout:
        vconsole = os.path.join(etc, "vconsole.conf")
        try:
            with open(vconsole, "w") as f:
                f.write(f"KEYMAP={layout}\n")
        except OSError as e:
            libcalamares.utils.warning(f"Failed to set keyboard: {e}")

    libcalamares.job.setprogress(0.9)

    # User creation
    username = gs.value("username")
    password = gs.value("password")
    sudoers_group = gs.value("sudoersGroup")
    if username and password:
        try:
            # Mount pseudo-fs for chroot
            for mp, fstype in [("/proc", "proc"), ("/sys", "sysfs"), ("/dev", None)]:
                target_mp = os.path.join(root, mp.lstrip("/"))
                os.makedirs(target_mp, exist_ok=True)
                if fstype:
                    subprocess.run(["mount", "-t", fstype, fstype, target_mp],
                                   capture_output=True)
                else:
                    subprocess.run(["mount", "--bind", mp, target_mp],
                                   capture_output=True)

            subprocess.run(["chroot", root, "useradd", "-m", "-G",
                            sudoers_group or "wheel", username],
                           capture_output=True, text=True, check=True)

            # Set password via chpasswd
            proc = subprocess.run(
                ["chroot", root, "sh", "-c", f"echo '{username}:{password}' | chpasswd -e"],
                capture_output=True, text=True
            )
            if proc.returncode != 0:
                libcalamares.utils.warning(f"Failed to set password: {proc.stderr}")

            # Unmount pseudo-fs
            for mp, _ in [("/proc", None), ("/sys", None), ("/dev", None)]:
                subprocess.run(["umount", "-l", os.path.join(root, mp.lstrip("/"))],
                               capture_output=True)
        except Exception as e:
            libcalamares.utils.warning(f"Failed to create user: {e}")

    libcalamares.job.setprogress(0.95)

    subprocess.run(["umount", os.path.join(root, "tmp")], capture_output=True)
    subprocess.run(["umount", "/mnt/target-tmp"], capture_output=True)
    subprocess.run(["rm", "-rf", os.path.join(root, "tmp"), empty_overlay],
                   capture_output=True)

    libcalamares.job.setprogress(1.0)
    libcalamares.utils.debug("Installation complete!")
    return None
