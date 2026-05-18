# Linux GTK Default Bar Template Unification Plan

## Purpose

Move the default Linux GTK playback and status bars to the same layout-template model. The default shell layout should reference named templates for both bars, while the template definitions should contain the actual child-component composition.

This keeps the default layout concise, makes the bars easier to customize from saved YAML layouts, and avoids encoding reusable bar composition in C++ composite components.

## Current Verified State

- `createDefaultLayout()` builds the playback row inline as a `box` with playback child components.
- `getBuiltInTemplates()` already defines `playback.defaultBar`, but the default layout does not use it.
- `getBuiltInTemplates()` defines `status.defaultBar` as a one-node alias to the `status.defaultBar` component.
- `status.defaultBar` is still registered as a C++ composite component that manually creates:
  - `status.playbackDetails`
  - elastic spacer
  - `status.nowPlaying`
  - elastic spacer
  - `status.importProgress`
  - `status.notification`
  - separator
  - `status.trackCount`
- `spacer` already exists as a non-visual `Gtk::Box` layout primitive in `Containers.cpp` and is registered as type `spacer`.
- There is no generic `separator` layout component yet, so the status bar cannot be represented entirely as a template until that primitive is added.

## Target State

### Default layout

`createDefaultLayout()` should use template nodes for both bars while keeping shell-region ownership unchanged:

- Playback region: `templateId = "playback.defaultBar"`
- Status region: `templateId = "status.defaultBar"`

The outer shell-only styling stays in the default layout where it describes the application chrome:

- Playback row keeps `id = "playback-row"` and `ao-playback-strip` through the template node's `id` and `layout` override.
- Status region keeps `ao-status-region` as the outer wrapper.

The intended default-layout shape is:

```text
box vertical (app-root)
  app.menuBar
  template id="playback-row" templateId="playback.defaultBar" layout.cssClasses=["ao-playback-strip"]
  split id="main-paned"
  box horizontal layout.cssClasses=["ao-status-region"]
    template templateId="status.defaultBar" layout.hexpand=true
```

This means `ao-status-region` remains on the outer shell wrapper. The status template expands only to the inner status bar container and its children.

### Built-in templates

`playback.defaultBar` should be the canonical playback-bar composition template. It should contain the current playback children:

1. `playback.outputButton`
2. `playback.playPauseButton`
3. `playback.stopButton`
4. `playback.seekSlider` with `hexpand = true`
5. `playback.timeLabel`
6. `playback.volumeControl`

`status.defaultBar` should become the canonical status-bar composition template. It should be a horizontal `box` with `cssClasses = ["ao-status-bar"]`, `hexpand = true`, and the current status children:

1. `status.playbackDetails`
2. `spacer` with `hexpand = true`
3. `status.nowPlaying`
4. `spacer` with `hexpand = true`
5. `status.importProgress`
6. `status.notification`
7. `separator` with `orientation = "vertical"` and `cssClasses = ["ao-status-separator"]`
8. `status.trackCount`

### Primitive layout components

Use the existing spacer primitive and add only the missing primitive needed to express the current composite status bar:

- `spacer`: already exists; use it with normal layout properties, especially `hexpand` and `vexpand`.
- `separator`: add a visual `Gtk::Separator` component with an `orientation` prop and normal layout properties.

`separator.orientation` should be an enum prop with values `horizontal` and `vertical`; the status-bar template must set it to `vertical` explicitly.

`separator` should live next to `spacer` in `Containers.cpp` and be registered as a container/basic layout component, not as a status-specific component, because it is a reusable layout primitive.

## Compatibility Rules

- Keep the public component type `status.defaultBar` registered for saved-layout compatibility during the first migration.
- Mark `status.defaultBar` as deprecated in comments/docs after the template version exists.
- New default layouts and newly reset layouts should use `type = "template"` with `templateId = "status.defaultBar"`, not the composite component type.
- Do not remove or rename existing status leaf components.
- Do not rename `playback.defaultBar`; instead, make the default layout actually consume it.

## Implementation Steps

1. Add only `separator` to `Containers.cpp` and expose it through `registerContainerComponents()` with an `orientation` enum prop (`horizontal`, `vertical`). Keep using the existing `spacer` component.
2. Update `getBuiltInTemplates()`:
   - Keep `playback.defaultBar` as a real child-composition template.
   - Replace the `status.defaultBar` alias with a real horizontal `box` template using status leaf components, existing spacers, and the new separator.
   - Add `ao-status-bar` to the status template's inner box so it matches the current `DefaultStatusBarComponent` styling.
3. Update `createDefaultLayout()`:
   - Replace the inline playback row with a `template` node for `playback.defaultBar`, preserving `id = "playback-row"` and `layout.cssClasses = ["ao-playback-strip"]` on the reference node.
   - Keep the outer status-region `box` with `layout.cssClasses = ["ao-status-region"]` in `createDefaultLayout()`.
   - Replace only that outer status box's direct `status.defaultBar` component child with a `template` node for `status.defaultBar` and `layout.hexpand = true`.
4. Keep the C++ `DefaultStatusBarComponent` registered for compatibility, but stop using it in built-in defaults.
5. Update component/header comments to distinguish:
   - `status.defaultBar` component: legacy compatibility composite.
   - `status.defaultBar` template: preferred default status-bar composition.
6. Update Linux GTK layout/editor tests to verify:
   - `createDefaultLayout()` contains template reference nodes for both default bars, not inline playback children or a direct `status.defaultBar` component ref.
   - The status-region wrapper remains a separate box with `ao-status-region`, and the status template owns `ao-status-bar`.
   - `playback.defaultBar` expands to the current playback component sequence.
   - `status.defaultBar` template contains the 8 expected leaf/primitive nodes in order: playback details, spacer, now playing, spacer, import progress, notification, separator, track count.
   - Legacy `status.defaultBar` component still instantiates.
7. Update `doc/design/linux-gtk-spacing-aesthetic-plan.md` if the implementation changes status-bar CSS ownership notes around `ao-status-region`, `ao-status-bar`, or `DefaultStatusBarComponent`. Otherwise, record that no design-doc update was needed.

## Test Plan

Run the focused layout test binary after a normal debug configure/build:

```bash
./build.sh debug
```

During iteration, focus on these existing test areas:

- `LayoutModelTest`
- `LayoutEditorTest`
- `ContainersTest`
- `LayoutComponentsTest`
- `StatusComponentsTest`

Add or update focused assertions for:

- `createDefaultLayout()` references `playback.defaultBar` and `status.defaultBar` through `type = "template"` nodes.
- The playback template reference carries `id = "playback-row"` and `ao-playback-strip` on the reference node.
- The status-region wrapper remains outside the status template and keeps `ao-status-region`.
- `status.defaultBar` template is a horizontal `box` with `ao-status-bar` and the 8 expected children in order.
- The `separator` component instantiates and honors `orientation = "vertical"`.
- The legacy `status.defaultBar` component type still instantiates through `ComponentRegistry`.

Manual validation should confirm that resetting the Linux GTK layout still shows the same playback controls and status-bar content as before.

## Acceptance Criteria

- The default layout references `playback.defaultBar` and `status.defaultBar` as templates.
- The status default bar composition no longer depends on the C++ composite component in new default layouts.
- The legacy `status.defaultBar` component remains available for existing saved layouts.
- Playback and status default bars follow the same customization model: shell region in `createDefaultLayout()`, reusable composition in `getBuiltInTemplates()`.
- Existing user-visible layout behavior remains unchanged.

## Assumptions

- The user's "playback default template" request means the default shell layout should consume the existing `playback.defaultBar` template, not that the current `playback.defaultBar` definition is missing.
- Template expansion should remain a build-time/runtime-layout concern; leaf widgets should stay as normal registered components.
