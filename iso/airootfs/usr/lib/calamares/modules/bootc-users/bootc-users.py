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

    # Use the live ISO's useradd with --root to target the installed system
    _status = "Creating user account..."
    proc = subprocess.run(
        ["useradd", "--root", root, "-m", "-G", sudoers_group or "wheel", username],
        capture_output=True, text=True
    )
    if proc.returncode != 0:
        libcalamares.utils.warning(f"Failed to create user: {proc.stderr}")
    else:
        libcalamares.utils.debug(f"User {username} created")

    libcalamares.job.setprogress(0.5)

    # Set user password via chpasswd --root
    _status = "Setting password..."
    proc = subprocess.run(
        ["chpasswd", "--root", root],
        input=f"{username}:{password}\n", capture_output=True, text=True
    )
    if proc.returncode != 0:
        libcalamares.utils.warning(f"Failed to set password: {proc.stderr}")
    else:
        libcalamares.utils.debug("Password set")

    libcalamares.job.setprogress(0.7)

    # Root password
    _status = "Setting root password..."
    root_password = gs.value("rootPassword")
    if root_password:
        proc = subprocess.run(
            ["chpasswd", "--root", root],
            input=f"root:{root_password}\n", capture_output=True, text=True
        )
        if proc.returncode != 0:
            libcalamares.utils.warning(f"Failed to set root password: {proc.stderr}")
        else:
            libcalamares.utils.debug("Root password set")

    libcalamares.job.setprogress(1.0)
    _status = "User accounts configured"
    libcalamares.utils.debug("User accounts configured")
    return None
