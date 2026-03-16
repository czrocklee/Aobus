#!/usr/bin/env python3
"""
Generate test audio files with metadata for rsc testing.
"""

import os
import sys
import tempfile
import shutil

# Check mutagen availability
try:
    from mutagen.mp3 import MP3
    from mutagen.m4a import M4A
    from mutagen.flac import FLAC
    HAS_MUTAGEN = True
except ImportError:
    HAS_MUTAGEN = False
    print("mutagen not available, trying alternative...")


def create_flac(path: str, title: str, artist: str, album: str, year: str, track: str):
    """Create a minimal FLAC file with metadata."""
    # FLAC requires actual audio data, but we can create minimal valid file
    # Use mutagen to add tags to empty/minimal file
    if HAS_MUTAGEN:
        # Create minimal FLAC structure
        with open(path, 'wb') as f:
            # Minimal FLAC header (fake but valid enough for parser)
            f.write(b'fLaC\x00\x00\x00\x00')

        audio = FLAC(path)
        audio['title'] = title
        audio['artist'] = artist
        audio['album'] = album
        audio['date'] = year
        audio['tracknumber'] = track
        audio.save()
    else:
        # Just create empty file
        with open(path, 'wb') as f:
            pass


def create_m4a(path: str, title: str, artist: str, album: str, year: str, track: str):
    """Create a minimal M4A file with metadata."""
    if HAS_MUTAGEN:
        # Create minimal M4A structure
        with open(path, 'wb') as f:
            # ftyp + free atom (minimal valid M4A)
            f.write(b'\x00\x00\x00\x18ftypm4a \x00\x00\x00\x00isomiso2mp41')
            f.write(b'\x00\x00\x00\x00free')

        audio = M4A(path)
        audio['\xa9nam'] = title  # title
        audio['\xa9ART'] = artist  # artist
        audio['\xa9alb'] = album  # album
        audio['\xa9day'] = year   # date
        audio['trkn'] = [(int(track), 0)]
        audio.save()
    else:
        with open(path, 'wb') as f:
            pass


def create_mp3(path: str, title: str, artist: str, album: str, year: str, track: str):
    """Create a minimal MP3 file with metadata."""
    if HAS_MUTAGEN:
        # Create minimal MP3 with ID3v2 tag
        with open(path, 'wb') as f:
            # ID3v2 header + minimal data
            f.write(b'ID3\x04\x00\x00\x00\x00\x00\x00')
            # Title frame
            title_bytes = title.encode('utf-8')
            f.write(b'TPE1')
            f.write((len(title_bytes) + 1).to_bytes(4, 'big'))
            f.write(b'\x00')
            f.write(title_bytes)

        audio = MP3(path)
        audio['TIT2'] = title
        audio['TPE1'] = artist
        audio['TALB'] = album
        audio['TYER'] = year
        audio['TRCK'] = track
        audio.save()
    else:
        with open(path, 'wb') as f:
            pass


def main():
    output_dir = sys.argv[1] if len(sys.argv) > 1 else './test_music_lib'

    os.makedirs(output_dir, exist_ok=True)

    # Test tracks
    tracks = [
        ('song_1.flac', 'Test Song 1', 'Test Artist', 'Test Album 1', '2021', '1'),
        ('song_2.flac', 'Test Song 2', 'Test Artist', 'Test Album 2', '2022', '2'),
        ('song_3.m4a', 'M4A Song 3', 'M4A Artist', 'M4A Album', '2020', '3'),
        ('song_4.m4a', 'M4A Song 4', 'M4A Artist', 'M4A Album', '2020', '4'),
        ('song_5.mp3', 'MP3 Song 5', 'MP3 Artist', 'MP3 Album', '2019', '5'),
    ]

    for filename, title, artist, album, year, track in tracks:
        path = os.path.join(output_dir, filename)
        ext = os.path.splitext(filename)[1].lower()

        if ext == '.flac':
            create_flac(path, title, artist, album, year, track)
        elif ext == '.m4a':
            create_m4a(path, title, artist, album, year, track)
        elif ext == '.mp3':
            create_mp3(path, title, artist, album, year, track)

        print(f"Created: {filename}")

    print(f"\nTest library created at: {output_dir}")
    print(f"Total files: {len(tracks)}")


if __name__ == '__main__':
    main()
