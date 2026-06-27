# UI model and GTK testing reference

## UI model tests

UI model tests should carry most UI behavior.

Cover:

- Initial/default state.
- Priority rules.
- Fallbacks for unknown IDs or invalid input.
- Grouping and ordering.
- Dismissal or local hiding.
- Transient vs persistent state.
- Signal emission and non-emission.
- Bulk setters and idempotence.

Keep GTK out of these tests. If a behavior can be expressed as `input state -> view state`, it belongs here.

Good shape:

```cpp
auto model = ActivityStatusModel{};
model.onNotificationPosted(feed({warning}), warning.id);

auto const& compact = model.viewState().compact;
CHECK(compact.kind == ActivityStatusKind::Warning);
CHECK(compact.text == "Partial import");
```

Signal tests should check emission and, when available, payload:

```cpp
std::vector<ChangeEvent> events;
auto sub = model.signalChanged().connect([&](auto event) { events.push_back(event); });

model.setActivePresentationId("albums");
model.setActivePresentationId("albums");

REQUIRE(events.size() == 1);
CHECK(model.activePresentationId() == "albums");
```

## GTK tests

GTK tests should verify adapter behavior, not duplicate all policy.

Use GTK test support helpers:

- `ensureGtkApplication()` before creating GTK widgets.
- `GtkRuntimeFixture` or `makeRuntime()` for runtime-backed widgets.
- `drainGtkEvents()` after posting runtime events or emitting GTK signals.
- `emitClicked`, `emitActivate`, focus/gesture helpers for user events.
- `hasCssClass`, `findWidget`, and stable `...ForTest()` accessors for assertions.

Prefer explicit accessors for important widgets:

```cpp
CHECK(status.labelForTest().get_text() == "Partial import");
CHECK(status.dismissButtonForTest().get_visible());
```

Use tree traversal and label text matching only when no better seam exists. Avoid over-coupling tests to incidental widget hierarchy.

A good GTK test usually proves one of these:

- Widget constructs and binds to its model/runtime.
- Runtime/model state renders into labels, visibility, CSS classes, sensitivity, or popover state.
- User event routes to the expected action or state mutation.
- Lifecycle cleanup avoids a known regression.
- A targeted geometry invariant remains stable.

Heavy coordinator tests that exercise real files, scanning, import/export, dialogs, permissions, and notification feeds are workflow or integration tests. Prefer fakeable seams for coordinator control flow, keep only a few end-to-end smoke/regression cases, and tag the heavy cases as `[workflow]`, `[integration]`, or `[regression]` as appropriate.

## GTK geometry tests

Geometry and measurement assertions are legitimate for real layout regressions, but keep them narrow.

Good geometry tests:

- Explain the regression or invariant.
- Assert one or two stable layout outcomes.
- Avoid validating every incidental widget in the tree.
- Do not duplicate pure row/grouping policy that can be tested in `uimodel` or helpers.

When a GTK test starts asserting field visibility, grouping, labels, widths, CSS classes, and click behavior all in one case, consider splitting policy into `uimodel` or helper tests and leaving a thin GTK regression.
