---
id: rfc.index
type: index
status: current
domain: documentation
summary: Defines and indexes tracked Aobus proposals before they become current behavior.
---
# Requests for comments

RFCs are reviewable proposals for cross-cutting, user-visible, persistent, or otherwise consequential changes.
They own proposed behavior only while the proposal is under evaluation and never override current architecture, specifications, or reference.

File names use a four-digit sequence and a concise noun phrase.
When an RFC is accepted or implemented, link the architecture, specification, reference, and decision documents it produced.
Execution details belong in the local ignored `doc/plan/` tree.

Use the [RFC template](../template/rfc.md).

## Dependency map

The dependency contract and category definitions are owned by the [documentation system](../README.md#rfc-dependencies).
Each row records the outgoing direct edges of one RFC; sequence numbers alone imply no order.

| RFC | Hard | Conditional | Integration |
|---|---|---|---|
| [0001](0001-safe-library-transfer.md) | None | None | None |
| [0002](0002-durable-library-storage.md) | None | None | None |
| [0003](0003-library-mutation-pipeline.md) | None | None | None |
| [0004](0004-scalable-library-tasks.md) | [0003](0003-library-mutation-pipeline.md) | [0002](0002-durable-library-storage.md) | [0001](0001-safe-library-transfer.md) |
| [0005](0005-coherent-playback-boundary.md) | None | None | None |
| [0006](0006-coherent-derived-track-views.md) | [0009](0009-pure-expression-binding.md) | None | [0008](0008-declarative-track-capability-bridge.md) |
| [0007](0007-revisioned-completion-vocabulary.md) | None | None | [0008](0008-declarative-track-capability-bridge.md), [0003](0003-library-mutation-pipeline.md) |
| [0008](0008-declarative-track-capability-bridge.md) | None | None | [0007](0007-revisioned-completion-vocabulary.md) |
| [0009](0009-pure-expression-binding.md) | None | None | None |
| [0010](0010-versioned-presentation-state.md) | None | None | [0008](0008-declarative-track-capability-bridge.md) |
| [0011](0011-executor-affine-reporting-feed.md) | None | None | [0005](0005-coherent-playback-boundary.md), [0012](0012-structured-async-fault-diagnostics.md), [0013](0013-coherent-application-reporting-policy.md) |
| [0012](0012-structured-async-fault-diagnostics.md) | None | None | [0013](0013-coherent-application-reporting-policy.md) |
| [0013](0013-coherent-application-reporting-policy.md) | None | None | [0003](0003-library-mutation-pipeline.md), [0004](0004-scalable-library-tasks.md), [0005](0005-coherent-playback-boundary.md), [0011](0011-executor-affine-reporting-feed.md) |
| [0014](0014-observable-atomic-replacement.md) | None | None | [0005](0005-coherent-playback-boundary.md), [0010](0010-versioned-presentation-state.md), [0013](0013-coherent-application-reporting-policy.md), [0015](0015-fail-closed-config-store.md) |
| [0015](0015-fail-closed-config-store.md) | None | None | [0005](0005-coherent-playback-boundary.md), [0010](0010-versioned-presentation-state.md) |
| [0016](0016-coherent-workspace-transactions.md) | None | None | [0005](0005-coherent-playback-boundary.md), [0006](0006-coherent-derived-track-views.md), [0013](0013-coherent-application-reporting-policy.md) |
| [0017](0017-versioned-workspace-session.md) | [0016](0016-coherent-workspace-transactions.md) | [0010](0010-versioned-presentation-state.md), [0015](0015-fail-closed-config-store.md) | [0013](0013-coherent-application-reporting-policy.md) |
| [0018](0018-interactive-session-lifecycle.md) | [0017](0017-versioned-workspace-session.md) | [0005](0005-coherent-playback-boundary.md), [0015](0015-fail-closed-config-store.md) | [0010](0010-versioned-presentation-state.md), [0013](0013-coherent-application-reporting-policy.md) |
| [0019](0019-transactional-active-library-switch.md) | [0015](0015-fail-closed-config-store.md), [0018](0018-interactive-session-lifecycle.md) | None | [0005](0005-coherent-playback-boundary.md), [0013](0013-coherent-application-reporting-policy.md) |
| [0020](0020-decoupled-media-interpretation.md) | None | None | [0003](0003-library-mutation-pipeline.md), [0004](0004-scalable-library-tasks.md) |
| [0021](0021-bounded-resource-delivery.md) | None | None | [0003](0003-library-mutation-pipeline.md), [0004](0004-scalable-library-tasks.md), [0012](0012-structured-async-fault-diagnostics.md) |
| [0022](0022-transaction-coherent-library-dictionary.md) | None | None | [0003](0003-library-mutation-pipeline.md), [0009](0009-pure-expression-binding.md) |
| [0023](0023-revision-bound-metadata-authoring.md) | [0022](0022-transaction-coherent-library-dictionary.md) | None | [0003](0003-library-mutation-pipeline.md), [0013](0013-coherent-application-reporting-policy.md), [0016](0016-coherent-workspace-transactions.md) |
| [0024](0024-versioned-predicate-dialect.md) | None | None | [0006](0006-coherent-derived-track-views.md), [0008](0008-declarative-track-capability-bridge.md), [0009](0009-pure-expression-binding.md) |
| [0025](0025-bounded-shell-layout-documents.md) | None | None | [0010](0010-versioned-presentation-state.md), [0015](0015-fail-closed-config-store.md) |
| [0026](0026-lifetime-safe-file-dialog-callbacks.md) | None | None | None |
| [0027](0027-loop-executor.md) | None | None | [0003](0003-library-mutation-pipeline.md), [0011](0011-executor-affine-reporting-feed.md), [0012](0012-structured-async-fault-diagnostics.md) |
| [0028](0028-bounded-audio-observation-delivery.md) | None | None | [0005](0005-coherent-playback-boundary.md), [0011](0011-executor-affine-reporting-feed.md), [0012](0012-structured-async-fault-diagnostics.md) |
| [0029](0029-versioned-cli-automation-protocol.md) | None | None | [0003](0003-library-mutation-pipeline.md), [0013](0013-coherent-application-reporting-policy.md) |

## Proposal inventory

Entries below retain durable proposal history; most are `draft` candidates, while terminal entries state their disposition.
The front-matter lifecycle state is authoritative; `in-review` identifies active adjudication.

- [RFC 0001: Safe library transfer](0001-safe-library-transfer.md) proposes fail-closed import validation, root-contained URIs, and preview-bound destructive authorization.
- [RFC 0002: Durable library storage lifecycle](0002-durable-library-storage.md) proposes explicit durability classes, stepwise migration, checkpoints, recovery, and storage maintenance.
- [RFC 0003: Unified library mutation pipeline](0003-library-mutation-pipeline.md) proposes one runtime owner for write scheduling, commit revision, and semantic change publication.
- [RFC 0004: Scalable library task execution](0004-scalable-library-tasks.md) proposes phased preparation, stable-file revalidation, bounded commits, cancellation, and production scale budgets.
- [RFC 0005: Coherent playback application boundary](0005-coherent-playback-boundary.md) proposes one compositional facade, coherent application transactions, and non-blocking preparation and persistence.
- [RFC 0006: Coherent derived track views](0006-coherent-derived-track-views.md) proposes explicit request outcomes, transactional view state, safe quick-filter construction, and asynchronous revision-checked rebuilds.
- [RFC 0007: Revisioned completion vocabulary](0007-revisioned-completion-vocabulary.md) proposes an immutable revisioned index with correct insertion, mutation, deletion, rebuild, and executor behavior.
- [RFC 0008: Declarative track capability bridge](0008-declarative-track-capability-bridge.md) proposes governed typed descriptors for query variables, runtime fields, completion, quick search, and presentation signals.
- [RFC 0009: Pure expression binding and evaluation context](0009-pure-expression-binding.md) proposes non-mutating compilation, plan-owned symbols, generation-aware dictionary binding, and typed evaluation outcomes.
- [RFC 0010: Versioned presentation state](0010-versioned-presentation-state.md) proposes stable persisted identifiers, frozen legacy migration, transactional validation, and fail-closed recovery.
- [RFC 0011: Executor-affine reporting feed](0011-executor-affine-reporting-feed.md) proposes one revisioned update stream, callback-executor confinement, bounded authoritative retention, and coherent activity projection.
- [RFC 0012: Injected asynchronous exception diagnostics](0012-structured-async-fault-diagnostics.md) is implemented as a narrow `exception_ptr` handler for unobserved coroutine and pre-hop workflow failures; the larger structured-fault model was not adopted.
- [RFC 0013: Coherent application reporting policy](0013-coherent-application-reporting-policy.md) proposes explicit operation dispositions, one semantic reporting owner, typed report intent, and cross-frontend equivalence.
- [RFC 0014: Observable atomic replacement](0014-observable-atomic-replacement.md) is rejected: narrower private-file creation, RAII cleanup, and deterministic failure tests closed the verified gaps without durability receipts or recovery propagation.
- [RFC 0015: Fail-closed grouped configuration transactions](0015-fail-closed-config-store.md) proposes candidate decoding, isolated whole-document transactions, explicit recovery, and receipt-bearing commits.
- [RFC 0016: Coherent workspace transactions](0016-coherent-workspace-transactions.md) proposes validated semantic commands, one revisioned aggregate snapshot, atomic history/view commits, and explicit application-navigation ownership.
- [RFC 0017: Versioned semantic workspace sessions](0017-versioned-workspace-session.md) proposes library-bound versioning, exact session-local active-view identity, bounded strict validation, and transactional restoration.
- [RFC 0018: Frontend-neutral interactive session lifecycle](0018-interactive-session-lifecycle.md) proposes one startup, restore, checkpoint, degraded-operation, and shutdown state machine shared by GTK and TUI.
- [RFC 0019: Transactional active-library switching](0019-transactional-active-library-switch.md) proposes canonical location identity, prepared runtime candidates, rollback-safe activation, and library-bound restorable sessions.
- [RFC 0020: Decoupled media interpretation](0020-decoupled-media-interpretation.md) is rejected: a narrower zero-copy `File`/`Visitor` boundary and private runtime adapter removed the cycle without its owned aggregate or capability registry.
- [RFC 0021: Bounded asynchronous resource delivery](0021-bounded-resource-delivery.md) proposes shared asynchronous byte reads, bounded transforms, typed request lifetimes, and non-blocking GTK, TUI, and MPRIS delivery.
- [RFC 0022: Transaction-coherent library dictionary](0022-transaction-coherent-library-dictionary.md) is implemented with transaction-local interning, atomic committed-index publication, dense dictionary ids, and pure query/format binding.
- [RFC 0023: Revision-bound metadata authoring](0023-revision-bound-metadata-authoring.md) proposes generation- and baseline-bound metadata/tag commands, typed conflicts, atomic multi-target edits, and compare-and-restore undo.
- [RFC 0024: Versioned predicate dialect](0024-versioned-predicate-dialect.md) rejects per-expression language identity in favor of the existing compatibility version or policy owned by each containing format and protocol.
- [RFC 0025: Bounded shell layout documents](0025-bounded-shell-layout-documents.md) proposes strict layout version gates, bounded candidate decoding and template expansion, and preservation on rejection.
- [RFC 0026: Lifetime-safe GTK file-dialog callbacks](0026-lifetime-safe-file-dialog-callbacks.md) is implemented as a coordinator-scoped guard that makes export-mode and native chooser callbacks harmless after teardown and requests native cancellation.
- [RFC 0027: Loop executor for non-toolkit hosts](0027-loop-executor.md) is implemented as a shared-queue owner-thread executor plus a CLI-owned task pump, without a generic host state machine.
- [RFC 0028: Bounded audio observation delivery](0028-bounded-audio-observation-delivery.md) proposes event classification, bounded/coalescing ingress, one-drain callback delivery, and explicit overload behavior.
- [RFC 0029: Versioned CLI automation protocol](0029-versioned-cli-automation-protocol.md) proposes a negotiated structured envelope, stable command/result/error kinds, compatibility rules, and mechanically checked schema coverage.
