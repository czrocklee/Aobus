---
id: development.test.layer-selection
---
# Test layer selection

Choose the lowest layer that proves the behavior. Higher layers should not duplicate lower-layer policy unless the behavior is specifically about binding, rendering, lifecycle, or integration.

## `lib` tests

Use `test/unit/<module>/...Test.cpp` for pure algorithms, data layouts, storage, parsing, serialization, query evaluation, audio primitives, and utility behavior.

Good `lib` tests usually:

- Use small explicit inputs and explicit expected outputs.
- Cover success, invalid input, malformed buffers, empty input, ordering, duplicates, missing records, and boundary values.
- Prefer builders or fixtures for normal behavior tests.
- Use raw bytes/layout structs only when the behavior is specifically about storage layout or malformed binary data.

Appropriate contracts:

- Serializer output and parse/serialize round trips.
- TrackStore create/read/update/delete postconditions.
- Query evaluator semantics, including optimizer regressions such as bloom-filter pruning.
- Atomic file write and failure behavior.

## `runtime` tests

Use `test/unit/runtime/...Test.cpp` for service contracts, runtime state transitions, event publication, subscriptions, projections, workspace/view/playback behavior, and async lifecycle.

Good `runtime` tests usually:

- Assert emitted callbacks and resulting service state.
- Keep service tests small and direct.
- Use deterministic executors, barriers, explicit callbacks, and fake services instead of wall-clock timing.
- Mark complete library import/export/scan flows `[integration]`; focused planning, validation, and cancellation contracts stay `[unit]`.

Appropriate contracts:

- NotificationService posts, keyed updates, bounds, expiry, and immutable update delivery.
- LifetimeScope cancellation prevents queued callback work.
- Workspace navigation changes active view state.
- Library export/import preserves user data across a round trip.

## `uimodel` tests

Use `test/unit/uimodel/...Test.cpp` for UI policy, view-state projection, menu models, editor models, layout models, selection summaries, field formatting, and presentation decisions that do not need GTK.

This is the preferred layer for most UI behavior.

Good `uimodel` tests usually:

- Feed model-like state in and assert view state out.
- Cover priority, fallback, grouping, local hiding, validation, and no-op cases.
- Verify signal emission counts and payloads when signals are part of the contract.
- Keep GTK out entirely.

Appropriate contracts:

- ActivityStatusFeedProjection compact/detail state priority.
- TrackPresentationCatalog menu ordering and preference fallback.
- LayoutTemplateExpander prop merge and recursive template protection.
- KeymapModel conflict detection and override behavior.

## Application-platform adapter tests

Use `[platform]` for frontend-independent native adapters under `app/platform`, such as the shared Linux MPRIS adapter.
Controlled command, metadata, and lifecycle rules can remain `[unit]`; results obtained through a real bus or another production boundary are `[integration]`.
Tests of a frontend's composition with that adapter retain the frontend layer, such as `[tui]` or `[gtk]`.
Native availability belongs to the conditional source registration; it does not require a platform-name tag or determine unit/integration scope.

## `linux-gtk` tests

Use `test/unit/linux-gtk/...Test.cpp` for GTK adapter behavior: widget construction, render binding, event-to-action routing, lifecycle cleanup, CSS class application, popovers/dialogs, and small targeted layout regressions.

GTK tests should be thin. Do not re-test all business policy that can be covered in `uimodel` or `runtime`.

GTK scope follows the [direction of the asserted value](#scope-and-independent-metadata).

Good GTK tests usually:

- Build the widget with a fixture.
- Drive a user-facing event such as click, activate, focus, gesture, or runtime signal.
- Assert a small number of stable widget outcomes.
- Use normal public accessors or stable semantic CSS classes when possible.
- Reserve geometry/measurement assertions for real regressions and document why they exist.

Appropriate contracts:

- A button binds to a view model and updates runtime state when clicked.
- A status widget renders warning/error CSS classes from model state.
- A detail popover closes when compact status becomes hidden.
- A layout regression keeps columns stable after section collapse.

## CLI and TUI frontend tests

Use `test/unit/cli/...Test.cpp` and `test/unit/tui/...Test.cpp` for frontend-owned parsing, formatting, rendering, input routing, and lifecycle behavior. Keep shared service and presentation policy in `runtime` or `uimodel`. Scope follows the [direction of the asserted value](#scope-and-independent-metadata): a CLI command or TUI action that changes library, playback, workspace, or notification state and asserts that state is `[integration]`; read-only output and TUI rendering of runtime state stay `[unit]`.

## WinUI frontend tests

Use `test/unit/winui/...Test.cpp` for native WinUI composition and frontend-owned behavior such as XAML resource lookup, Windows layout dialects, shell policy, startup options, and process-boundary adapters.

Keep pure shared policy in `uimodel` tests. A WinUI test may use the `[winui]` layer tag when the behavior belongs to the Windows frontend even if the implementation is a small pure helper. Many WinRT-free WinUI policy tests are intentionally compiled into `ao_core_test` on every host; suite membership does not change their frontend layer.

## Scope and independent metadata

Choose scope from the contract under test, not from the directory or binary:

- `[unit]` verifies a focused component contract with controlled collaborators,
  such as TrackStore read-back, YAML schema rejection, or fake-backend state
  transitions. An owned temporary database, file, or GTK fixture does not make
  it an integration test.
- `[integration]` verifies collaboration across production boundaries, such as
  export-to-import round trips, CLI-to-runtime mutations, decoder pipelines,
  subprocess protocols, or a real daemon. The asserted outcome decides: a case
  is integration when the value it checks is produced across such a boundary,
  such as stream facts from the real decoder. A real media file that only
  serves as playable input to a transport, session, or token contract with a
  fake output device stays unit.

UI-model and frontend components apply the asserted outcome by direction. A
case that drives the real runtime through a view model, session, or widget and
asserts runtime state, or state the runtime persisted, is `[integration]`.
Runtime state projected into view state or rendered into widgets, and
frontend-owned configuration written by the component itself, stay `[unit]`
even with an owned runtime fixture.

Scope promises neither speed nor hermeticity, and it is unrelated to the
`ao_integration_test` binary.

Useful existing samples:

- Runtime service contract style: `test/unit/runtime/NotificationServiceTest.cpp`.
- UI policy style: `test/unit/uimodel/status/activity/ActivityStatusFeedProjection*Test.cpp` and `test/unit/uimodel/library/presentation/TrackPresentationCatalogTest.cpp`.
- Pure helper style: `test/unit/linux-gtk/layout/components/TrackFieldGridTextTest.cpp`.
- Thin GTK adapter style: `test/unit/linux-gtk/track/TrackPresentationButtonTest.cpp`.
