#!/bin/bash
# Build the drm-spifb kernel module
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "=== Building drm-spifb kernel module ==="
cd "$PROJECT_DIR/drm-spifb"
make
echo ""

echo "=== Done ==="
echo ""
echo "Next steps:"
echo "  1. sudo $SCRIPT_DIR/install-overlay.sh"
echo "  2. sudo cp $PROJECT_DIR/drm-spifb/drm-spifb.ko /lib/modules/\$(uname -r)/extra/"
echo "  3. sudo depmod -a"
echo "  4. Add to /boot/firmware/config.txt:"
echo "       dtparam=spi=on"
echo "       dtoverlay=numworks-spifb"
echo "  5. sudo reboot"
