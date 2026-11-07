#!/usr/bin/env python3
"""Build an emulator disk image from a generated library tree.

usage: mkimage.py --base BASE.img --tree OUT --image NEW.img

The image keeps BASE's partition table and firmware partition (everything
before the FAT32 volume) and replaces the FAT32 volume with a new one, sized
for the tree, that holds the tree's `iPod_Control`. iPod MBRs count in 4 KiB
sectors; the FAT32 volume itself uses 512-byte sectors and 4 KiB clusters, as
newfs_msdos formats it, with the boot-sector marker the firmware requires
(see mark_fat_volume). BASE is never modified. The output is sparse.
"""
import argparse
import os
import struct
import subprocess
import tempfile
from pathlib import Path

MBR_SECTOR = 4096
FAT_TYPES = (0x0B, 0x0C)
MiB = 1 << 20


def fat_partition(mbr):
    """Return (entry index, first 4 KiB sector) of the FAT32 partition."""
    if mbr[510:512] != b'\x55\xaa':
        raise SystemExit('base image has no MBR signature')
    for index in range(4):
        entry = mbr[446 + 16 * index:462 + 16 * index]
        if entry[4] in FAT_TYPES:
            return index, struct.unpack_from('<I', entry, 8)[0]
    raise SystemExit('base image has no FAT32 partition')


def tree_bytes(root):
    return sum(p.stat().st_size for p in root.rglob('*') if p.is_file())


def copy_sparse(source, destination, offset):
    """Append `source` to `destination` at `offset`, leaving zero blocks as holes."""
    block = 4 * MiB
    with open(source, 'rb') as src, open(destination, 'r+b') as dst:
        position = offset
        while chunk := src.read(block):
            if chunk.count(0) != len(chunk):
                dst.seek(position)
                dst.write(chunk)
            position += len(chunk)
        dst.truncate(position)


def mark_fat_volume(fat):
    """Write "FAT" at +0x34 (BPB reserved bytes) of the boot sector and its backup.

    Firmware 2.0.5 probes the data partition's boot sector as a possible nested
    MBR (0x08082ce0). Finding no partition entries, it accepts the volume only if
    boot+0x34 or boot+0x2b starts with "FAT"; otherwise the library is never read
    and Music shows "No Music". newfs_msdos leaves both zero, and FAT drivers
    ignore the reserved field. See wiki/13-classic6g-music.md and
    tools/ipod_classic/mkdisk6g4k.py.
    """
    with open(fat, 'r+b') as f:
        boot = f.read(512)
        bytes_per_sector, = struct.unpack_from('<H', boot, 0x0B)
        backup, = struct.unpack_from('<H', boot, 0x32)
        for sector in {0, backup}:
            f.seek(sector * bytes_per_sector + 0x34)
            f.write(b'FAT')


def build_image(base, tree, image, cluster_sectors=8):
    base, tree, image = Path(base), Path(tree), Path(image)
    if base.resolve() == image.resolve():
        raise SystemExit('the output must differ from the base image')
    with base.open('rb') as f:
        mbr = bytearray(f.read(512))
    index, first = fat_partition(mbr)
    fat_offset = first * MBR_SECTOR

    # Files plus FAT tables, directories and cluster slack, rounded up.
    payload = tree_bytes(tree / 'iPod_Control')
    volume = (payload * 103 // 100 + 256 * MiB + 64 * MiB - 1) // (64 * MiB) * (64 * MiB)

    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')   # iPod media descriptor
    with tempfile.TemporaryDirectory(dir=image.parent) as scratch:
        fat = Path(scratch) / 'fat.img'
        with fat.open('wb') as f:
            f.truncate(volume)
        subprocess.run(['newfs_msdos', '-F', '32', '-S', '512', '-c', str(cluster_sectors), '-v', 'IPOD',
                        '-s', str(volume // 512), str(fat)], check=True, capture_output=True)
        mark_fat_volume(fat)
        subprocess.run(['mcopy', '-s', '-Q', '-i', str(fat), str(tree / 'iPod_Control'), '::/'],
                       check=True, env=env)
        image.unlink(missing_ok=True)
        with image.open('wb') as out, base.open('rb') as src:
            remaining = fat_offset
            while remaining:
                chunk = src.read(min(remaining, 4 * MiB))
                out.write(chunk)
                remaining -= len(chunk)
        struct.pack_into('<I', mbr, 446 + 16 * index + 12, volume // MBR_SECTOR)
        with image.open('r+b') as out:
            out.write(mbr)
        copy_sparse(fat, image, fat_offset)
    print(f'{image}: FAT32 at {fat_offset}, {volume // MiB} MiB volume, '
          f'{payload / MiB:.0f} MiB of files')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--base', required=True, type=Path, help='image whose boot area is reused')
    ap.add_argument('--tree', required=True, type=Path, help='synthlib.py output directory')
    ap.add_argument('--image', required=True, type=Path, help='new disk image')
    ap.add_argument('--cluster-sectors', type=int, default=8, help='512-byte sectors per cluster')
    a = ap.parse_args()
    build_image(a.base, a.tree, a.image, a.cluster_sectors)


if __name__ == '__main__':
    main()
