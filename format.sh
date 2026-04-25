#!/bin/sh
# Format C++ source files with clang-format

set -e

cd "$(dirname "$0")"

echo "Formatting C++ files with clang-format..."

# Format source files using find
find app include lib test -name '*.cpp' -o -name '*.h' | xargs clang-format -i

echo "Done."
