#!/bin/bash
# Reflash the NumWorks-Pi calculator (N0100) without touching it.
#
#   tools/reflash-n0100.sh [epsilon.bin]
#
# Needs firmware built with ENABLE_RPI=1 (SYS:DFU support) running in the Pi
# app, the Pi reachable over SSH, and the calculator plugged in over USB.
# The Pi asks the calculator to enter DFU, the calculator acknowledges and
# the Pi powers off; once it is down we flash, and the reset at the end
# boots the new firmware. Build with RPI_AUTOSTART=1 to have it power the Pi
# straight back up.
#
# Falls back to the ROM bootloader (reset + 6) if the calculator is already
# there. Only ever flashes a device exposing the STM32F412's 1 MiB sector map
# at 0x08000000: an N0110 plugged in alongside (64 KiB internal flash plus
# external flash at 0x90000000) never matches, in either DFU mode.
set -euo pipefail

BIN=${1:-"$(dirname "$0")/../firmware/output/release/device/n0100/epsilon.bin"}
PI=${NWPI_HOST:-pi@nwpi.local}
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=5 "$PI")

[ -f "$BIN" ] || { echo "No firmware at $BIN" >&2; exit 1; }
size=$(stat -f%z "$BIN" 2>/dev/null || stat -c%s "$BIN")
[ "$size" -le 1048576 ] || { echo "$BIN is $size bytes, over 1 MiB" >&2; exit 1; }

# "vid:pid serial" of the N0100 in Epsilon DFU or ROM DFU, if present
find_n0100() {
	dfu-util -l 2>/dev/null |
		grep -E '\[0483:(a291|df11)\].*alt=0.*Flash */0x08000000/04\*016Kg,01\*064Kg,07\*128Kg"' |
		sed -E 's/.*\[(0483:[0-9a-f]+)\].*serial="([^"]*)".*/\1 \2/' | head -1 || true
}

dev=$(find_n0100)
if [ -z "$dev" ]; then
	echo "Asking the calculator to enter DFU..."
	"${SSH[@]}" 'nwpi-ctl dfu'
	for _ in $(seq 30); do
		dev=$(find_n0100)
		[ -n "$dev" ] && break
		sleep 1
	done
	[ -n "$dev" ] || { echo "Calculator did not show up in DFU" >&2; exit 1; }
	echo "Waiting for the Pi to power off..."
	for _ in $(seq 60); do
		ping -c1 -t1 "${PI#*@}" >/dev/null 2>&1 || break
		sleep 1
	done
	sleep 8	# halt completes after the network goes down
fi

read -r id serial <<<"$dev"
echo "Flashing $BIN to $id serial $serial"
# The progress bars redraw with \r: split them into lines so grep drops them
dfu-util -d "$id" -S "$serial" -a 0 -s 0x08000000:leave -D "$BIN" 2>&1 |
	tr '\r' '\n' | grep -vE '^(Download|Erase)[[:space:]]+\[' || true
echo "Done; the calculator reboots into the new firmware."
