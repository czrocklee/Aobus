#!/bin/bash
# generate_test_library.sh - Generate a complex test music library

set -e

OUTPUT_DIR="${1:-./test_music_lib}"
RSC="${2:-/tmp/build/rsc}"

mkdir -p "$OUTPUT_DIR"

echo "Generating complex test music library in $OUTPUT_DIR..."

# Generate FLAC files - Rock genre
for i in 1 2 3; do
    ffmpeg -f lavfi -i "sine=frequency=$((440+i*50)):duration=2" \
        -metadata title="Rock Song $i" \
        -metadata artist="The Rockers" \
        -metadata album="Rock Album $i" \
        -metadata date="202$i" \
        -metadata track="$i" \
        -metadata genre="Rock" \
        -y "$OUTPUT_DIR/rock_$i.flac" 2>/dev/null || true
done

# Generate M4A files - Jazz genre
for i in 1 2 3; do
    ffmpeg -f lavfi -i "sine=frequency=$((520+i*50)):duration=2" \
        -c:a copy \
        -metadata title="Jazz Tune $i" \
        -metadata artist="Jazz Cats" \
        -metadata album="Jazz Collection" \
        -metadata date="201$i" \
        -metadata track="$i" \
        -metadata genre="Jazz" \
        -y "$OUTPUT_DIR/jazz_$i.m4a" 2>/dev/null || true
done

# Generate MP3 files - Classical genre
for i in 1 2 3; do
    ffmpeg -f lavfi -i "sine=frequency=$((300+i*30)):duration=2" \
        -codec libmp3lame -b:a 128k \
        -metadata title="Classical Piece $i" \
        -metadata artist="Classical Beats" \
        -metadata album="Symphony $i" \
        -metadata date="202$i" \
        -metadata track="$i" \
        -metadata genre="Classical" \
        -y "$OUTPUT_DIR/classical_$i.mp3" 2>/dev/null || true
done

# Generate more diverse tracks - Electronic
for i in 1 2; do
    ffmpeg -f lavfi -i "sine=frequency=$((660+i*70)):duration=1" \
        -metadata title="Electronic Track $i" \
        -metadata artist="Synth Masters" \
        -metadata album="Electronic Dreams" \
        -metadata date="2022" \
        -metadata track="$i" \
        -metadata genre="Electronic" \
        -y "$OUTPUT_DIR/electronic_$i.flac" 2>/dev/null || true
done

# Generate Pop tracks
for i in 1 2; do
    ffmpeg -f lavfi -i "sine=frequency=$((800+i*100)):duration=1" \
        -metadata title="Pop Hit $i" \
        -metadata artist="Pop Stars" \
        -metadata album="Summer Hits" \
        -metadata date="2023" \
        -metadata track="$i" \
        -metadata genre="Pop" \
        -y "$OUTPUT_DIR/pop_$i.flac" 2>/dev/null || true
done

echo "Generated audio files:"
ls -la "$OUTPUT_DIR"/*.flac "$OUTPUT_DIR"/*.m4a "$OUTPUT_DIR"/*.mp3 2>/dev/null || true
echo "files created"

# Initialize with rsc
cd "$OUTPUT_DIR"
echo ""
echo "Running rsc init..."
$RSC init

echo ""
echo "Done! Test library created at $OUTPUT_DIR"
echo "Total tracks: $(find "$OUTPUT_DIR" -name "*.m4a" -o -name "*.flac" -o -name "*.mp3" 2>/dev/null | wc -l)"
