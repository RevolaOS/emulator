#!/bin/sh
# Drive the iPod-Video machine and prove the UI responds to the click wheel.
#
#   navtest.sh [outdir]
#
# Boots the osos image the way the ROM loader does, waits for Rockbox to come
# up, leaves the Now Playing screen, then turns the wheel one item at a time
# and finally selects. The selection bar moving between frames is the evidence
# that navigation works; note that the Now Playing screen animates on its own,
# so a small pixel difference proves nothing -- only a moved highlight does.
#
# Screenshots land in <outdir> as PNG (QEMU writes PPM; we convert here so the
# result is viewable without extra tools).

set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
qemu="$root/build/qemu-system-arm"
osos="$root/assets/images-5g/osos_full.bin"
disk="$root/assets/ipod-video-disk.img"
out=${1:-$root/assets/nav}

ENTRY_ROCKBOX=0x735a00

[ -x "$qemu" ] || { echo "cannot find $qemu; build it first" >&2; exit 1; }
[ -f "$osos" ] || { echo "cannot find $osos" >&2; exit 1; }
[ -f "$disk" ] || { echo "cannot find $disk" >&2; exit 1; }

mkdir -p "$out"
rm -f "$out"/*.ppm "$out"/*.png

# Rockbox takes a while to get through its boot; the menu does not appear the
# instant the logo does. Shoot early, let it settle, then interleave keys and
# shots. sendkey holds the key down for a moment because the firmware only
# samples the wheel on its tick -- a zero-length press can fall between two
# samples and never be seen.
{
    sleep 25;  echo "screendump $out/00-boot.ppm"
    sleep 10;  echo "sendkey w 100"           # MENU: leave Now Playing
    sleep 3;   echo "screendump $out/01-menu.ppm"
    sleep 1;   echo "sendkey e 100"           # wheel clockwise
    sleep 2;   echo "screendump $out/02-wheel1.ppm"
    sleep 1;   echo "sendkey e 100"
    sleep 2;   echo "screendump $out/03-wheel2.ppm"
    sleep 1;   echo "sendkey e 100"
    sleep 2;   echo "screendump $out/04-wheel3.ppm"
    sleep 1;   echo "sendkey q 100"           # wheel anticlockwise
    sleep 2;   echo "screendump $out/05-wheelback.ppm"
    sleep 1;   echo "sendkey ret 100"         # SELECT: enter the item
    sleep 3;   echo "screendump $out/06-enter.ppm"
    sleep 1;   echo quit
} | "$qemu" -M "iPod-Video,entry=$ENTRY_ROCKBOX" \
        -kernel "$osos" \
        -drive "if=ide,index=0,file=$disk,format=raw,file.locking=off" \
        -snapshot \
        -display none -serial null -monitor stdio >/dev/null 2>&1 || true

# PPM -> PNG, and report how much each frame differs from the one before it.
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

    lit = sum(1 for i in range(0, len(data), 3) if data[i:i + 3] != b"\x00\x00\x00")
    if prev is None:
        print("%-28s %6d px lit" % (os.path.basename(path), lit))
    else:
        diff = sum(1 for i in range(0, len(data), 3)
                   if data[i:i + 3] != prev[i:i + 3])
        print("%-28s %6d px lit   %6d px differ from the previous"
              % (os.path.basename(path), lit, diff))
    prev = data
EOF

rm -f "$out"/*.ppm
echo
echo "PNGs in $out"
