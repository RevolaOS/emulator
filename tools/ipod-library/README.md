# Synthetic music libraries

`synthlib.py` generates a synthetic iPod Classic music library with cover
artwork for firmware 2.0.5, and `mkimage.py` turns it into a FAT32 disk
image for QEMU. They exist to measure the firmware at library sizes no
real test library has, for example the heap cost of ArtworkDB changes at 20,000
or 60,000 tracks.

The iTunesDB and ArtworkDB are written by
[iopenpod](https://pypi.org/project/iopenpod/)'s `iPodDB.library` API. All
metadata and cover pixels are synthetic and derived from `--seed`, so the
same arguments produce the same files.

## Setup

iopenpod needs Python 3.12. Only its codec package is used, so it is installed
without its GUI dependencies:

```sh
cd tools/ipod-library
uv venv -p 3.12 .venv
uv pip install -p .venv/bin/python --no-deps -r requirements.txt
```

`mkimage.py` also needs `newfs_msdos` (macOS) and mtools.

## Usage

```sh
.venv/bin/python synthlib.py -n 20000 --audio track.mp3 \
    --out /tmp/lib20k --image /tmp/lib20k.img --base /path/to/test-disk.img
ipodvm launch --firmware build/x/osos-cfw.bin --disk /tmp/lib20k.img
```

| Option | Meaning |
| --- | --- |
| `-n` | number of tracks |
| `--audio` | MPEG Layer III file that every track plays |
| `--album-tracks MIN-MAX` | tracks per album (default 6-12) |
| `--artists` | number of artists (default: half the number of albums) |
| `--art-fraction` | probability that an album has a cover (default 1.0) |
| `--art-mode track\|album` | one ArtworkDB image per track (default) or per album |
| `--no-auxiliary` | omit the MHOD 6 auxiliary record in each image |
| `--seed` | seed for all metadata, covers and database identities |
| `--image`, `--base` | build a disk image that reuses the base image's partition table and firmware partition |

The output directory holds `iPod_Control/` and `manifest.json` (parameters,
expected image and thumbnail counts, file sizes). The image is sparse.

## Library layout

- Each album has one artist, 6-12 tracks and a 320×320 cover: a hashed colour,
  a stripe pattern and the album number. Tracks of one album share the cover,
  so thumbnail sharing finds real duplicates.
- The default artwork layout is the one iTunes writes for a Classic: one image
  record (`mhii`) per track, with thumbnails in formats 1055, 1060, 1061, 1068
  and 1069, and the tracks of an album pointing at the same ithmb byte ranges.
  Thumbnail files roll over at 256 MiB (`F1060_1.ithmb` holds 1,310
  thumbnails, then `F1060_2.ithmb`), as iTunes does. `--art-mode album` writes
  one image record per album instead, shared by its tracks.
- The track's artwork reference (MHIT +0x160) names its image, and the image
  names the track's database ID (MHII +0x14).
- The iTunesDB declares hashing scheme 1 with a zeroed hash58. Firmware 2.0.5
  ignores a database without scheme 1, and QEMU's `SHA1_ZERO` mode accepts
  the zeroed hash.
- The FAT32 boot sector carries the "FAT" marker at +0x34 that firmware 2.0.5
  requires before it reads the volume (see `mark_fat_volume` in `mkimage.py`).
