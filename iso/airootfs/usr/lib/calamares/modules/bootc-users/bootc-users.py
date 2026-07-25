#!/usr/bin/env python3
import libcalamares
import subprocess
import os
import glob

_status = "..."

def pretty_name():
    return "Creating user accounts"

def pretty_status_message():
    return _status

def deployment_root(root):
    """Find the active ostree deployment root under the Calamares mount point."""
    dirs = sorted(glob.glob(os.path.join(root, "ostree/deploy/default/deploy/*.0")))
    return dirs[-1] if dirs else root

def run():
    global _status
    _status = "Creating user..."
    libcalamares.job.setprogress(0)

    gs = libcalamares.globalstorage
    root = gs.value("rootMountPoint")
    if not root:
        return ("No root mount point", "GlobalStorage rootMountPoint is not set")

    deploy = deployment_root(root)

    username = gs.value("username")
    password = gs.value("password")
    sudoers_group = gs.value("sudoersGroup")

    if not username or not password:
        libcalamares.utils.debug("No user to create, skipping")
        libcalamares.job.setprogress(1.0)
        return None

    libcalamares.utils.debug(f"Creating user: {username}")

    # Remount both roots rw
    for path in [root, deploy]:
        subprocess.run(["mount", "-o", "remount,rw", path], capture_output=True)
        subprocess.run(["chattr", "-i", path], capture_output=True)

    # Create wheel group (may not exist in bootc image)
    subprocess.run(["groupadd", "--root", deploy, "-f", "wheel"], capture_output=True)

    # Create user in deployment
    _status = "Creating user account..."
    proc = subprocess.run(
        ["useradd", "--root", deploy, "-m", "-G", sudoers_group or "wheel", username],
        capture_output=True, text=True
    )
    if proc.returncode != 0:
        libcalamares.utils.warning(f"Failed to create user: {proc.stderr}")
    else:
        libcalamares.utils.debug(f"User {username} created")

    libcalamares.job.setprogress(0.5)

    # Set user password via chpasswd (uses deployment's own PAM and crypt)
    _status = "Setting password..."
    proc = subprocess.run(
        ["chpasswd", "--root", deploy],
        input=f"{username}:{password}\n", capture_output=True, text=True
    )
    if proc.returncode == 0:
        libcalamares.utils.debug("Password set")
    else:
        libcalamares.utils.warning(f"Failed to set password: {proc.stderr}")

    libcalamares.job.setprogress(0.7)

    # Root password
    _status = "Setting root password..."
    root_password = gs.value("rootPassword")
    if root_password:
        proc = subprocess.run(
            ["chpasswd", "--root", deploy],
            input=f"root:{root_password}\n", capture_output=True, text=True
        )
        if proc.returncode == 0:
            libcalamares.utils.debug("Root password set")
        else:
            libcalamares.utils.warning(f"Failed to set root password: {proc.stderr}")

    # Remount ro
    for path in [deploy, root]:
        subprocess.run(["mount", "-o", "remount,ro", path], capture_output=True)

    libcalamares.job.setprogress(1.0)
    _status = "User accounts configured"
    libcalamares.utils.debug("User accounts configured")
    return None
