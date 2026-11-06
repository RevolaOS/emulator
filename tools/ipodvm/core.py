"""VM lifecycle, input, screenshots and profiling for iPod Classic RepriseOS VMs.

Functions return plain data and report progress through `log` (stderr), so
the CLI and the MCP server can share them. Each VM lives in
/tmp/ipodvm/<name>/, short enough for macOS's 104-byte Unix socket limit.
"""
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

TOOL = Path(__file__).resolve().parent
QEMU_ROOT = TOOL.parent.parent
QEMU = QEMU_ROOT / 'build/qemu-system-arm'
PLUGIN_DIR = QEMU_ROOT / 'build/contrib/plugins'
PLUGIN = PLUGIN_DIR / 'libfuncprof.so'
RUNS = Path('/tmp/ipodvm')

# IMG1 header stripped for QEMU's raw -kernel load at 0x08000000.
IMG1_HEADER = 0x800

# Complete feature-test configuration (AGENTS.md, README "Running the music player").
ENVIRONMENT = dict(
    IRAMSPILL='0x10000', SHA1_ZERO='1', SM1_IRQ='1', SM1_SLOTS='1',
    SM1_NSLOTS='2', SM1_DECODE='1', SM1_FEED_OFF='1', DISP_ALPHA='1',
    PMUCHARGER='adapter', HLE_AUDIO_DATAPATH='1', HLE_AUDIO_SUSTAIN='1',
)

WHEEL_KEYS = {'q', 'e'}
WHEEL_WAIT, BUTTON_WAIT = 3.0, 5.0
KEY_NAMES = {'select': 'ret', 'menu': 'w', 'up': 'q', 'down': 'e',
             'prev': 'a', 'next': 'd', 'play': 's'}
VALID_KEYS = set(KEY_NAMES) | set(KEY_NAMES.values())

# Firmware sleep spins in IRAM polling a bit QEMU never sets; LR reads 0 there.
# 0x22003b70 is the normal idle loop and is not sleep.
IDLE_LOOP = 0x22003b70


class Failure(Exception):
    pass


def log(message):
    print(message, file=sys.stderr, flush=True)


# ---- QMP ------------------------------------------------------------------

class QMP:
    def __init__(self, run):
        self.run = run
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(30)
        try:
            self.socket.connect(str(run / 'qmp.sock'))
            self.stream = self.socket.makefile('rwb', buffering=0)
            if 'QMP' not in json.loads(self.stream.readline()):
                raise Failure('unexpected QMP greeting')
            self.execute('qmp_capabilities')
        except OSError as error:
            self.socket.close()
            raise Failure(f'{run.name}: cannot reach QMP ({error}); is the VM running?')
        except Failure:
            self.socket.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.socket.close()

    def execute(self, name, arguments=None):
        message = {'execute': name}
        if arguments is not None:
            message['arguments'] = arguments
        self.stream.write((json.dumps(message) + '\n').encode())
        while True:
            line = self.stream.readline()
            if not line:
                raise Failure('QMP disconnected')
            reply = json.loads(line)
            if 'error' in reply:
                raise Failure(f"QMP {name}: {reply['error'].get('desc', reply['error'])}")
            if 'return' in reply:
                return reply['return']

    def hmp(self, line):
        return self.execute('human-monitor-command', {'command-line': line})

    def running(self):
        return self.execute('query-status')['running']

    def cont(self):
        self.execute('cont')

    def stop(self):
        self.execute('stop')

    def run_for(self, seconds):
        """Give the guest real run time, then pause it again."""
        self.cont()
        try:
            time.sleep(seconds)
        finally:
            self.stop()

    def frame(self):
        """Return (width, height, rgb bytes) of the current display."""
        with tempfile.NamedTemporaryFile(suffix='.ppm', dir=RUNS) as tmp:
            self.execute('screendump', {'filename': tmp.name})
            return read_ppm(Path(tmp.name).read_bytes())

    def png(self, path):
        self.execute('screendump', {'filename': str(path), 'format': 'png'})
        return path

    def shot(self, label):
        return self.png(next_shot(self.run, label))

    def registers(self):
        text = self.hmp('info registers')
        regs = {f'r{int(n)}': int(v, 16)
                for n, v in re.findall(r'\bR(\d\d)=([0-9a-fA-F]{8})', text)}
        regs.update(sp=regs.get('r13', 0), lr=regs.get('r14', 0), pc=regs.get('r15', 0))
        return regs


def read_ppm(data):
    fields, offset = [], 0
    while len(fields) < 4:
        while data[offset:offset + 1].isspace():
            offset += 1
        if data[offset:offset + 1] == b'#':
            offset = data.index(b'\n', offset) + 1
            continue
        end = offset
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[offset:end])
        offset = end
    if fields[0] != b'P6' or fields[3] != b'255':
        raise Failure('unsupported screendump format')
    width, height = int(fields[1]), int(fields[2])
    return width, height, data[offset + 1:offset + 1 + width * height * 3]


def digest(frame, region=None):
    width, height, rgb = frame
    if region is None:
        return hashlib.sha1(rgb).hexdigest()
    x, y, w, h = region
    rows = (rgb[((y + row) * width + x) * 3:((y + row) * width + x + w) * 3]
            for row in range(h) if y + row < height)
    return hashlib.sha1(b''.join(rows)).hexdigest()


def is_blank(frame):
    rgb = frame[2]
    return rgb.count(0) == len(rgb)


def parse_region(text):
    return tuple(int(v) for v in text.split(',')) if text else None


# ---- run directories -------------------------------------------------------

def run_dir(name):
    run = RUNS / name
    if not (run / 'launch.json').exists():
        raise Failure(f'no VM named {name!r}; see `ipodvm list`')
    return run


def pid_alive(pid):
    # A long-lived parent (the MCP server) must reap its exited QEMU children,
    # or they linger as zombies that still answer kill(pid, 0).
    try:
        if os.waitpid(pid, os.WNOHANG)[0] == pid:
            return False
    except ChildProcessError:
        pass
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def vm_pid(run):
    try:
        pid = int((run / 'pid').read_text())
    except (FileNotFoundError, ValueError):
        return None
    return pid if pid_alive(pid) else None


def next_shot(run, label):
    shots = run / 'shots'
    shots.mkdir(exist_ok=True)
    index = len(list(shots.glob('*.png')))
    safe = re.sub(r'[^A-Za-z0-9_-]+', '-', label).strip('-') or 'shot'
    return shots / f'{index:03d}-{safe}.png'


def parse_layout(path):
    text = Path(path).read_text()
    values = {}
    for key in ('CFW_PAYLOAD_BASE', 'CFW_PAYLOAD_FILE_OFFSET'):
        match = re.search(rf'#define\s+{key}\s+(0x[0-9a-fA-F]+)', text)
        if not match:
            raise Failure(f'{path}: {key} not found')
        values[key] = int(match.group(1), 16)
    return values['CFW_PAYLOAD_BASE'], values['CFW_PAYLOAD_FILE_OFFSET']


def find_layout(start):
    for directory in [start, *start.parents]:
        candidate = directory / 'payload/native/layout.h'
        if candidate.is_file():
            return candidate
    return None


def owner_label(name, cwd):
    """Window title naming the VM, where it was launched from and by which session."""
    try:
        top = subprocess.run(['git', 'rev-parse', '--show-toplevel'], cwd=cwd, text=True,
                             capture_output=True, check=True).stdout.strip()
        branch = subprocess.run(['git', 'branch', '--show-current'], cwd=cwd, text=True,
                                capture_output=True, check=True).stdout.strip()
        place = Path(top).name + (f' ({branch})' if branch else '')
    except (subprocess.CalledProcessError, FileNotFoundError):
        place = Path(cwd).name
    parts = [name, place]
    session = os.environ.get('CLAUDE_CODE_SESSION_ID')
    if session:
        parts.append(f'claude {session[:8]}')
    return ' · '.join(parts)


# ---- ELF symbols -----------------------------------------------------------

def elf_sections(data):
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x2e)
    return [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize)
            for i in range(shnum)]


def elf_functions(data):
    """Return [(addr, size, name)] for STT_FUNC symbols of a 32-bit LE ELF."""
    sections = elf_sections(data)
    functions = []
    for section in sections:
        if section[1] != 2:          # SHT_SYMTAB
            continue
        strtab = sections[section[6]]
        for offset in range(section[4], section[4] + section[5], 16):
            name, value, size, info, _, _ = struct.unpack_from('<IIIBBH', data, offset)
            if info & 0xf != 2 or value == 0:   # STT_FUNC
                continue
            start = strtab[4] + name
            label = data[start:data.index(b'\0', start)].decode()
            functions.append((value & ~1, size, label))
    return sorted(set(functions))


def elf_bytes(data, addr, length):
    for s in elf_sections(data):
        kind, sh_addr, sh_offset, sh_size = s[1], s[3], s[4], s[5]
        if kind == 1 and sh_addr <= addr and addr + length <= sh_addr + sh_size:
            return data[sh_offset + addr - sh_addr:sh_offset + addr - sh_addr + length]
    return None


def profile_symbols(elf, patterns, fns, image, base, file_offset):
    """Select watched functions and check the ELF describes this image's code."""
    entries = []
    if elf:
        data = Path(elf).read_bytes()
        if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
            raise Failure(f'{elf}: not a 32-bit little-endian ELF')
        # Linker interworking veneers only tail-jump to the real function.
        # Only code inside the loaded payload can be checked against the image.
        payload_end = base + len(image) - file_offset
        functions = [f for f in elf_functions(data)
                     if not re.fullmatch(r'__.+_from_(arm|thumb)', f[2])
                     and base <= f[0] < payload_end]
        patterns = patterns or ['*']
        chosen = [f for f in functions if any(fnmatch.fnmatch(f[2], p) for p in patterns)]
        if not chosen:
            raise Failure(f'no functions in {elf} match {patterns}')
        payload = image[file_offset:]
        mismatched = []
        for addr, size, name in chosen:
            length = max(4, min(size, 16))
            if elf_bytes(data, addr, length) != payload[addr - base:addr - base + length]:
                mismatched.append(name)
        if mismatched:
            raise Failure(f'{elf} does not match the firmware payload '
                          f'({len(mismatched)} functions differ, e.g. '
                          f'{", ".join(mismatched[:5])}); rebuild or pass the matching ELF')
        entries += [(addr, name) for addr, _, name in chosen]
    for spec in fns or []:
        addr, _, name = spec.partition(':')
        entries.append((int(addr, 0), name or addr))
    unique = {}
    for addr, name in entries:      # aliases share an entry; keep the first name
        unique.setdefault(addr, name)
    return sorted(unique.items())


def ensure_plugin():
    source = QEMU_ROOT / 'contrib/plugins/funcprof.c'
    if not PLUGIN.exists() or PLUGIN.stat().st_mtime < source.stat().st_mtime:
        subprocess.run(['make', '-C', str(PLUGIN_DIR), PLUGIN.name], check=True,
                       stdout=subprocess.DEVNULL)


# ---- test disks ------------------------------------------------------------

def fat_offset(disk):
    """Byte offset of the disk's FAT32 volume.

    iPod Classic disks use 4 KiB sectors, so MBR LBAs are tried as both 512-
    and 4096-byte units and confirmed by the FAT32 boot-sector signature.
    """
    with open(disk, 'rb') as f:
        mbr = f.read(512)
        if mbr[510:512] != b'\x55\xaa':
            raise Failure(f'{disk}: no MBR')
        for i in range(4):
            lba, = struct.unpack_from('<I', mbr, 0x1be + 16 * i + 8)
            for sector in (4096, 512):
                if lba:
                    f.seek(lba * sector + 0x52)
                    if f.read(8) == b'FAT32   ':
                        return lba * sector
    raise Failure(f'{disk}: no FAT32 volume found')


def prepare_disk(base, out, themes=None, clean=True):
    """Clone `base` to `out` and install theme packs from `themes`.

    `themes` mirrors the device's /themes tree (classic/default.bin,
    classic/dark.bin, <id>/theme.bin, ...); only .bin files are copied, as the
    installer does. With `clean`, existing /themes entries are removed first so
    stale packs from older catalogs cannot be discovered.
    """
    base, out = Path(base).expanduser().resolve(), Path(out).expanduser().resolve()
    if base == out:
        raise Failure('output must differ from the base disk')
    for tool in ('mcopy', 'mmd', 'mdeltree', 'mdir'):
        if not shutil.which(tool):
            raise Failure(f'{tool} not found; install mtools (brew install mtools)')
    out.unlink(missing_ok=True)
    subprocess.run(['cp', '-c', str(base), str(out)], check=True)
    volume = f'{out}@@{fat_offset(out)}'
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')   # iPod media byte 0xf2

    def mtools(*command, check=True):
        return subprocess.run(command, env=env, capture_output=True, text=True, check=check)

    installed = []
    if themes:
        themes = Path(themes).expanduser().resolve()
        packs = sorted(p for p in themes.rglob('*.bin') if p.is_file())
        if not packs:
            raise Failure(f'no .bin packs under {themes}')
        mtools('mmd', '-i', volume, '::/themes', check=False)
        if clean:
            listing = mtools('mdir', '-b', '-i', volume, '::/themes', check=False).stdout
            for entry in filter(None, (line.strip() for line in listing.splitlines())):
                tool = 'mdeltree' if entry.endswith('/') else 'mdel'
                mtools(tool, '-i', volume, entry.rstrip('/'))
            left = mtools('mdir', '-b', '-i', volume, '::/themes', check=False).stdout.strip()
            if left:
                raise Failure(f'could not clear /themes on {out}: {left}')
        for pack in packs:
            relative = pack.relative_to(themes)
            folder = '::/themes'
            for part in relative.parts[:-1]:
                folder += f'/{part}'
                mtools('mmd', '-i', volume, folder, check=False)
            mtools('mcopy', '-o', '-i', volume, str(pack), f'{folder}/{relative.name}')
            installed.append(f'/themes/{relative.as_posix()}')
    return {'disk': str(out), 'fat_offset': fat_offset(out), 'installed': installed}


# ---- lifecycle -------------------------------------------------------------

def launch(name, firmware, disk, layout=None, title=None, headless=False, keep_running=False,
           wait=True, boot_timeout=180, elf=None, profile=None, fns=None, trace=False,
           timeline=None, signpost=False, cwd=None):
    """Start a VM; with `wait`, return once the first screen is stable (paused)."""
    cwd = Path(cwd or Path.cwd())
    firmware = Path(firmware).expanduser().resolve()
    disk = Path(disk).expanduser().resolve()
    layout = Path(layout) if layout else find_layout(cwd)
    if layout is None and elf:
        layout = find_layout(Path(elf).resolve().parent)
    if layout is None:
        raise Failure('payload/native/layout.h not found; run from the firmware '
                      'repository or pass a layout path')
    if profile is not None and not elf:
        raise Failure('profiling payload functions needs the matching payload.elf')
    base, file_offset = parse_layout(layout)
    if not QEMU.exists():
        raise Failure(f'{QEMU} missing; build QEMU first (see README.md)')

    RUNS.mkdir(exist_ok=True)
    run = RUNS / name
    if run.exists():
        if vm_pid(run):
            raise Failure(f'{name} is already running; quit it first')
        shutil.rmtree(run)
    run.mkdir(parents=True)

    image = firmware.read_bytes()
    (run / 'body.bin').write_bytes(image[IMG1_HEADER:])
    (run / 'payload.bin').write_bytes(image[file_offset:])
    # APFS clone: instant, and -snapshot keeps guest writes off it anyway.
    subprocess.run(['cp', '-c', str(disk), str(run / 'disk.img')], check=True)

    environment = dict(ENVIRONMENT, HLE_AUDIO_DISK=str(run / 'disk.img'),
                       AUDIOWAV=str(run / 'audio.wav'))
    title = title or owner_label(name, cwd)
    command = [str(QEMU), '-name', title, '-M', 'iPod-Classic',
               '-kernel', str(run / 'body.bin'),
               '-device', f'loader,file={run / "payload.bin"},addr={base:#010x},force-raw=on',
               '-drive', f'if=ide,format=raw,file={run / "disk.img"}', '-snapshot',
               '-serial', 'null', '-monitor', 'none',
               '-qmp', f'unix:{run / "qmp.sock"},server=on,wait=off',
               '-gdb', f'unix:{run / "gdb.sock"},server=on,wait=off',
               '-no-reboot', '-no-shutdown',
               '-display', 'none' if headless else 'sdl']

    profiling = None
    if profile is not None or fns:
        entries = profile_symbols(elf, profile, fns, image, base, file_offset)
        ensure_plugin()
        symbols = run / 'profile.syms'
        symbols.write_text(''.join(f'{addr:#010x} {label}\n' for addr, label in entries))
        os.mkfifo(run / 'profile.ctl')
        plugin = [str(PLUGIN), f'syms={symbols}', f'ctl={run / "profile.ctl"}',
                  f'out={run / "profile-exit.json"}']
        if trace:
            plugin.append('trace=on')
        if timeline:
            plugin.append(f'timeline={timeline}')
        if signpost:
            plugin.append('signpost=on')
        command += ['-plugin', ','.join(plugin)]
        profiling = {'functions': len(entries),
                     'elf': str(Path(elf).resolve()) if elf else None}

    record = {'title': title, 'command': command, 'environment': environment,
              'firmware': {'path': str(firmware), 'sha256': hashlib.sha256(image).hexdigest()},
              'disk': str(disk), 'layout': str(layout.resolve()), 'profile': profiling}
    (run / 'launch.json').write_text(json.dumps(record, indent=2) + '\n')

    with (run / 'qemu.log').open('w') as output:
        process = subprocess.Popen(command, env=dict(os.environ, **environment), stdout=output,
                                   stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                   start_new_session=True, cwd=run)
    (run / 'pid').write_text(str(process.pid))
    log(f'{name}: QEMU pid {process.pid}, run dir {run}')

    deadline = time.time() + 15
    while not (run / 'qmp.sock').exists():
        if process.poll() is not None:
            raise Failure(f'QEMU exited ({process.returncode}); see {run / "qemu.log"}:\n'
                          + (run / 'qemu.log').read_text()[-2000:])
        if time.time() > deadline:
            raise Failure('QMP socket did not appear')
        time.sleep(0.2)
    time.sleep(0.3)
    result = {'name': name, 'pid': process.pid, 'run': str(run), 'title': title,
              'profile': profiling}
    if not wait:
        result['state'] = 'booting'
        return result

    log(f'{name}: waiting for the first stable screen (boot takes ~45-75 s)...')
    with QMP(run) as qmp:
        started, previous = time.time(), None
        while True:
            time.sleep(5)
            frame = qmp.frame()
            current = None if is_blank(frame) else digest(frame)
            if current and current == previous:
                break
            previous = current
            if time.time() - started > boot_timeout:
                raise Failure(f'no stable screen after {boot_timeout} s; '
                              f'take a screenshot and check {run / "qemu.log"}')
        if not keep_running:
            qmp.stop()
        result.update(state='running' if keep_running else 'paused',
                      boot_seconds=round(time.time() - started), shot=str(qmp.shot('boot')))
    return result


def status(name):
    run = run_dir(name)
    record = json.loads((run / 'launch.json').read_text())
    with QMP(run) as qmp:
        state = qmp.execute('query-status')['status']
        regs = qmp.registers()
        warning = sleep_warning(regs)
    return {'name': name, 'state': state, 'pid': vm_pid(run), 'pc': f'{regs["pc"]:#010x}',
            'lr': f'{regs["lr"]:#010x}', 'title': record.get('title', ''),
            'firmware': record['firmware'], 'profile': record.get('profile'),
            'warning': warning, 'run': str(run)}


def list_vms():
    if not RUNS.exists():
        return []
    vms = []
    for run in sorted(RUNS.iterdir()):
        if (run / 'launch.json').exists():
            record = json.loads((run / 'launch.json').read_text())
            pid = vm_pid(run)
            vms.append({'name': run.name, 'running': pid is not None, 'pid': pid,
                        'title': record.get('title', ''), 'run': str(run)})
    return vms


def quit_vm(name):
    run = run_dir(name)
    pid = vm_pid(run)
    if pid is None:
        return {'name': name, 'state': 'not running'}
    with QMP(run) as qmp:
        try:
            qmp.execute('quit')
        except Failure:
            pass
    for _ in range(50):
        if not pid_alive(pid):
            break
        time.sleep(0.1)
    return {'name': name, 'state': 'stopped'}


def resume(name, seconds=None):
    with QMP(run_dir(name)) as qmp:
        if seconds:
            qmp.run_for(seconds)
            return {'name': name, 'state': 'paused', 'ran_seconds': seconds}
        qmp.cont()
    return {'name': name, 'state': 'running'}


def pause(name):
    with QMP(run_dir(name)) as qmp:
        qmp.stop()
    return {'name': name, 'state': 'paused'}


def hmp(name, line):
    with QMP(run_dir(name)) as qmp:
        return qmp.hmp(line)


def screenshot(name, output=None):
    run = run_dir(name)
    with QMP(run) as qmp:
        path = Path(output).resolve() if output else next_shot(run, 'shot')
        return {'shot': str(qmp.png(path))}


# ---- input -----------------------------------------------------------------

def sleep_warning(regs):
    pc, lr = regs.get('pc', 0), regs.get('lr', 1)
    if 0x22000000 <= pc < 0x22010000 and lr == 0 and pc != IDLE_LOOP:
        return (f'guest looks asleep (PC={pc:#010x}, LR=0): idle sleep ignores input; '
                f'relaunch the VM')
    return None


def press(qmp, keys, keep_running=False):
    """Resume, send each key with its settle wait, then pause."""
    keys = [KEY_NAMES.get(k, k) for k in keys]
    unknown = [k for k in keys if k not in VALID_KEYS]
    if unknown:
        raise Failure(f'unknown keys {unknown}; use {sorted(VALID_KEYS)}')
    qmp.cont()
    try:
        for key in keys:
            qmp.hmp(f'sendkey {key} 200')
            time.sleep(WHEEL_WAIT if key in WHEEL_KEYS else BUTTON_WAIT)
    except BaseException:
        qmp.stop()      # never leave the guest running into idle sleep
        raise
    if not keep_running:
        qmp.stop()


def key(name, keys, keep_running=False):
    run = run_dir(name)
    with QMP(run) as qmp:
        before = digest(qmp.frame())
        press(qmp, keys, keep_running)
        changed = digest(qmp.frame()) != before
        result = {'shot': str(qmp.shot('-'.join(keys))), 'changed': changed}
        if not changed:
            result['warning'] = sleep_warning(qmp.registers())
    return result


def press_verified(qmp, which, region=None, attempts=3, settle=25.0, touch=('q', 'e')):
    """Press Select/Menu until the frame (or region) changes, retrying safely.

    An unchanged frame gets `settle` more seconds of guest time first (slow
    screens). A retry follows a wheel touch only if the touch left the frame
    unchanged; otherwise the first press may have landed late, or the touch
    moved the selection, and retrying could double-apply.
    """
    key_name = KEY_NAMES.get(which, which)
    before = digest(qmp.frame(), region)
    for attempt in range(1, attempts + 1):
        press(qmp, [key_name])
        if digest(qmp.frame(), region) != before:
            return attempt
        qmp.run_for(settle)
        if digest(qmp.frame(), region) != before:
            return attempt
        warning = sleep_warning(qmp.registers())
        if warning:
            raise Failure(warning)
        if attempt == attempts:
            raise Failure(f'{which}: screen unchanged after {attempt} attempts; '
                          f'{qmp.shot(which + "-dropped")}')
        press(qmp, list(touch))
        if digest(qmp.frame(), region) != before:
            raise Failure(f'{which}: the wheel touch changed the screen, so the first press '
                          f'may have landed late or the list was at an edge; not retrying. '
                          f'Check {qmp.shot(which + "-touch-changed")}')
        log(f'{which}: attempt {attempt} looked dropped; retrying')
    return attempts


def button(name, which, region=None, attempts=3, settle=25.0):
    run = run_dir(name)
    with QMP(run) as qmp:
        attempt = press_verified(qmp, which, region, attempts, settle)
        return {'shot': str(qmp.shot(which)), 'attempts': attempt}


def wait(name, until='stable', timeout=60.0, interval=2.0, region=None, keep_running=False):
    """Run the guest until the frame differs from the start, or two polls match."""
    run = run_dir(name)
    with QMP(run) as qmp:
        start = previous = digest(qmp.frame(), region)
        started = time.time()
        qmp.cont()
        try:
            while time.time() - started < timeout:
                time.sleep(interval)
                current = digest(qmp.frame(), region)
                if until == 'change' and current != start:
                    break
                if until == 'stable' and current == previous:
                    break
                previous = current
            else:
                raise Failure(f'screen did not {until} within {timeout} s')
        finally:
            if not keep_running:
                qmp.stop()
        return {'shot': str(qmp.shot(f'wait-{until}')),
                'seconds': round(time.time() - started, 1)}


# ---- profiling --------------------------------------------------------------

def profile_send(run, *lines):
    ctl = run / 'profile.ctl'
    if not ctl.exists():
        raise Failure(f'{run.name} was launched without profiling')
    try:
        fd = os.open(ctl, os.O_WRONLY | os.O_NONBLOCK)
    except OSError as error:
        raise Failure(f'profiler control not listening ({error})')
    with os.fdopen(fd, 'w') as stream:
        stream.write(''.join(line + '\n' for line in lines))


def profile_dump_with(run, qmp, output):
    output = Path(output).resolve()
    output.unlink(missing_ok=True)
    was_running = qmp.running()
    profile_send(run, f'dump {output}')
    if not was_running:
        qmp.cont()
    try:
        deadline = time.time() + 30
        while not output.exists():
            if time.time() > deadline:
                raise Failure('profiler did not write the dump within 30 s')
            time.sleep(0.05)
    finally:
        if not was_running:
            qmp.stop()
    data = json.loads(output.read_text())
    data['summary'] = str(output)
    trace = Path(f'{output}.trace.json')
    if trace.exists():
        data['trace'] = str(trace)
    return data


def default_profile_path(run):
    return run / f'profile-{time.strftime("%H%M%S")}.json'


def profile_control(name, action):
    if action not in ('reset', 'start', 'stop'):
        raise Failure(f'unknown profiler action {action!r}')
    profile_send(run_dir(name), action)
    return {'name': name, 'queued': action}


def profile_dump(name, output=None):
    run = run_dir(name)
    with QMP(run) as qmp:
        return profile_dump_with(run, qmp, output or default_profile_path(run))


def profile_keys(name, keys, settle=0.0, output=None):
    """Reset the profiler, send keys, let the guest settle, dump."""
    run = run_dir(name)
    with QMP(run) as qmp:
        profile_send(run, 'reset', 'start')
        try:
            press(qmp, keys, keep_running=True)
            if settle:
                time.sleep(settle)
            data = profile_dump_with(run, qmp, output or default_profile_path(run))
        finally:
            qmp.stop()
        data['shot'] = str(qmp.shot('profile-' + '-'.join(keys)))
    return data


def instruments(name, template='Time Profiler', seconds=None, keys=(), open_trace=True):
    run = run_dir(name)
    pid = vm_pid(run)
    if pid is None:
        raise Failure(f'{name} is not running')
    trace = run / f'instruments-{time.strftime("%H%M%S")}.trace'
    command = ['xcrun', 'xctrace', 'record', '--template', template,
               '--attach', str(pid), '--output', str(trace)]
    if seconds:
        command += ['--time-limit', f'{seconds}s']
    recorder = subprocess.Popen(command, stdout=sys.stderr)
    try:
        with QMP(run) as qmp:
            if keys:
                time.sleep(3)            # let xctrace attach
                press(qmp, keys, keep_running=True)
            else:
                qmp.cont()
            try:
                recorder.wait()
            except KeyboardInterrupt:
                recorder.send_signal(2)
                recorder.wait()
            finally:
                qmp.stop()
    finally:
        if recorder.poll() is None:
            recorder.terminate()
    if not trace.exists():
        raise Failure('xctrace did not write a trace')
    if open_trace:
        subprocess.run(['open', str(trace)])
    return {'trace': str(trace)}
