#!/usr/bin/env python3
import libcalamares
import subprocess
import os

_status = "..."

def pretty_name():
    return "Create user accounts"

def pretty_status_message():
    return _status

def run():
    global _status
    _status = "Creating user..."
    libcalamares.job.setprogress(0)

    gs = libcalamares.globalstorage
    root = gs.value("rootMountPoint")
    if not root:
        return ("No root mount point", "GlobalStorage rootMountPoint is not set")

    username = gs.value("username")
    password = gs.value("password")
    sudoers_group = gs.value("sudoersGroup")

    if not username or not password:
        libcalamares.utils.debug("No user to create, skipping")
        libcalamares.job.setprogress(1.0)
        return None

    libcalamares.utils.debug(f"Creating user: {username}")

    # Mount required filesystems for chroot
    for mp, fstype in [("/proc", "proc"), ("/sys", "sysfs"), ("/dev", None)]:
        target_mp = os.path.join(root, mp.lstrip("/"))
        os.makedirs(target_mp, exist_ok=True)
        if fstype:
            subprocess.run(["mount", "-t", fstype, fstype, target_mp], capture_output=True)
        else:
            subprocess.run(["mount", "--bind", mp, target_mp], capture_output=True)

    libcalamares.job.setprogress(0.3)

    # User creation
    _status = "Creating user account..."
    proc = subprocess.run(
        ["chroot", root, "useradd", "-m", "-G", sudoers_group or "wheel", username],
        capture_output=True, text=True
    )
    if proc.returncode != 0:
        libcalamares.utils.warning(f"Failed to create user: {proc.stderr}")

    libcalamares.job.setprogress(0.5)

    # User password
    _status = "Setting password..."
    proc = subprocess.run(
        ["chroot", root, "sh", "-c", f"echo '{username}:{password}' | chpasswd"],
        capture_output=True, text=True
    )
    if proc.returncode != 0:
        libcalamares.utils.warning(f"Failed to set password: {proc.stderr}")

    libcalamares.job.setprogress(0.7)

    # Root password
    _status = "Setting root password..."
    root_password = gs.value("rootPassword")
    if root_password:
        proc = subprocess.run(
            ["chroot", root, "sh", "-c", f"echo 'root:{root_password}' | chpasswd"],
            capture_output=True, text=True
        )
        if proc.returncode != 0:
            libcalamares.utils.warning(f"Failed to set root password: {proc.stderr}")

    # Unmount
    _status = "Finalizing..."
    for mp, _ in [("/proc", None), ("/sys", None), ("/dev", None)]:
        subprocess.run(["umount", "-l", os.path.join(root, mp.lstrip("/"))], capture_output=True)

    libcalamares.job.setprogress(1.0)
    _status = "User accounts configured"
    libcalamares.utils.debug("User accounts configured")
    return None
