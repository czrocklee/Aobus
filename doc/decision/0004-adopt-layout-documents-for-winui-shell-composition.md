---
id: decision.0004.winui-layout-documents
type: decision
status: accepted
domain: application-shell
summary: Adopts the shared layout-document language for WinUI shell composition while retaining Windows-owned presets, construction, policy, resources, and state.
---
# Decision 0004: adopt layout documents for WinUI shell composition

## Context

The native Windows frontend defines complete Modern and Classic shell subtrees
in XAML, keeps both alive, and mirrors their named elements through controller
code. Structural variation therefore duplicates native elements and binding
paths even though both shells present the same long-lived library, playback,
workspace, and settings owners.

GTK already composes its shell from the shared version 1 `LayoutDocument`
language, but its presets, component factories, CSS, responsive rules, and
widget lifetimes are GTK-specific. Treating that implementation as the portable
runtime would couple WinUI to policies that do not match its XAML resources,
`ListView` interaction model, width policy, or desktop settings.

The 2026-08-02 maintainer design review directly adjudicated this boundary for
one implementation PR. The implementation review that introduces this record
is the contemporaneous evidence; no RFC is retained for this decision.

## Decision

WinUI adopts the shared version 1 layout-document language, parsing, template
expansion, stable ids, catalog validation, and native-independent preparation.
Windows owns separate built-in Modern and Classic presets, its accepted
component catalog, native construction and placement, controller binding,
responsive policy, and generation lifetime. GTK keeps its own presets and
runtime. There is no shared native build plan or responsive classifier.

The WinUI window retains a compiled XAML frame, one layout host,
`RootGrid.Resources`, styles, `DataTemplate` resources, and `ListView`. Shell
composition moves into the two compiled Windows layout documents. Windows may
validate and interpret a `styleKey` extension; GTK's `cssClasses` remains
GTK-only. Native resource values are defaults, with explicit document placement
and controller-owned semantic state taking local-value precedence.

Exactly one Windows shell generation is live. Switching Modern or Classic
constructs and validates a complete inactive candidate, publishes it as one
generation, then retires and destroys the old native tree, controllers,
subscriptions, and view-local asynchronous work. Long-lived session and
semantic view state is reconciled into the candidate; focus, hover, scroll,
realized containers, and other view-only transient state may reset. Generation
tokens suppress callbacks from retired views.

`winui::ShellStatePolicy` and `winui::DesktopSettings` remain authoritative for
Windows responsive shell state and persistent navigation and inspector state.
Windows-specific navigation and inspector components receive that authority
through build-context accessors; they do not map it into the generic component
state store.

The first adoption supports only the two built-in Windows presets and the
components they use. It does not introduce a Windows layout editor,
user-authored presets, GTK catalog parity, runtime XAML parsing, or a new native
widget-test framework.

## Alternatives considered

### Keep two static XAML shell trees

Rejected because every structural shell difference continues to require a
second native subtree and mirrored controller wiring, and both generations
remain live even though only one is visible.

### Port the GTK runtime and presets to Windows

Rejected because the portable asset is the document language, not GTK widget
construction. GTK CSS, authored responsive axes, widget placement, and
lifetime rules do not describe WinUI resources, controls, or desktop behavior.

### Share a frontend-neutral build plan and responsive classifier

Rejected because it would either encode native placement decisions in the
shared layer or erase real differences: Windows uses fixed width-policy
boundaries and a compound shell state, while GTK presets author different
breakpoints and axes. Parsing and validation can remain neutral without making
build and responsive policy neutral.

### Route Windows pane persistence through the generic component state store

Rejected because `winui::DesktopSettings` already atomically owns pane values
with window, shell, and track-view state. An adapter would give per-preset
component state a false claim of authority and cannot express participation in
that shared settings candidate.

### Replace compiled templates and ListView with runtime XAML or ItemsRepeater

Rejected because dynamic composition does not require replacing native resource
lookup or the established selection, keyboard, recycling, and item-container
model. That migration would add a second independent behavior change.

## Consequences

- Modern and Classic can vary structurally without two permanent native trees
  or mirrored named-element controller branches.
- Sharing the language does not promise identical presets, component catalogs,
  appearance, or responsive behavior across frontends.
- A shell switch reconstructs the native tree, so semantic state needs explicit
  ownership and reconciliation while view-only state may reset.
- Candidate construction and validation must fail as a unit. A failed switch
  keeps the current generation; a bad built-in startup document produces a
  minimal fatal layout error rather than a hidden static-shell fallback.
- Windows retains native styles, templates, theme re-resolution, and `ListView`
  behavior, reducing the migration surface but requiring Windows-specific
  builders and placement tests.
- Built-in presets and catalog contracts become shipped resources that require
  permanent parse, expansion, and validation coverage.
- Reversing this choice requires restoring a statically composed shell or
  replacing the layout language; it is not a per-user compatibility mode.

## Current authorities

- [System architecture](../architecture/system-overview.md)
- [Application shell architecture](../architecture/application-shell.md)
- [Windows desktop shell specification](../spec/shell/windows-desktop.md)
- [Layout lifecycle specification](../spec/shell/layout-lifecycle.md)
- [Layout document reference](../reference/shell/layout-document.md)
- [Windows desktop state reference](../reference/windows/desktop-state.md)

## Supersession

Not superseded.
