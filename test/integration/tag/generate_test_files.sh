#!/bin/sh
# generate_test_files.sh - Generate test audio files for tag integration tests
# Usage: ./generate_test_files.sh <output_dir>
# Example: ./generate_test_files.sh test/integration/tag/test_data

set -e

if [ -z "$1" ]; then
    echo "Usage: $0 <output_dir>"
    exit 1
fi

OUTPUT_DIR="$1"
mkdir -p "$OUTPUT_DIR"

# Generate a small test cover image (1x1 red PNG)
COVER_PNG="$OUTPUT_DIR/cover.png"
ffmpeg -f lavfi -i "color=c=red:s=1x1" -frames:v 1 -y "$COVER_PNG" 2>/dev/null

echo "Generating test audio files in $OUTPUT_DIR..."

# ============================================================================
# FLAC files (lossless, 16-bit/44.1kHz default, stereo)
# ============================================================================
ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "title=Test Title" \
    -metadata "artist=Test Artist" \
    -metadata "album=Test Album" \
    -metadata "genre=Rock" \
    -metadata "track=1" \
    -metadata "date=2024" \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -y "$OUTPUT_DIR/basic_metadata.flac" 2>/dev/null
echo "  Created basic_metadata.flac"

ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -i "$COVER_PNG" \
    -map 0:a -map 1:v \
    -c:v png -disposition:v attached_pic \
    -metadata "title=Test Title" \
    -metadata "artist=Test Artist" \
    -metadata "album=Test Album" \
    -metadata "genre=Rock" \
    -metadata "track=1" \
    -metadata "date=2024" \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -y "$OUTPUT_DIR/with_cover.flac" 2>/dev/null
echo "  Created with_cover.flac"

ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -y "$OUTPUT_DIR/empty.flac" 2>/dev/null
echo "  Created empty.flac"

# FLAC hi-res: 24-bit, 96kHz, stereo
ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "title=HiRes Title" \
    -metadata "artist=HiRes Artist" \
    -metadata "album=HiRes Album" \
    -metadata "genre=Electronic" \
    -metadata "track=2" \
    -metadata "date=2025" \
    -af "aformat=sample_fmts=s32:channel_layouts=stereo" \
    -ar 96000 \
    -y "$OUTPUT_DIR/hires.flac" 2>/dev/null
echo "  Created hires.flac (24-bit/96kHz stereo)"

# ============================================================================
# MP4/M4A files (AAC/ALAC, 44.1kHz stereo)
# ============================================================================
ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "title=Test Title" \
    -metadata "artist=Test Artist" \
    -metadata "album=Test Album" \
    -metadata "genre=Rock" \
    -metadata "track=1" \
    -metadata "date=2024" \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -y "$OUTPUT_DIR/basic_metadata.m4a" 2>/dev/null
echo "  Created basic_metadata.m4a"

ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -i "$COVER_PNG" \
    -map 0:a -map 1:v \
    -c:v png -disposition:v attached_pic \
    -metadata "title=Test Title" \
    -metadata "artist=Test Artist" \
    -metadata "album=Test Album" \
    -metadata "genre=Rock" \
    -metadata "track=1" \
    -metadata "date=2024" \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -y "$OUTPUT_DIR/with_cover.m4a" 2>/dev/null
echo "  Created with_cover.m4a"

ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -y "$OUTPUT_DIR/empty.m4a" 2>/dev/null
echo "  Created empty.m4a"

# M4A hi-res: ALAC 24-bit, 96kHz, stereo
ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "title=HiRes Title" \
    -metadata "artist=HiRes Artist" \
    -metadata "album=HiRes Album" \
    -metadata "genre=Electronic" \
    -metadata "track=2" \
    -metadata "date=2025" \
    -af "aformat=sample_fmts=s32:channel_layouts=stereo" \
    -ar 96000 \
    -codec:a alac \
    -y "$OUTPUT_DIR/hires.m4a" 2>/dev/null
echo "  Created hires.m4a (ALAC 24-bit/96kHz stereo)"

# ============================================================================
# MP3 files (uses ID3v2, 128kbps/44.1kHz stereo)
# ============================================================================
ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "title=Test Title" \
    -metadata "artist=Test Artist" \
    -metadata "album=Test Album" \
    -metadata "genre=Rock" \
    -metadata "track=1" \
    -metadata "date=2024" \
    -id3v2_version 3 \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -y "$OUTPUT_DIR/basic_metadata.mp3" 2>/dev/null
echo "  Created basic_metadata.mp3"

ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -i "$COVER_PNG" \
    -map 0:a -map 1:v \
    -metadata "title=Test Title" \
    -metadata "artist=Test Artist" \
    -metadata "album=Test Album" \
    -metadata "genre=Rock" \
    -metadata "track=1" \
    -metadata "date=2024" \
    -id3v2_version 3 \
    -disposition:v:0 attached_pic \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -y "$OUTPUT_DIR/with_cover.mp3" 2>/dev/null
echo "  Created with_cover.mp3"

ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -id3v2_version 3 \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -y "$OUTPUT_DIR/empty.mp3" 2>/dev/null
echo "  Created empty.mp3"

# MP3 hi-res: 320kbps, 48kHz, stereo
ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "title=HiRes Title" \
    -metadata "artist=HiRes Artist" \
    -metadata "album=HiRes Album" \
    -metadata "genre=Electronic" \
    -metadata "track=2" \
    -metadata "date=2025" \
    -id3v2_version 3 \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -b:a 320k -ar 48000 \
    -y "$OUTPUT_DIR/hires.mp3" 2>/dev/null
echo "  Created hires.mp3 (320kbps/48kHz stereo)"

# Cleanup cover image
rm -f "$COVER_PNG"

echo "Done. Generated 12 test audio files."