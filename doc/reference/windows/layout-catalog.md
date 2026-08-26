---
id: windows.layout-catalog
type: reference
status: current
domain: application-shell
summary: Enumerates the Windows shell layout catalog, accepted layout fields, element mapping, style-key resolution, and the two built-in preset documents.
---
# Windows layout catalog

## Scope and version

This reference owns the exact Windows-side surfaces of the version 1 layout language: the component and action ids the Windows catalog registers, the layout fields it accepts and rejects, the native element each component constructs, the `styleKey` and `surface` extensions, and the two built-in preset documents.

It uses the same unversioned `LayoutDocument` version 1 language as the [layout document reference](../shell/layout-document.md). The Windows catalog is a deliberately narrower identity set than the [GTK layout catalog](../shell/layout-catalog.md): the two share no preset and no component construction, but a type both shells present takes its identity and shared properties from the [shared component vocabulary](../shell/component-vocabulary.md) rather than being described twice.

These surfaces are shipped contracts today. The WinUI window builds its Modern and Classic trees from these documents; [decision 0004](../../decision/0004-adopt-layout-documents-for-winui-shell-composition.md) records why it adopted the shared language while retaining Windows-owned presets and construction.

## Code boundary

Two boundaries run through this reference, and they are not the same one. What both shells decide the same way - parsing the version 1 common fields, walking a candidate against a catalog, the parse-expand-validate step - belongs to the **UIModel** layer in the [system architecture](../../architecture/system-overview.md), under the [application shell architecture](../../architecture/application-shell.md). What is this shell's own belongs to this shell, however portable the code is: a component catalog naming `windows.navigationPane` is not a shared model just because deciding it needs no XAML.

So the shared traversal lives in `app/uimodel/layout/document/`, and everything below - the catalog, the dialect that extends the shared rules, the element lattice, the themed surfaces, the style resolution, and the WinUI half of placement - lives in the Windows-only `aobus-winui-lib` under `app/windows-winui/`, in the `ao::winui` namespace. Public frontend headers live under `app/windows-winui/include/ao/winui/`. Pure Windows-owned rule sources that name no platform API are compiled directly into `ao_core_test` on every host; rules that name WinRT or XAML types run only in the native Windows test profile. Neither path exports a second WinUI model target. The preset resources, native construction, controllers, and resource lookup all remain under `app/windows-winui/`.

## Surface

### Built-in preset documents

| Preset | Preset id | Resource |
|---|---|---|
| Modern | `windows.modern` | `windows_modern_layout.yaml` |
| Classic | `windows.classic` | `windows_classic_layout.yaml` |

Both documents are `version: 1`, declare no templates, and use disjoint node ids so component runtime state never crosses a shell.

They ship as application content in `Assets\Layout` beside the executable, not as compiled-in strings, so the shell a build produces is the one on disk: editing a preset takes a restart rather than a rebuild, and a malformed edit is rejected as a whole candidate the way a user document would be. The folder is named once, by `winui::kShellPresetFolder`, and the pairing between it and what the WinUI project packages is a test rather than a convention.

### Registered component types

| Family | Type ids |
|---|---|
| Container | **`box`**, **`split`** |
| Shell | **`app.menuBar`**, `windows.inspectorPane`, `windows.libraryPath`, `windows.navigationPane`, `windows.statusBar`, `windows.titleBar` |
| Track | **`track.coverArt`**, `track.detail`, **`track.presentationButton`**, **`track.quickFilter`**, **`track.table`** |
| Playback | `playback.nowPlayingInfo`, **`playback.outputDeviceSelector`**, **`playback.seekSlider`**, **`playback.soulButton`**, **`playback.timeLabel`**, **`playback.transportButton`**, **`playback.volumeControl`** |
| Status | **`status.activity`**, **`status.message`**, **`status.selectionInfo`**, **`status.trackCount`** |
| Generic | **`actionButton`**, **`label`**, **`menuButton`** |

Bold ids come from the [shared component vocabulary](../shell/component-vocabulary.md), which owns their display name, category, child range, action slots, and shared properties. The rest are Windows' own.

`template` is a document node type handled by expansion and is not a registered component. Any other type, including a `[TemplateError]` node produced by a failed expansion, is an unknown component type.

### Component properties

Shared properties - `orientation`, `spacing`, `text`, `variant` on `track.presentationButton`, `placeholderStyle`, `command`, `strokeWidth`, `glyphScale`, `mode` - are defined by the [shared component vocabulary](../shell/component-vocabulary.md) and are not restated here. What follows is what Windows adds.

| Type | Property | Kind | Values | Default |
|---|---|---|---|---|
| `split` | `initialPositionPercent` | Double | — | `0.5` |
| `windows.navigationPane` | `presentation` | Enum | `navigationView`, `tree` | `navigationView` |
| `playback.seekSlider` | `presentation` | Enum | `overlay`, `inline` | `inline` |
| `playback.volumeControl` | `presentation` | Enum | `flyout`, `inline` | `flyout` |
| `status.trackCount`, `status.selectionInfo` | `variant` | Enum | `status`, `summary` | `status` |
| `actionButton`, `menuButton` | `glyph` | String | — | empty |
| `label`, `actionButton`, `menuButton` | `textResourceKey` | String | — | empty |
| `playback.soulButton` | `showGlyph` | Bool | — | `true` |
| `menuButton` | `menuId` | Enum | `modernOverflow`, `nowPlayingOverflow` | `modernOverflow` |

Windows draws the live transport icon as the soul's inner mark, so the only question a document can answer here is whether to draw it: `showGlyph`, a Windows property. GTK's `glyph` chooses between two static ornaments, which is a different question, so neither name is shared.

### Child counts

| Type | Children |
|---|---|
| `box`, `windows.titleBar`, `windows.statusBar` | any |
| `split` | exactly 2 |
| `windows.inspectorPane` | exactly 1 |
| `windows.navigationPane` | exactly 1 in `navigationView` presentation, exactly 0 in `tree` |
| every other type | 0 |

### Components that require a stable id

`track.detail`, `track.table`, `windows.inspectorPane`, and `windows.navigationPane` must carry an authored `id`. A shell switch destroys and rebuilds every element, so these are located by id when semantic view state is reconciled into the candidate.

### Panes that own their width

`windows.inspectorPane`, and `windows.navigationPane` in its `tree` presentation, size themselves from `winui::DesktopSettings` and own the boundary the user drags to change it. They also collapse entirely when the resolved shell policy hides them. Author them as children of a `box`, which sizes a slot from the child, and do not give them `hexpand` or a `widthRequest`: a `split` keeps its share of the axis whatever the child does, so a collapsed pane would leave its space behind, and a `widthRequest` becomes a `MinWidth` that would stop a drag short of the setting's own minimum.

The `navigationView` presentation is the exception. It draws its own pane inside the region it is given and hosts the workspace as its content, so it expands like any other container and turns the resolved navigation presentation into a display mode rather than a width.

### Registered action ids

| Action id | Category | Capabilities |
|---|---|---|
| `library.open` | Library | `RequiresAnchor` |
| `library.rescan` | Library | None |
| `shell.toggleInspector` | Shell | None |
| `shell.showSystemMenu` | Shell | `RequiresAnchor`, `PresentsMenu` |
| `shell.showSoul` | Shell | None |
| `playback.play` | Playback | None |
| `playback.pause` | Playback | None |
| `playback.playPause` | Playback | None |
| `playback.stop` | Playback | None |
| `playback.next` | Playback | None |
| `playback.previous` | Playback | None |
| `playback.toggleShuffle` | Playback | None |
| `playback.cycleRepeat` | Playback | None |
| `playback.showOutputDeviceSelector` | Playback | `RequiresAnchor`, `PresentsMenu` |

Menus, reveal-current-track, and column editing remain native behavior of the component that owns them and have no layout-catalog action id.
Transport components also run their native command directly, but every `playback.*` transport command is registered so layout action slots and the shared keymap resolve the same stable inventory.
The separate shared keymap action `workspace.revealCurrentTrack` is registered directly by the native shell and is therefore runnable by `Ctrl+L` without becoming authorable in a layout-document action slot.

`actionButton` and `playback.soulButton` accept the primary click, primary long press, and secondary click; `playback.soulButton` defaults `secondaryAction` to `shell.showSystemMenu` and `primaryLongPressAction` to `shell.showSoul`. Every other component accepts none.

No component here accepts `secondaryLongPressAction`. Windows raises one holding sequence per press regardless of which button started it, so the binder cannot tell a secondary hold from a primary one and refuses the slot. Offering it in the catalog would let a document validate and then be rejected in full while being built, so the catalog does not offer it; a test holds every descriptor to that. GTK's soul button does accept the slot, because GDK distinguishes the two.

`playback.soulButton` has no `primaryAction` default, because that slot is the soul's own play/pause gesture: the soul runs the transport when the document leaves the slot alone, and presents playback without driving it when the document names an action there. Nothing else in the catalog puts a native gesture and an action slot on the same event, so this is the only component where authoring an action takes behavior away.

A bound slot becomes one native gesture: `primaryAction` is a button's `Click` and a plain `Tapped` on any other element, `secondaryAction` is `RightTapped`, and `primaryLongPressAction` is a completed `Holding`. Windows raises one holding sequence per press regardless of pointer button, so `secondaryLongPressAction` cannot be distinguished from `primaryLongPressAction` and binding it rejects the document. An action id that the running shell registers no handler for also rejects the document, because a compiled preset naming an unimplemented capability is a shipped-artifact defect.

### Text resources

The shared `text` property is the words a reader sees, verbatim, so a document that sets it reads the same in every shell. Naming a localized string is the separate Windows-owned `textResourceKey` property: it wins where authored, and falls back to `text` when the resource map does not define the key, which keeps a localization gap visible rather than blank. Neither case rejects the document. On a control that shows a glyph, the resolved string becomes the tooltip and the automation name instead of the visible content.

### Accepted layout fields

| Field | Windows interpretation |
|---|---|
| `hexpand`, `vexpand` | The parent container allocates a star slot on the matching axis; otherwise the slot is auto-sized |
| `halign`, `valign` | Child `HorizontalAlignment` / `VerticalAlignment`; `fill` maps to `Stretch`, `start` to `Left`/`Top`, `end` to `Right`/`Bottom`, `center` to `Center` |
| `widthRequest`, `heightRequest` | Child `MinWidth` / `MinHeight`; a negative value keeps the version 1 meaning of no minimum |
| `visible` | Authored gate: `false` always collapses, and runtime state may additionally hide an authored-visible element but never reveals a hidden one |
| `styleKey` | Windows extension naming a `Style` resource |
| `surface` | Windows extension naming the themed surface the element paints itself as |

`cssClasses` is a GTK extension and is rejected. A component may also declare per-child layout fields in its descriptor; no built-in Windows component currently does.

A field the document does not author leaves the corresponding `Style` setter in effect. An authored field is applied as a local value, so explicit placement and controller-owned semantic state win over a style default.

### Native element mapping

| Element | Component types |
|---|---|
| `Grid` | `box`, `split`, `windows.titleBar`, `windows.statusBar`, `windows.inspectorPane`, `windows.navigationPane` in `tree` presentation, `track.quickFilter`, `playback.nowPlayingInfo`, `status.activity` |
| `ScrollViewer` | `track.detail`, `track.table` |
| `Border` | `track.coverArt` |
| `NavigationView` | `windows.navigationPane` in `navigationView` presentation |
| `MenuBar` | `app.menuBar` |
| `Button` | `track.presentationButton`, `playback.transportButton`, `playback.soulButton`, `playback.outputDeviceSelector`, `playback.volumeControl` in `flyout` presentation, `actionButton`, `menuButton` |
| `Slider` | `playback.seekSlider`, `playback.volumeControl` in `inline` presentation |
| `TextBlock` | `label`, `playback.timeLabel`, `status.message`, `status.selectionInfo`, `status.trackCount`, `windows.libraryPath` |

Every structural container is a `Grid` because WinUI expresses remaining-space allocation on a row or column definition, which is what `hexpand` and `vexpand` mean. `windows.inspectorPane` and the `tree` navigation pane are among them despite what they show: each carries a resize thumb over one edge, so the thumb and the pane content share a cell.

The mapping names what the component hands to its parent, not what it displays. `track.table` is its scrolling viewport rather than the surface of headers and rows inside it, the `tree` pane is its cell rather than the `TreeView`, and `track.coverArt` is the `Border` that rounds and clips the artwork and its placeholder together rather than the `Image` inside it. That is what a `styleKey` targets and what a `surface` paints, and the runtime rejects a component whose element is not the kind its entry declares.

`track.quickFilter` hands its parent a `Grid` containing the native `AutoSuggestBox` and a Create List action that appears only for a valid non-empty resolved expression.
Its adapter takes completion identity, ranking, replacement ranges, and insertion text from the shared track-filter UIModel, keeps arrow navigation from rewriting the draft, maps the native UTF-16 caret to shared UTF-8 byte positions, and applies accepted replacements back at UTF-16 boundaries.

Style-target compatibility uses this element lattice: `Panel` and `Border`, `TextBlock`, and `Control` derive from `FrameworkElement`; `Grid` from `Panel`; `ContentControl`, `ItemsControl`, `Slider`, `AutoSuggestBox`, `TreeView`, and `MenuBar` from `Control`; `ButtonBase`, `ScrollViewer`, and `NavigationView` from `ContentControl`; `Button` from `ButtonBase`; and `ListView` from `ItemsControl`.

### Components that follow the focused selection

A shell has one focused view, and building one detail snapshot reads every selected track out of the library. The components that follow that selection — `track.detail` and `track.coverArt` — therefore share one `TrackDetailProjection`, owned by the generation and created the first time a component asks for it: a document showing no track detail pays for none, and a document showing both does not aggregate the selection twice. Each component holds a handle for as long as it lives, so no teardown order between them can leave one reading a projection that is already gone.

A component owns the adapter that drives it, and publishes nothing back to the shell. The elements a generation builds live and die with it, so an adapter that outlived one would be holding elements no longer in the window; keeping the two together is what lets a shell switch rebuild presentation while the semantic state behind it — the active list, the loaded theme, the resource loader, the session — stays put in the shell.

`track.coverArt` is the first component that decides part of its own size. Everything else takes what its slot allocates; the cover takes the width the frame gives it — `ModernInspectorCoverStyle` stretches it across the pane up to a 320px cap — and answers with a matching height, because a cover is square and no XAML panel expresses that. A cap the pane is wider than leaves the cover centred in the room it did not take.

When the selection carries no artwork, the component draws the placeholder style the node authored, from the same set both shells offer. The Windows entry carries no `targetSize`: unlike GTK it does not decode to a target, so the only thing left to author is what to draw.

### Frame-owned item templates

A `track.table` draws its column headers and rows from compiled `DataTemplate` resources the window frame owns, not from anything the document describes: `TrackHeaderCellTemplate` and `TrackRowTemplate`, plus the optional `TrackListItemStyle` container style. A document naming `track.table` in a frame that defines neither template is rejected, because a shipped frame missing them is a build defect rather than a condition to discover at runtime.

The detail sections resolve `InspectorSectionHeaderStyle` the same way, falling back to an unstyled heading when it is absent.

A `playback.seekSlider` resolves one chrome dictionary per presentation: `ModernSeekOverlayResources` for `overlay` and `ClassicSeekInlineResources` for `inline`. Each is a keyed `ResourceDictionary` whose entries the component copies into the one slider it built. The keys inside are WinUI's own — `SliderTrackThemeHeight` and the rest of the stock template's metrics — so declaring them in the window would re-chrome every slider in the shell; behind a key of their own they reach exactly one. The entries are copied rather than the dictionary merged, because a `ResourceDictionary` can be the merged child of only one scope at a time and every generation builds a slider that wants the same one. Absent, the slider keeps stock chrome: it still seeks, so a missing dictionary is a presentation gap rather than a reason to reject the document.

The overlay dictionary also holds `ModernSeekThumbTemplate`. A thumb is drawn by a `ControlTemplate`, which a `Style` cannot carry, so it is frame-owned rather than reachable through `styleKey`.

### Style-key resolution

A `styleKey` resolves only against the window's `RootGrid.Resources`. The outcomes are:

| Outcome | Condition |
|---|---|
| `NoStyleAuthored` | The node authored no `styleKey` |
| `Applied` | The key resolved in `RootGrid.Resources` and its `TargetType` is the constructed element's kind or a base of it |
| `MissingKey` | The key did not resolve, or resolved only in application resources |
| `IncompatibleTarget` | The key resolved in scope but its `TargetType` does not accept the constructed element |

The shipped frame declares these styles for the shipped presets. `ChromeLessButtonStyle` is shared; the rest belong to one preset each.

| Style | `TargetType` | Carries |
|---|---|---|
| `ChromeLessButtonStyle` | `Button` | Transparent chrome, 36px hit target, 18px radius |
| `ModernTitleBarStyle` | `Grid` | 16px side padding, and a transparent fill so the caption drag region takes hits |
| `ModernLibraryPathStyle` | `TextBlock` | 12px subdued text, trimmed, inset clear of the caption buttons |
| `ModernTrackControlsStyle` | `Grid` | 12×8 padding |
| `ModernSubduedTextStyle` | `TextBlock` | 0.55 opacity |
| `ModernInspectorStyle` | `Grid` | Card margin, 1px divider border, 16px radius |
| `ModernInspectorCoverStyle` | `Border` | Cover inset, 320px cap, 16px radius, and the card fill behind the artwork |
| `ModernNowPlayingStyle` | `Grid` | 1px divider along the top edge |
| `ModernNowPlayingRowStyle` | `Grid` | 16×8 padding on the content row, so the seek overlay above still reaches both edges |
| `ModernSeekOverlayStyle` | `Slider` | Negative margins that hang the overlay over the strip's top edge, and the z-order that keeps it there |
| `ModernTimeLabelStyle` | `TextBlock` | 11px subdued text |
| `ClassicPlaybackStripStyle` | `Grid` | 6px padding, 1px divider below |
| `ClassicToolbarStyle` | `Grid` | 6×3 padding, 1px divider above and below |
| `ClassicInspectorStyle` | `Grid` | 1px divider along the leading edge |
| `ClassicInspectorCoverStyle` | `Border` | 10px inset, 200px cap, Classic chrome radius, 1px divider border, and card fill |
| `ClassicStatusBarStyle` | `Grid` | 6×2 padding, 1px divider above |

Each `TargetType` is the element the native element mapping says the node constructs, and the pairing of every authored `styleKey` with the frame that declares it is a test, not a convention.

### Themed surfaces

A `styleKey` carries geometry, spacing, and typography. It does not carry colour: a `Style` is parsed once when the window loads, while the active theme changes underneath it. A node names a themed surface instead, and the shell resolves that slot against the loaded `winui::Theme` while the generation is being built.

| Slot | Theme token |
|---|---|
| `window` | `shared.windowBackground` |
| `surface` | `shared.surface` |
| `modern.navigation` | `modern.navigationBackground` |
| `modern.inspector` | `modern.inspectorBackground` |
| `modern.nowPlaying` | `modern.nowPlayingBackground` |
| `classic.toolbar` | `classic.toolbarBackground` |
| `classic.tree` | `classic.treeBackground` |
| `classic.statusBar` | `classic.statusBackground` |

With no theme override loaded the stock brushes apply: `modern.inspector`, `modern.nowPlaying`, `classic.toolbar`, and `classic.statusBar` take `CardBackgroundFillColorDefaultBrush`, `classic.tree` takes `ApplicationPageBackgroundThemeBrush`, and `window`, `surface`, and `modern.navigation` name no brush at all so those elements keep the stock chrome.

The brush is written to whichever background the element owns, in the order `Panel`, `Border`, `Control`. A slot on an element that owns none is rejected rather than dropped, because an authored slot that silently does nothing reads as intent that was honoured. That rules out `TextBlock`, which draws glyphs and nothing behind them.

## Validation rules

A built-in document is one candidate. The first defect in document order rejects it entirely; there are no per-node diagnostic placeholders. Traversal visits actions, properties, layout fields, and child count for a node before its children, so the reported defect is deterministic for a given document.

Rejection reasons are exact:

| Reason | Condition |
|---|---|
| `UnknownComponentType` | The type is not registered in the Windows catalog |
| `UnsupportedLayoutField` | `cssClasses`, a `surface` on an element that owns no background, or a field neither common nor declared by the component |
| `InvalidLayoutFieldValue` | An alignment outside the version 1 vocabulary, a non-numeric size request, an empty or non-string `styleKey`, or a `surface` naming no known slot |
| `UnknownProperty` | A property the component does not declare |
| `InvalidPropertyValue` | A property whose value type does not match its declared kind, or an enum value outside its declared list |
| `ChildCountBelowMinimum` | Fewer children than the descriptor or the authored presentation requires |
| `ChildCountAboveMaximum` | More children than the descriptor or the authored presentation allows |
| `MissingRequiredId` | A component that must be locatable carries no id |
| `DuplicateNodeId` | An id already used elsewhere in the document |
| `UnsupportedActionSlot` | An action property whose slot the component policy disallows |
| `UnknownAction` | A bound or defaulted action id outside the Windows action catalog |
| `UnsupportedSurface` | An authored tooltip subtree; Windows components own their own tooltips |

An action binding of `none` or an empty string leaves the slot explicitly unbound and is accepted.

Preparation additionally applies the shared `LayoutDocumentLimits`: a document over 256 KiB is rejected before parsing, and authored and effective trees are bounded by the shared entry, depth, and value-byte budgets.

## Compatibility and versioning

Windows uses the same `version: 1` language as GTK, and the frontend extensions do not change that version because the property maps are open. A document declaring another version is rejected as unsupported.

The catalog ids are internal shipped identities, not a user-authoring surface: this adoption supports only the two built-in presets, with no Windows layout editor, user-authored preset, or migration format.

## Examples

```yaml
version: 1
root:
  id: windows-classic-root
  type: box
  props:
    orientation: vertical
  layout:
    hexpand: true
    vexpand: true
  children:
    - id: classic-workspace
      type: box
      props:
        orientation: horizontal
      layout:
        hexpand: true
        vexpand: true
      children:
        - id: classic-navigation
          type: windows.navigationPane
          props:
            presentation: tree
          layout:
            vexpand: true
        - id: classic-track-table
          type: track.table
          layout:
            hexpand: true
            vexpand: true
```

## Implementation authority

Everything below is the WinUI shell's own, which is why it lives in
`aobus-winui-lib` rather than in `ao_app_uimodel`.
Pure rules carrying no WinRT dependency compile in `ao_core_test` on every host; native Windows builds additionally cover their XAML composition.

- [`LayoutCatalog.cpp`](../../../app/windows-winui/layout/LayoutCatalog.cpp) owns registered component and action ids, presentation-dependent element and child rules, and required ids.
- [`LayoutDialect.cpp`](../../../app/windows-winui/layout/LayoutDialect.cpp) owns what this shell adds to the shared rules: the styling field it rejects, its `styleKey`, and its themed surfaces. Whole-candidate rejection itself belongs to the shared [`LayoutValidation.cpp`](../../../app/uimodel/layout/document/LayoutValidation.cpp).
- [`PlacementPlan.cpp`](../../../app/windows-winui/layout/PlacementPlan.cpp) owns the parent/child split. The common fields are read once by the shared [`LayoutPlacement.cpp`](../../../app/uimodel/layout/document/LayoutPlacement.cpp).
- [`StyleLookup.cpp`](../../../app/windows-winui/layout/StyleLookup.cpp) and [`ElementKind.cpp`](../../../app/windows-winui/layout/ElementKind.cpp) own style scope and target compatibility.
- [`ShellDocument.cpp`](../../../app/windows-winui/layout/ShellDocument.cpp) owns preset ids and resource names; the parse-expand-validate step itself is the shared [`LayoutDocumentLoader.cpp`](../../../app/uimodel/layout/document/LayoutDocumentLoader.cpp).
- [`windows_modern_layout.yaml`](../../../app/windows-winui/layout/windows_modern_layout.yaml) and [`windows_classic_layout.yaml`](../../../app/windows-winui/layout/windows_classic_layout.yaml) are the shipped documents.

## Test authority

- [`ShellDocumentTest.cpp`](../../../test/unit/winui/layout/ShellDocumentTest.cpp) proves both shipped documents parse, expand, and validate, that their node ids stay disjoint, that no pane owning its width is given a proportional slot, and that each preset offers play/pause exactly once.
- [`LayoutDialectTest.cpp`](../../../test/unit/winui/layout/LayoutDialectTest.cpp) covers every rejection reason as this shell's dialect produces it. That the shared traversal is genuinely dialect-driven is proved separately by [`LayoutValidationTest.cpp`](../../../test/unit/uimodel/layout/document/LayoutValidationTest.cpp), which runs it against an invented vocabulary no frontend ships.
- [`LayoutCatalogTest.cpp`](../../../test/unit/winui/layout/LayoutCatalogTest.cpp), [`PlacementPlanTest.cpp`](../../../test/unit/winui/layout/PlacementPlanTest.cpp), [`StyleLookupTest.cpp`](../../../test/unit/winui/layout/StyleLookupTest.cpp), and [`ElementKindTest.cpp`](../../../test/unit/winui/layout/ElementKindTest.cpp) cover the catalog, placement mapping, style planning, and element lattice.
- [`FrameResourceTest.cpp`](../../../test/unit/winui/FrameResourceTest.cpp) reads the shipped frame and proves every `styleKey` the presets name is declared in the window scope with a `TargetType` the node's element accepts, that the resources the components resolve by name are declared, and that the seek chrome stays out of the window scope.
- [`ThemeSurfaceTest.cpp`](../../../test/unit/winui/layout/ThemeSurfaceTest.cpp) covers the themed surface vocabulary, its theme tokens, and which elements accept a slot.
- [`QuickFilterCompletionAdapterTest.cpp`](../../../test/unit/winui/track/QuickFilterCompletionAdapterTest.cpp) covers the platform-free UTF-8/UTF-16 completion boundary on every host; native Windows builds cover the XAML wiring itself.

## Related documents

- [Layout document reference](../shell/layout-document.md) owns the shared version 1 language.
- [GTK layout catalog](../shell/layout-catalog.md) owns the separate GTK identities.
- [Windows desktop shell specification](../../spec/shell/windows-desktop.md) owns observable Windows shell behavior.
- [Windows desktop state reference](desktop-state.md) owns persisted Windows settings.
- [Decision 0004](../../decision/0004-adopt-layout-documents-for-winui-shell-composition.md) records why Windows adopts the shared language while owning its presets and construction.
