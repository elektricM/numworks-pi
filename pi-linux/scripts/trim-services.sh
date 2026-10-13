#!/bin/bash
# Free RAM and CPU on the Pi Zero 2 W by masking services the calculator
# setup never uses. Run as pi (it uses sudo for the system units):
#   ./pi-linux/scripts/trim-services.sh
# Undo one with `systemctl unmask <unit>` (add --user for user units).
#
# Kept on purpose: avahi (nwpi.local), NetworkManager, wpa_supplicant,
# zerotier-one, ssh, gvfsd (pcmanfm's trash), kanshi (display modes).
set -e

# apt metadata refreshes in the background; also comes back with upgrades
SYSTEM="packagekit.service packagekit-offline-update.service
cups.service cups.socket cups.path cups-browsed.service
ModemManager.service"

# Removable media (phones, cameras, online accounts), desktop portals and
# audio: the calculator has no media slots, no sandboxed apps, no speaker
USER_UNITS="gvfs-afc-volume-monitor.service gvfs-goa-volume-monitor.service
gvfs-gphoto2-volume-monitor.service gvfs-mtp-volume-monitor.service
gvfs-udisks2-volume-monitor.service
xdg-desktop-portal.service xdg-desktop-portal-gtk.service
xdg-desktop-portal-wlr.service xdg-document-portal.service
xdg-permission-store.service
pipewire.service pipewire.socket pipewire-pulse.service pipewire-pulse.socket
wireplumber.service filter-chain.service mpris-proxy.service"

for u in $SYSTEM; do
	sudo systemctl disable --now "$u" 2>/dev/null || true
	sudo systemctl mask "$u" >/dev/null 2>&1 || true
done
for u in $USER_UNITS; do
	systemctl --user disable --now "$u" 2>/dev/null || true
	systemctl --user mask "$u" >/dev/null 2>&1 || true
done
echo "Masked. Reboot to start clean: sudo reboot"
