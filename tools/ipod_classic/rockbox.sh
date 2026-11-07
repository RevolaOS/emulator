#!/bin/sh
# rockbox.sh -- boot Rockbox on the iPod-Classic machine and prove it reaches a
# NAVIGABLE main menu.  Run from WSL: the binary in build/ is a Linux ELF.
#
#   sh tools/ipod_classic/rockbox.sh [outdir] [disk-image] [rockbox.bin]
#
# WHAT IT DOES
#   Loads Rockbox's raw app image straight into SDRAM with -kernel (the same
#   osos-style path the retailOS uses; the machine reproduces the bootloader
#   hand-off -- a valid SP and a running 1 MHz usec timer -- so the app boots
#   without the Rockbox bootloader; see ipod_classic_handoff() and wiki/15).
#   Then it:
#     - waits for the main menu to settle and screendumps it,
#     - turns the click wheel forward twice, back once, and presses SELECT,
#       screendumping each step,
#   and reports, per frame, how many pixels are lit and where the gold
#   selection bar sits, so a machine can see the menu drew AND that the wheel
#   moved the highlight.  Every .ppm is also written out as a .png.
#
# HOW TO READ IT
#   * The menu is DRAWN when a frame has tens of thousands of lit pixels
#     (a themed cabbiev2 boot is ~74000; the built-in-font fallback ~3800).
#   * The wheel NAVIGATES when the gold selection-bar row moves down by one
#     item pitch per forward click and back up on a reverse click.
#   * SELECT WORKS when the frame after it differs wholesale from the menu.
#
# THE DISK
#   Rockbox mounts the FAT32 partition and, if it carries a /.rockbox tree,
#   loads its theme, fonts and codecs (needed by the next milestone, audio).
#   With any other FAT disk Rockbox still reaches the menu but prints
#   "Installation incomplete" first and falls back to the built-in font.
#   Build a /.rockbox disk (needs the app's rockbox.zip and a firmware image):
#
#     W=$HOME/rbwork; rm -rf $W; mkdir $W; cd $W
#     unzip -q .../assets/rockbox/build-full/rockbox.zip          # makes .rockbox/
#     truncate -s 300M $HOME/6g/rbfat.img
#     mkfs.vfat -F 32 -n ROCKBOX $HOME/6g/rbfat.img
#     mcopy -s -i $HOME/6g/rbfat.img .rockbox ::
#     python3 tools/ipod_classic/mkdisk6g4k.py \
#             $HOME/6g/Erb.img $HOME/6g/fwzero.img $HOME/6g/rbfat.img
#
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
qemu="$root/build/qemu-system-arm"
out=${1:-$HOME/6g/rockbox}
disk=${2:-$HOME/6g/Erb.img}
bin=${3:-$root/assets/rockbox/build-full/rockbox.bin}
[ -f "$bin" ] || bin="$root/assets/rockbox/build-rb/rockbox.bin"

[ -x "$qemu" ] || { echo "cannot find $qemu; build it first" >&2; exit 1; }
[ -f "$bin" ]  || { echo "cannot find rockbox.bin ($bin); build the app" >&2; exit 1; }
[ -f "$disk" ] || {
    echo "cannot find the disk $disk." >&2
    echo "Build one with /.rockbox like this (see this script's header):" >&2
    echo "  unzip rockbox.zip; mkfs.vfat -F32 rbfat.img; mcopy -s .rockbox ::;" >&2
    echo "  python3 tools/ipod_classic/mkdisk6g4k.py Erb.img fwzero.img rbfat.img" >&2
    exit 1
}

mkdir -p "$out"
rm -f "$out"/*.ppm "$out"/*.png "$out"/qemu.err

# Do not step on another session's QEMU: this tree runs several on purpose.
pkill -9 -x qemu-system-arm 2>/dev/null || true
sleep 1

# The menu is up and still by ~14 s of wall clock; 16 gives it room. Keys are
# held 150 ms because the wheel model paces its packets on the virtual clock.
{
    sleep 16; echo "screendump $out/0-menu.ppm"
    sleep 2;  echo "sendkey e 150"
    sleep 3;  echo "screendump $out/1-wheel-fwd.ppm"
    sleep 2;  echo "sendkey e 150"
    sleep 3;  echo "screendump $out/2-wheel-fwd2.ppm"
    sleep 2;  echo "sendkey q 150"
    sleep 3;  echo "screendump $out/3-wheel-back.ppm"
    sleep 2;  echo "sendkey ret 150"
    sleep 4;  echo "screendump $out/4-select.ppm"
    sleep 1;  echo quit
} | WHEELTRACE=1 "$qemu" -M iPod-Classic \
        -kernel "$bin" \
        -drive "if=ide,format=raw,file=$disk,file.locking=off" \
        -snapshot \
        -display none -serial null -monitor stdio \
        >/dev/null 2>"$out/qemu.err" || true

echo
echo "=== the wheel ==="
grep WHEELTRACE "$out/qemu.err" || echo "  (nothing: WHEELTRACE printed nothing)"
echo

python3 - "$out" <<'EOF'
import glob, os, struct, sys, zlib

out = sys.argv[1]

def read_ppm(path):
    b = open(path, "rb").read()
    p = b.split(b"\n", 3)
    w, h = map(int, p[1].split())
    return w, h, p[3]

def write_png(path, w, h, data, scale=2):
    W, H = w * scale, h * scale
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        sy = y // scale
        for x in range(W):
            i = (sy * w + x // scale) * 3
            raw += data[i:i + 3]
    def chunk(t, p):
        b = t + p
        return struct.pack(">I", len(p)) + b + struct.pack(">I", zlib.crc32(b) & 0xffffffff)
    open(path, "wb").write(
        b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
        chunk(b"IEND", b""))

def lit(data):
    return sum(1 for i in range(0, len(data), 3) if data[i:i+3] != b"\x00\x00\x00")

def gold_row(w, h, data):
    """Row with the most gold (selection-bar) pixels: high R, high G, low B."""
    best = (-1, 0)
    for y in range(h):
        n = 0
        for x in range(w):
            i = (y * w + x) * 3
            if data[i] > 150 and data[i+1] > 120 and data[i+2] < 110:
                n += 1
        if n > best[1]:
            best = (y, n)
    return best

def differ(a, b):
    return sum(1 for i in range(0, len(a), 3) if a[i:i+3] != b[i:i+3])

shots = sorted(glob.glob(os.path.join(out, "*.ppm")))
if not shots:
    print("NO SCREENSHOTS: Rockbox never booted")
    raise SystemExit(1)

frames, rows, lits = {}, {}, {}
for p in shots:
    w, h, d = read_ppm(p)
    write_png(p[:-4] + ".png", w, h, d)
    name = os.path.basename(p)[:-4]
    frames[name] = d
    lits[name] = lit(d)
    rows[name] = gold_row(w, h, d)
    print("%-14s %6d px lit   gold bar at y=%3d (%d px)"
          % (name, lits[name], rows[name][0], rows[name][1]))

print()
ok = True

def check(cond, msg):
    global ok
    print(("  OK   " if cond else "  FAIL ") + "  " + msg)
    ok = ok and cond

# 1) the menu drew
check(lits.get("0-menu", 0) > 2000,
      "the main menu was DRAWN (%d px)" % lits.get("0-menu", 0))
# 2) forward wheel moved the bar down, and it came back
y0 = rows.get("0-menu", (-1, 0))[0]
y1 = rows.get("1-wheel-fwd", (-1, 0))[0]
y2 = rows.get("2-wheel-fwd2", (-1, 0))[0]
y3 = rows.get("3-wheel-back", (-1, 0))[0]
check(y1 > y0 and y2 > y1, "the wheel moved the highlight DOWN (y %d->%d->%d)" % (y0, y1, y2))
check(y3 < y2, "the wheel backwards moved the highlight UP (y %d->%d)" % (y2, y3))
# 3) SELECT changed the screen
d = differ(frames["3-wheel-back"], frames["4-select"]) if "4-select" in frames else 0
check(d > 3000, "SELECT changed the screen (%d px differ)" % d)

print()
print("PNGs in %s" % out)
print("VERDICT:", "OK -- Rockbox reaches its main menu and navigates"
      if ok else "FAIL")
raise SystemExit(0 if ok else 2)
EOF
