#!/bin/bash
while IFS= read -r -d '' entry; do
  st="${entry:0:2}"
  f="${entry:3}"

  if [[ "$st" =~ ^[RC] ]]; then
    IFS= read -r -d '' f || break
  fi

  if [ -f "$f" ] && [[ "$f" =~ \.(cpp|h)$ ]]; then
    clang-format -i "$f"
    printf 'formatted %s\n' "$f"
  fi
done < <(git status --porcelain -z)
