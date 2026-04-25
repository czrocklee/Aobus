---
name: manage-git-flow
description: Targeted formatting and commit standards for RockStudio. Use when a user asks to commit changes to ensure only modified files are formatted before the commit.
---

# Manage Git Flow

This skill provides the procedure for correctly formatting and committing changes in RockStudio.

## 1. Targeted Formatting
When a user asks to commit, you MUST run `clang-format` ONLY on the files you modified or created. Do NOT use global formatting scripts.

Run this command before performing any commit:
```bash
for f in $(git status --porcelain | awk '{print $2}'); do 
  if [ -f "$f" ] && [[ "$f" =~ \.(cpp|h)$ ]]; then 
    clang-format -i "$f"; 
  fi; 
done
```

## 2. Commit Procedure
1. **Review**: Always run `git status && git diff HEAD && git log -n 3` to verify the exact state and match the project's commit style.
2. **Message**: Use clear, concise summaries in the imperative mood.
   - Example: `perf: optimize TrackRow memory usage`
   - Example: `refactor: move TrackRowDataProvider to platform directory`
3. **Verify**: Run `git status` after the commit to confirm a clean working tree.
