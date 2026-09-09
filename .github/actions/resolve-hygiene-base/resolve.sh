#!/usr/bin/env bash
set -euo pipefail

if [[ -n "$PULL_REQUEST_BASE_SHA" ]]; then
  base="$PULL_REQUEST_BASE_SHA"
elif [[ -n "$PUSH_BEFORE_SHA" && ! "$PUSH_BEFORE_SHA" =~ ^0+$ ]]; then
  base="$PUSH_BEFORE_SHA"
elif [[ -n "$DEFAULT_BRANCH" && "$WORKFLOW_REF" != "refs/heads/$DEFAULT_BRANCH" ]] &&
     git rev-parse --verify --quiet "refs/remotes/origin/$DEFAULT_BRANCH^{commit}" >/dev/null; then
  base="$(git merge-base HEAD "refs/remotes/origin/$DEFAULT_BRANCH")"
else
  base="HEAD~1"
fi

base_sha="$(git rev-parse --verify "$base^{commit}")"
printf 'Hygiene base: %s\n' "$base_sha"
printf 'sha=%s\n' "$base_sha" >> "$GITHUB_OUTPUT"

# Disable rename folding so moving code into a documentation path still
# selects native validation. A failed diff must never produce a fast path.
changed_files="$(mktemp)"
trap 'rm -f "$changed_files"' EXIT
git diff --name-only --no-renames -z "$base_sha" HEAD > "$changed_files"
head_sha="$(git rev-parse --verify 'HEAD^{commit}')"
commit_count="$(git rev-list --count "$base_sha..$head_sha")"
file_count=0
docs_only=true
if [[ ! -s "$changed_files" || "${GITHUB_EVENT_NAME:-}" == workflow_dispatch ]]; then
  docs_only=false
fi
while IFS= read -r -d '' path; do
  file_count=$((file_count + 1))
  case "$path" in
    AGENTS.md|CLAUDE.md|GEMINI.md|CONTRIBUTING.md|README.md|doc/*.md|.agents/skills/*.md) ;;
    *) docs_only=false ;;
  esac
done < "$changed_files"
printf 'Validation range: %s..%s (%s commits, %s changed paths)\n' \
  "$base_sha" "$head_sha" "$commit_count" "$file_count"
if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
  printf 'Hygiene checks the final tree across `%s..%s`: **%s commits**, **%s changed paths**. Platform-incompatible files are covered by their native job.\n' \
    "$base_sha" "$head_sha" "$commit_count" "$file_count" >> "$GITHUB_STEP_SUMMARY"
fi
printf 'docs-only=%s\n' "$docs_only" >> "$GITHUB_OUTPUT"
