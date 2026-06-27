# Layer selection reference

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
- Treat full library import/export/scan flows as workflow or integration-style tests, even when they live in the core test target.

Appropriate contracts:

- NotificationService posts, updates, dismisses, and feed revisions.
- LifetimeScope cancellation prevents queued callback work.
- Workspace navigation changes active view state.
- Library export/import preserves user data across a round trip.

## `uimodel` tests

Use `test/unit/uimodel/...Test.cpp` for UI policy, view-state projection, menu models, editor models, layout models, selection summaries, field formatting, and presentation decisions that do not need GTK.

This is the preferred layer for most UI behavior.

Good `uimodel` tests usually:

- Feed model-like state in and assert view state out.
- Cover priority, fallback, grouping, dismissal, validation, and no-op cases.
- Verify signal emission counts and payloads when signals are part of the contract.
- Keep GTK out entirely.

Appropriate contracts:

- ActivityStatusModel compact/detail state priority.
- TrackPresentationViewModel menu ordering and fallback.
- LayoutTemplateExpander prop merge and recursive template protection.
- KeymapModel conflict detection and override behavior.

## `linux-gtk` tests

Use `test/unit/linux-gtk/...Test.cpp` for GTK adapter behavior: widget construction, render binding, event-to-action routing, lifecycle cleanup, CSS class application, popovers/dialogs, and small targeted layout regressions.

GTK tests should be thin. Do not re-test all business policy that can be covered in `uimodel` or `runtime`.

Good GTK tests usually:

- Build the widget with a fixture.
- Drive a user-facing event such as click, activate, focus, gesture, or runtime signal.
- Assert a small number of stable widget outcomes.
- Use explicit `...ForTest()` accessors or stable semantic CSS classes when possible.
- Reserve geometry/measurement assertions for real regressions and document why they exist.

Appropriate contracts:

- A button binds to a view model and updates runtime state when clicked.
- A status widget renders warning/error CSS classes from model state.
- A detail popover closes when compact status becomes hidden.
- A layout regression keeps columns stable after section collapse.

## Workflow, integration, and regression placement

Use `[workflow]` when the test exercises multiple production components but still runs in a unit target.

Use `[integration]` when it relies on real files, real codecs, real GTK event loop behavior, environment permissions, or cross-component wiring.

Use `[regression]` when the test protects a known bug or fragile invariant. Add a short comment when the assertion is non-obvious.

Useful existing samples:

- Runtime service contract style: `test/unit/runtime/NotificationServiceTest.cpp`.
- UI policy style: `test/unit/uimodel/status/ActivityStatusModel*Test.cpp` and `test/unit/uimodel/track/TrackPresentationViewModelTest.cpp`.
- Pure helper style: `test/unit/linux-gtk/layout/components/TrackFieldGridTextUtilsTest.cpp`.
- Thin GTK adapter style: `test/unit/linux-gtk/track/TrackPresentationButtonTest.cpp`.
