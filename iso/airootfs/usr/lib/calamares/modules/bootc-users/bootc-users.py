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

def unobscure(s):
    """Reverse Calamares::String::obscure (bidirectional XOR)."""
    return "".join(c if ord(c) <= 0x21 else chr(0x1001F - ord(c)) for c in s)

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

    # Home directories live at the persistent ostree /var on the partition root,
    # not inside the deployment's var/ (which is overridden by a bind mount on boot).
    # The persistent var is at /ostree/deploy/default/var/ on the partition.
    var_home_persist = os.path.join(root, "ostree", "deploy", "default", "var", "home")

    username = gs.value("username")
    # Password in GS is obscured via Calamares::String::obscure(); reverse it
    password = unobscure(gs.value("password"))

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
    subprocess.run(["chroot", deploy, "groupadd", "-f", "wheel"], capture_output=True)

    # Create user using deployment's own useradd
    _status = "Creating user account..."
    proc = subprocess.run(
        ["chroot", deploy, "useradd", "-m", "-G", "wheel", username],
        capture_output=True, text=True
    )
    if proc.returncode != 0:
        libcalamares.utils.warning(f"Failed to create user: {proc.stderr}")
    else:
        libcalamares.utils.debug(f"User {username} created")

    # Create home directory in persistent /var (bootc overrides deployment's /var with a mount on boot)
    _status = "Creating home directory..."
    os.makedirs(var_home_persist, exist_ok=True)
    home_path = os.path.join(var_home_persist, username)
    os.makedirs(home_path, exist_ok=True)
    uid = gid = 1000
    passwd_path = os.path.join(deploy, "etc", "passwd")
    if os.path.exists(passwd_path):
        with open(passwd_path) as f:
            for line in f:
                parts = line.strip().split(":")
                if len(parts) >= 4 and parts[0] == username:
                    uid = int(parts[2])
                    gid = int(parts[3])
                    break
    os.chown(home_path, uid, gid)
    os.chmod(home_path, 0o700)

    libcalamares.job.setprogress(0.5)

    # Set password using deployment's own chpasswd (reads stdin, unlike passwd which reads /dev/tty)
    _status = "Setting password..."
    for user in [username, "root"]:
        proc = subprocess.run(
            ["chroot", deploy, "chpasswd"],
            input=f"{user}:{password}\n", capture_output=True, text=True
        )
        if proc.returncode == 0:
            libcalamares.utils.debug(f"Password set for {user}")
        else:
            libcalamares.utils.warning(f"Failed to set password for {user}: {proc.stderr}")

    libcalamares.job.setprogress(0.7)

    # Remount ro
    for path in [deploy, root]:
        subprocess.run(["mount", "-o", "remount,ro", path], capture_output=True)

    libcalamares.job.setprogress(1.0)
    _status = "User accounts configured"
    libcalamares.utils.debug("User accounts configured")
    return None
