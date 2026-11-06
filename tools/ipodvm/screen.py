"""Read the iPod screen as structured data and navigate menus by name.

Text comes from macOS Vision (screentext.swift, compiled on first use). The
selection bar is found from pixels, not a fixed colour, so themes and accent
colours work: it is the longest run of rows down the list's left edge whose
colour differs strongly from the list background. Its height gives the row
pitch, its width the list width (half-screen lists have a preview pane).
"""
from collections import Counter
from difflib import SequenceMatcher
import json
from pathlib import Path
import subprocess
import time

from core import (QEMU_ROOT, QMP, Failure, digest, is_blank, log, press, press_verified,
                  run_dir, sleep_warning)

OCR_SOURCE = Path(__file__).resolve().parent / 'screentext.swift'
OCR = QEMU_ROOT / 'build/ipodvm/screentext'

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


def ensure_ocr():
    if not OCR.exists() or OCR.stat().st_mtime < OCR_SOURCE.stat().st_mtime:
        OCR.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['swiftc', '-O', '-o', str(OCR), str(OCR_SOURCE)], check=True,
                       stdout=subprocess.DEVNULL)


def ocr(png):
    ensure_ocr()
    result = subprocess.run([str(OCR), str(png)], capture_output=True, text=True)
    if result.returncode:
        raise Failure(f'text recognition failed: {result.stderr.strip()}')
    return json.loads(result.stdout)


# ---- pixel analysis ----------------------------------------------------------

def pixel(frame, x, y):
    width, _, rgb = frame
    i = (y * width + x) * 3
    return rgb[i], rgb[i + 1], rgb[i + 2]


def distance(a, b):
    return sum(abs(p - q) for p, q in zip(a, b))


def selection_bar(frame):
    """Return (top, bottom, right, pitch) of the selection bar, or None."""
    width, height, _ = frame
    column = [pixel(frame, EDGE_X, y) for y in range(HEADER, height)]
    inner = [pixel(frame, INNER_X, y) for y in range(HEADER, height)]
    background = Counter((r >> 3, g >> 3, b >> 3) for r, g, b in column).most_common(1)[0][0]
    background = tuple(v << 3 for v in background)
    marked = [distance(a, background) > 90 or distance(b, background) > 90
              for a, b in zip(column, inner)]
    best, start = None, None
    for i, bar_row in enumerate(marked + [False]):
        if bar_row:
            if start is None:
                start = i
        elif start is not None:
            if BAR_HEIGHT[0] <= i - start <= BAR_HEIGHT[1] and (
                    best is None or i - start > best[1] - best[0]):
                best = (start, i)
            start = None
    if best is None:
        return None
    # A list bar starts at the screen edge; progress bars and icons do not.
    edge = sum(distance(column[i], background) > 90 for i in range(*best))
    if edge * 2 < best[1] - best[0]:
        return None
    top, bottom = best[0] + HEADER, best[1] + HEADER - 1
    # Follow neighbouring pixels (tolerates horizontal gradients) on rows near
    # the bar's top and bottom edges (clear of text); the widest wins, so
    # rounded corners or an icon in one row cannot shorten it.
    right = EDGE_X
    for y in {min(top + d, bottom) for d in (1, 2)} | {max(bottom - d, top) for d in (1, 2)}:
        x = EDGE_X
        while x + 1 < width and distance(pixel(frame, x + 1, y), pixel(frame, x, y)) < 40:
            x += 1
        right = max(right, x)
    if right + 1 < MIN_BAR_WIDTH:
        return None
    # A one-pixel separator line under the bar adds to the pitch.
    below = bottom + 1
    separator = below < height and distance(pixel(frame, EDGE_X, below), background) > 20
    return top, bottom, right + 1, bottom - top + 1 + (1 if separator else 0)


def split_value(text):
    """'Menu Cov... Theme' -> ('Menu Cov…', 'Theme') when OCR merges label and value."""
    for ellipsis in ('...', '…'):
        head, found, tail = text.partition(ellipsis)
        if found and tail.strip():
            return head.rstrip() + '…', tail.strip()
    return text, None


ARROWS = {'>', '›', '〉', '»'}


def analyse(frame, boxes):
    """Turn a frame and OCR boxes into {state, title, rows, selected, preview}."""
    width, height, _ = frame
    if is_blank(frame):
        return {'state': 'blank', 'title': None, 'rows': [], 'selected': None, 'preview': []}
    for box in boxes:
        box['cy'] = box['y'] + box['h'] / 2
    # Status icons (battery, charge bolt, play state) read as one or two stray
    # glyphs. Keep a short box only when it is the whole title, left of the icons
    # (a real two-letter title such as "EQ").
    header = sorted((b for b in boxes if b['cy'] < HEADER), key=lambda b: b['x'])
    header = [b for i, b in enumerate(header)
              if len(b['text'].strip()) > 2
              or (len(header) == 1 and b['x'] + b['w'] <= BATTERY_X)]
    title = ' '.join(b['text'] for b in header) or None
    if title and title[0] == '¡':        # "iPod" in the title font
        title = 'i' + title[1:]
    body = sorted((b for b in boxes if b['cy'] >= HEADER), key=lambda b: (b['cy'], b['x']))
    bar = selection_bar(frame)
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
    rows, selected = [], None
    for index, group in enumerate(groups):
        first_line = [b for b in group if abs(b['cy'] - group[0]['cy']) < pitch * 0.3]
        other_lines = [b for b in group if b not in first_line]
        submenu = any(b['text'].strip() in ARROWS for b in group)
        words = [b for b in sorted(first_line, key=lambda b: b['x'])
                 if b['text'].strip() not in ARROWS]
        label_boxes = [b for b in words if b['x'] < right * 0.55] or words[:1]
        value_boxes = [b for b in words if b not in label_boxes]
        label = ' '.join(b['text'] for b in label_boxes).replace('...', '…')
        value = ' '.join(b['text'] for b in value_boxes) or None
        if value is None:
            label, value = split_value(label)
        row = {'index': index, 'label': label}
        if value:
            row['value'] = value
        detail = ' '.join(b['text'] for b in sorted(other_lines, key=lambda b: (b['cy'], b['x'])))
        if detail:
            row['detail'] = detail
        if submenu:
            row['submenu'] = True
        centre = sum(b['cy'] for b in group) / len(group)
        if top <= group[0]['cy'] <= bottom or top <= centre <= bottom:
            row['selected'] = True
            selected = index
        rows.append(row)
    return {'state': 'list', 'title': title, 'rows': rows, 'selected': selected,
            'preview': preview, 'list_width': right}


def read_with(qmp, label='screen'):
    frame = qmp.frame()
    shot = qmp.shot(label)
    data = analyse(frame, ocr(shot))
    data['shot'] = str(shot)
    data['_frame'] = frame
    return data


def public(data):
    return {k: v for k, v in data.items() if not k.startswith('_')}


def screen(name):
    run = run_dir(name)
    with QMP(run) as qmp:
        data = read_with(qmp)
        if data['state'] in ('blank', 'other'):
            data['warning'] = sleep_warning(qmp.registers())
    return public(data)


# ---- navigation ------------------------------------------------------------

def normalise(text):
    return ' '.join(text.casefold().replace('’', "'").replace('...', '…').split())


def match_row(rows, target):
    """Find the row for `target`: exact label, then a truncated label, then a prefix."""
    want = normalise(target)
    for row in rows:
        if normalise(row['label']) == want:
            return row
    for row in rows:
        label = normalise(row['label'])
        if label.endswith('…') and want.startswith(label[:-1].rstrip()):
            return row
    for row in rows:
        if normalise(row['label']).startswith(want):
            return row
    return None


def is_title(data, title):
    """Loose title comparison: Vision reads the title font's i as I or ¡."""
    seen, want = normalise(data['title'] or ''), normalise(title)
    return seen == want or SequenceMatcher(None, seen, want).ratio() >= 0.75


def list_region(data):
    return (0, HEADER, data.get('list_width') or data['_frame'][0], data['_frame'][1] - HEADER)


def wake(qmp, data, steps):
    if data['state'] == 'charging':
        press(qmp, ['w'])
        steps.append('woke from the Charging screen')
        data = read_with(qmp, 'wake')
    if data['state'] in ('blank', 'other') and sleep_warning(qmp.registers()):
        raise Failure(sleep_warning(qmp.registers()))
    return data


def neighbours(rows, row):
    above = rows[row['index'] - 1]['label'] if row['index'] > 0 else None
    below = rows[row['index'] + 1]['label'] if row['index'] + 1 < len(rows) else None
    return above, below


def at_target(data, target, context):
    """Whether the selected row is `target`.

    A selected label that is too long scrolls (marquee) and reads garbled, so
    fall back to the neighbours the target had when it was last seen.
    """
    row = match_row(data['rows'], target)
    if row is not None:
        return bool(row.get('selected'))
    if context is None or data['selected'] is None:
        return False
    selected = data['rows'][data['selected']]
    above, below = neighbours(data['rows'], selected)
    return (context[0] is not None and above == context[0]) or (
        context[1] is not None and below == context[1])


def move_to(qmp, data, target, steps):
    """Scroll until `target` is the selected row; return the new screen."""
    direction, bounces, seen, context = 'e', 0, [], None
    for _ in range(40):
        seen += [r['label'] for r in data['rows'] if r['label'] not in seen]
        if at_target(data, target, context):
            return data
        row = match_row(data['rows'], target)
        if row is not None:
            context = neighbours(data['rows'], row)
        if row is not None and data['selected'] is not None:
            delta = row['index'] - data['selected']
            keys = ['e' if delta > 0 else 'q'] * min(abs(delta), MAX_WHEEL_BATCH)
        else:
            keys = [direction] * MAX_WHEEL_BATCH
        before = digest(data['_frame'], list_region(data))
        press(qmp, keys)
        data = read_with(qmp, f'scroll-{target}')
        if digest(data['_frame'], list_region(data)) == before and row is None:
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
    attempt = press_verified(qmp, 'select', region, touch=('e', 'q') if top_row else ('q', 'e'))
    data = read_with(qmp, f'open-{target}')
    steps.append(f'select {target!r} -> {data["title"]!r}'
                 + (f' (attempt {attempt})' if attempt > 1 else ''))
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
        press(qmp, ['w'])
        data = read_with(qmp, 'menu')
        steps.append(f'menu: {before!r} -> {data["title"]!r}')
        if data['title'] == before:
            qmp.run_for(10)       # slow screens: let the press take effect
            data = read_with(qmp, 'menu')
    raise Failure(f'could not reach the main menu; stuck at {data["title"]!r}')


def goto(name, path, from_root=True):
    """Navigate along `path` ('Settings/Theme' or a list of row labels)."""
    parts = [p.strip() for p in (path.split('/') if isinstance(path, str) else path) if p.strip()]
    run = run_dir(name)
    steps = []
    with QMP(run) as qmp:
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
