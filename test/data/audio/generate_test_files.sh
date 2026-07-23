#!/bin/sh
# generate_test_files.sh - Generate shared audio fixtures for media and decoder tests
# Usage: ./generate_test_files.sh <output_dir>
# Example: ./generate_test_files.sh test/data/audio

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
    -metadata "composer=Test Composer" \
    -metadata "grouping=Symphony No. 5" \
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
    -metadata "composer=Test Composer" \
    -metadata "grouping=Symphony No. 5" \
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
    -metadata "composer=HiRes Composer" \
    -metadata "grouping=The Four Seasons" \
    -metadata "track=2" \
    -metadata "date=2025" \
    -af "aformat=sample_fmts=s32:channel_layouts=stereo" \
    -ar 96000 \
    -y "$OUTPUT_DIR/hires.flac" 2>/dev/null
echo "  Created hires.flac (24-bit/96kHz stereo)"

ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "title=Classical Fixture" \
    -metadata "artist=Classical Artist" \
    -metadata "album=Classical Album" \
    -metadata "genre=Classical" \
    -metadata "composer=Fixture Composer" \
    -metadata "conductor=Fixture Conductor" \
    -metadata "ensemble=Fixture Ensemble" \
    -metadata "soloist=Fixture Soloist" \
    -metadata "work=Fixture Work" \
    -metadata "movementname=Fixture Movement" \
    -metadata "movement=2/4" \
    -metadata "track=3/9" \
    -metadata "date=2026" \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -y "$OUTPUT_DIR/classical_metadata.flac" 2>/dev/null
echo "  Created classical_metadata.flac"

ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "title=Classical Fallback" \
    -metadata "orchestra=Fixture Fallback Ensemble" \
    -metadata "performer=Fixture Fallback Soloist" \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -y "$OUTPUT_DIR/classical_fallback.flac" 2>/dev/null
echo "  Created classical_fallback.flac"

# ============================================================================
# WAV files (PCM, RIFF/WAVE)
# ============================================================================
ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "INAM=Test Title" \
    -metadata "IART=Test Artist" \
    -metadata "IPRD=Test Album" \
    -metadata "IGNR=Rock" \
    -metadata "ICRD=2024" \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -codec:a pcm_s16le \
    -y "$OUTPUT_DIR/basic_metadata.wav" 2>/dev/null
echo "  Created basic_metadata.wav"

ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -codec:a pcm_s16le \
    -y "$OUTPUT_DIR/empty.wav" 2>/dev/null
echo "  Created empty.wav"

ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "INAM=HiRes Title" \
    -metadata "IART=HiRes Artist" \
    -metadata "IPRD=HiRes Album" \
    -metadata "IGNR=Electronic" \
    -metadata "ICRD=2025" \
    -af "aformat=sample_fmts=s32:channel_layouts=stereo" \
    -ar 96000 \
    -codec:a pcm_s24le \
    -y "$OUTPUT_DIR/hires.wav" 2>/dev/null
echo "  Created hires.wav (24-bit/96kHz stereo)"

ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "INAM=Float Title" \
    -metadata "IART=Float Artist" \
    -af "aformat=sample_fmts=flt:channel_layouts=stereo" \
    -ar 48000 \
    -codec:a pcm_f32le \
    -y "$OUTPUT_DIR/float32.wav" 2>/dev/null
echo "  Created float32.wav (32-bit float/48kHz stereo)"

ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -af "aformat=sample_fmts=u8:channel_layouts=mono" \
    -codec:a pcm_u8 \
    -y "$OUTPUT_DIR/u8.wav" 2>/dev/null
echo "  Created u8.wav (8-bit unsigned/44.1kHz mono)"

# ============================================================================
# MP4/M4A files (AAC/ALAC, 44.1kHz stereo)
# ============================================================================
ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "title=Test Title" \
    -metadata "artist=Test Artist" \
    -metadata "album=Test Album" \
    -metadata "genre=Rock" \
    -metadata "composer=Test Composer" \
    -metadata "grouping=Symphony No. 5" \
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
    -metadata "composer=Test Composer" \
    -metadata "grouping=Symphony No. 5" \
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
    -metadata "composer=HiRes Composer" \
    -metadata "grouping=The Four Seasons" \
    -metadata "track=2" \
    -metadata "date=2025" \
    -af "aformat=sample_fmts=s32:channel_layouts=stereo" \
    -ar 96000 \
    -codec:a alac \
    -y "$OUTPUT_DIR/hires.m4a" 2>/dev/null
echo "  Created hires.m4a (ALAC 24-bit/96kHz stereo)"

# M4A ALAC 16-bit: used by decoder tests for 16-bit ALAC conversion paths
ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "title=ALAC 16 Title" \
    -metadata "artist=ALAC 16 Artist" \
    -metadata "album=ALAC 16 Album" \
    -metadata "genre=Classical" \
    -metadata "composer=ALAC 16 Composer" \
    -metadata "grouping=Decoder Fixtures" \
    -metadata "track=3" \
    -metadata "date=2026" \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -codec:a alac \
    -y "$OUTPUT_DIR/alac16.m4a" 2>/dev/null
echo "  Created alac16.m4a (ALAC 16-bit/44.1kHz stereo)"

ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "title=Classical Fixture" \
    -metadata "artist=Classical Artist" \
    -metadata "album=Classical Album" \
    -metadata "genre=Classical" \
    -metadata "composer=Fixture Composer" \
    -metadata "conductor=Fixture Conductor" \
    -metadata "ensemble=Fixture Ensemble" \
    -metadata "soloist=Fixture Soloist" \
    -metadata "work=Fixture Work" \
    -metadata "movementname=Fixture Movement" \
    -metadata "movement=2/4" \
    -metadata "track=3/9" \
    -metadata "date=2026" \
    -movflags use_metadata_tags \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -y "$OUTPUT_DIR/classical_metadata.m4a" 2>/dev/null
echo "  Created classical_metadata.m4a"

ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "title=Classical Fallback" \
    -metadata "orchestra=Fixture Fallback Ensemble" \
    -movflags use_metadata_tags \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -y "$OUTPUT_DIR/classical_fallback.m4a" 2>/dev/null
echo "  Created classical_fallback.m4a"

# ============================================================================
# MP3 files (uses ID3v2, 128kbps/44.1kHz stereo)
# ============================================================================
ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "title=Test Title" \
    -metadata "artist=Test Artist" \
    -metadata "album=Test Album" \
    -metadata "genre=Rock" \
    -metadata "composer=Test Composer" \
    -metadata "grouping=Symphony No. 5" \
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
    -metadata "composer=Test Composer" \
    -metadata "grouping=Symphony No. 5" \
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
    -metadata "composer=HiRes Composer" \
    -metadata "grouping=The Four Seasons" \
    -metadata "track=2" \
    -metadata "date=2025" \
    -id3v2_version 3 \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -b:a 320k -ar 48000 \
    -y "$OUTPUT_DIR/hires.mp3" 2>/dev/null
echo "  Created hires.mp3 (320kbps/48kHz stereo)"

ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "title=Classical Fixture" \
    -metadata "artist=Classical Artist" \
    -metadata "album=Classical Album" \
    -metadata "genre=Classical" \
    -metadata "composer=Fixture Composer" \
    -metadata "conductor=Fixture Conductor" \
    -metadata "ensemble=Fixture Ensemble" \
    -metadata "soloist=Fixture Soloist" \
    -metadata "work=Fixture Work" \
    -metadata "movementname=Fixture Movement" \
    -metadata "movement=2/4" \
    -metadata "track=3/9" \
    -metadata "date=2026" \
    -id3v2_version 3 \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -y "$OUTPUT_DIR/classical_metadata.mp3" 2>/dev/null
echo "  Created classical_metadata.mp3"

ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata "title=Classical Fallback" \
    -metadata "orchestra=Fixture Fallback Ensemble" \
    -id3v2_version 3 \
    -af "aformat=sample_fmts=s16:channel_layouts=stereo" \
    -y "$OUTPUT_DIR/classical_fallback.mp3" 2>/dev/null
echo "  Created classical_fallback.mp3"

# MP3 VBR without Xing/VBRI: the high-bitrate first segment makes a
# header/file-size duration estimate much shorter than the real stream.
VBR_TMP_DIR=$(mktemp -d)
ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -map_metadata -1 \
    -codec:a libmp3lame -b:a 320k -ar 44100 \
    -write_xing 0 -id3v2_version 0 \
    -y "$VBR_TMP_DIR/high.mp3" 2>/dev/null
ffmpeg -f lavfi -i "sine=frequency=440:duration=19" \
    -map_metadata -1 \
    -codec:a libmp3lame -b:a 32k -ar 44100 \
    -write_xing 0 -id3v2_version 0 \
    -y "$VBR_TMP_DIR/low.mp3" 2>/dev/null
cat "$VBR_TMP_DIR/high.mp3" "$VBR_TMP_DIR/low.mp3" > "$OUTPUT_DIR/vbr_no_seek_table.mp3"
rm -r "$VBR_TMP_DIR"
echo "  Created vbr_no_seek_table.mp3 (VBR without Xing/VBRI)"

# Cleanup cover image
rm -f "$COVER_PNG"

echo "Done. Generated 25 test audio files."
