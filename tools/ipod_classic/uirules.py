#!/usr/bin/env python3
"""uirules.py -- read Apple's user-interface rule table straight out of the
retailOS image, so "does MENU do anything on this screen" is a question with an
answer instead of an opinion.

    python3 tools/ipod_classic/uirules.py assets/classic6g/osos_decrypted.bin
    python3 tools/ipod_classic/uirules.py IMAGE --grep button.menu.up
    python3 tools/ipod_classic/uirules.py IMAGE --grep 229444595 --raw

Every screen layout in the image carries a list of (trigger, handler, actions)
rules in a plain, self-describing serialisation.  This walks them.

THE FORMAT, worked out byte by byte from the block at file offset 0x0073e580
and then checked against all 1 380 rules in the image:

    STRING := u32 len ; char data[len]              (no NUL, not aligned)
    RULE   := STRING trigger ; u8 suffix            <- suffix is ASCII '0'..'9'
              STRING handler                        <- length 0 means none
              u32 naction ; ACTION[naction]
    ACTION := STRING name ; u32 nparam ; STRING param[nparam]

The one-byte suffix after the trigger name is why a naive strings(1) shows
"wheel1" and "button.menu.up1" with a trailing digit while handler and action
names have none: the digit is not part of the string, it is the field after it.
Its meaning is UNKNOWN -- every rule in the image has '1'.

Rules come in groups, each group being "u32 count; RULE[count]" padded to four
bytes, and the groups hang off a bigger container that this does not decode.
So the SCREEN name printed for each rule is the nearest preceding
*_Screen / *_Layout string in the file: a good label, not a parse.  With
--raw the file offset is printed too, which is what to quote in a bug report.

WHAT IT IS FOR, and the case that paid for it: on the first-boot language
chooser MENU looks broken.  It is not.

    $ uirules.py osos_decrypted.bin --grep button.menu.up | grep ResetAll
    0073e628 SettingsMenus_ResetAllSettings_Language_Layout
             button.menu.up1 -> (no handler) -> (no actions)

An empty rule, written by Apple.  The image holds 108 button.menu.up rules; 62
of them do something and 46, including that one, deliberately do nothing.
Measuring MENU there measures nothing.

The label on the left is the NEAREST PRECEDING name, so it can be one screen
behind on the rule that starts a new block -- the one quoted above was checked
by hand against the bytes at 0x0073e620..0x0073e71d and is right.  Trust the
offset, treat the label as a hint.
"""
import argparse
import bisect
import re
import struct
import sys

NAME = re.compile(rb'^[A-Za-z0-9_.]+$')

# Triggers are named after what fires them. This is the vocabulary the image
# actually uses; it is the anchor for finding rules, not a filter on output.
TRIGGER = re.compile(
    rb'(button\.[a-z.]+'
    rb'|wheel'
    rb'|list\.[a-z0-9.]*(chosen|delayedselected|selected|entered|exited)'
    rb'|controller\.[A-Za-z0-9_]+)')


class Blob:
    def __init__(self, data):
        self.d = data

    def string(self, off, maxlen=200):
        """u32 length then that many bytes, or (None, off) if it is not one."""
        if off + 4 > len(self.d):
            return None, off
        (n,) = struct.unpack_from('<I', self.d, off)
        if n > maxlen:
            return None, off
        s = self.d[off + 4:off + 4 + n]
        if len(s) != n:
            return None, off
        if n and not NAME.match(s):
            return None, off
        return s.decode('ascii'), off + 4 + n

    def rule(self, off):
        """Parse one RULE at off. None if the bytes there are not one."""
        d = self.d
        trig, o = self.string(off)
        if not trig:
            return None
        if o >= len(d) or not (0x30 <= d[o] <= 0x39):
            return None
        suffix = chr(d[o])
        o += 1
        handler, o = self.string(o)
        if handler is None or o + 4 > len(d):
            return None
        (nact,) = struct.unpack_from('<I', d, o)
        o += 4
        if nact > 16:
            return None
        actions = []
        for _ in range(nact):
            name, o = self.string(o)
            if not name or o + 4 > len(d):
                return None
            (nparam,) = struct.unpack_from('<I', d, o)
            o += 4
            if nparam > 16:
                return None
            params = []
            for _ in range(nparam):
                p, o = self.string(o)
                if p is None:
                    return None
                params.append(p)
            actions.append((name, params))
        return trig, suffix, handler, actions, o


def context_index(data):
    """Offsets of every length-prefixed screen/layout name, for labelling."""
    names = {}
    for m in re.finditer(rb'[A-Za-z0-9_.]{5,90}', data):
        s, e = m.start(), m.end()
        if s >= 4:
            (ln,) = struct.unpack_from('<I', data, s - 4)
            if ln == e - s:
                v = data[s:e].decode('ascii')
                if v.endswith('_Screen') or v.endswith('_Layout') \
                        or '_Screen_' in v:
                    names[s] = v
    return sorted(names), names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('--grep', default=None,
                    help='only rules whose text contains this')
    ap.add_argument('--raw', action='store_true',
                    help='print the file offset of every rule')
    a = ap.parse_args()

    data = open(a.image, 'rb').read()
    blob = Blob(data)
    keys, names = context_index(data)

    def label(off):
        j = bisect.bisect_left(keys, off)
        return names[keys[j - 1]] if j else '?'

    seen = set()
    n_total = n_shown = 0
    for m in TRIGGER.finditer(data):
        off = m.start() - 4
        if off < 0 or off in seen:
            continue
        r = blob.rule(off)
        if r is None or r[0] != m.group().decode():
            continue
        seen.add(off)
        n_total += 1
        trig, suffix, handler, actions, _ = r
        acts = '; '.join('%s(%s)' % (nm, ','.join(ps)) for nm, ps in actions)
        line = '%-52s %s%s -> %-28s %s' % (
            label(off), trig, suffix, handler or '(no handler)',
            acts or '(no actions)')
        if a.raw:
            line = '%08x %s' % (off, line)
        if a.grep and a.grep not in line:
            continue
        n_shown += 1
        print(line)

    print('\n%d rules parsed, %d shown' % (n_total, n_shown), file=sys.stderr)


if __name__ == '__main__':
    sys.exit(main())
