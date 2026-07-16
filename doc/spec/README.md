---
id: spec.index
type: index
status: current
domain: documentation
summary: Routes normative behavioral contracts grouped by Aobus subsystem.
---
# Specifications

Specifications define current behavior that implementations and tests must preserve.
They own invariants, state models, commands, transitions, ordering, failure, cancellation, persistence semantics, and frontend-neutral observations.

Group specifications by subsystem, including library, playback, query, presentation, and frontend behavior.
Keep exact field, grammar, command, and binary-layout inventories in reference even when a specification links to them.

Use the [specification template](../template/spec.md).

## Failure and reporting

- [Outcome channels](failure/outcome-channel.md) define normal values, recoverable results, asynchronous observations, cancellation, and invariant faults.
- [Notification feed](reporting/notification-feed.md) defines runtime feed mutation, revision, identity, and observation.

## Storage

- [LMDB operations](storage/lmdb-operation.md) define core environment, transaction, database, cursor, read, write, commit, abort, and operational failure behavior.

## Media

- [Media file reading](media/file-reading.md) defines supported-file recognition, mapping, visitor delivery, payload extraction, parser containment, and mapped-view lifetimes.

## Resource

- [Cover-art resource delivery](resource/cover-art-delivery.md) defines immutable blob creation, primary cover selection, runtime materialization, GTK and TUI transforms, MPRIS export, caching, and stale-result behavior.

## Persistence

- [Reusable YAML adapter](persistence/yaml-adapter.md) defines RapidYAML callback containment, parsing, arena lifetime, file reading, node helpers, and scalar conversion.
- [Grouped configuration store](persistence/config-store.md) defines lazy whole-file loading, candidate decoding, atomic group saves and removals, failure, and concurrency behavior.
- [Atomic file replacement](persistence/atomic-replacement.md) defines complete temporary writes, data barriers, replacement, permissions, cleanup, and platform failure behavior.

## Library

- [Library specifications](library/README.md) route reads, mutations, changes, scans, sources, projections, and transfers.

## Playback

- [Audio quality analysis](playback/quality-analysis.md) defines graph evidence, fidelity axes, conversion proof, runtime publication, and verdict precedence.
- [Playback succession cursor](playback/cursor.md) defines live-projection launch, anchors, navigation, repeat, shuffle, prepared-next, and failure walking.
- [Decoder session](playback/decoder-session.md) defines decoder lifecycle, format negotiation, PCM representations, and failures.
- [Audio execution and concurrency](playback/audio-execution.md) defines control serialization, event delivery, realtime rendering, gapless transitions, generation fences, backend lifetime, and shutdown.
- [Playback session persistence](playback/session-persistence.md) defines strict restore, normalization, deferred transport, dirty revisions, saving, retry, discard, and shutdown.

## Query

- [Predicate evaluation](query/predicate-evaluation.md) defines boolean compilation, field truth, access profiles, and failures.
- [Query expression completion](query/expression-completion.md) defines tolerant cursor contexts, suggestions, ranking, and replacement.
- [Format expression evaluation](query/format-evaluation.md) defines per-track scalar string compilation and output.

## Presentation

- [Activity status](presentation/activity-status.md) defines notification and library-task projection, priority, detail eligibility, timeout, and local suppression.
- [Track filtering](presentation/track-filter.md) defines quick-search resolution, runtime view filtering, status, and Smart List derivation.
- [Track-list presentation](presentation/track-presentation.md) defines grouping, sorting, classical order, and recommendation.
- [List presentation preference](presentation/list-preference.md) defines per-list selection, fallback, persistence ownership, and filter independence.
- [Track-column layout](presentation/track-column-layout.md) defines shared fixed/flexible sizing, weighted solving, resizing, and frontend adaptation.
- [Selection summary](presentation/selection-summary.md) defines selected-track count, aggregate duration, and shared display text.
- [Volume control](presentation/volume-control.md) defines shared volume projection, scroll/mute policy, icon mapping, and GTK precision interaction.
- [Track-field value completion](presentation/field-completion.md) defines live metadata vocabularies and editor replacement behavior.
- [Metadata editing](presentation/metadata-editing.md) defines detail aggregation, field policy, custom metadata, tag mutation, validation, and undo eligibility.

## Workspace

- [Workspace navigation](workspace/navigation.md) defines semantic targets, open/focused views, history commit, replay, closure, and observations.
- [Workspace session](workspace/session.md) defines snapshot capture, checkpointing, candidate restoration, fallback, and initial history.

## Linux GTK

- [GTK active-library lifecycle](linux-gtk/active-library-lifecycle.md) defines startup selection, runtime construction, same-root reuse, replacement, checkpointing, and shutdown.
- [GTK dialog lifecycle](linux-gtk/dialog-lifecycle.md) defines custom-dialog roles, preferences, native chooser handoff, and layout-editor actions.
- [GTK MPRIS](linux-gtk/mpris.md) defines protocol ownership, command routing, observation, name lifecycle, and degradation.
- [GTK track detail](linux-gtk/track-detail.md) defines field-grid, inline editing, custom-metadata undo, tag-chip flow, and constrained layout.

## CLI

- [CLI execution](cli/execution.md) defines composition, selection, output, streams, dry-run, failure, and exit behavior.

## TUI

- [TUI interaction](tui/interaction.md) defines modal shell state, panels, input, playback dock, seek, completion, notification, and rendering behavior.

## Application shell

- [Shell layout lifecycle](shell/layout-lifecycle.md) defines preset selection, loading, template expansion, GTK construction, editor rebuilds, component state, and teardown.
- [Keyboard shortcuts](shell/keyboard-shortcut.md) define neutral merge, conflicts, live editing, persistence, and GTK accelerator application.
- [Shell layout adaptation](shell/layout-adaptation.md) defines allocation-driven responsiveness, image targets, responsive classes, and collapsible splits.
