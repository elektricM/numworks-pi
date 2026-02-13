#!/bin/bash
# Full Pi setup for NumWorks calculator integration
# Run on the Pi after cloning the repo: sudo ./pi-linux/scripts/setup.sh
#
# If a kernel upgrade happens during apt, the script will ask you to
# reboot and re-run it so the driver builds against the new kernel.
#
# Safe to run multiple times — idempotent operations.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PI_LINUX="$(dirname "$SCRIPT_DIR")"
CONFIG_DIR="$PI_LINUX/config"

if [ "$EUID" -ne 0 ]; then
    echo "Run as root: sudo $0"
    exit 1
fi

ACTUAL_USER="${SUDO_USER:-$(logname 2>/dev/null || echo pi)}"
ACTUAL_HOME=$(eval echo "~$ACTUAL_USER")

echo "=== NumWorks Pi Setup ==="
echo "User: $ACTUAL_USER  Home: $ACTUAL_HOME"
echo ""

# --- System update ---
echo "--- Updating system ---"
KERNEL_BEFORE=$(uname -r)
apt-get update
apt-get full-upgrade -y

# Check if a new kernel was installed for our architecture
NEW_KERNEL=$(ls -1d /lib/modules/*-v7* 2>/dev/null | sort -V | tail -1 | xargs basename 2>/dev/null || true)
if [ -n "$NEW_KERNEL" ] && [ "$NEW_KERNEL" != "$KERNEL_BEFORE" ]; then
    echo ""
    echo "*** Kernel upgraded: $KERNEL_BEFORE -> $NEW_KERNEL ***"
    echo "*** Reboot and re-run this script to build against the new kernel. ***"
    echo ""
    echo "  sudo reboot"
    echo "  # after reboot:"
    echo "  sudo bash ~/numworks-pi/pi-linux/scripts/setup.sh"
    exit 0
fi

# --- Build dependencies ---
echo ""
echo "--- Installing build dependencies ---"
apt-get install -y linux-headers-$(uname -r) build-essential mesa-utils

# --- Display driver ---
echo ""
echo "--- Building drm-spifb kernel module ---"
cd "$PI_LINUX/drm-spifb"
make
mkdir -p "/lib/modules/$(uname -r)/extra"
cp drm-spifb.ko "/lib/modules/$(uname -r)/extra/"
depmod -a

# --- Device Tree overlay ---
echo ""
echo "--- Installing DT overlay ---"
"$SCRIPT_DIR/install-overlay.sh"

# --- Keyboard daemon ---
echo ""
echo "--- Building and installing nwinput ---"
cd "$PI_LINUX/uinput-serial-keyboard"
gcc -o nwinput uinput.c
cp nwinput /usr/local/bin/nwinput
cp "$SCRIPT_DIR/nw-resolution" /usr/local/bin/nw-resolution
chmod +x /usr/local/bin/nw-resolution
cp "$CONFIG_DIR/nw-resolution.desktop" /usr/share/applications/
cp "$CONFIG_DIR/nwinput.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable nwinput

# --- Boot config ---
echo ""
echo "--- Configuring boot ---"

# config.txt: append NumWorks entries (idempotent - check for exact line)
if ! grep -q '^dtoverlay=numworks-spifb' /boot/firmware/config.txt; then
    cat >> /boot/firmware/config.txt <<'EOF'

# NumWorks SPI display
dtparam=spi=on
dtoverlay=numworks-spifb,vwidth=640,vheight=480
EOF
    echo "Appended NumWorks config to config.txt"
else
    echo "config.txt already configured"
fi

# cmdline.txt: remove serial console if present
if grep -q 'console=serial0' /boot/firmware/cmdline.txt; then
    sed -i 's/console=serial0,[0-9]* //' /boot/firmware/cmdline.txt
    echo "Removed serial console from cmdline.txt"
fi

# --- System config ---
echo ""
echo "--- System configuration ---"

# Kernel modules
cp "$CONFIG_DIR/modules-load.d-numworks.conf" /etc/modules-load.d/numworks.conf

# Keyboard layout (sets for fbcon/VT; labwc uses its own env file)
cp "$CONFIG_DIR/keyboard" /etc/default/keyboard

# Mask serial getty (conflicts with nwinput on ttyS0)
systemctl mask serial-getty@ttyS0.service 2>/dev/null || true

# labwc user environment
mkdir -p "$ACTUAL_HOME/.config/labwc"
cp "$CONFIG_DIR/labwc-environment" "$ACTUAL_HOME/.config/labwc/environment"
chown -R "$ACTUAL_USER:$ACTUAL_USER" "$ACTUAL_HOME/.config/labwc"

# labwc system environment (WLR_RENDERER=pixman for greeter)
mkdir -p /etc/xdg/labwc
if ! grep -q '^WLR_RENDERER=' /etc/xdg/labwc/environment 2>/dev/null; then
    echo 'WLR_RENDERER=pixman' >> /etc/xdg/labwc/environment
fi

# --- Boot optimizations ---
echo ""
echo "--- Boot optimizations ---"

# Disable cloud-init completely (services + disable file)
touch /etc/cloud/cloud-init.disabled
systemctl mask cloud-init.service cloud-init-local.service cloud-config.service cloud-final.service 2>/dev/null || true

# Mask unnecessary services
echo "Masking unnecessary services..."
systemctl mask ModemManager.service 2>/dev/null || true
systemctl mask cups.service cups-browsed.service 2>/dev/null || true
systemctl mask bluetooth.service 2>/dev/null || true
systemctl mask e2scrub_reap.service 2>/dev/null || true
systemctl mask udisks2.service 2>/dev/null || true
systemctl mask packagekit.service 2>/dev/null || true

# Fix netplan NM reload storm (don't remove, just fix permissions)
# Netplan triggers 4x NetworkManager reloads on boot (~17s wasted)
# Fix: restrict permissions so NM doesn't see the yaml files
echo "Fixing netplan permissions to reduce NM reload delays..."
chmod 600 /etc/netplan/*.yaml 2>/dev/null || true
chmod 600 /lib/netplan/*.yaml 2>/dev/null || true

# Shutdown timeout (prevents 90s hangs)
if ! grep -q '^DefaultTimeoutStopSec=10s' /etc/systemd/system.conf; then
    if grep -q '^#DefaultTimeoutStopSec=' /etc/systemd/system.conf; then
        sed -i 's/^#DefaultTimeoutStopSec=.*/DefaultTimeoutStopSec=10s/' /etc/systemd/system.conf
    else
        echo 'DefaultTimeoutStopSec=10s' >> /etc/systemd/system.conf
    fi
fi

# --- Filesystem protection ---
echo ""
echo "--- Filesystem protection ---"
echo ""
echo "WARNING: Hard power-off can corrupt the SD card."
echo "Consider enabling overlayfs (read-only root) via raspi-config:"
echo "  sudo raspi-config → Performance → Overlay File System → Enable"
echo ""
echo "Or mount critical partitions read-only in /etc/fstab."
echo ""

echo "=== Setup complete ==="
echo ""
echo "Reboot to activate: sudo reboot"
