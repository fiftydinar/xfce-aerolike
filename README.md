# xfce-aerolike

![](assets/showcase.png)

> [!IMPORTANT]  
> This custom image is still WIP and in an alpha phase, so only use this for testing.

> [!IMPORTANT]  
> For any theming issues, report here, don't report to the upstream developers.

Aero-like XFCE custom image based on Arch Linux base bootc image.  
Tries to incorporate as many elements from Aero design as possible, but it won't be a 100% copy of Windows on purpose.

## Theming Credits and Base

For the Aerolike GTK2, GTK3, GTK4 and LightDM theme, I credit:
- ['ReVista' - x35gaming](https://github.com/x35gaming/revista), [slightly modified version by contrarybaton60](https://github.com/contrarybaton60/vista-stuff-xfce4/tree/main/gtk-theme)

For the Qt5 and Qt6 Kvantum theme, I credit:
- ['AeroThemePlasma' - wackyideas](https://gitgud.io/aeroshell/atp/aerothemeplasma)

For the Emerald window decoration theme, I credit:
- ['Aero Glass' - winvistlinux, ILoveNat](https://www.gnome-look.org/p/1835389)

For the X11 cursors theme, I credit:
- ['Windows 7 Aero' - ILexian](https://github.com/lLexian/Windows-7-Aero-Cursors_Linux) + ['Aero Mouse Cursors with Drop Shadow' - Infinality](https://www.xfce-look.org/p/999972/)

For the XFCE-desktop GTK3 theme, I credit:
- ['aeroish-xfce4' - w4lll](https://github.com/w4lll/aeroish-xfce4/tree/main/config)

For the default icon set, I credit:
- ['Obsidian' - madmaxms](https://github.com/madmaxms/iconpack-obsidian)

## Highlights

Now that we know what's the theming base and other defaults, I'll highlight what I added on top of it.

- Image setup in shortly is explained like this:
  - arch-base + dracut + bootc (with composefs) + SystemD + xorg + LightDM + NetworkManager + chronyd + bluez + unbound + avahi + Pipewire + mesa (OpenGL) + vulkan + compiz + emerald + XFCE4 base + XFCE4 goodies like screenshotter and its applet plugins + theming
- Native **bcachefs** root support — the kernel, initramfs, and GRUB modules are placed on a separate ext4 `/boot` partition since GRUB does not support bcachefs. The live ISO installer handles this automatically.
- Uses `unbound` as the DNS resolver (recursive, DNSSEC-validating) instead of `systemd-resolved`, paired with `avahi` for mDNS and `NetworkManager` for connection management. Split-DNS for VPNs is handled via a NetworkManager dispatcher script.
- Uses [facebook's `oomd`](https://github.com/facebookincubator/oomd) for proactive OOM prevention (PSI-based, per-cgroup, kills by memory size/growth) instead of `systemd-oomd`.
- Has automatic seamless system updates enabled (runs atomic `bootc upgrade` once per day).
- Uses `compiz` as the compositing window manager and `emerald` as the window decorator.
- Additional `compiz` defaults that enables blur, snap and grid plugins + blurs taskbar and start menu. Also modified grid plugin to use colors matching the default background. Disabled `sync_to_vblank` (no-op on yserver) and `unredirect_fullscreen_windows` to eliminate alt-tab glitches.
- Modified XFCE-desktop GTK3 theme to make applets size square-consistent, to make all applets use the Aero button hover and press theme and to make Start menu coloring closer to taskbar.
- Preconfigured variables, config and scripts for default theming, which includes: LightDM login screen, GTK2, GTK3, GTK4 (including Adwaita), Qt5, Qt6 and XFCE-desktop.
- Force GTK apps to use server-side window decorations through [gtk-nocsd](https://codeberg.org/MorsMortium/gtk-nocsd)
- Installed and preconfigured Whisker-menu, docklike-taskbar, xfce4-power-manager, network-manager-applet, xfce4-pulseaudio-plugin, system tray, notifier, clock.
- Installs `qt5ct` and `qt6ct` in addition to Kvantum, so the Qt apps behavior can be modified further.
- Uses the cool teal glass background as the default.
- Uses Noto Sans as the font, 9 as the size.
- etc...

## How to install

### Live ISO (Recommended)

Download the latest ISO from the [Releases page](https://github.com/fiftydinar/xfce-aerolike/releases) and write it to a USB drive:

```bash
sudo dd if=xfce-aerolike-*.iso of=/dev/sdX bs=4M status=progress
```

1. Boot from the USB
2. Click the **Install** icon on the desktop
3. Choose **Online** or **Offline** mode:
   - **Online**: pulls the latest image from the registry during installation
   - **Offline**: uses the image bundled on the ISO (no internet required)
4. Follow the Calamares installer steps (partitioning, user creation, etc.)
5. Reboot and boot the `xfce-aerolike` entry

### Switching from Vauxite

If you already have a bootc-based system (e.g. Fedora Vauxite), you can switch directly:

```bash
sudo bootc switch --enforce-container-sigpolicy ghcr.io/fiftydinar/xfce-aerolike:latest
sudo reboot
```

Then create a user and set a password:

```bash
sudo useradd -m -G wheel <username>
sudo passwd <username>
```

## Caveats

This image is based on the experimental work of [arch-bootc](https://github.com/fiftydinar/arch-bootc) base image, so some issues might arise.  

- **Bcachefs root requires a separate ext4 `/boot` partition** — GRUB 2.14 does not include `bcachefs.mod`, so kernels, initramfs, and BLS entries must live on an ext4 boot partition. The Calamares installer handles this automatically.
- Using different initramfs other than `dracut` is unsupported
  - Using `mkinitcpio` and others might work with some modifications, but upstream primarily uses `dracut`, which is also used here
- Secure boot doesn't work and is unsupported  
  - For unsigned kernel by default
- ~~Update sizes are big (around 2GB)~~ (update 2026.07.08: There are partial delta updates with `chunkah` now)
  - ~~This is because `bootc` doesn't have support for more efficient delta updates, so it downloads almost full image. Provided auto-update `bootc` timer won't trigger if the network connection is metered, so you can set that in network settings to disable those updates. Or disable the timer by issuing `systemctl --system disable bootc-fetch-apply-updates.timer` in terminal.~~

