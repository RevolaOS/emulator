#!/usr/bin/env python3
"""Generate a synthetic iPod Classic (firmware 2.0.5) music library with artwork.

The iTunesDB and ArtworkDB are written by iopenpod's `iPodDB.library` API, the
public codec behind iPod Sync. All metadata and cover pixels are synthetic and
derived from `--seed`, so the same arguments produce the same files.

Every track points at the one MP3 given with `--audio`, so any track plays.
Albums hold 6-12 tracks by default. A cover belongs to an album: tracks of one
album share the same pixels, and covers of different albums differ in colour,
pattern and the printed album number.

Artwork layouts (`--art-mode`):

- `track` (default) matches iTunes: one ArtworkDB image (mhii) per track, each
  with its own 5 thumbnail records, and the tracks of an album pointing at the
  same ithmb byte ranges.
- `album` writes one mhii per album, shared by all of its tracks.

Output: `OUT/iPod_Control/{iTunes,Artwork,Music}` and `OUT/manifest.json` with
the parameters and the expected record counts. `--image` also builds a FAT32
disk image from that tree (see mkimage.py).
"""
import argparse
import colorsys
import hashlib
import json
import random
import shutil
import struct
import sys
import time
from pathlib import Path

import iPodDB.library.creation as creation
from iPodDB.library import (
    ArtworkAsset, ArtworkPixels, CoverFormat, CoverPixelFormat, FileDependency,
    IPodLibrary, LibrarySnapshot, PreparedMedia, Track, TrackMetadata, WriteChecksum,
    WriteResources, WriteTarget, content_chunks, create_empty_library,
)
from PIL import Image, ImageDraw, ImageFont

# Cover formats iTunes writes for the 6th/7th generation Classic, in its order.
# 1069 is absent from iopenpod's device registry but present in every
# iTunes-written Classic ArtworkDB, so the list is given here explicitly.
COVER_FORMATS = (
    CoverFormat(1055, 128, 128, 256, CoverPixelFormat.RGB565_LE),
    CoverFormat(1060, 320, 320, 640, CoverPixelFormat.RGB565_LE),
    CoverFormat(1061, 55, 55, 112, CoverPixelFormat.RGB565_LE),
    CoverFormat(1068, 128, 128, 256, CoverPixelFormat.RGB565_LE),
    CoverFormat(1069, 142, 142, 284, CoverPixelFormat.RGB565_LE),
)
COVER_EDGE = 320            # largest format; covers are drawn at this size
DB_VERSION = 0x75           # iTunesDB version iTunes writes for firmware 2.0.5
ARTWORK_ROOT_VALUE = 6      # MHFD root value of iTunes-written Classic ArtworkDBs
AUDIO_PATH = 'iPod_Control/Music/F00/SYNTH000.mp3'
DATE_ADDED = 1_700_000_000  # fixed, so output does not depend on the clock
# Firmware 2.0.5 only loads a database that declares hashing scheme 1. QEMU's
# SHA1_ZERO mode accepts it when the hash58 signature is blanked, so the
# database is signed for this placeholder FireWire GUID and the hash is zeroed.
PLACEHOLDER_GUID = bytes.fromhex('000A270000000001')
HASH58 = slice(0x58, 0x6C)

WORDS = ('amber blue cold dawn echo fire gold haze iron jade kite lune mist neon opal '
         'pine quiet rain salt tide umber vale wave xeno yarn zest').split()

MP3_BITRATES = {  # kbps by (MPEG-1?, layer III) and index
    True: (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320),
    False: (0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160),
}
MP3_RATES = {3: (44100, 48000, 32000), 2: (22050, 24000, 16000), 0: (11025, 12000, 8000)}


def mp3_facts(path):
    """Return (size, length_ms, bitrate_kbps, sample_rate) of an MPEG Layer III file.

    Uses the Xing/Info frame count when present, otherwise assumes constant
    bitrate from the first frame header.
    """
    size = path.stat().st_size
    with path.open('rb') as f:
        data = f.read(1 << 20)
    offset = 0
    if data[:3] == b'ID3':
        offset = 10 + ((data[6] & 127) << 21 | (data[7] & 127) << 14
                       | (data[8] & 127) << 7 | (data[9] & 127))
    while offset + 4 <= len(data) and not (data[offset] == 0xFF and data[offset + 1] & 0xE0 == 0xE0):
        offset += 1
    if offset + 4 > len(data):
        raise SystemExit(f'{path}: no MPEG frame header found')
    header = struct.unpack('>I', data[offset:offset + 4])[0]
    version, layer = (header >> 19) & 3, (header >> 17) & 3
    if layer != 1 or version == 1:
        raise SystemExit(f'{path}: not an MPEG Layer III file')
    mpeg1 = version == 3
    bitrate = MP3_BITRATES[mpeg1][(header >> 12) & 15]
    rate = MP3_RATES[version][(header >> 10) & 3]
    mono = (header >> 6) & 3 == 3
    side = (17 if mono else 32) if mpeg1 else (9 if mono else 17)
    tag = offset + 4 + side
    samples_per_frame = 1152 if mpeg1 else 576
    if data[tag:tag + 4] in (b'Xing', b'Info') and struct.unpack('>I', data[tag + 4:tag + 8])[0] & 1:
        frames = struct.unpack('>I', data[tag + 8:tag + 12])[0]
        length_ms = frames * samples_per_frame * 1000 // rate
    else:
        length_ms = (size - offset) * 8 // bitrate
    return size, length_ms, bitrate, rate


def words(rnd, k, length):
    text = ' '.join(rnd.choice(WORDS).capitalize() for _ in range(4))
    return f'{text[:length]} {k}'


def build_catalog(a, rnd):
    """Albums, artists and tracks; each album has one artist and 6-12 tracks."""
    lo, hi = a.album_tracks
    sizes = []
    while sum(sizes) < a.n:
        sizes.append(rnd.randint(lo, hi))
    # Take the overshoot from albums above MIN. If every album is at MIN, drop
    # the last one and hand its tracks to albums below MAX.
    while (excess := sum(sizes) - a.n) != 0:
        if excess > 0:
            shrinkable = [i for i, s in enumerate(sizes) if s > lo]
            if shrinkable:
                sizes[rnd.choice(shrinkable)] -= 1
            else:
                sizes.pop()
        else:
            growable = [i for i, s in enumerate(sizes) if s < hi]
            if not growable:
                raise SystemExit(f'{a.n} tracks do not fit albums of {lo}-{hi} tracks')
            sizes[rnd.choice(growable)] += 1
    artists = a.artists or max(1, len(sizes) // 2)
    genres = [f'Genre {i:02d}' for i in range(a.genres)]
    albums = []
    for k, size in enumerate(sizes):
        artist = rnd.randrange(artists)
        albums.append(dict(
            index=k, size=size, artist=f'Artist {artist:05d}',
            name=f'Album {k:05d} {words(rnd, k, 12).rsplit(" ", 1)[0]}',
            genre=rnd.choice(genres), year=1960 + rnd.randrange(65),
            art=rnd.random() < a.art_fraction))
    return albums, artists


def cover(album, seed):
    """320x320 RGB888 cover, identical for every track of `album`."""
    digest = hashlib.sha256(f'{seed}:{album["index"]}'.encode()).digest()
    hue = digest[0] / 255
    bg = tuple(round(c * 255) for c in colorsys.hsv_to_rgb(hue, 0.55 + digest[1] / 600, 0.75))
    fg = tuple(round(c * 255) for c in colorsys.hsv_to_rgb((hue + 0.5) % 1, 0.35, 0.98))
    image = Image.new('RGB', (COVER_EDGE, COVER_EDGE), bg)
    draw = ImageDraw.Draw(image)
    # A stripe band whose angle and width also vary per album, so neighbouring
    # albums with similar hues remain distinguishable at 55x55.
    width = 18 + digest[2] % 40
    step = width * 2 + digest[3] % 30
    slope = (digest[4] % 5) - 2
    dark = tuple(max(0, c - 40) for c in bg)
    for x in range(-COVER_EDGE * 3, COVER_EDGE * 3, step):
        draw.polygon([(x, 0), (x + width, 0), (x + width + slope * COVER_EDGE // 2, COVER_EDGE),
                      (x + slope * COVER_EDGE // 2, COVER_EDGE)], fill=dark)
    draw.rectangle((24, 96, COVER_EDGE - 24, 224), fill=bg)
    font = ImageFont.load_default(size=96)
    text = f'{album["index"]:04d}'
    box = draw.textbbox((0, 0), text, font=font)
    draw.text(((COVER_EDGE - box[2] - box[0]) / 2, (COVER_EDGE - box[3] - box[1]) / 2),
              text, font=font, fill=fg)
    return ArtworkPixels(COVER_EDGE, COVER_EDGE, image.tobytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('-n', type=int, required=True, help='number of tracks')
    ap.add_argument('--out', type=Path, required=True, help='output directory (replaced)')
    ap.add_argument('--audio', type=Path, required=True, help='MP3 file every track plays')
    ap.add_argument('--album-tracks', type=lambda s: tuple(map(int, s.split('-'))), default=(6, 12),
                    metavar='MIN-MAX', help='tracks per album (default 6-12)')
    ap.add_argument('--artists', type=int, default=0, help='number of artists (default albums/2)')
    ap.add_argument('--genres', type=int, default=20)
    ap.add_argument('--art-fraction', type=float, default=1.0,
                    help='probability that an album has a cover (default 1.0)')
    ap.add_argument('--art-mode', choices=('track', 'album'), default='track',
                    help='one ArtworkDB image per track (iTunes) or per album')
    ap.add_argument('--no-auxiliary', action='store_true',
                    help='omit the MHOD 6 auxiliary record newer iTunes adds to each image')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--image', type=Path, help='also build a FAT32 disk image here')
    ap.add_argument('--base', type=Path, help='disk image whose boot area --image reuses')
    a = ap.parse_args()
    if a.image and not a.base:
        ap.error('--image needs --base')
    lo, hi = a.album_tracks
    if not 0 < lo <= hi:
        ap.error('--album-tracks must be MIN-MAX with 0 < MIN <= MAX')

    rnd = random.Random(a.seed)
    # iopenpod draws the database, secondary and master playlist identities
    # from `secrets`; seed them so the output is reproducible.
    creation.randbits = random.Random(f'ids:{a.seed}').getrandbits
    started = time.monotonic()

    size, length_ms, bitrate, rate = mp3_facts(a.audio)
    with a.audio.open('rb') as f:
        audio_sha = hashlib.file_digest(f, 'sha256').hexdigest()
    audio = FileDependency(AUDIO_PATH, size, audio_sha)

    albums, artist_count = build_catalog(a, rnd)
    tracks, media, covers = [], [], {}
    for album in albums:
        artwork_id = 0
        if album['art']:
            artwork_id = 1 + album['index']
            covers[artwork_id] = album
        for number in range(1, album['size'] + 1):
            track_id = len(tracks) + 1
            tracks.append(Track(
                track_id=track_id, title=words(rnd, track_id, 18), artist=album['artist'],
                album=album['name'], album_artist=album['artist'], genre=album['genre'],
                year=album['year'], track_number=number, length_ms=length_ms, size_bytes=size,
                bitrate_kbps=bitrate, artwork_id=artwork_id,
                metadata=TrackMetadata(location=AUDIO_PATH, file_format='MPEG audio file',
                                       sample_rate_hz=rate, total_tracks=album['size'],
                                       date_added=DATE_ADDED, last_modified=DATE_ADDED)))
            # MP3: filetype 'MP3 ', mp3 flag 1, no audio-format flag, as iTunes records it.
            media.append(PreparedMedia(track_id, audio, 0x4D503320, 1, 0xFFFF, 0, 0))

    target = WriteTarget(
        WriteChecksum.HASH58, firewire_guid=PLACEHOLDER_GUID, database_version=DB_VERSION, cover_formats=COVER_FORMATS,
        max_database_bytes=1 << 30, artwork_root_value=ARTWORK_ROOT_VALUE,
        supports_sparse_artwork=a.art_mode == 'album',
        include_auxiliary_image_records=not a.no_auxiliary)
    empty = create_empty_library('Synthetic', target, supports_podcasts=True)
    if empty.prepared is None:
        raise SystemExit(f'empty library: {empty.issues}')
    library = IPodLibrary(empty.prepared.itunes)
    snapshot = LibrarySnapshot(tuple(tracks), library.snapshot.playlists, library.snapshot.device_name)
    plan = library.analyze(library.begin_draft(snapshot), target)
    if plan.blocked:
        raise SystemExit(f'plan: {plan.issues[:5]}')
    print(f'{len(tracks)} tracks, {len(albums)} albums, {artist_count} artists, '
          f'{len(covers)} covers; drawing covers', file=sys.stderr)
    artwork = tuple(ArtworkAsset(i, cover(covers[i], a.seed)) for i in plan.required_artwork)
    print('preparing databases', file=sys.stderr)
    result = library.prepare(plan, WriteResources(
        media=tuple(media), artwork=artwork, file_inventory=(audio,),
        pending_playback_sidecars=False))
    if result.prepared is None:
        raise SystemExit(f'prepare: {result.issues[:5]}')
    prepared = result.prepared

    if a.out.exists():
        shutil.rmtree(a.out)
    itunes = bytearray(prepared.itunes)
    itunes[HASH58] = bytes(HASH58.stop - HASH58.start)
    files = [('iPod_Control/iTunes/iTunesDB', bytes(itunes))]
    if prepared.artwork is not None:
        files.append(('iPod_Control/Artwork/ArtworkDB', prepared.artwork))
    files += [(f.relative_path, f.data) for f in prepared.artwork_files]
    for relative, data in files:
        path = a.out / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open('wb') as f:
            for chunk in content_chunks(data):
                f.write(chunk)
    music = a.out / AUDIO_PATH
    music.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(a.audio, music)
    (a.out / 'iPod_Control/Device').mkdir(exist_ok=True)

    art_tracks = sum(1 for t in tracks if t.artwork_id)
    manifest = dict(
        tracks=len(tracks), albums=len(albums), artists=artist_count, covers=len(covers),
        tracks_with_art=art_tracks,
        expected_images=art_tracks if a.art_mode == 'track' else len(covers),
        expected_thumbnails_per_image=len(COVER_FORMATS),
        art_mode=a.art_mode, auxiliary_records=not a.no_auxiliary, seed=a.seed,
        album_tracks=list(a.album_tracks), art_fraction=a.art_fraction,
        audio=dict(path=AUDIO_PATH, size=size, length_ms=length_ms, bitrate_kbps=bitrate,
                   sample_rate=rate, sha256=audio_sha),
        files={r: (a.out / r).stat().st_size for r, _ in files},
    )
    (a.out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'{a.out}: {len(tracks)} tracks, {manifest["expected_images"]} images, '
          f'{sum(manifest["files"].values()):,} bytes of databases and thumbnails '
          f'in {time.monotonic() - started:.0f} s', file=sys.stderr)

    if a.image:
        from mkimage import build_image
        build_image(a.base, a.out, a.image)


if __name__ == '__main__':
    main()
