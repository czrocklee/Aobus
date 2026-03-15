#!/bin/bash
# Run clang-tidy on RockStudio C++ files
# Uses cppgen skill rules from .clang-tidy

set -e

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

# Find all C++ source and header files, excluding build directory
FILES=$(find . -type f \( -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) | grep -v build | grep -v ".cache")

echo "Running clang-tidy with cppgen rules..."
echo "Checks: $CHECKS"
echo ""

if command -v clang-tidy &> /dev/null; then
  clang-tidy -checks="$CHECKS" $FILES
else
  echo "Error: clang-tidy not found"
  echo "Install with: apt install clang-tidy or brew install llvm"
  exit 1
fi
