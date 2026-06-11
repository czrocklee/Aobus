---
name: run-council
description: >-
  Convene the aobus-fleet SynthesisEngine for a high-stakes plan or code review. Members draft
  independently, challenge peers, optionally revise, and return an advisory dossier for chair synthesis.
---

# Run Council

Use a council when independent model diversity is worth the cost: architecture, risky reviews,
error-contract choices, concurrency design, or disputed root cause. The fleet runs member rounds only;
the chair performs final synthesis and owns the verdict.

## Intent

Minimal intent for repository-wide or discussion-only review:

```yaml
schema: aobus-fleet-intent/v1
id: optional-council-id
task-kind: council-review
invariant: Identify correctness and regression risks.
scope: []
depends-on: []
overrides: {}
body: |
  Review the supplied change for correctness, regressions, and missing tests.
  Findings must cite concrete files and behavior.
```

When the council should focus on specific files, `scope` items must be objects, not bare path strings.
Each item needs a path and one or more operations from `create`, `modify`, and `delete`:

```yaml
schema: aobus-fleet-intent/v1
id: custom-metadata-review
task-kind: council-review
invariant: Preserve the intended metadata import boundary.
scope:
  - path: lib/tag/mpeg/id3v2/Reader.cpp
    operations: [modify]
  - path: test/unit/tag/MpegFileTest.cpp
    operations: [modify]
  - path: doc/design/track-detail-grid.md
    operations: [modify]
depends-on: []
overrides: {}
body: |
  Review the supplied implementation for correctness risks, regressions, and missing tests.
```

The engine injects the scope list into every member prompt ("Scope (focus on these paths and
operations):"); the body does not need to restate it.

For review-only council runs, choose operations that describe the existing change under review. Do not
invent a `read` or `review` operation; the schema accepts only `create`, `modify`, and `delete`.

Use `task-kind: council-plan` for implementation planning. Registered depths: `council-review` runs
`challenge`, `council-plan` runs `full`. An override may only tighten — reduce depth (for example
`depth: panel` on a review) or increase quorum; requesting `depth: full` on `council-review` is a
relaxation and is rejected.

## Run And Synthesize

```bash
/tmp/build/debug/tool/fleet/aobus-fleet run --registry config/agent-fleet.yaml --repo "$PWD" \
  --out /tmp/aobus-fleet/council-$(date +%s) /tmp/council-intent.yaml
```

Read `dossier.md`, `manifest.yaml`, `trace.yaml`, and the per-member round artifacts under
`members/<member>/<round>/`. Each round directory contains `prompt.md`, `stdout.txt`, `stderr.txt`, and
`result.yaml`; inspect these when a member is missing from the dossier or when prompt/context quality is
in question. Timed-out, failed, or empty members are quarantined and omitted from the dossier.
The dossier is always `ADVISORY`; write the final plan or review yourself after checking claims against
the repository.

After closing the council — once the final plan or review is written — append a brief performance
evaluation of each participating member. Judge from the round artifacts, not the dossier alone: draft
quality and concreteness (R1), substance of challenges and whether peer claims were actually verified
against the repository (R2), and responsiveness in revision (R3). One or two sentences per member is
enough; note quarantined members and the quarantine reason. This evaluation is chair commentary for
future roster and depth decisions, not part of the verdict.

Round context is isolated per member: R1 is independent; R2 contains only other members' drafts; R3
contains the member's own original draft and own challenge notes plus only other members' challenges.
Each prompt states the round position and where the output goes, and instructs members to verify peer
claims against the repository. Verify these boundaries in the saved `prompt.md` artifacts when
diagnosing council quality.
