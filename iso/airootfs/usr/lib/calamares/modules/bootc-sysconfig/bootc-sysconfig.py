#!/usr/bin/env python3
import libcalamares
import os

_status = "..."

def pretty_name():
    return "Configuring system settings"

def pretty_status_message():
    return _status

def run():
    global _status
    _status = "Configuring system..."
    libcalamares.job.setprogress(0)

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
            os.makedirs(etc, exist_ok=True)
            with open(os.path.join(etc, "hostname"), "w") as f:
                f.write(hostname.strip() + "\n")
            libcalamares.utils.debug(f"Set hostname: {hostname}")
        except OSError as e:
            libcalamares.utils.warning(f"Failed to set hostname: {e}")

    libcalamares.job.setprogress(0.3)

    # Timezone
    _status = "Setting timezone..."
    region = gs.value("locationRegion")
    zone = gs.value("locationZone")
    if region and zone:
        tz_path = f"/usr/share/zoneinfo/{region}/{zone}"
        target_tz = os.path.join(etc, "localtime")
        try:
            if os.path.exists(tz_path):
                os.makedirs(etc, exist_ok=True)
                if os.path.exists(target_tz) or os.path.islink(target_tz):
                    os.remove(target_tz)
                os.symlink(tz_path, target_tz)
                libcalamares.utils.debug(f"Set timezone: {region}/{zone}")
            else:
                libcalamares.utils.warning(f"Timezone file not found: {tz_path}")
        except OSError as e:
            libcalamares.utils.warning(f"Failed to set timezone: {e}")

    libcalamares.job.setprogress(0.6)

    # Keyboard
    _status = "Setting keyboard layout..."
    layout = gs.value("xkbLayout")
    if layout:
        vconsole = os.path.join(etc, "vconsole.conf")
        try:
            os.makedirs(etc, exist_ok=True)
            with open(vconsole, "w") as f:
                f.write(f"KEYMAP={layout}\n")
            libcalamares.utils.debug(f"Set keyboard: {layout}")
        except OSError as e:
            libcalamares.utils.warning(f"Failed to set keyboard: {e}")

    libcalamares.job.setprogress(1.0)
    _status = "System settings configured"

    # Auto-login for LightDM (drop-in with alphanumeric priority)
    _status = "Configuring auto-login..."
    autologin_user = gs.value("autologinUser")
    if autologin_user:
        dropin_dir = os.path.join(etc, "lightdm", "lightdm.conf.d")
        dropin = os.path.join(dropin_dir, "90-calamares-autologin.conf")
        try:
            os.makedirs(dropin_dir, exist_ok=True)
            with open(dropin, "w") as f:
                f.write("# Auto-login configured by Calamares installer\n")
                f.write("[Seat:*]\n")
                f.write(f"autologin-user={autologin_user}\n")
                f.write("autologin-session=xfce\n")
            libcalamares.utils.debug(f"Set auto-login for {autologin_user}")
        except OSError as e:
            libcalamares.utils.warning(f"Failed to set auto-login: {e}")

    libcalamares.utils.debug("System settings configured")
    return None
