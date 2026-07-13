---
name: run-council
description: >-
  Convene the aobus-council runner only when the user explicitly asks for a council
  (for example, says "ask council", "convene council", "summon council", "consult council",
  "council", or "run-council") for a plan or code review.
  Do not use this skill merely because a task is high-stakes, architectural, concurrent, or risky.
---

# Run Council

Follow the frontmatter trigger rule strictly. Council is the only agent mechanism: it runs a
registered roster in copied workspaces and returns an advisory dossier. The chair still checks the
claims and owns the final plan or review.

## Intent

Minimal repository-wide or discussion-only review:

```yaml
schema: aobus-council-intent/v1
id: council-review-id
task-kind: council-review
invariant: Identify correctness and regression risks.
depends-on: []
overrides: {}
body: |
  Review the supplied change for correctness, regressions, and missing tests.
  Findings must cite concrete files and behavior.
```

When the council should focus on specific files, use `focus:` hints. These are advisory prompt
context, not a hard write boundary. Items must be objects, not bare path strings. A trailing `/` marks
a directory prefix:

```yaml
schema: aobus-council-intent/v1
id: custom-metadata-review
task-kind: council-review
invariant: Preserve the intended metadata import boundary.
focus:
  - path: lib/tag/mpeg/id3v2/Reader.cpp
  - path: test/unit/tag/MpegFileTest.cpp
depends-on: []
overrides: {}
body: |
  Review the supplied implementation for correctness risks, regressions, and missing tests.
```

Do not use `scope:`. That field belonged to the removed gate/patch mechanism and is rejected.

Use `task-kind: council-plan` for implementation planning. Registered depths are `panel`,
`challenge`, and `full`; registry defaults live in `config/agent-council.yaml`. `panel` runs one
independent-review round, `challenge` adds a peer-challenge round among usable draft members, and
`full` adds a self-revision round after peer challenge.

## Roster

A `roster:` override replaces the registered panel with any subset of the agent catalog in
`config/agent-council.yaml`:

```yaml
overrides:
  roster: [anthropic-sonnet, openai-gpt-mini, google-gemini-flash]
  depth: panel
  quorum: 2
```

The merged roster must reference known agents, contain no duplicates, use distinct vendors, and
satisfy `1 <= quorum <= roster size`. Only `roster`, `depth`, and `quorum` may be overridden.

## Run And Synthesize

```bash
mkdir -p /tmp/aobus-council
out="$(mktemp -d /tmp/aobus-council/council-XXXXXX)"
./ao council run --registry config/agent-council.yaml --repo "$PWD" --out "$out" /tmp/council-intent.yaml
```

For each intent, read `dossier.md` first, then `evidence.yaml`, under
`$out/<intent-id>/`; per-member round artifacts live under
`members/<member>/<round>/`. Treat `usable: false` in `evidence.yaml` as not
contributing to quorum, even when the member process exited successfully. The
artifact layout, round ids, sandbox binds, exit codes, and failure semantics
are owned by `doc/development/agent-council.md`.

The dossier is advisory. Write the final plan or review yourself after checking claims against the
repository.
