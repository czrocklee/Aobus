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

```yaml
schema: aobus-fleet-intent/v1
id: optional-council-id
task-kind: council-review
invariant: Identify correctness risks without mutating the repository.
scope: []
depends-on: []
overrides: {}
body: |
  Review the supplied change for correctness, regressions, and missing tests.
  Findings must cite concrete files and behavior.
```

Use `task-kind: council-plan` for implementation planning. Registered depths: `council-review` runs
`challenge`, `council-plan` runs `full`. An override may only tighten — reduce depth (for example
`depth: panel` on a review) or increase quorum; requesting `depth: full` on `council-review` is a
relaxation and is rejected.

## Run And Synthesize

```bash
/tmp/build/debug/tool/fleet/aobus-fleet run --registry config/agent-fleet.yaml --repo "$PWD" \
  --out /tmp/aobus-fleet/council-$(date +%s) /tmp/council-intent.yaml
```

Read `dossier.md`, member logs, `manifest.yaml`, and `trace.yaml`. Timed-out, mutating, failed, or empty
members are quarantined and omitted. The dossier is always `ADVISORY`; write the final plan or review
yourself after checking claims against the repository.
