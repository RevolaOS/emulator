#!/bin/sh
# Run the iPod-Video machine.
#
#   run.sh rockbox   boot the Rockbox bootloader out of the osos image
#   run.sh apple     boot Apple's retailOS out of the same image
#
# Both load the whole osos at DRAM_START the way the ROM loader does; they
# differ only in where execution starts. 0x735a00 is the entryOffset from this
# device's firmware partition directory, which is where ipodpatcher appended
# the Rockbox bootloader.

set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
qemu="$root/build/qemu-system-arm"
osos="$root/assets/images-5g/osos_full.bin"
ENTRY_ROCKBOX=0x735a00
ENTRY_APPLE=0

[ -x "$qemu" ] || { echo "cannot find $qemu; build it first" >&2; exit 1; }
[ -f "$osos" ] || { echo "cannot find $osos" >&2; exit 1; }

case "${1:-rockbox}" in
    rockbox) entry=$ENTRY_ROCKBOX ;;
    apple)   entry=$ENTRY_APPLE ;;
    *) echo "uso: $0 [rockbox|apple]" >&2; exit 1 ;;
esac

shift 2>/dev/null || true

exec "$qemu" -M "iPod-Video,entry=$entry" \
    -kernel "$osos" \
    -display none -monitor none \
    -serial stdio \
    -d unimp,guest_errors \
    "$@"
