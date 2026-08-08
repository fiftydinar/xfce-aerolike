#!/usr/bin/env python3
import libcalamares
import os
import subprocess
import glob

_status = "..."

def pretty_name():
    return "Configuring system settings"

def pretty_status_message():
    return _status

def deployment_root(root):
    dirs = sorted(glob.glob(os.path.join(root, "ostree/deploy/default/deploy/*.0")))
    return dirs[-1] if dirs else root

def run():
    global _status
    _status = "Configuring system..."
    libcalamares.job.setprogress(0)

    gs = libcalamares.globalstorage
    root = gs.value("rootMountPoint")
    if not root:
        return ("No root mount point", "GlobalStorage rootMountPoint is not set")

    deploy = deployment_root(root)
    etc = os.path.join(deploy, "etc")

    # Remount both roots rw
    for path in [root, deploy]:
        subprocess.run(["mount", "-o", "remount,rw", path], capture_output=True)
        subprocess.run(["chattr", "-i", path], capture_output=True)

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

    # Locale / language
    _status = "Setting language and locale..."
    locale_conf = gs.value("localeConf")
    if locale_conf and isinstance(locale_conf, dict):
        try:
            os.makedirs(etc, exist_ok=True)
            with open(os.path.join(etc, "locale.conf"), "w") as f:
                for key in ["LANG", "LC_NUMERIC", "LC_TIME", "LC_MONETARY",
                            "LC_PAPER", "LC_NAME", "LC_ADDRESS",
                            "LC_TELEPHONE", "LC_MEASUREMENT", "LC_IDENTIFICATION"]:
                    val = locale_conf.get(key)
                    if val:
                        f.write(f"{key}={val}\n")
            libcalamares.utils.debug(f"Locale written: {locale_conf.get('LANG', '')}")
        except OSError as e:
            libcalamares.utils.warning(f"Failed to set locale: {e}")
    else:
        libcalamares.utils.debug("No locale configuration in GlobalStorage")

    # Keyboard
    _status = "Setting keyboard layout..."
    layout = gs.value("keyboardLayout")
    variant = gs.value("keyboardVariant") or ""
    if layout:
        # Console keymap: always 'us' as safe default. The X11 config below handles
        # the actual layout with variants and switching; console mappings are too
        # numerous to maintain and often have different names than X11 layouts.
        vconsole = os.path.join(etc, "vconsole.conf")
        try:
            os.makedirs(etc, exist_ok=True)
            with open(vconsole, "w") as f:
                f.write("KEYMAP=us\n")
            libcalamares.utils.debug("Set console keymap: us")
        except OSError as e:
            libcalamares.utils.warning(f"Failed to set console keymap: {e}")

        # X11 keyboard config (for desktop environment)
        xorg_dir = os.path.join(etc, "X11", "xorg.conf.d")
        xorg_conf = os.path.join(xorg_dir, "00-keyboard.conf")
        try:
            os.makedirs(xorg_dir, exist_ok=True)
            model = gs.value("keyboardModel") or ""
            add_layout = gs.value("keyboardAdditionalLayout") or ""
            add_variant = gs.value("keyboardAdditionalVariant") or ""
            switcher = gs.value("keyboardGroupSwitcher") or ""
            model_line = f'\tOption "XkbModel" "{model}"\n' if model else ""
            variant_line = f'\tOption "XkbVariant" "{variant}"\n' if variant else ""
            switcher_line = ""
            if add_layout:
                layout = f"{layout},{add_layout}"
                variant_combined = f"{variant},{add_variant}" if add_variant else f"{variant},"
                variant_line = f'\tOption "XkbVariant" "{variant_combined}"\n'
                switcher_line = f'\tOption "XkbOptions" "{switcher}"\n' if switcher else ""
            with open(xorg_conf, "w") as f:
                f.write('Section "InputClass"\n'
                        '\tIdentifier "system-keyboard"\n'
                        '\tMatchIsKeyboard "on"\n'
                        f'{model_line}'
                        f'\tOption "XkbLayout" "{layout}"\n'
                        f'{variant_line}'
                        f'{switcher_line}'
                        'EndSection\n')
            libcalamares.utils.debug(f"Set X11 keyboard: {layout}")
        except OSError as e:
            libcalamares.utils.warning(f"Failed to set X11 keyboard: {e}")

    libcalamares.job.setprogress(1.0)
    _status = "System settings configured"

    # Auto-login for LightDM (drop-in with alphanumeric priority)
    _status = "Configuring auto-login..."
    autologin_user = gs.value("autoLoginUser") if gs.contains("autoLoginUser") else ""
    if autologin_user:
        # LightDM's lightdm-autologin PAM service requires the user to be in
        # the 'autologin' group (pam_succeed_if user ingroup autologin).
        # Without this the PAM check fails and autologin falls back to the
        # greeter.
        subprocess.run(["chroot", deploy, "groupadd", "-f", "autologin"], capture_output=True)
        subprocess.run(["chroot", deploy, "gpasswd", "-a", autologin_user, "autologin"], capture_output=True)
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

    # Ensure LightDM shows user list (not just "Other...")
    _status = "Configuring greeter..."
    greeter_conf = os.path.join(etc, "lightdm", "lightdm-gtk-greeter.conf")
    try:
        os.makedirs(os.path.dirname(greeter_conf), exist_ok=True)
        # Replace hide-users if present, otherwise append
        if not os.path.exists(greeter_conf):
            with open(greeter_conf, "w") as f:
                f.write("[greeter]\nhide-users=false\n")
        else:
            subprocess.run(["sed", "-i",
                "s/^hide-users=.*$/hide-users=false/; t; $a hide-users=false",
                greeter_conf], capture_output=True)
        libcalamares.utils.debug("Configured greeter to show users")
    except OSError as e:
        libcalamares.utils.warning(f"Failed to configure greeter: {e}")

    # Create AccountsService entry so user appears in greeter
    # (writes to persistent ostree var, not deployment's var which is overridden on boot)
    username = gs.value("username")
    if username:
        accounts_dir = os.path.join(root, "ostree", "deploy", "default", "var", "lib", "AccountsService", "users")
        try:
            os.makedirs(accounts_dir, exist_ok=True)
            with open(os.path.join(accounts_dir, username), "w") as f:
                f.write(f"[User]\nXSession=xfce\nIcon=\n")
            libcalamares.utils.debug(f"Created AccountsService entry for {username}")
        except OSError as e:
            libcalamares.utils.warning(f"Failed to create AccountsService entry: {e}")

    # Remount both roots ro
    for path in [deploy, root]:
        subprocess.run(["mount", "-o", "remount,ro", path], capture_output=True)

    libcalamares.utils.debug("System settings configured")
    return None
