---
id: architecture.concern
type: architecture
status: draft
domain: domain
summary: Defines one system boundary or cross-cutting architecture concern.
---
# Architecture title

## Scope

State the architectural question this document owns and what it excludes.
Confirm that the subject meets the focused-architecture threshold in the [documentation system](../README.md), and identify its portfolio role from the [architecture landscape](../architecture/README.md).
Describe current structure even when it is undesirable; link an RFC for the proposed replacement instead of presenting future intent as current.

## System context

Link the architecture landscape and system architecture, then place the concern in their portfolio, layer, and subsystem maps.
Name the primary upstream authorities, downstream consumers, and adjacent architecture owners so the relationship can be added to the landscape without inventing a second classification here.
Name public and implementation paths here when the concern owns a code boundary.

## Responsibilities

Assign state, policy, and orchestration ownership.

## Boundaries and dependency direction

Refine the allowed and forbidden dependencies without restating the top-level layer model.
Distinguish dependencies, coordination relationships, and facts explicitly owned by an adjacent architecture.

## Data and control flow

Describe the important cross-boundary sequence.

## Structural constraints

Define ownership, dependency, executor-affinity, and lifetime constraints.
Link to specifications for observable behavior invariants.

## Failure, cancellation, and lifetime boundaries

Describe ownership without duplicating subsystem behavior specifications.

## Implementation map

Link stable symbols and source locations.

## Test map

Link the tests that protect architectural boundaries.

## Related documents

Link specifications, reference, decisions, and adjacent architecture owners without using this section as a substitute for the landscape relationship map.
