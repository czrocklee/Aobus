---
name: execute-plan
description: >-
  Execute an already-decided implementation plan through the aobus-fleet GateEngine. The fleet works
  in isolated repository copies, independently extracts and validates a patch, and returns a proposal
  or advisory for chair review. It never applies the result to the real tree.
---

# Execute Plan

Use this skill only after the implementation approach, invariant, exact file scope, and allowed
operations are decided. The chair still owns architecture, semantic review, application, and final
real-tree validation.

## Intent

Write an `aobus-fleet-intent/v1` YAML file outside the repository:

```yaml
schema: aobus-fleet-intent/v1
id: optional-stable-id
task-kind: implement-plan
invariant: Preserve existing behavior while implementing the approved plan.
scope:
  - path: lib/audio/Player.cpp
    operations: [modify]
  - path: test/unit/audio/PlayerTest.cpp
    operations: [modify]
depends-on: []
overrides: {}
body: |
  Implement the approved design exactly as described.
  Do not redesign the API or expand scope.
```

Use exact repository-relative paths. Operations are `create`, `modify`, or `delete`. The body contains
the implementation instructions, not command strings. Overrides may only tighten the registered
binding.

## Run

```bash
nix-shell --run "/tmp/build/debug/tool/fleet/aobus-fleet validate-config --registry config/agent-fleet.yaml"
nix-shell --run "/tmp/build/debug/tool/fleet/aobus-fleet run --registry config/agent-fleet.yaml \
  --repo "$PWD" --out /tmp/aobus-fleet/run-$(date +%s) /tmp/intent.yaml"
```

Read the phase `manifest.yaml`, `review.md`, `evidence.yaml`, harness `patch`, candidate logs, and
`trace.yaml`. `PROPOSAL` means the registered oracle independently passed; `ADVISORY` means no oracle
was configured. Neither means accepted. On failure, `manifest.yaml` carries `escalation-action:`
(`retry`, `switch-route`, `require-council`, `stop-route`, or `return-chair`) — the registry policy
for that failure reason; act on it instead of re-deriving policy. It is `none` for successful phases.

Apply or modify an accepted patch using the ordinary repository workflow, run real-tree validation,
then record the outcome:

```bash
/tmp/build/debug/tool/fleet/aobus-fleet review record --out /tmp/aobus-fleet/run-... --phase <phase-id> \
  --verdict accept --reason "Reviewed and validated on the real tree"
```

Risk-oracle and policy failures return an escalation while preserving evidence for heavier review.
