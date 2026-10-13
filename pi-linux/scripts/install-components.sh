#!/bin/bash
# Install or update the NumWorks-Pi components from this checkout:
# display driver (DKMS, for every installed kernel), DT overlay, nwpid and
# nwpi-ctl, and the apps. Part of setup.sh; also run it alone after pulling
# changes:  sudo ./pi-linux/scripts/install-components.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PI_LINUX="$(dirname "$SCRIPT_DIR")"

if [ "$EUID" -ne 0 ]; then
    echo "Run as root: sudo $0"
    exit 1
fi

# --- Display driver (DKMS) ---
echo ""
echo "--- Installing drm-spifb (DKMS) ---"
apt-get install -y dkms "linux-headers-$(uname -r)"
SRC="$PI_LINUX/drm-spifb"
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$SRC/dkms.conf")
# Drop any registered version and hand-installed copies
for v in $(dkms status -m drm-spifb 2>/dev/null | sed -n 's|^drm-spifb/\([^,:]*\).*|\1|p' | sort -u); do
    dkms remove -m drm-spifb -v "$v" --all || true
done
rm -rf "/usr/src/drm-spifb-$VER"
mkdir -p "/usr/src/drm-spifb-$VER"
cp "$SRC"/*.c "$SRC"/*.h "$SRC/Makefile" "$SRC/dkms.conf" "/usr/src/drm-spifb-$VER/"
dkms add -m drm-spifb -v "$VER"
# Build for every installed kernel of this Pi's flavour that has headers
FLAVOUR=${KERNEL_FLAVOUR:-$(uname -r | sed 's/.*-//')}
for dir in /lib/modules/*-"$FLAVOUR"; do
    kver=$(basename "$dir")
    if [ -e "$dir/build" ]; then
        # Only once DKMS succeeded, drop a hand-installed copy that would
        # shadow its module; a failed build leaves the old driver working
        if dkms install -m drm-spifb -v "$VER" -k "$kver"; then
            built=$(find "/var/lib/dkms/drm-spifb/$VER/$kver" -name 'drm-spifb.ko*' | head -1)
            for f in "$dir"/extra/drm-spifb.ko*; do
                if [ -e "$f" ] && ! cmp -s "$f" "$built"; then rm -f "$f"; fi
            done
            depmod "$kver"
        else
            echo "*** drm-spifb failed to build for $kver; do not boot it ***"
        fi
    else
        echo "No headers for $kver, skipping (install linux-headers-$kver)"
    fi
done

# --- Device Tree overlay ---
echo ""
echo "--- Installing DT overlay ---"
"$SCRIPT_DIR/install-overlay.sh"

# --- nwpid (NumWorks Pi Daemon) and nwpi-ctl ---
echo ""
echo "--- Building and installing nwpid ---"
cd "$PI_LINUX/nwpid"
make clean && make
install -m 755 nwpid /usr/local/bin/nwpid.new
mv /usr/local/bin/nwpid.new /usr/local/bin/nwpid
install -m 755 nwpi-ctl /usr/local/bin/nwpi-ctl
cp nwpid.service /etc/systemd/system/
# Disable old nwinput if present
systemctl disable nwinput 2>/dev/null || true
systemctl stop nwinput 2>/dev/null || true
systemctl daemon-reload
systemctl enable nwpid
systemctl restart nwpid

# --- Apps ---
echo ""
echo "--- Installing apps ---"
apt-get install -y python3-picamera2 python3-pygame
for app in "$PI_LINUX"/apps/*/; do
    name=$(basename "$app")
    install -m 755 "$app/$name" "/usr/local/bin/$name"
    for entry in "$app"/*.desktop; do
        [ -e "$entry" ] && cp "$entry" /usr/share/applications/
    done
done

# --- Games (started through nw-game) ---
echo ""
echo "--- Installing games ---"
apt-get install -y --no-install-recommends chocolate-doom freedoom ltris \
    lbreakout2 sgt-puzzles scummvm scummvm-data beneath-a-steel-sky \
    flight-of-the-amazon-queen
rm -rf /usr/local/share/nw-game
cp -r "$PI_LINUX/config/games" /usr/local/share/nw-game

# --- Desktop launchers ---
# Opened with a single OK press (single_click), without pcmanfm's
# "execute?" prompt (quick_exec)
echo ""
echo "--- Installing desktop launchers ---"
DESKTOP_USER=${SUDO_USER:-pi}
DESKTOP_HOME=$(getent passwd "$DESKTOP_USER" | cut -d: -f6)
install -d -o "$DESKTOP_USER" -g "$DESKTOP_USER" "$DESKTOP_HOME/Desktop"
install -m 755 -o "$DESKTOP_USER" -g "$DESKTOP_USER" \
    "$PI_LINUX"/config/desktop/*.desktop "$DESKTOP_HOME/Desktop/"
install -d -o "$DESKTOP_USER" -g "$DESKTOP_USER" "$DESKTOP_HOME/Desktop/Puzzles"
install -m 755 -o "$DESKTOP_USER" -g "$DESKTOP_USER" \
    "$PI_LINUX"/config/desktop/Puzzles/*.desktop "$DESKTOP_HOME/Desktop/Puzzles/"
LIBFM="$DESKTOP_HOME/.config/libfm/libfm.conf"
if [ -f "$LIBFM" ]; then
    sed -i 's/^single_click=.*/single_click=1/; s/^quick_exec=.*/quick_exec=1/' "$LIBFM"
    grep -q '^quick_exec=' "$LIBFM" || sed -i '/^\[config\]/a quick_exec=1' "$LIBFM"
fi

echo ""
echo "Components installed. Reboot to load a new driver: sudo reboot"
