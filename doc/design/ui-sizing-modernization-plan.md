# Aobus UI Sizing Modernization Plan

## Summary

GTK layout sizing is currently split across C++ `set_size_request()` calls, CSS `min-width` and `min-height` rules, and YAML layout properties. This makes sizing behavior difficult to predict and hard to edit from the layout system.

This plan moves structural sizing into the YAML layout document and keeps C++, CSS, and YAML responsible for separate concerns.

This is a breaking schema cleanup. Existing `minWidth` and `minHeight` layout fields do not need a compatibility path and should be renamed to the new fields in built-in presets and tests.

## Principles

### YAML Owns Structural Sizing

YAML layout files are the source of truth for structural widget requests, expansion, alignment, and placement. Examples include fixed button slots, sidebar widths, panel minimum dimensions, and layout-level size requests.

Use camelCase layout fields to match the existing schema style:

```yaml
layout:
  widthRequest: 48
  heightRequest: 48
  halign: center
  valign: center
```

### CSS Owns Visual Sizing

CSS remains responsible for visual details such as padding, border radius, colors, typography, borders, and control affordances.

CSS should not define structural page or panel dimensions. Remove structural `min-width`, `min-height`, `width`, and `height` rules from layout-level classes.

Do not blindly remove every CSS size. Keep CSS dimensions that belong to control appearance or interaction affordance, such as slider handles, compact icon button touch targets, and visual glyph padding.

### C++ Owns Intrinsic Geometry

C++ should avoid arbitrary hardcoded layout dimensions. It may still compute intrinsic geometry when the widget content requires it, such as:

- text labels that reserve width based on measured text,
- custom drawing widgets with `measure_vfunc()` implementations,
- canvas or image widgets that preserve aspect ratio,
- progress controls with component-owned visual affordance sizes.

If a custom widget has no content-driven natural size, it must either implement a meaningful measurement policy or require explicit YAML layout sizing at every use site.

## Implementation Plan

### 1. Centralize Common Layout Prop Application

Move common layout application to the component creation path in `ComponentRegistry::create()` after the component factory returns an `ILayoutComponent`.

The common path should apply:

- `hexpand`
- `vexpand`
- `halign`
- `valign`
- `widthRequest`
- `heightRequest`
- `visible`
- `cssClasses`

The required order is:

1. `ComponentRegistry::create()` calls the component factory.
2. The factory returns a component with only component-specific setup applied.
3. `ComponentRegistry::create()` applies `applyCommonProps(component.widget(), node)` exactly once.
4. Tooltip wrapping and interaction decoration happen after the widget has received common layout properties.

After this is centralized, remove duplicate generic calls to `applyCommonProps()` from:

- container self setup,
- container child setup,
- tooltip component setup in `ComponentRegistry::create()`,
- root widget setup in `LayoutHost::setLayout()`.

Container factories are also invoked through `ComponentRegistry::create()`, so container widgets should not keep self-level `applyCommonProps()` calls after centralization.

Keep container-specific layout handling in containers. Examples:

- `centerBox` still owns child `slot`,
- `absoluteCanvas` still owns child `x`, `y`, `width`, `height`, and `zIndex`,
- `tabs` still owns tab metadata such as `title` and `icon`.

### 2. Rename `minWidth` and `minHeight`

The existing `applyCommonProps()` path already converts layout fields into `Gtk::Widget::set_size_request()`. This step is a breaking schema rename of those fields, not a new sizing mechanism.

Remove the old common layout fields:

```yaml
layout:
  minWidth: 48
  minHeight: 48
```

Replace them with:

```yaml
layout:
  widthRequest: 48
  heightRequest: 48
```

Apply both values in a single `set_size_request()` call. Do not use allocated size while building widgets; allocation is not stable before GTK layout has run.

Recommended logic:

```cpp
auto width = -1;
auto height = -1;

if (auto const it = layout.find("widthRequest"); it != layout.end())
{
  width = static_cast<std::int32_t>(it->second.asInt());
}

if (auto const it = layout.find("heightRequest"); it != layout.end())
{
  height = static_cast<std::int32_t>(it->second.asInt());
}

if (width >= 0 || height >= 0)
{
  widget.set_size_request(width, height);
}
```

Update the layout editor common property list to expose `widthRequest` and `heightRequest`, and remove `minWidth` and `minHeight`.

Do not use generic `width` and `height` for size requests. Those names remain reserved for absolute-canvas child geometry.

### 3. Update Built-In Layout Presets

Update `app/linux-gtk/layout/default_layout.yaml` and `app/linux-gtk/layout/modern_layout.yaml` so structural size requests live in YAML.

For example:

```yaml
- type: playback.soulButton
  layout:
    widthRequest: 48
    heightRequest: 48
    halign: center
    valign: center
```

Any component that depends on an explicit layout size must receive one in the preset.

`absoluteCanvas` children keep using their existing positional fields:

```yaml
layout:
  x: 24
  y: 16
  width: 200
  height: 48
  zIndex: 2
```

These fields are canvas-specific placement data, not generic widget size requests. The layout editor should label or group `width` and `height` separately from `widthRequest` and `heightRequest` so the distinction is clear.

### 4. Rework Soul Button Sizing

`AobusSoul` currently reports no intrinsic size from `measure_vfunc()`. Removing component-level size requests without adding YAML requests will allow soul widgets to collapse.

Use one policy consistently:

- Remove the `size` prop from `playback.soulButton` and `playback.soulPlayPauseButton`.
- Remove the `size`-driven `_soul.set_size_request(size, size)` calls from playback components.
- Require `layout.widthRequest` and `layout.heightRequest` at every layout use that needs a visible fixed soul size.
- Configure the inner `AobusSoul` to fill the outer `Gtk::Button` content allocation instead of carrying its own size request.

This is intentionally a breaking change. Keep drawing-specific properties such as `strokeWidth`, `glyphScale`, `glyph`, and `showFullLogo` as component props.

The distinction matters: common layout properties apply to the component's returned widget, which is the outer `Gtk::Button` for soul button components. The old `size` prop targeted the inner `AobusSoul` child. The implementation must account for this by making the inner soul fill the button allocation and by auditing CSS padding on `.ao-soul-button`.

### 5. Purge Structural C++ Size Requests

Review GTK frontend `set_size_request()` calls and classify each one before changing it.

Current inventory:

| File | Call site | Classification |
| --- | --- | --- |
| `app/linux-gtk/layout/components/PlaybackComponents.cpp` | `_soul.set_size_request(size, size)` in `playback.soulPlayPauseButton` | Move to YAML via `layout.widthRequest` and `layout.heightRequest`; remove the `size` prop. |
| `app/linux-gtk/layout/components/PlaybackComponents.cpp` | `_soul.set_size_request(size, size)` in `playback.soulButton` | Move to YAML via `layout.widthRequest` and `layout.heightRequest`; remove the `size` prop. |
| `app/linux-gtk/layout/components/Containers.cpp` | `widget.set_size_request(width, height)` in `applyCommonProps()` | Keep; rename input fields from `minWidth`/`minHeight` to `widthRequest`/`heightRequest`. |
| `app/linux-gtk/playback/TimeLabel.cpp` | measured label width | Keep in C++; this is content-derived intrinsic sizing. |
| `app/linux-gtk/list/ListNavigationPanel.cpp` | navigation panel default width | Keep in C++ unless the panel becomes layout-runtime managed. |
| `app/linux-gtk/track/StatusSlot.cpp` | progress bar width | Keep in C++; this is internal control affordance sizing. |
| `app/linux-gtk/playback/AobusSoulWindow.cpp` | large soul logo size from monitor geometry | Keep in C++; this is dynamic geometry outside YAML layout presets. |
| `app/linux-gtk/layout/editor/LayoutEditorDialog.cpp` | editor label widths | Keep in C++; this is layout editor chrome, not user layout content. |

Move to YAML when the size is structural:

- panel width,
- sidebar width,
- fixed layout slot size,
- preset-specific cover or hero size.

Keep in C++ when the size is intrinsic or content-derived:

- measured time label width,
- custom drawing bounds,
- aspect-ratio image calculations,
- component-owned progress/control affordance width.

### 6. Clean CSS Sizing Rules

Review CSS size rules and remove those that define layout structure. Move those values to YAML `widthRequest` and `heightRequest`.

Keep CSS size rules that express visual or interaction affordances. Examples include slider knobs, icon button minimum touch area, decorative glyph padding, and similar control-level styling.

Initial CSS classification:

Keep as control affordance or reset:

- `min-width: 0` and `min-height: 0` reset rules,
- playback button minimum touch targets,
- seek and volume trough/handle sizes,
- compact icon button sizes,
- status/control bar minimum heights where the value is a control affordance.

Candidates to move to YAML or component layout config:

- structural sidebar or navigation widths,
- detail pane and hero cover structural dimensions,
- popover or selector widths that define layout structure rather than control drawing.

Do not remove reset rules just because they use `min-width` or `min-height`; those rules often counter GTK or Adwaita defaults and are not structural layout requests.

## Tests

Add or update focused GTK layout tests for:

- `ComponentRegistry::create()` applying common layout props to every component,
- `widthRequest` and `heightRequest` producing the expected GTK size request,
- removed `minWidth` and `minHeight` assumptions,
- `playback.soulButton` receiving size from YAML layout properties,
- container-specific layout properties still working after common prop centralization.

Run:

```bash
./ao test --gtk "[layout]"
```

For broader validation after touching shared GTK component behavior, run the full debug build:

```bash
./ao check
```

## Manual Verification

Launch the GTK application and verify:

- soul buttons are visible and correctly sized,
- sidebars and panels keep their intended dimensions,
- control bars do not collapse,
- CSS-only control affordances still look correct,
- changing a `widthRequest` or `heightRequest` in YAML changes the UI after reload or restart without C++ changes.
