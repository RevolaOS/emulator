"""Read the iPod screen as structured data and navigate menus by name.

Text is the strings the firmware draws in one full redraw, collected with
breakpoints. The selection bar is found from pixels, not a fixed colour, so
themes and accent colours work: it is the longest run of rows down the list's
left edge whose colour differs strongly from the list background. Its height
gives the row pitch, its width the list width (half-screen lists have a
preview pane).
"""
from collections import Counter
import contextlib
import struct

from core import (GDB, QMP, Failure, asleep, check_panic, digest, is_blank, is_panic, press,
                  press_verified, run_dir, settle_frame)

# Firmware 2.0.5, named as in revola's firmware/osos-2.0.5.h.
DRAW_STRING = 0x082883f8        # TextView_DrawString(view, text)
MARQUEE_COLOR = 0x081a8f0c      # SITE_MarqueeTextView_DrawColor_call: r0 context, r4 view
DRAW_UTF8 = 0x08259648          # GraphicsContext_DrawUtf8(context, text)
FONT_ASCENT = 0x08298784        # GraphicsContext_GetFontAscent(context)
FONT_DESCENT = 0x082987b8       # GraphicsContext_GetFontDescent(context)
MEASURE_UTF8 = 0x08259d10       # GraphicsContext_MeasureUtf8(context, text)
GET_ROOT_VIEW = 0x08113600      # UI_GetRootView()
INVALIDATE_RECT = 0x082655a0    # View_InvalidateRect(view, rect)
SELECTED_ROW = 0x08993504       # SelectedRowTextView_vtable
VIEW_BOUNDS = 0x80
TEXT_VIEW_CONTEXT, TEXT_VIEW_TEXT = 0xa4, 0xf8
CONTEXT_PEN = 0x00              # x, then baseline, as GraphicsContext_SetTextPen stores them
CONTEXT_ORIGIN = 0x2c           # GraphicsContext_origin_x, then y
CONTEXT_CLIP = 0x34             # GraphicsContext_clip_top, then left, bottom, right
# The UI task's loop starts each pass here, woken by its 33 ms timer, with
# r0-r3, r12, lr and the flags unused.
UI_LOOP = 0x08288b90
UI_LOOP_CODE = bytes.fromhex('1c108de2')    # add r1, sp, #28; other firmware differs
UI_WAIT = 1.0                   # seconds to wait for the UI loop or the next string

HEADER = 21              # title bar height in pixels
EDGE_X = 1               # column sampled for the selection bar (left of icons and text)
# Inside the bar on half- and full-width lists; covers rows where the selected
# row's scrolling label crosses EDGE_X.
INNER_X = 158
ROOT_TITLE = 'iPod'
MAX_WHEEL_BATCH = 3
BAR_HEIGHT = (12, 70)    # one-line rows are ~23 px, two-line rows ~53 px
MIN_BAR_WIDTH = 150      # list bars span the list: 160 (with preview pane) or 320
BATTERY_X = 260          # status icons sit right of this on full-width title bars
# A scrolled list shows a 6 px scrollbar thumb at its right edge for about two
# seconds; change detection leaves that strip out so the fade is not a change.
SCROLLBAR = 8


def drawn_strings(qmp):
    """Return the strings of one full redraw as boxes {text, x, y, w, h, selected}.

    Every string the UI draws passes through TextView_DrawString, which leaves
    the view's graphics context holding the string's clip in screen
    coordinates. A scrolling (marquee) row's text is rendered once and only
    passes the marquee site, with the clip already set. Text that RevolaOS
    draws on a canvas goes to GraphicsContext_DrawUtf8 at the context's pen.
    Breakpoints on the three collect the redraw that follows
    View_InvalidateRect on the root view, called at the top of the UI loop;
    a canvas string's font is measured with calls at its breakpoint. The guest
    stops at each string and is left as it was, running or paused. Nothing is
    returned while the UI loop does not run (boot, standby) or on other
    firmware.
    """
    loop, draw, marquee, utf8 = (
        (0, address, 4) for address in (UI_LOOP, DRAW_STRING, MARQUEE_COLOR, DRAW_UTF8))
    running = qmp.running()
    boxes = {}
    try:
        with GDB(qmp.run) as gdb:
            active = set()

            def points(*wanted):
                for point in active - set(wanted):
                    gdb.remove(point)
                    active.discard(point)
                for point in set(wanted) - active:
                    gdb.insert(point)
                    active.add(point)

            def resume():
                """Run to the next point; None when the guest reaches none in time."""
                gdb.send('c')
                if gdb.wait_stop(UI_WAIT) is None:
                    gdb.interrupt()
                    return None
                return gdb.registers()

            def step_off():
                points()
                gdb.step()
                points(loop, draw, marquee, utf8)

            def add(text, x, y, w, h, selected=False):
                data = b''
                while b'\0' not in data and len(data) < 1024:
                    data += gdb.read(text + len(data), 256)
                text = data.split(b'\0')[0].decode('utf-8', 'replace').strip()
                if text:
                    # The selected row can be drawn twice in one redraw.
                    box = boxes.setdefault((x, y, text), {
                        'text': text, 'x': x, 'y': y, 'w': w, 'h': h, 'selected': False})
                    box['selected'] |= selected

            def add_view(view, context, text):
                top, left, bottom, right = struct.unpack(
                    '<4i', gdb.read(context + CONTEXT_CLIP, 16))
                selected = struct.unpack('<I', gdb.read(view, 4))[0] == SELECTED_ROW
                add(text, left, top, right - left, bottom - top, selected)

            try:
                regs = None
                if gdb.read(UI_LOOP, 4) == UI_LOOP_CODE:
                    points(loop)
                    regs = resume()
                    points()
                root = gdb.call(GET_ROOT_VIEW) if regs else 0
                if root:
                    gdb.call(INVALIDATE_RECT, root, root + VIEW_BOUNDS)
                    # The redraw starts some passes later and ends with its pass.
                    points(draw, marquee, utf8)
                    while regs := resume():
                        if regs['pc'] == DRAW_STRING:
                            # The clip is set by the time the function returns.
                            view, text, back = regs['r0'], regs['r1'], regs['lr']
                            points((0, back & ~1, 2 if back & 1 else 4))
                            if not resume():
                                break
                            if text:
                                add_view(view, view + TEXT_VIEW_CONTEXT, text)
                            points(loop, draw, marquee, utf8)
                        elif regs['pc'] == MARQUEE_COLOR:
                            view = regs['r4']
                            text, = struct.unpack('<I', gdb.read(view + TEXT_VIEW_TEXT, 4))
                            if text:
                                add_view(view, regs['r0'], text)
                            step_off()
                        elif regs['pc'] == DRAW_UTF8:
                            context, text = regs['r0'], regs['r1']
                            points()
                            if text:
                                ascent = gdb.call(FONT_ASCENT, context)
                                descent = gdb.call(FONT_DESCENT, context)
                                width = gdb.call(MEASURE_UTF8, context, text)
                                x, baseline = struct.unpack(
                                    '<2i', gdb.read(context + CONTEXT_PEN, 8))
                                left, top = struct.unpack(
                                    '<2i', gdb.read(context + CONTEXT_ORIGIN, 8))
                                add(text, left + x, top + baseline - ascent, width,
                                    ascent + descent)
                            step_off()
                        else:
                            break
            finally:
                with contextlib.suppress(Failure):
                    points()
    finally:
        if running:
            qmp.cont()
    return list(boxes.values())


# ---- pixel analysis ----------------------------------------------------------

def pixel(frame, x, y):
    width, _, rgb = frame
    i = (y * width + x) * 3
    return rgb[i], rgb[i + 1], rgb[i + 2]


def distance(a, b):
    return sum(abs(p - q) for p, q in zip(a, b))


def selection_bar(frame, edge_x=EDGE_X):
    """Return (top, bottom, right, pitch) of the selection bar, or None.

    `edge_x` is a column inside the bar, left of its text.
    """
    width, height, _ = frame
    column = [pixel(frame, edge_x, y) for y in range(HEADER, height)]
    inner = [pixel(frame, INNER_X, y) for y in range(HEADER, height)]
    background = Counter((r >> 3, g >> 3, b >> 3) for r, g, b in column).most_common(1)[0][0]
    background = tuple(v << 3 for v in background)
    marked = [distance(a, background) > 90 or distance(b, background) > 90
              for a, b in zip(column, inner)]
    at_edge = [distance(a, background) > 90 for a in column]
    best, start = None, None
    for i, bar_row in enumerate(marked + [False]):
        if bar_row:
            if start is None:
                start = i
        elif start is not None:
            # INNER_X can also hit a scrollbar thumb next to the bar; the bar
            # itself starts and ends with rows marked at the screen edge.
            edge_rows = [j for j in range(start, i) if at_edge[j]]
            if edge_rows:
                top, end = edge_rows[0], edge_rows[-1] + 1
                if BAR_HEIGHT[0] <= end - top <= BAR_HEIGHT[1] and (
                        best is None or end - top > best[1] - best[0]):
                    best = (top, end)
            start = None
    if best is None:
        return None
    # A list bar starts at the screen edge; progress bars and icons do not.
    edge = sum(at_edge[i] for i in range(*best))
    if edge * 2 < best[1] - best[0]:
        return None
    top, bottom = best[0] + HEADER, best[1] + HEADER - 1
    # Follow neighbouring pixels (tolerates horizontal gradients) on rows near
    # the bar's top and bottom edges (clear of text); the widest wins, so
    # rounded corners or an icon in one row cannot shorten it.
    right = edge_x
    for y in {min(top + d, bottom) for d in (1, 2)} | {max(bottom - d, top) for d in (1, 2)}:
        x = edge_x
        while x + 1 < width and distance(pixel(frame, x + 1, y), pixel(frame, x, y)) < 40:
            x += 1
        right = max(right, x)
    if right + 1 < MIN_BAR_WIDTH:
        return None
    # A one-pixel separator line under the bar adds to the pitch.
    below = bottom + 1
    separator = below < height and distance(pixel(frame, edge_x, below), background) > 20
    return top, bottom, right + 1, bottom - top + 1 + (1 if separator else 0)


def analyse(frame, boxes):
    """Turn a frame and its drawn strings into {state, title, rows, selected, preview}."""
    width, height, _ = frame
    if is_blank(frame):
        return {'state': 'blank', 'title': None, 'rows': [], 'selected': None, 'preview': []}
    if is_panic(frame):
        return {'state': 'panic', 'title': None, 'rows': [], 'selected': None, 'preview': []}
    for box in boxes:
        box['cy'] = box['y'] + box['h'] / 2
    header = sorted((b for b in boxes if b['cy'] < HEADER and b['x'] < BATTERY_X),
                    key=lambda b: b['x'])
    title = header[0]['text'] if header else None
    body = sorted((b for b in boxes if b['cy'] >= HEADER), key=lambda b: (b['cy'], b['x']))
    bar = selection_bar(frame)
    if bar is None and any(b['selected'] for b in body):
        # Thumbnail lists: the bar starts right of the artwork, where the text does.
        text_x = Counter(b['x'] for b in body).most_common(1)[0][0]
        bar = selection_bar(frame, max(text_x - 1, 0))
    if bar is None:
        state = 'charging' if title == 'Charging' else 'other'
        return {'state': state, 'title': title, 'rows': [], 'selected': None,
                'text': [b['text'] for b in body], 'preview': []}

    top, bottom, right, pitch = bar
    listed = [b for b in body if b['x'] < right - 4]
    preview = [b['text'] for b in body if b['x'] >= right - 4]
    # Group into rows by distance from each row's first line.
    groups = []
    for b in listed:
        if groups and b['cy'] - groups[-1][0]['cy'] < pitch * 0.75:
            groups[-1].append(b)
        else:
            groups.append([b])
    # The selected row's text has its own view class; the bar decides without one.
    by_view = any(b['selected'] for b in listed)
    rows, selected = [], None
    for index, group in enumerate(groups):
        first_line = [b for b in group if abs(b['cy'] - group[0]['cy']) < pitch * 0.3]
        other_lines = [b for b in group if b not in first_line]
        words = sorted(first_line, key=lambda b: b['x'])
        row = {'index': index, 'label': words[0]['text']}
        value = ' '.join(b['text'] for b in words[1:])
        if value:
            row['value'] = value
        detail = ' '.join(b['text'] for b in sorted(other_lines, key=lambda b: (b['cy'], b['x'])))
        if detail:
            row['detail'] = detail
        centre = sum(b['cy'] for b in group) / len(group)
        if any(b['selected'] for b in group) if by_view else (
                top <= group[0]['cy'] <= bottom or top <= centre <= bottom):
            row['selected'] = True
            selected = index
        rows.append(row)
    return {'state': 'list', 'title': title, 'rows': rows, 'selected': selected,
            'preview': preview, 'list_width': right}


def read_with(qmp, label='screen'):
    frame = qmp.frame()
    shot = qmp.shot(label)
    # Neither screen comes from the UI loop.
    boxes = [] if is_blank(frame) or is_panic(frame) else drawn_strings(qmp)
    data = analyse(frame, boxes)
    data['shot'] = str(shot)
    data['_frame'] = frame
    return data


def public(data):
    return {k: v for k, v in data.items() if not k.startswith('_')}


def screen(name):
    run = run_dir(name)
    with QMP(run) as qmp:
        data = read_with(qmp)
        if asleep(qmp.registers()):
            data['asleep'] = True
    return public(data)


# ---- navigation ------------------------------------------------------------

def normalise(text):
    return ' '.join(text.casefold().replace('’', "'").replace('...', '…').split())


def match_row(rows, target):
    """Find the row for `target`: exact label, then a prefix."""
    want = normalise(target)
    for row in rows:
        if normalise(row['label']) == want:
            return row
    for row in rows:
        if normalise(row['label']).startswith(want):
            return row
    return None


def is_title(data, title):
    return normalise(data['title'] or '') == normalise(title)


def list_region(data):
    """The list area without its scrollbar strip, for change detection."""
    width = data.get('list_width') or data['_frame'][0]
    return (0, HEADER, width - SCROLLBAR, data['_frame'][1] - HEADER)


def wake(qmp, data, steps):
    if data['state'] == 'charging':
        press(qmp, ['w'])
        steps.append('woke from the Charging screen')
        data = read_with(qmp, 'wake')
    return data


def move_to(qmp, data, target, steps):
    """Scroll until `target` is the selected row; return the new screen."""
    direction, bounces, seen = 'e', 0, []
    for _ in range(40):
        seen += [r['label'] for r in data['rows'] if r['label'] not in seen]
        row = match_row(data['rows'], target)
        if row is not None and row.get('selected'):
            return data
        if row is not None and data['selected'] is not None:
            delta = row['index'] - data['selected']
            keys = ['e' if delta > 0 else 'q'] * min(abs(delta), MAX_WHEEL_BATCH)
        else:
            keys = [direction] * MAX_WHEEL_BATCH
        region = list_region(data)
        before = (digest(data['_frame'], region), data['selected'],
                  [r['label'] for r in data['rows']])
        press(qmp, keys, region=region)
        data = read_with(qmp, f'scroll-{target}')
        after = (digest(data['_frame'], region), data['selected'],
                 [r['label'] for r in data['rows']])
        unmoved = after[0] == before[0] or (after[1] is not None and after[1:] == before[1:])
        if unmoved and row is None:
            # Reached an end of the list without finding it: try the other way once.
            bounces += 1
            if bounces > 1:
                break
            direction = 'q' if direction == 'e' else 'e'
        steps.append(f'{"".join(keys)} -> {selected_label(data)}')
    raise Failure(f'{target!r} not found in {data["title"]!r}; rows seen while scrolling: '
                  f'{seen}')


def selected_label(data):
    if data['selected'] is None:
        return None
    return data['rows'][data['selected']]['label']


def open_row(qmp, data, target, steps):
    """Select the highlighted row and return the screen it leads to."""
    top_row = data['selected'] == 0
    region = list_region(data)
    verified = press_verified(qmp, 'select', region,
                              touch=('e', 'q') if top_row else ('q', 'e'))
    attempt = verified['attempts']
    data = read_with(qmp, f'open-{target}')
    steps.append(f'select {target!r} -> {data["title"]!r}'
                 + (f' (attempt {attempt})' if attempt > 1 else '')
                 + (f' ({verified["warning"]})' if 'warning' in verified else ''))
    return data


def to_root(qmp, data, steps):
    for _ in range(10):
        if is_title(data, ROOT_TITLE) and data['state'] == 'list':
            return data
        if is_title(data, 'Language') and data['state'] == 'list':
            data = move_to(qmp, data, 'English', steps)
            data = open_row(qmp, data, 'English', steps)
            continue
        before = data['title']
        # The main menu's preview pane animates; watch only the list it leaves.
        press(qmp, ['w'], region=list_region(data) if data['state'] == 'list' else None)
        data = read_with(qmp, 'menu')
        steps.append(f'menu: {before!r} -> {data["title"]!r}')
        if data['title'] == before:
            # Slow screens: give the press up to 10 s more to take effect.
            settle_frame(qmp, digest(data['_frame']), 10)
            data = read_with(qmp, 'menu')
    raise Failure(f'could not reach the main menu; stuck at {data["title"]!r}')


def goto(name, path, from_root=True):
    """Navigate along `path` ('Settings/Theme' or a list of row labels)."""
    parts = [p.strip() for p in (path.split('/') if isinstance(path, str) else path) if p.strip()]
    run = run_dir(name)
    steps = []
    with QMP(run) as qmp:
        check_panic(qmp, qmp.frame())
        data = wake(qmp, read_with(qmp), steps)
        if from_root:
            data = to_root(qmp, data, steps)
        for part in parts:
            if data['state'] != 'list':
                raise Failure(f'{data["title"]!r} is not a list; cannot open {part!r}')
            data = move_to(qmp, data, part, steps)
            data = open_row(qmp, data, part, steps)
    result = public(data)
    result['steps'] = steps
    return result
