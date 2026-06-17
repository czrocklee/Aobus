# GTK Testing Guidelines

How to write GTK (`app/linux-gtk/**`) unit tests that prove behavior without
becoming fragile widget-tree, pixel, or "does not crash" tests.

This complements two existing documents:

- `doc/plan/linux-gtk-gui-test-coverage-plan.md` — the coverage layering model
  and non-goals this doc operationalizes.
- `doc/design/developer-test-suites.md` — how the `gtk` suite is wired into
  `./ao test` and coverage.

The reference implementation of the style below is
`test/unit/linux-gtk/playback/TransportButtonTest.cpp`: the logic is proven in
the view model with `RenderLog`, and the widget gets a single smoke test.

## The Layering Rule

Decide where each assertion belongs before writing it:

```text
Pure runtime / uimodel logic   ← put the vast majority of semantic assertions here
                                 (no GTK, fastest and most stable)
GTK adapter semantics          ← drive a service / view model, assert SEMANTIC state:
                                 text, sensitivity, CSS class, model contents, draft data
Interactive / dialog behavior  ← show/present only when lifecycle is required, drain
                                 idle/timeouts with drainGtkEvents, assert the result
Full-window smoke              ← constructs, binds, shows/hides under Xvfb;
                                 one section max, never chase coverage here
```

Most "awkward" GTK tests come from bypassing an existing view model and
re-proving its semantics at the widget layer. If a view model already exists for
the component (`AobusSoulViewModel`, `AudioOutputViewModel`, `TransportViewModel`,
…), the semantics belong in the view model test and the widget keeps a thin smoke.

## Three Questions Before Writing a GTK Assertion

1. **Whose code does it test?** If the answer is "gtkmm" — `set_text` followed by
   `get_text`, `get_visible` being true after `show()`, default size `> 0` — delete it.
2. **Can the same semantics be asserted in a view model?** If yes, move it to the
   uimodel test and leave the widget a smoke test.
3. **Is it asserting behavior or implementation shape?** Allocation sizes
   (`min_content_width == 360`), deep `dynamic_cast` chains, and walking a
   widget's internal controller list are shape. Keep shape assertions only when
   the structure *is* the contract (e.g. a progress dialog must contain exactly
   one `Gtk::ProgressBar`).

### Driving order of preference

1. Real service / fake provider that fires the callback chain.
2. Public widget signal (`signal_response()`, `signal_clicked()`, …).
3. `g_signal_emit_by_name(...)` reflection.

Reflection (and reaching into `observe_controllers()` to find a gesture) is a
last resort, allowed only when there is no public entry point **and** the test
asserts the final observable effect (e.g. the window is hidden), not merely that
the controller exists.

## Show/Present Lifecycle Rule

The GTK test suite runs under Xvfb/headless display, so `show()` and `present()`
are allowed when they buy a real behavior assertion. Use them for behavior that
depends on GTK realization, map/show lifecycle, popup/menu/popover population,
focus routing, or a single full-window smoke test.

Do not use `show()` or `present()` just to prove gtkmm behavior such as
`get_visible()`, default size, non-zero allocation, or construction survival.
If a test can assert the same semantic result without realizing a widget, prefer
the direct API path.

## What Not to Test

- gtkmm built-in behavior: setter/getter round-trips, `visible` after `show()`,
  default size `> 0`.
- Constants and magic numbers: min content sizes, fixed pixels, theme colors,
  fonts, allocation sizes. (Plan non-goal.)
- Sections that call a method and assert nothing ("runs without crashing"),
  especially on god-object windows. That is coverage noise, not a test.
- Tautologies such as `&someReference != nullptr`.
- Semantics already covered in a view model test — do not re-prove them at the
  widget layer.
- Generated GTK resources, `main()` glue, platform entry points — unless a
  specific bug is found there.

## Geometry Contract Tests

Geometry assertions are allowed when geometry is the component's own contract:
custom flow layout, constrained grids, split/collapsible panes, image fitting,
responsive allocation classes, or declarative size-request properties. Mark
these tests with the `[geometry]` tag or an explicit section name so future
cleanup can distinguish them from incidental gtkmm/default-size assertions.

Even in geometry tests, assert the project behavior: clamping, persistence,
wrapping, ellipsizing room, row/column stability, or a documented size-request
property. Do not assert theme-dependent pixels, default window size, or
"greater than zero" allocation smoke.

## Shared Test Infrastructure

New GTK tests must reuse `test/unit/linux-gtk/GtkTestSupport.h` before
hand-rolling helpers. It already provides `GtkRuntimeFixture`, `makeRuntime`,
`ensureGtkApplication`, `drainGtkEvents`, `collectScrolledWindows`,
`RenderLog<T>`, `FakePlaybackEvents`, and `ManualTrackDetailMock`.

When the same helper appears in two or more test files, promote it into
`GtkTestSupport.h` instead of copying it. Current promotion candidates:

- `collectAll<T>(root)` / `findLabelByText` / `findButtonByLabel` — generic
  widget-tree search (the typed generalization of `collectScrolledWindows`).
- `hasCssClass(widget, name)` — replaces the repeated
  `for (auto const& c : w.get_css_classes())` loop.
- `emitClicked(button)` / `emitReleased(gesture)` — one place to own the GTK
  C-API reflection.
- A shared `FakeOutputProvider` / `StubBackend` — currently duplicated between
  the uimodel and GTK audio tests (see `test/unit/audio/TestUtility.h` for the
  shared-fake precedent).

Keep helpers header-only, narrow, and stateless. Do not grow a test framework.

## Writing Code That Is Easier to Test

Testability is a property of the production code, not just the test. The
guidelines above only hold if the source keeps logic out of widgets:

- **Logic in the view model, the widget only renders.** New decision logic (e.g.
  "a click hides the window") goes into the view model so it can be tested with
  `RenderLog` and no GTK; the widget keeps a smoke test.
- **Avoid god-object windows that carry behavior.** When a window owns session,
  layout, and menu wiring, the only honest test left is "does not crash." Push
  testable behavior down into collaborators (coordinators, controllers) that can
  be constructed and asserted in isolation; the window becomes wiring.
- **Pure logic must not depend on GTK types.** A pure mapping (e.g. copying
  service pointers into sub-contexts) should not force a test to build a
  `Gtk::Window` and an `AppRuntime`. Prefer a free function or a struct free of
  GTK references so the test approaches a pure-function test.
- **Expose narrow semantic accessors instead of forcing tests to crawl the
  widget tree.** A `entry()` accessor is good; a "current completion candidates"
  query is better — let the test assert intent, not structure.
- **Use callbacks / interfaces as seams.** Injecting a callback struct, firing
  it, and asserting observable state (as with `ImportExportCallbacks`) is the
  preferred shape for adapter tests.

## Checklist for a New GTK Test

- [ ] Semantics that can live in a view model are tested there, not at the widget.
- [ ] Every `SECTION` makes at least one assertion about observable behavior.
- [ ] No gtkmm round-trips, allocation sizes, or magic-number constants.
- [ ] Geometry assertions, if any, are tagged `[geometry]` and test a component contract.
- [ ] Reflection/gesture-poking, if used, asserts the final effect.
- [ ] Reused helpers come from `GtkTestSupport.h`; new shared ones are promoted there.
- [ ] Full-window coverage is a single smoke section, not a coverage farm.
