#!/bin/sh
# Drive the iPod-Video machine with Apple's own firmware and prove it draws
# and navigates.
#
#   appletest.sh [outdir]
#
# Boots osos_full.bin at entry 0, which is the retailOS rather than Rockbox's
# bootloader. Two runs:
#
#   boot-*   the screen at 10, 20, 40, 60, 90 and 120 s, to show the UI comes
#            up and stays up.
#   nav-*    the wheel and the buttons, before and after each press.
#
# What counts as proof is a *large* pixel difference: the highlight moving one
# item is around 14 000 pixels and a whole new screen is over 20 000. A handful
# of pixels is the battery icon and proves nothing.
#
# The retailOS talks to the video chip's firmware, which we do not execute, and
# it prints its panics over ARM semihosting -- so semihosting has to be on or a
# failure is silent.

set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
qemu="$root/build/qemu-system-arm"
osos="$root/assets/images-5g/osos_full.bin"
disk="$root/assets/ipod-video-disk.img"
out=${1:-$root/assets/apple}

[ -x "$qemu" ] || { echo "cannot find $qemu; build it first" >&2; exit 1; }
[ -f "$osos" ] || { echo "cannot find $osos" >&2; exit 1; }
[ -f "$disk" ] || { echo "cannot find $disk" >&2; exit 1; }

mkdir -p "$out"
rm -f "$out"/*.ppm "$out"/*.png

run() {
    "$qemu" -M "iPod-Video,entry=0" \
        -kernel "$osos" \
        -drive "if=ide,index=0,file=$disk,format=raw,file.locking=off" \
        -snapshot \
        -display none -serial none -monitor stdio \
        -semihosting-config enable=on,target=native >/dev/null 2>&1 || true
}

# The first-run language screen is up by about eight seconds.
{
    sleep 10;  echo "screendump $out/boot-010s.ppm"
    sleep 10;  echo "screendump $out/boot-020s.ppm"
    sleep 20;  echo "screendump $out/boot-040s.ppm"
    sleep 20;  echo "screendump $out/boot-060s.ppm"
    sleep 30;  echo "screendump $out/boot-090s.ppm"
    sleep 30;  echo "screendump $out/boot-120s.ppm"
    sleep 2;   echo quit
} | run

{
    sleep 25;  echo "screendump $out/nav-0-language.ppm"
    sleep 2;   echo "sendkey e 200"           # wheel clockwise, twice
    sleep 2;   echo "sendkey e 200"
    sleep 3;   echo "screendump $out/nav-1-wheel.ppm"
    sleep 1;   echo "sendkey ret 200"         # SELECT: choose that language
    sleep 6;   echo "screendump $out/nav-2-select.ppm"
    sleep 1;   echo "sendkey e 200"           # wheel down the main menu
    sleep 2;   echo "sendkey e 200"
    sleep 3;   echo "screendump $out/nav-3-wheel.ppm"
    sleep 1;   echo "sendkey ret 200"         # SELECT: enter that item
    sleep 6;   echo "screendump $out/nav-4-enter.ppm"
    sleep 1;   echo "sendkey w 200"           # MENU: back out again
    sleep 6;   echo "screendump $out/nav-5-menu.ppm"
    sleep 2;   echo quit
} | run

# PPM -> PNG, with how lit each frame is and how far it moved.
python3 - "$out" <<'EOF'
import glob, os, struct, sys, zlib

out = sys.argv[1]
prev = None
for path in sorted(glob.glob(os.path.join(out, "*.ppm"))):
    blob = open(path, "rb").read()
    parts = blob.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    data = parts[3]

    raw = b"".join(b"\x00" + data[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(tag, payload):
        body = tag + payload
        return (struct.pack(">I", len(payload)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xffffffff))
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) +
           chunk(b"IEND", b""))
    open(path[:-4] + ".png", "wb").write(png)

    name = os.path.basename(path)
    lit = sum(1 for i in range(0, len(data), 3) if data[i:i + 3] != b"\x00\x00\x00")
    line = "%-22s %6d/%d px lit (%5.2f%%)" % (
        name, lit, w * h, 100.0 * lit / (w * h))
    if prev is not None and not name.startswith("nav-0"):
        diff = sum(1 for i in range(0, len(data), 3)
                   if data[i:i + 3] != prev[i:i + 3])
        line += "   %6d px differ from the previous" % diff
    print(line)
    prev = data
EOF

rm -f "$out"/*.ppm
echo
echo "PNGs in $out"
