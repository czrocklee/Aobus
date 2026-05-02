#!/bin/sh
# test_aobus.sh - Integration tests for aobus CLI
# Assumes: running from workspace root, inside nix-shell

set -e

TEST_DIR="/tmp/aobus_shell_test"
AOBUS="/tmp/build/debug/app/aobus/aobus"

cleanup() { rm -rf "$TEST_DIR"; mkdir -p "$TEST_DIR"; }

echo "=== aobus CLI Integration Tests ==="
cleanup

# Test 1: Init with no files
echo "Test 1: Init with no files..."
cd "$TEST_DIR"
"$AOBUS" init
echo "  PASS"

# Test 2: Generate test audio files
echo "Test 2: Generate test files..."
ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata title="Rock Song" -metadata artist="Rock Artist" \
    -metadata album="Rock Album" -metadata date="2021" -metadata track="1" \
    -metadata genre="Rock" -y "$TEST_DIR/rock.flac" 2>/dev/null
ffmpeg -f lavfi -i "sine=frequency=440:duration=1" \
    -metadata title="Pop Song" -metadata artist="Pop Artist" \
    -metadata album="Pop Album" -metadata date="2022" -metadata track="2" \
    -metadata genre="Pop" -y "$TEST_DIR/pop.flac" 2>/dev/null
echo "  PASS"

# Test 3: Init with files
echo "Test 3: Init with audio files..."
"$AOBUS" init
"$AOBUS" track show | grep -q "Rock Song"
"$AOBUS" track show | grep -q "Pop Song"
echo "  PASS"

# Test 4: JSON output
echo "Test 4: JSON output..."
"$AOBUS" track show --json | grep -q "Rock Song"
"$AOBUS" track show --json | grep -q "artist"
echo "  PASS"

# Test 5: Limit/offset
echo "Test 5: Limit/offset..."
count=$("$AOBUS" track show --json | grep -c "id")
[ "$count" -eq 2 ]
count=$("$AOBUS" track show --limit 1 --json | grep -c "id")
[ "$count" -eq 1 ]
echo "  PASS"

# Test 6: Tag add/show
echo "Test 6: Tag add/show..."
"$AOBUS" tag add 1 fav | grep -q "added tag"
"$AOBUS" tag show 1 | grep -q "fav"
echo "  PASS"

# Test 7: Tag remove
echo "Test 7: Tag remove..."
"$AOBUS" tag remove 1 fav | grep -q "removed tag"
"$AOBUS" tag show 1 | grep -q "no tags"
echo "  PASS"

# Test 8: List create/show
echo "Test 8: List create/show..."
"$AOBUS" list create --name "My List" | grep -q "add list"
"$AOBUS" list show | grep -q "0"
echo "  PASS"

# Test 9: Track delete
echo "Test 9: Track delete..."
"$AOBUS" track delete 1 | grep -q "deleted track"
! "$AOBUS" track show --json | grep -q "Rock Song"
echo "  PASS"

# Test 10: Non-existent tag query
echo "Test 10: Non-existent tag query..."
# This should not throw an error
"$AOBUS" track show --filter "#NonExistentTag" > /dev/null
echo "  PASS"

rm -rf "$TEST_DIR"
echo ""
echo "=== All tests passed! ==="
