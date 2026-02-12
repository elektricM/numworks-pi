#!/bin/bash
# Change the NumWorks SPI display virtual resolution
# Usage: sudo resolution.sh [1x|1.5x|2x]
set -e

CONFIG="/boot/firmware/config.txt"

case "$1" in
    1x|1)
        VWIDTH=320; VHEIGHT=240; LABEL="1x (320x240, native)" ;;
    1.5x|1.5)
        VWIDTH=480; VHEIGHT=360; LABEL="1.5x (480x360)" ;;
    2x|2)
        VWIDTH=640; VHEIGHT=480; LABEL="2x (640x480)" ;;
    *)
        # Show current setting
        CURRENT=$(grep 'dtoverlay=numworks-spifb' "$CONFIG" 2>/dev/null | head -1)
        echo "Current: $CURRENT"
        echo ""
        echo "Usage: sudo $0 <resolution>"
        echo ""
        echo "  1x    320x240  Native, large UI"
        echo "  1.5x  480x360  Default, good balance"
        echo "  2x    640x480  Small UI, more content"
        exit 0
        ;;
esac

if [ "$EUID" -ne 0 ]; then
    echo "Run as root: sudo $0 $1"
    exit 1
fi

# Update the overlay line in config.txt
sed -i "s/dtoverlay=numworks-spifb.*/dtoverlay=numworks-spifb,vwidth=$VWIDTH,vheight=$VHEIGHT/" "$CONFIG"

echo "Resolution set to $LABEL"
echo "Reboot to apply: sudo reboot"
