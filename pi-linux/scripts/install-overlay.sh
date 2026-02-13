#!/bin/bash
# Compile and install the NumWorks spifb Device Tree overlay
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OVERLAY_DIR="$SCRIPT_DIR/../overlay"
DTS_FILE="$OVERLAY_DIR/numworks-spifb.dts"

if [ "$EUID" -ne 0 ]; then
    echo "Run as root: sudo $0"
    exit 1
fi

# Check dtc is installed
if ! command -v dtc &>/dev/null; then
    echo "Error: dtc (device-tree-compiler) not found"
    echo "Install with: sudo apt-get install device-tree-compiler"
    exit 1
fi

# Check source file exists
if [ ! -f "$DTS_FILE" ]; then
    echo "Error: DTS source not found: $DTS_FILE"
    exit 1
fi

# Trixie uses /boot/firmware/, Bullseye uses /boot/
if [ -d /boot/firmware/overlays ]; then
    OVERLAY_DEST=/boot/firmware/overlays
elif [ -d /boot/overlays ]; then
    OVERLAY_DEST=/boot/overlays
else
    echo "Error: Cannot find overlays directory"
    exit 1
fi

echo "Compiling DT overlay..."
DTBO_FILE="$OVERLAY_DEST/numworks-spifb.dtbo"

# Compile with error output visible
if ! dtc -@ -I dts -O dtb -o "$DTBO_FILE" "$DTS_FILE"; then
    echo "Error: DT overlay compilation failed"
    exit 1
fi

# Verify output was created
if [ ! -f "$DTBO_FILE" ]; then
    echo "Error: Output file not created: $DTBO_FILE"
    exit 1
fi

echo "Installed: $DTBO_FILE"
echo ""
echo "Add to config.txt:"
echo "  dtparam=spi=on"
echo "  dtoverlay=numworks-spifb"
echo ""
echo "Optional parameters:"
echo "  dtoverlay=numworks-spifb,vwidth=480,vheight=360   # 1.5x (default)"
echo "  dtoverlay=numworks-spifb,vwidth=640,vheight=480   # 2x"
echo "  dtoverlay=numworks-spifb,vwidth=320,vheight=240   # native 1x"
echo "  dtoverlay=numworks-spifb,speed=70000000            # SPI speed override"
