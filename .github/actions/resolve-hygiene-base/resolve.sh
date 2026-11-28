#!/usr/bin/env bash
set -euo pipefail

if [[ "${GITHUB_EVENT_NAME:-}" == pull_request ]]; then
  if [[ ! "${PULL_REQUEST_BASE_SHA:-}" =~ ^[0-9a-f]{40}$ ]]; then
    echo '::error::A full pull request base commit SHA is required.' >&2
    exit 1
  fi
  base="$PULL_REQUEST_BASE_SHA"
elif [[ -n "$PUSH_BEFORE_SHA" && ! "$PUSH_BEFORE_SHA" =~ ^0+$ ]]; then
  base="$PUSH_BEFORE_SHA"
elif [[ -n "$DEFAULT_BRANCH" && "$WORKFLOW_REF" != "refs/heads/$DEFAULT_BRANCH" ]] &&
     git rev-parse --verify --quiet "refs/remotes/origin/$DEFAULT_BRANCH^{commit}" >/dev/null; then
  base="$(git merge-base HEAD "refs/remotes/origin/$DEFAULT_BRANCH")"
else
  base="HEAD~1"
fi

base_sha="$(git rev-parse --verify --end-of-options "$base^{commit}")"
head_sha="$(git rev-parse --verify 'HEAD^{commit}')"
if [[ "${GITHUB_EVENT_NAME:-}" == pull_request ]]; then
  if [[ "$base_sha" != "$base" ]]; then
    echo '::error::The pull request base SHA must identify a commit directly.' >&2
    exit 1
  fi
  if ! git merge-base --is-ancestor "$base_sha" "$head_sha"; then
    echo '::error::The pull request base must be an ancestor of the checkout.' >&2
    exit 1
  fi
fi
printf 'Hygiene base: %s\n' "$base_sha"
printf 'sha=%s\n' "$base_sha" >> "$GITHUB_OUTPUT"

# Disable rename folding so moving code into a documentation path still
# selects native validation. A failed diff must never produce a fast path.
changed_files="$(mktemp)"
trap 'rm -f "$changed_files"' EXIT
git diff --name-status --no-renames -z "$base_sha" "$head_sha" > "$changed_files"
commit_count="$(git rev-list --count "$base_sha..$head_sha")"
file_count=0
docs_only=true
checker_reports=false
if [[ ! -s "$changed_files" || "${GITHUB_EVENT_NAME:-}" == workflow_dispatch ]]; then
  docs_only=false
fi
while IFS= read -r -d '' status && IFS= read -r -d '' path; do
  file_count=$((file_count + 1))
  case "$path" in
    app/windows-winui/deployment/README.md)
      # Deployment requires these instructions at this path.
      if [[ "$status" == D || "$status" == T ]]; then
        docs_only=false
      fi
      ;;
    AGENTS.md|CONTRIBUTING.md|README.md|doc/*.md|.agents/skills/*.md|tool/*.md|app/*.md|asset/*.md) ;;
    *)
      docs_only=false
      # The Debug probe is for the checker forwarding and receipt contract,
      # not every dependency shared by a native build.
      case "$path" in
        .bazelrc|.github/workflows/ci.yml|.github/actions/resolve-hygiene-base/*|\
        bazel/lint/lint.bzl|bazel/compile_context.bzl|bazel/compiler_policy.bzl|bazel/msvc_policy.bzl|\
        tool/lint/BUILD.bazel|tool/lint/build.bzl|\
        tool/lint/check/LocalInitializationStyleCheck.*|lib/utility/BUILD.bazel|lib/utility/Base64.cpp|\
        script/ao/graph/bazel_lint_action.py|script/ao/graph/bazel_lint_schema.py|script/ao/graph/bazel_lint_reports.py|\
        script/ao/portal/bazel_lint.py|test/lint/check_checker_reports.py)
          checker_reports=true
          ;;
      esac
      ;;
  esac
done < "$changed_files"
printf 'Validation range: %s..%s (%s commits, %s changed paths)\n' \
  "$base_sha" "$head_sha" "$commit_count" "$file_count"
if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
  printf 'Hygiene checks the final tree across `%s..%s`: **%s commits**, **%s changed paths**. Platform-incompatible files are covered by their native job.\n' \
    "$base_sha" "$head_sha" "$commit_count" "$file_count" >> "$GITHUB_STEP_SUMMARY"
fi
printf 'docs-only=%s\nchecker-reports=%s\n' "$docs_only" "$checker_reports" >> "$GITHUB_OUTPUT"
