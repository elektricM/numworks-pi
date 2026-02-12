#!/bin/bash
# Full Pi setup for NumWorks calculator integration
# Run on the Pi after cloning the repo: sudo ./pi-linux/scripts/setup.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PI_LINUX="$(dirname "$SCRIPT_DIR")"
CONFIG_DIR="$PI_LINUX/config"

if [ "$EUID" -ne 0 ]; then
    echo "Run as root: sudo $0"
    exit 1
fi

ACTUAL_USER="${SUDO_USER:-pi}"
ACTUAL_HOME=$(eval echo "~$ACTUAL_USER")

echo "=== NumWorks Pi Setup ==="
echo "User: $ACTUAL_USER  Home: $ACTUAL_HOME"
echo ""

# --- System update ---
echo "--- Updating system ---"
apt-get update
apt-get upgrade -y

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
cp "$CONFIG_DIR/nwinput.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable nwinput

# --- Boot config ---
echo ""
echo "--- Configuring boot ---"

# config.txt: append NumWorks section if not already present
if ! grep -q 'numworks-spifb' /boot/firmware/config.txt; then
    cat >> /boot/firmware/config.txt <<'EOF'

[all]
enable_uart=1

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

# Keyboard layout
cp "$CONFIG_DIR/keyboard" /etc/default/keyboard

# Mask serial getty (conflicts with nwinput on ttyS0)
systemctl mask serial-getty@ttyS0.service 2>/dev/null || true

# labwc user environment
mkdir -p "$ACTUAL_HOME/.config/labwc"
cp "$CONFIG_DIR/labwc-environment" "$ACTUAL_HOME/.config/labwc/environment"
chown -R "$ACTUAL_USER:$ACTUAL_USER" "$ACTUAL_HOME/.config/labwc"

# labwc system environment (WLR_RENDERER=pixman for greeter)
mkdir -p /etc/xdg/labwc
if ! grep -q 'WLR_RENDERER' /etc/xdg/labwc/environment 2>/dev/null; then
    echo 'WLR_RENDERER=pixman' >> /etc/xdg/labwc/environment
fi

# --- Boot optimizations ---
echo ""
echo "--- Boot optimizations ---"

# Disable cloud-init
touch /etc/cloud/cloud-init.disabled

# Fix netplan permissions (prevents NM reload storm)
if [ -f /lib/netplan/00-network-manager-all.yaml ]; then
    chmod 600 /lib/netplan/00-network-manager-all.yaml
fi

# Remove netplan WiFi YAMLs if any (they cause NM reloads)
rm -f /etc/netplan/90-NM-*.yaml

# Shutdown timeout (prevents 90s hangs)
if ! grep -q '^DefaultTimeoutStopSec=10s' /etc/systemd/system.conf; then
    if grep -q '^#DefaultTimeoutStopSec=' /etc/systemd/system.conf; then
        sed -i 's/^#DefaultTimeoutStopSec=.*/DefaultTimeoutStopSec=10s/' /etc/systemd/system.conf
    else
        echo 'DefaultTimeoutStopSec=10s' >> /etc/systemd/system.conf
    fi
fi

echo ""
echo "=== Setup complete ==="
echo ""
echo "Remaining manual steps:"
echo "  1. Configure WiFi if not already done"
echo "  2. Reboot: sudo reboot"
