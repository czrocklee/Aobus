#!/usr/bin/env python3
"""
Generate test audio files with metadata for rsc testing.
"""

import os
import sys

# Check mutagen availability
try:
    from mutagen.flac import FLAC
    from mutagen.id3 import ID3, TALB, TDRC, TIT1, TIT2, TPE1, TRCK
    from mutagen.mp4 import MP4

    HAS_MUTAGEN = True
except ImportError:
    HAS_MUTAGEN = False
    print("mutagen not available; files will be created without metadata.")


def create_flac(path: str, title: str, artist: str, album: str, year: str, track: str, work: str | None = None) -> None:
    """Create a minimal FLAC file with metadata."""
    # FLAC requires actual audio data, but we can create minimal valid file
    # Use mutagen to add tags to empty/minimal file
    with open(path, "wb") as f:
        # Minimal FLAC header (fake but valid enough for parser)
        f.write(b"fLaC\x00\x00\x00\x00")

    audio = FLAC(path)
    audio["title"] = title
    audio["artist"] = artist
    audio["album"] = album
    audio["date"] = year
    audio["tracknumber"] = track
    if work:
        audio["grouping"] = work
    audio.save()


def create_m4a(path: str, title: str, artist: str, album: str, year: str, track: str, work: str | None = None) -> None:
    """Create a minimal M4A file with metadata."""
    # Create minimal M4A structure
    with open(path, "wb") as f:
        # ftyp + free atom (minimal valid M4A)
        f.write(b"\x00\x00\x00\x18ftypm4a \x00\x00\x00\x00isomiso2mp41")
        f.write(b"\x00\x00\x00\x00free")

    audio = MP4(path)
    audio["\xa9nam"] = title  # title
    audio["\xa9ART"] = artist  # artist
    audio["\xa9alb"] = album  # album
    audio["\xa9day"] = year  # date
    audio["trkn"] = [(int(track), 0)]
    if work:
        audio["\xa9grp"] = work
    audio.save()


def create_mp3(path: str, title: str, artist: str, album: str, year: str, track: str, work: str | None = None) -> None:
    """Create a minimal MP3 file with an ID3v2 tag."""
    # Start from an empty file; ID3.save prepends a well-formed tag header.
    with open(path, "wb"):
        pass

    tags = ID3()
    tags.add(TIT2(encoding=3, text=[title]))
    tags.add(TPE1(encoding=3, text=[artist]))
    tags.add(TALB(encoding=3, text=[album]))
    tags.add(TDRC(encoding=3, text=[year]))
    tags.add(TRCK(encoding=3, text=[track]))
    if work:
        tags.add(TIT1(encoding=3, text=[work]))
    tags.save(path)


def create_untagged(path: str) -> None:
    """Fallback when mutagen is unavailable or tagging fails: an empty placeholder file."""
    with open(path, "wb"):
        pass


def main() -> None:
    output_dir = sys.argv[1] if len(sys.argv) > 1 else "./test_music_lib"

    os.makedirs(output_dir, exist_ok=True)

    creators = {".flac": create_flac, ".m4a": create_m4a, ".mp3": create_mp3}

    # Test tracks
    tracks = [
        ("song_1.flac", "Test Song 1", "Test Artist", "Test Album 1", "2021", "1", "Symphony No. 5"),
        ("song_2.flac", "Test Song 2", "Test Artist", "Test Album 2", "2022", "2", "The Four Seasons"),
        ("song_3.m4a", "M4A Song 3", "M4A Artist", "M4A Album", "2020", "3", "Moonlight Sonata"),
        ("song_4.m4a", "M4A Song 4", "M4A Artist", "M4A Album", "2020", "4", None),
        ("song_5.mp3", "MP3 Song 5", "MP3 Artist", "MP3 Album", "2019", "5", "Toccata and Fugue"),
    ]

    for filename, title, artist, album, year, track, work in tracks:
        path = os.path.join(output_dir, filename)
        ext = os.path.splitext(filename)[1].lower()

        if HAS_MUTAGEN:
            try:
                creators[ext](path, title, artist, album, year, track, work)
                print(f"Created: {filename}")
                continue
            except Exception as exc:
                print(f"WARNING: tagging {filename} failed ({exc}); writing untagged placeholder.", file=sys.stderr)

        create_untagged(path)
        print(f"Created (untagged): {filename}")

    print(f"\nTest library created at: {output_dir}")
    print(f"Total files: {len(tracks)}")


if __name__ == "__main__":
    main()
