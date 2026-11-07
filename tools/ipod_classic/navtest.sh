#!/bin/sh
# navtest.sh -- prove that Apple's retailOS on the iPod-Classic machine reacts
# to the click wheel AND to the MENU button, and give tomorrow a way to notice
# when it stops.
#
#   sh tools/ipod_classic/navtest.sh [outdir] [disk-image]
#
# Run it from WSL: the binary in build/ is a Linux ELF.
#
# WHAT IT DOES
#   Boots the retailOS off the firmware partition image, waits for the
#   language screen to settle, takes a baseline, takes a SECOND baseline to
#   show the screen is standing still, and only then starts pressing keys:
#
#     on the LANGUAGE screen   wheel forward twice, wheel back once, SELECT
#     on the MAIN MENU         a second settle check, wheel forward, wheel
#                              back, SELECT into Music, MENU back out, MENU
#                              again at the top of the stack
#
#   The second half is the part that tests going BACK, and it only exists
#   because the first half cannot: on the first-boot language screen Apple
#   binds MENU to an EMPTY rule. Read out of the firmware image at file offset
#   0x0073e628, in the rule list of SettingsMenus_ResetAllSettings_Language_-
#   Layout: trigger "button.menu.up1", handler length 0, action count 0.
#   Nothing to fire. Pressing MENU there is not a test of anything, so this
#   script now walks to a screen that does have somewhere to go back to.
#
# HOW TO READ THE OUTPUT
#   Two numbers per frame: how many pixels are lit, and how many differ from
#   the frame before.
#
#   * The language screen has NO CLOCK, and two frames of a settled one usually
#     differ by exactly 0 pixels. Usually: the battery icon repaints on its own
#     now and then, 23 px at x=145..151, y=4..13 -- one run of this script
#     measured 0 there and the next 23. So the floor is checked against a limit
#     of 200 px, not against zero, and the measured value is printed. Above the
#     limit the whole run is a FAIL, because every threshold below assumes
#     nothing moves that could be mistaken for navigation. 06-menu-idle does the
#     same for the main menu, but only for the LIST half: the preview pane there
#     is not still and should not be, because the label under the icon is longer
#     than the pane in Japanese and scrolls by itself (about 960 px between two
#     shots 8 s apart, all of it in the two lines of text at x=184..293,
#     y=165..182 -- looked at, not just counted).
#   * Moving the selection bar one item has to change about 7 400 pixels: the
#     bar is 160x23 and the row pitch is 24, so two bars' worth change colour.
#     Counted on a real frame, the strict lower bound is 6 584. Anything under
#     ~1 000 is not navigation, whatever else it is.
#   * SELECT leaving a screen changes tens of thousands.
#
#   Two verdicts are not about SIZE at all, and they are the strong ones:
#     - a wheel click on the MAIN MENU has to change the LEFT half (the list)
#       and the RIGHT half (the preview pane) at the same time, because
#       "list.pid.NNN.delayedselected -> navigator.SwitchLayout(
#       MainMenus_Main_Screen_XXX)" says the pane follows the highlight.
#     - MENU out of the Music screen has to land back on the frame we left,
#       so 10-menu-back is compared against 08-menu-wheel-back and has to
#       agree to within a few hundred pixels. "Lots of pixels changed" is not
#       evidence of going back; "changed back into the frame we came from" is.
#
# A KNOWN FLAKE, so that nobody spends an afternoon on it: SELECT is sometimes
# thrown away by the guest. Measured with a dedicated run of five round trips
# (wiki/11 s.18.8): SELECT from the main menu landed 5 times out of 10, MENU 5
# out of 5, with the wheel model reporting every packet queued and none dropped.
# The cause is NOT found; the suspect and the experiment are in that section.
#
# IT IS NOT ONLY 09-into-music, and an earlier version of this header said it
# was. Measured over four runs (wiki/11 s.19.5): SELECT landed 5 times out of
# 7, and one of the two it lost was 05-select, the FIRST one, on the language
# screen. Losing that one is the nastiest case because it does not look like
# itself: every later screenshot lands on the language screen instead of the
# main menu, so 07/08 still show ~8 000 px of list movement -- the language
# list really is moving -- with 0 px in the panel, and that reads like "the
# preview pane is broken" when it is the correct behaviour for the screen we
# are accidentally still on (s.18.5). Before believing any panel or MENU
# failure below, check that 05-select is above 3 000 px.
#
# So: if this run fails at 05-select or ONLY at 09-into-music, repeat it before
# looking for a regression. MENU itself has not been seen to fail since the
# button state started being repeated: 2 of 2 where there was somewhere to go
# back to, and 4 of 4 correctly doing nothing at the top of the stack.
#
# The second signal, and it is independent of the pixels: DISPTRACE=1 prints a
# line every time the guest hands the compositor a new source address, which is
# one repaint. A boot that is standing still emits ten and then nothing. Each
# wheel event the user interface acts on adds at least one. If the pixels move
# but this does not, the screenshot timing is wrong; if this moves but the
# pixels do not, the event arrived and the handler decided the screen did not
# need to change -- or a layer the guest switched off is being drawn over the
# top of everything, which is exactly the bug that used to hide the main menu.

set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
qemu="$root/build/qemu-system-arm"
osos="$root/assets/classic6g/osos_decrypted.bin"
out=${1:-$HOME/6g/nav}
disk=${2:-$HOME/6g/E.img}

[ -x "$qemu" ] || { echo "cannot find $qemu; build it first" >&2; exit 1; }
[ -f "$osos" ] || { echo "cannot find $osos" >&2; exit 1; }
[ -f "$disk" ] || {
    echo "cannot find the disk $disk." >&2
    echo "Build it like this (wiki/11 sec.15):" >&2
    echo "  truncate -s 64M \$HOME/6g/fat32.img" >&2
    echo "  mkfs.vfat -F 32 -n IPOD \$HOME/6g/fat32.img" >&2
    echo "  python3 tools/ipod_classic/mkdisk6g4k.py \$HOME/6g/E.img \\" >&2
    echo "          assets/extracted/Firmware-24.9.1.2 \$HOME/6g/fat32.img" >&2
    exit 1
}

mkdir -p "$out"
rm -f "$out"/*.ppm "$out"/*.png "$out"/disp.txt "$out"/qemu.err

# -x, matching the process NAME, not -f: a "pkill -f qemu-system-arm" also
# matches any shell whose own command line mentions it, including the one that
# launched this script. That self-kill costs a whole boot to work out.
pkill -9 -x qemu-system-arm 2>/dev/null || true
sleep 1

# AND NOW LOOK FOR THE ONES THAT PKILL CANNOT SEE, because this has now cost
# four runs across two sessions. The line above only matches the NAME
# "qemu-system-arm"; this tree also produces private copies under other names
# on purpose, so that one session cannot kill another's -- ~/evt/qemu-evt for
# evtrace.py, and ~/6g/ipod-classic-run has been seen too. Those survive, sit
# at 90-plus per cent of a core, and the guest then misses its own boot
# deadlines and software-resets to diskmode: the run below produces a full set
# of screenshots of the wrong screens (wiki/11 s.18.9 and s.19.5).
#
# So do not kill them -- they may be another session's work in flight -- but do
# NOT let this run pretend afterwards that its numbers meant something. Say it
# up front, where it is still cheap to stop.
strays=$(ps -eo comm= 2>/dev/null | grep -c -i -E 'qemu|ipod-classic-run' || true)
[ -n "$strays" ] || strays=0
if [ "$strays" -gt 0 ]; then
    echo "*** WARNING: $strays QEMU-like process(es) running IN ADDITION to"
    echo "*** this one. 'pkill -x qemu-system-arm' does not touch the ones with"
    echo "*** other names (qemu-evt, ipod-classic-run). With the machine loaded"
    echo "*** the guest resets to diskmode and the verdict will NOT be valid."
    echo "*** Check 'ps aux | grep -i qemu' and 'uptime' before continuing."
    echo
fi

# The first pixel lands between 22 and 30 s of wall clock and there is one
# late repaint at about 39 s; 48 s is past both with room to spare. Keys are
# held for 150 ms because the wheel model paces its packets on the virtual
# clock and a zero-length press can fall between two of its ticks.
{
    sleep 48;  echo "screendump $out/00-idle.ppm"
    sleep 4;   echo "screendump $out/01-idle-again.ppm"
    sleep 2;   echo "sendkey e 150"
    sleep 4;   echo "screendump $out/02-wheel-fwd.ppm"
    sleep 2;   echo "sendkey e 150"
    sleep 4;   echo "screendump $out/03-wheel-fwd2.ppm"
    sleep 2;   echo "sendkey q 150"
    sleep 4;   echo "screendump $out/04-wheel-back.ppm"
    # SELECT gets a long leash: leaving this screen means loading another one
    # off an emulated disk, and the transition it plays is a wipe that takes
    # its time. 25 s is past the end of it in every run measured.
    sleep 2;   echo "sendkey ret 150"
    sleep 25;  echo "screendump $out/05-select.ppm"
    # ... and now we are on the main menu, so start again: prove it is still,
    # then move.
    sleep 8;   echo "screendump $out/06-menu-idle.ppm"
    sleep 2;   echo "sendkey e 150"
    sleep 8;   echo "screendump $out/07-menu-wheel.ppm"
    sleep 2;   echo "sendkey q 150"
    sleep 8;   echo "screendump $out/08-menu-wheel-back.ppm"
    sleep 2;   echo "sendkey ret 150"
    sleep 12;  echo "screendump $out/09-into-music.ppm"
    sleep 2;   echo "sendkey w 150"
    sleep 12;  echo "screendump $out/10-menu-back.ppm"
    sleep 2;   echo "sendkey w 150"
    sleep 10;  echo "screendump $out/11-menu-at-top.ppm"
    sleep 1;   echo quit
} | DISPTRACE=1 WHEELTRACE=1 "$qemu" -M iPod-Classic \
        -kernel "$osos" \
        -drive "if=ide,format=raw,file=$disk,file.locking=off" \
        -snapshot \
        -display none -serial null -monitor stdio \
        >/dev/null 2>"$out/qemu.err" || true

grep -c SRC_SLOT0 "$out/qemu.err" > "$out/disp.txt" 2>/dev/null || echo 0 > "$out/disp.txt"

echo
# A run in which the guest rebooted has nothing to say: every sleep above is
# counted from power-on, so one reset slides the whole schedule and the
# screenshots land on the wrong screens. Measured example: a run that reset
# once showed the Low Battery screen where 00-idle expects the language list
# and a black panel where 02-wheel-fwd expects a moved highlight. Say so
# loudly instead of letting the verdicts pretend.
# "grep -c" prints 0 AND exits 1 when there is no match, so a "|| echo 0" tail
# makes the variable "0\n0" and the test below dies with "Illegal number".
resets=$(grep -c "software reset" "$out/qemu.err" 2>/dev/null || true)
[ -n "$resets" ] || resets=0
if [ "$resets" -gt 0 ]; then
    echo "*** WARNING: the guest reset $resets time(s) during the run"
    echo "*** (SWRCON = 0xaa5 in stderr). All the timings below are counted"
    echo "*** from power-on, so the screenshots land on the wrong screens"
    echo "*** and the verdict is NOT valid. Repeat the run."
    echo "*** Before looking for culprits: ps aux | grep qemu, and uptime."
    echo
fi
echo "=== what the wheel model said ==="
grep WHEELTRACE "$out/qemu.err" || echo "  (nothing: WHEELTRACE printed nothing)"
echo
echo "=== repaints (writes of the layer 0 source address) ==="
echo "  $(cat "$out/disp.txt")   (an idle run is 10)"
echo

python3 - "$out" <<'EOF'
import glob, os, struct, sys, zlib

out = sys.argv[1]

def read_ppm(path):
    blob = open(path, "rb").read()
    parts = blob.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]

def write_png(path, w, h, data):
    raw = b"".join(b"\x00" + data[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(tag, payload):
        body = tag + payload
        return (struct.pack(">I", len(payload)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xffffffff))
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) +
           chunk(b"IEND", b""))
    open(path, "wb").write(png)

def differ(a, b, x0=0, x1=None, w=320):
    x1 = w if x1 is None else x1
    n = 0
    h = len(a) // (w * 3)
    for y in range(h):
        row = y * w
        for x in range(x0, x1):
            i = (row + x) * 3
            if a[i:i + 3] != b[i:i + 3]:
                n += 1
    return n

shots = sorted(glob.glob(os.path.join(out, "*.ppm")))
if not shots:
    print("NO SCREENSHOTS: the machine never booted")
    raise SystemExit(1)

prev = None
frames = {}
diffs = {}
lefts = {}
lit = {}
for path in shots:
    w, h, data = read_ppm(path)
    write_png(path[:-4] + ".png", w, h, data)
    name = os.path.basename(path)[:-4]
    frames[name] = data
    n = sum(1 for i in range(0, len(data), 3) if data[i:i + 3] != b"\x00\x00\x00")
    lit[name] = n
    if prev is None:
        print("%-20s %6d px lit" % (name, n))
    else:
        d = differ(data, prev)
        # Where did it change? The list is the left 160 columns; the preview
        # panel is the right 160. A moved highlight is a LEFT-hand event and a
        # preview that follows it is a RIGHT-hand one.
        left = differ(data, prev, 0, 160)
        diffs[name] = d
        lefts[name] = left
        print("%-20s %6d px lit   %6d px differ (%d in the list, "
              "%d in the pane)" % (name, n, d, left, d - left))
    prev = data

print()
ok = True

FLOOR_MAX = 200


def floor_check(name, where, half=None):
    """half=None: the whole frame has to be still. half='left': only the list.

    WHY THE LIMIT IS 200 AND NOT 0. These screens have no clock and for most of
    a run they really do stand at 0 px between two shots. But two things move on
    their own and both are correct:

      * THE BATTERY ICON, 23 px at x=145..151, y=4..13, with the green fill
        among the colours -- measured on the language screen and again between
        two visits to the main menu. It repaints when it feels like it: one run
        of this script showed 0 px here and the next showed 23.
      * THE SCROLLING LABEL of the main menu's preview pane, about 960 px, which
        is why the main menu is only floored on its LIST half (see below).

    So the premise is not "nothing moves", it is "nothing moves that could be
    mistaken for navigation". Moving the highlight one row costs 8 000 px,
    forty times this limit, and the smallest verdict below asks for 3 000. The
    measured value is printed every run so that a change is visible even when
    it passes."""
    global ok
    d = diffs.get(name) if half is None else lefts.get(name)
    if d is None:
        print("FAIL: %s is missing, cannot check the floor of %s"
              % (name, where))
        ok = False
    elif d > FLOOR_MAX:
        # And this FAILS the test, it is not a warning. The whole verdict below
        # rests on nothing moving on these screens that could be mistaken for
        # navigation. If that stops being true, the thresholds no longer tell
        # "the highlight moved" from "something flickered", and a test that
        # keeps saying OK with its premise broken is worse than none.
        print("FAIL: %s changes %d px without touching anything, more than "
              "the %d the battery icon costs. The noise floor of %s is no "
              "longer valid and neither are the thresholds below."
              % (name, d, FLOOR_MAX, where))
        ok = False
    else:
        print("  floor of %s: %d px (limit %d)" % (where, d, FLOOR_MAX))

floor_check("01-idle-again", "the language screen")
floor_check("06-menu-idle", "the main menu LIST", half="left")
if "06-menu-idle" in diffs:
    print("  (the main menu pane is NOT still: %d px, and that is correct "
          "-- the long label scrolls by itself. That is why the pane "
          "threshold is 3000 and not 1.)" % (diffs["06-menu-idle"] - lefts["06-menu-idle"]))

def verdict(name, what, hard=True):
    global ok
    d = diffs.get(name)
    tag = "" if hard else "   [informational]"
    if d is None:
        print("  %-20s NO SCREENSHOT%s" % (name, tag))
        if hard:
            ok = False
    elif d >= 3000:
        print("  %-20s %6d px  -> %s%s" % (name, d, what, tag))
    elif d >= 1000:
        print("  %-20s %6d px  -> something moved, but less than a whole "
              "item costs%s" % (name, d, tag))
        if hard:
            ok = False
    else:
        print("  %-20s %6d px  -> NO. That is not navigation%s" % (name, d, tag))
        if hard:
            ok = False

def verdict_both_halves(name, hard=True):
    """The preview pane has to move WITH the highlight, not instead of it."""
    global ok
    d, l = diffs.get(name), lefts.get(name)
    if d is None:
        print("  %-20s NO SCREENSHOT" % name)
        if hard:
            ok = False
        return
    r = d - l
    if l >= 3000 and r >= 3000:
        print("  %-20s list %5d px + pane %5d px  -> the highlight moved "
              "AND the pane followed it" % (name, l, r))
    else:
        print("  %-20s list %5d px + pane %5d px  -> NO: both halves "
              "are needed" % (name, l, r))
        if hard:
            ok = False

def verdict_same_as(name, ref, tol=1500, only_if=None):
    """Did MENU land back on the frame we left, or just somewhere else?

    The tolerance has room in it on purpose: a real change of screen costs
    6 000 px or more here, and the only thing that legitimately differs
    between two visits to the same menu is the scrolling label in the preview
    pane, worth under 1 000. 1 500 separates the two with no overlap. Measured
    on the run that went in: 0 px."""
    global ok
    if name not in frames or ref not in frames:
        print("  %-20s NO SCREENSHOT" % name)
        ok = False
        return
    # A trap worth guarding: if SELECT never got us out of the main menu, then
    # "MENU landed back on the frame we left" is TRUE for the stupidest of
    # reasons -- we never left. Without this the test would pass hardest
    # exactly when the navigation did nothing at all.
    if only_if is not None and diffs.get(only_if, 0) < 3000:
        print("  %-20s nothing can be said: %s never changed the "
              "screen, so there was no return trip to measure"
              % (name, only_if))
        ok = False
        return
    d = differ(frames[name], frames[ref])
    if d <= tol:
        print("  %-20s %6d px different from %s  -> MENU WENT BACK TO THE SAME "
              "SCREEN" % (name, d, ref))
    else:
        print("  %-20s %6d px different from %s  -> changed, but it is NOT "
              "the screen we left" % (name, d, ref))
        ok = False

print("VERDICT")
print(" the wheel on the language screen:")
verdict("02-wheel-fwd",  "the highlight moved")
verdict("03-wheel-fwd2", "the highlight moved")
verdict("04-wheel-back", "the highlight came back")
print()
print(" SELECT, which leaves the language screen for the main menu:")
verdict("05-select", "SELECT changed the screen")
print()
print(" the wheel on the main menu -- BOTH halves:")
verdict_both_halves("07-menu-wheel")
verdict_both_halves("08-menu-wheel-back")
print()
print(" going in and coming BACK:")
verdict("09-into-music", "SELECT entered Music")
verdict("10-menu-back",  "MENU changed the screen")
verdict_same_as("10-menu-back", "08-menu-wheel-back", only_if="09-into-music")
print()
print(" MENU at the top of the stack -- here the correct result is that NOTHING happens:")
print("   on the main menu there is no screen underneath to go back to, so")
print("   the list has to stay still. The pane may move: its")
print("   label scrolls by itself.")
l = lefts.get("11-menu-at-top")
if l is None:
    print("  %-20s NO SCREENSHOT" % "11-menu-at-top")
    ok = False
elif l <= FLOOR_MAX:
    print("  %-20s list %5d px + pane %5d px  -> did not navigate, which is "
          "correct" % ("11-menu-at-top", l, diffs["11-menu-at-top"] - l))
else:
    print("  %-20s list %5d px  -> the list moved, and from the main "
          "menu MENU should not lead anywhere"
          % ("11-menu-at-top", l))
    ok = False
print()
print("PNGs in %s" % out)
raise SystemExit(0 if ok else 2)
EOF
