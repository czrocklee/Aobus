---
id: development.test.uimodel-and-gtk
type: development
status: current
domain: development
summary: Defines UIModel and GTK test placement, fixture, lifecycle, and assertion practices.
---
# UIModel and GTK testing

UI behavior should be tested at the lowest layer that can express the contract.
Most semantic behavior belongs in `uimodel`; GTK tests should stay thin and
prove adapter behavior, lifecycle behavior, or targeted geometry regressions.

The reference implementation of this style is
`test/unit/linux-gtk/playback/TransportButtonTest.cpp`: the logic is proven in
the view model with `RenderLog`, and the widget gets a small smoke test.

## Layering rule

Decide where each assertion belongs before writing it:

```text
Pure runtime / uimodel logic   -> put most semantic assertions here
GTK adapter semantics          -> drive a service or view model, then assert
                                  semantic widget state such as text,
                                  sensitivity, CSS class, model contents, or
                                  draft data
Interactive / dialog behavior  -> show/present only when lifecycle is required,
                                  drain idle/timeouts with drainGtkEvents, and
                                  assert the result
Full-window smoke              -> constructs, binds, shows/hides under Xvfb;
                                  one section max, never chase coverage here
```

Most awkward GTK tests come from bypassing an existing view model and re-proving
its semantics at the widget layer. If a view model already exists for the
component (`AobusSoulViewModel`, `OutputDeviceViewModel`, `TransportViewModel`,
and similar), the semantics belong in the view model test and the widget keeps a
thin adapter or smoke test.

## UIModel tests

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

Keep GTK out of these tests. If a behavior can be expressed as
`input state -> view state`, it belongs here.

Good shape:

```cpp
auto feedProjection = ActivityStatusFeedProjection{};
feedProjection.handleNotificationPosted(feed({warning}), warning.id);

auto const& compact = feedProjection.viewState().compact;
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

A good GTK test usually proves one of these:

- Widget construction and model/runtime binding.
- Runtime/model state rendered into labels, visibility, CSS classes,
  sensitivity, model contents, draft data, or popover state.
- User events routed to the expected action or state mutation.
- Lifecycle cleanup for a known regression.
- A targeted geometry invariant.

Before writing a GTK assertion, ask:

1. Whose code does it test? If the answer is gtkmm (`set_text` followed by
   `get_text`, visible after `show()`, default size greater than zero), delete
   it.
2. Can the same semantics be asserted in a view model? If yes, move it to
   `uimodel` and leave the widget a smoke or adapter test.
3. Is it asserting behavior or implementation shape? Allocation sizes, deep
   `dynamic_cast` chains, and walking controller internals are shape. Keep shape
   assertions only when the structure is the contract, such as a progress dialog
   containing exactly one `Gtk::ProgressBar`.

## Driving GTK behavior

Prefer driving behavior in this order:

1. Real service or fake provider that fires the callback chain.
2. Public widget signal such as `signal_response()` or `signal_clicked()`.
3. `g_signal_emit_by_name(...)` reflection.

Reflection and reaching into controller lists are last resorts. Use them only
when there is no public entry point, and assert the final observable effect, not
merely that the controller exists.

Use GTK test support helpers from `test/unit/linux-gtk/GtkTestSupport.h`:

- `ensureGtkApplication()` before creating GTK widgets.
- `GtkWindowFixture` when a widget must be mounted or presented.
- `GtkRuntimeFixture` or `makeRuntime()` for runtime-backed widgets.
- `drainGtkEvents()` after posting runtime events or emitting GTK signals.
- `emitClicked`, `emitActivate`, `emitClosed`, `emitFocusEnter`,
  `emitFocusLeave`, `emitGesturePressed`, and `emitGestureReleased` for user
  events.
- `collectAll`, `findLabelByText`, `findButtonByLabel`, `hasCssClass`,
  `findWidget`, and `findWidgetByClass` for assertions.

Prefer normal public accessors for important widgets when that observability is
part of the component contract:

```cpp
CHECK(status.label().get_text() == "Partial import");
CHECK(status.dismissButton().get_visible());
```

Use tree traversal and label text matching only when no better seam exists.
Avoid over-coupling tests to incidental widget hierarchy.

For private-access and seam decisions, use
`doc/development/test/fixture-and-helper.md#testability-seams`.

## Show and present

The GTK test suite runs under Xvfb/headless display, so `show()` and `present()`
are allowed when they buy a real behavior assertion. Use them for behavior that
depends on GTK realization, map/show lifecycle, popup/menu/popover population,
focus routing, or a single full-window smoke test.

Do not use `show()` or `present()` just to prove gtkmm behavior such as
`get_visible()`, default size, non-zero allocation, or construction survival. If
a test can assert the same semantic result without realizing a widget, prefer the
direct API path.

## Geometry tests

Geometry and measurement assertions are legitimate when geometry is the
component's own contract: custom flow layout, constrained grids, split/collapsible
panes, image fitting, responsive allocation classes, or declarative size-request
properties.

Mark geometry tests with the `[geometry]` tag or an explicit section name so
future cleanup can distinguish them from incidental gtkmm/default-size
assertions.

Good geometry tests:

- Explain the regression or invariant.
- Assert one or two stable layout outcomes.
- Test project behavior such as clamping, persistence, wrapping, ellipsizing
  room, row/column stability, or a documented size-request property.
- Avoid theme-dependent pixels, default window size, or "greater than zero"
  allocation smoke.
- Do not duplicate pure row/grouping policy that can be tested in `uimodel` or
  helpers.

When a GTK test starts asserting field visibility, grouping, labels, widths, CSS
classes, and click behavior all in one case, split policy into `uimodel` or
helper tests and leave a thin GTK regression.

## What not to test

- gtkmm built-in behavior: setter/getter round-trips, `visible` after `show()`,
  default size greater than zero.
- Constants and magic numbers: min content sizes, fixed pixels, theme colors,
  fonts, allocation sizes.
- Sections that call a method and assert nothing. That is coverage noise, not a
  test.
- Tautologies such as `&someReference != nullptr`.
- Semantics already covered in a view model test.
- Generated GTK resources, `main()` glue, and platform entry points unless a
  specific bug is found there.

## Shared GTK test infrastructure

New GTK tests must reuse `test/unit/linux-gtk/GtkTestSupport.h` before
hand-rolling helpers. It already provides application/window fixtures,
runtime-backed fixtures, event-loop draining, widget search helpers, CSS
assertion helpers, signal emitters, `RenderLog<T>`, `FakePlaybackEvents`, and
`ManualTrackDetailMock`.

When the same new helper appears in two or more test files, promote it into
`GtkTestSupport.h` instead of copying it. Before adding another widget-tree
search, CSS, signal, focus, gesture, runtime, or window helper, check
`GtkTestSupport.h` first. Shared output/backend fakes should follow the shared
fake precedent in `test/unit/audio/BackendTestSupport.h` rather than being
duplicated between uimodel and GTK audio tests.

Keep helpers header-only, narrow, and stateless. Do not grow a test framework.

## Heavy coordinator tests

Heavy coordinator tests that exercise real files, scanning, import/export,
dialogs, permissions, and notification feeds are workflow or integration tests.
Prefer fakeable seams for coordinator control flow, keep only a few end-to-end
smoke/regression cases, and tag heavy cases as `[workflow]`, `[integration]`, or
`[regression]` as appropriate.

## Writing testable GTK code

Testability is a property of the production code, not just the test. These
guidelines depend on keeping logic out of widgets:

- Put decision logic in view models. New behavior such as "a click hides the
  window" should be tested with `RenderLog` or equivalent without GTK; the widget
  keeps a smoke or adapter test.
- Avoid god-object windows that carry behavior. Push session, layout, menu, and
  dialog behavior into collaborators that can be constructed and asserted in
  isolation.
- Keep pure logic free of GTK types. A pure mapping should not force a test to
  build a `Gtk::Window` and an `AppRuntime`.
- Expose narrow semantic accessors instead of forcing tests to crawl widget
  trees. A `entry()` accessor is useful; a query for current completion
  candidates is better.
- Apply the testability seam order from [fixtures and helpers](fixture-and-helper.md) before adding
  any new GTK observation surface.
- Use callbacks or interfaces as seams. Injecting a callback struct, firing it,
  and asserting observable state is the preferred adapter-test shape.

## GTK test checklist

- Semantics that can live in a view model are tested there, not at the widget.
- Every `SECTION` makes at least one assertion about observable behavior.
- No gtkmm round-trips, allocation sizes, or magic-number constants.
- Geometry assertions, if any, are tagged `[geometry]` and test a component
  contract.
- Reflection/gesture-poking, if used, asserts the final effect.
- Reused helpers come from `GtkTestSupport.h`; new shared helpers are promoted
  there.
- Testability seams follow [fixtures and helpers](fixture-and-helper.md).
- Full-window coverage is a single smoke section, not a coverage farm.
