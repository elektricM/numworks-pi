#!/bin/bash
# Compile and install the NumWorks spifb Device Tree overlay
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OVERLAY_DIR="$SCRIPT_DIR/../overlay"

if [ "$EUID" -ne 0 ]; then
    echo "Run as root: sudo $0"
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
dtc -@ -I dts -O dtb -o "$OVERLAY_DEST/numworks-spifb.dtbo" \
    "$OVERLAY_DIR/numworks-spifb.dts" 2>/dev/null

echo "Installed: $OVERLAY_DEST/numworks-spifb.dtbo"
echo ""
echo "Add to config.txt:"
echo "  dtparam=spi=on"
echo "  dtoverlay=numworks-spifb"
echo ""
echo "Optional parameters:"
echo "  dtoverlay=numworks-spifb,vwidth=480,vheight=360   # 1.5x (default)"
echo "  dtoverlay=numworks-spifb,vwidth=640,vheight=480   # 2x"
echo "  dtoverlay=numworks-spifb,vwidth=320,vheight=240   # native 1x"
echo "  dtoverlay=numworks-spifb,speed=70000000,fps=60    # SPI speed, refresh rate"
