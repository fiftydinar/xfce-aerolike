#!/usr/bin/env python3
import libcalamares
import subprocess
import os

_status = "Configuring..."

def pretty_name():
    return "Configure installed system"

def pretty_status_message():
    return _status

def run():
    global _status
    _status = "Configuring installed system..."
    libcalamares.job.setprogress(0.0)
    gs = libcalamares.globalstorage
    root = gs.value("rootMountPoint")
    if not root:
        return ("No root mount point", "GlobalStorage rootMountPoint is not set")

    etc = os.path.join(root, "etc")

    # Hostname
    _status = "Setting hostname..."
    hostname = gs.value("hostname")
    if hostname:
        try:
            with open(os.path.join(etc, "hostname"), "w") as f:
                f.write(hostname.strip() + "\n")
            libcalamares.utils.debug(f"Set hostname: {hostname}")
        except OSError as e:
            libcalamares.utils.warning(f"Failed to set hostname: {e}")

    libcalamares.job.setprogress(0.2)

    # Timezone
    _status = "Setting timezone..."
    region = gs.value("locationRegion")
    zone = gs.value("locationZone")
    if region and zone:
        tz_path = f"/usr/share/zoneinfo/{region}/{zone}"
        target_tz = os.path.join(etc, "localtime")
        try:
            if os.path.exists(target_tz) or os.path.islink(target_tz):
                os.remove(target_tz)
            os.symlink(tz_path, target_tz)
            libcalamares.utils.debug(f"Set timezone: {region}/{zone}")
        except OSError as e:
            libcalamares.utils.warning(f"Failed to set timezone: {e}")

    libcalamares.job.setprogress(0.4)

    # Keyboard
    _status = "Setting keyboard layout..."
    layout = gs.value("xkbLayout")
    if layout:
        vconsole = os.path.join(etc, "vconsole.conf")
        try:
            with open(vconsole, "w") as f:
                f.write(f"KEYMAP={layout}\n")
            libcalamares.utils.debug(f"Set keyboard: {layout}")
        except OSError as e:
            libcalamares.utils.warning(f"Failed to set keyboard: {e}")

    libcalamares.job.setprogress(0.6)

    # User creation
    _status = "Creating user account..."
    username = gs.value("username")
    password = gs.value("password")
    sudoers_group = gs.value("sudoersGroup")
    if username and password:
        libcalamares.utils.debug(f"Creating user: {username}")
        try:
            for mp, fstype in [("/proc", "proc"), ("/sys", "sysfs"), ("/dev", None)]:
                target_mp = os.path.join(root, mp.lstrip("/"))
                os.makedirs(target_mp, exist_ok=True)
                if fstype:
                    subprocess.run(["mount", "-t", fstype, fstype, target_mp],
                                   capture_output=True)
                else:
                    subprocess.run(["mount", "--bind", mp, target_mp],
                                   capture_output=True)

            libcalamares.job.setprogress(0.7)

            subprocess.run(["chroot", root, "useradd", "-m", "-G",
                            sudoers_group or "wheel", username],
                           capture_output=True, text=True, check=True)

            libcalamares.job.setprogress(0.8)

            proc = subprocess.run(
                ["chroot", root, "sh", "-c",
                 f"echo '{username}:{password}' | chpasswd"],
                capture_output=True, text=True
            )
            if proc.returncode != 0:
                libcalamares.utils.warning(f"Failed to set password: {proc.stderr}")

            for mp, _ in [("/proc", None), ("/sys", None), ("/dev", None)]:
                subprocess.run(["umount", "-l", os.path.join(root, mp.lstrip("/"))],
                               capture_output=True)
        except Exception as e:
            libcalamares.utils.warning(f"Failed to create user: {e}")

    _status = "Configuration complete"
    libcalamares.job.setprogress(1.0)
    libcalamares.utils.debug("Configuration complete!")
    return None
