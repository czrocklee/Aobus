#!/bin/bash
# Run clang-tidy on RockStudio C++ files
# Assumes running inside nix-shell (like cmake build)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SOURCE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="/tmp/build"

# Ensure we have a compile database
if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
  echo "Configuring project to generate compile_commands.json..."
  cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
fi

CLANG_TIDY_CHECKS=(
  "-*,readability-include-declaration"
  "modernize-use-nullptr"
  "modernize-use-std-arrays"
  "modernize-raw-string-literal"
  "modernize-use-std-numbers"
  "modernize-loop-convert"
  "modernize-use-emplace"
  "cppcoreguidelines-special-member-functions"
  "cppcoreguidelines-avoid-goto"
  "cppcoreguidelines-pro-type-const-cast"
  "cppcoreguidelines-pro-type-reinterpret-cast"
  "performance-move-const-arg"
  "performance-unnecessary-copy"
  "performance-unnecessary-value-param"
)

CHECKS=$(IFS=,; echo "${CLANG_TIDY_CHECKS[*]}")

FILES=(
  "$SOURCE_DIR/src/lmdb/Environment.cpp"
  "$SOURCE_DIR/src/lmdb/Transaction.cpp"
  "$SOURCE_DIR/src/lmdb/Database.cpp"
  "$SOURCE_DIR/src/core/MusicLibrary.cpp"
  "$SOURCE_DIR/src/core/ResourceStore.cpp"
  "$SOURCE_DIR/src/core/DictionaryStore.cpp"
  "$SOURCE_DIR/src/core/TrackStore.cpp"
  "$SOURCE_DIR/src/core/TrackRecord.cpp"
  "$SOURCE_DIR/src/core/TrackLayout.cpp"
  "$SOURCE_DIR/src/core/ListLayout.cpp"
  "$SOURCE_DIR/src/core/ListStore.cpp"
  "$SOURCE_DIR/src/expr/ExecutionPlan.cpp"
  "$SOURCE_DIR/src/expr/PlanEvaluator.cpp"
  "$SOURCE_DIR/src/expr/Expression.cpp"
  "$SOURCE_DIR/src/expr/Parser.cpp"
  "$SOURCE_DIR/src/expr/Serializer.cpp"
  "$SOURCE_DIR/src/tag/File.cpp"
  "$SOURCE_DIR/src/tag/flac/File.cpp"
  "$SOURCE_DIR/src/tag/flac/MetadataBlock.cpp"
  "$SOURCE_DIR/src/tag/mp4/File.cpp"
  "$SOURCE_DIR/src/tag/mp4/Atom.cpp"
  "$SOURCE_DIR/src/tag/mpeg/File.cpp"
  "$SOURCE_DIR/src/tag/mpeg/Frame.cpp"
  "$SOURCE_DIR/src/tag/mpeg/id3v2/Reader.cpp"
)

echo "Running clang-tidy with cppgen rules..."
echo "Checks: $CHECKS"
echo ""

# Get GCC include paths and pass to clang-tidy
EXTRA_ARGS=()
while IFS= read -r include_path; do
  [[ -n "$include_path" ]] || continue
  EXTRA_ARGS+=("--extra-arg-before=-isystem$include_path")
done < <(g++ -E -x c++ - -v < /dev/null 2>&1 \
  | sed -n '/#include <...> search starts here:/,/End of search list\./p' \
  | sed '1d;$d' \
  | sed 's/^[[:space:]]*//')

clang-tidy -checks="$CHECKS" -p="$BUILD_DIR" "${EXTRA_ARGS[@]}" "${FILES[@]}"
