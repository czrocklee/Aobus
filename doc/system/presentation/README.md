---
id: architecture.presentation
---
# Presentation architecture

Presentation is the boundary where runtime facts become reusable view state and
where interactive intent becomes runtime commands. This page explains that
boundary and routes feature-specific contracts; it is not an inventory of
UIModel types, frontend components, or tests.

## Layer model

Interactive presentation follows one dependency direction:

```text
runtime state and commands
          |
          v
platform-neutral UIModel
          |
          v
GTK / WinUI / TUI / AppKit adapters
```

The CLI calls runtime directly for non-interactive tasks. It does not construct
an interactive message catalog or use UIModel as a serialization layer.

Runtime is authoritative for application behavior: canonical identities,
library and workspace state, track-source membership, playback, notifications,
and commands. It exposes typed snapshots and subscriptions without toolkit
vocabulary.

UIModel owns deterministic, platform-neutral presentation policy. It may combine
runtime snapshots, derive semantic view state, format display values, retain an
edit draft or gesture, and emit a runtime command or typed edit result. It does
not own storage transactions, playback succession, audio-device control,
runtime retry policy, or platform lifetime.

Frontend adapters own native resources and interaction mechanics: windows and
widgets, terminal cells, input routing, native icons, platform scheduling,
responsive layout, dialogs, and teardown. Equivalent cross-frontend behavior
uses one runtime or UIModel authority rather than being reimplemented in each
adapter.

## Values crossing the boundary

State flows outward and intent flows inward:

```text
runtime semantic snapshot or event + raw arguments
  -> UIModel projection or formatter
  -> native rendering

native input event
  -> frontend event translation
  -> UIModel interaction policy when needed
  -> runtime command or typed mutation request
```

Runtime and Core carry machine identities, structured absence, typed report and
progress intent, and external data. UIModel contributes semantic presentation
kinds and shared authored wording. Frontends map those kinds to native
representations.

Presentation never changes track-source membership. A list id and filter select
a source; sorting, grouping, visible fields, and redundant-field suppression
shape its presentation. Persisted ids and query tokens remain identities rather
than localized display text.

Interactive composition roots inject immutable message catalogs. Shared
domain-to-message choices belong in UIModel; native string conversion and
frontend-local binding stay in adapters. Locale-aware text ordering is a
separate leaf capability: display text, locale-independent identity, and
transient locale-dependent sort keys remain separate values. New projections
capture the current ordering policy. Replacing that policy updates the runtime's
existing workspace-view projections, while an already detached playback
projection retains its captured policy and order until released. See
[localization](localization.md) and [text semantics](text-catalog.md).

## Ownership and lifetime rules

- Runtime has no dependency on UIModel or frontend code.
- UIModel depends on runtime interfaces and stable Core values, never on a
  platform UI library.
- UIModel values contain semantic information, not toolkit handles, CSS classes,
  native icon names, or terminal geometry.
- Frontends retain subscriptions and view models for no longer than the runtime
  services they observe, and tear down callbacks and native resources before
  their owners disappear.
- Runtime snapshots remain authoritative after a widget tree or terminal frame
  is rebuilt.
- UI-local persisted preferences may affect presentation but do not replace
  canonical runtime state. Their schemas use explicit versions and stable tokens,
  not C++ enum ordinals.
- Display text is never parsed for control flow or persisted as identity.
- A submitted asynchronous UIModel operation may retain its shared operation
  state after a value facade is moved or destroyed; composition must keep any
  borrowed runtime services alive until settlement. Frontend teardown still
  cancels or drains its workflow.

Runtime failures cross the boundary as typed results, snapshots, or
observational events. UIModel may choose semantic display state but does not
invent runtime recovery. Frontends choose placement and own cancellation tied
to their native lifetime.

## Topic contracts

Choose the contract for the behavior being changed:

- [Activity status and local dismissal](activity-status.md)
- [Selection count and duration](selection-summary.md)
- [Track filtering and completion](track-filter.md)
- [List navigation](list-tree.md)
- [Grouping and sorting](track-presentation.md)
- [Saved-List order authoring](list-order-authoring.md)
- [Per-List preferences](list-preference.md)
- [Track column sizing](track-column-layout.md)
- [Metadata and tag editing](metadata-editing.md)
- [Field-value completion](field-completion.md)
- [Volume and mute interaction](volume-control.md)
- [Finished library-scan reporting](library-scan-report.md)
- [Locale and catalog lifetime](localization.md)
- [Semantic text and terminology](text-catalog.md)

Neighboring owners are [workspace](../workspace/README.md),
[interactive session lifecycle](../session-lifecycle.md),
[application shell](../shell/README.md), [library](../library/structure.md), and
[track expressions](../query/README.md).

## Source orientation

Platform-neutral public interfaces are under
[`app/include/ao/uimodel/`](../../../app/include/ao/uimodel), with
implementations under [`app/uimodel/`](../../../app/uimodel). Runtime interfaces
are under [`app/include/ao/rt/`](../../../app/include/ao/rt). GTK, TUI, WinUI,
and AppKit adapters live in their respective application directories.
[`app/uimodel/CMakeLists.txt`](../../../app/uimodel/CMakeLists.txt) and
[`ArchitectureAudit.cmake`](../../../app/cmake/ArchitectureAudit.cmake) enforce
important dependency and vocabulary boundaries.
