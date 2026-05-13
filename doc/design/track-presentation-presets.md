# Track Presentation Presets

This is the entry point for the track presentation preset redesign.

Aobus should expose high-level music views such as **Songs**, **Albums**, **Artists**, **Classical: Composers**, and user-defined **Custom Views** instead of exposing low-level group-by, sort-by, and column controls directly in the main track table.

The core rule is:

> Runtime owns what a presentation means. GTK owns how that presentation is rendered and how row values are loaded.

Runtime may own semantic fields such as `Title`, `Artist`, and `Duration`, but it must not take over GTK row value loading. The current `trackId -> TrackRowCache -> TrackRowObject -> cell factory` path stays in GTK.

## Document Map

Read these in order when implementing the feature:

1. [Architecture](track-presentation/01-architecture.md)
   - problem statement
   - layer responsibilities
   - data-loading boundary
   - built-in preset matrix

2. [Runtime Design](track-presentation/02-runtime-design.md)
   - runtime data model
   - `TrackPresentationField`
   - `TrackPresentationSpec`
   - preset registry
   - `ViewService` API changes
   - projection snapshot changes
   - header skeletons

3. [GTK Integration](track-presentation/03-gtk-integration.md)
   - runtime field to GTK column mapping
   - `TrackViewPage` toolbar changes
   - `TrackColumnController` role after presets
   - GTK header skeletons

4. [Custom Views and Persistence](track-presentation/04-custom-views-and-persistence.md)
   - custom view definition model
   - app config / UI state considerations
   - custom view editor shape
   - migration rules

5. [Implementation Phases](track-presentation/05-implementation-phases.md)
   - staged execution plan
   - per-phase class/file changes
   - acceptance criteria
   - verification commands

6. [Testing Plan](track-presentation/06-testing-plan.md)
   - runtime unit tests
   - GTK mapping tests
   - integration/manual checks
   - regression risks

## Final Target

```text
User chooses a View preset
  -> runtime applies complete presentation spec
  -> projection groups/sorts by spec
  -> GTK maps visible semantic fields to columns
  -> GTK keeps lazy track row loading and cell rendering
```

The final UI should make low-level group-by, sort-by, and column choices available only through custom view creation/editing, not through the main track-list toolbar.
