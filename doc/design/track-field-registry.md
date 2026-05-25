# Track Field Registry

## Purpose

This document describes the implemented field-centric model for track presentation, editing, and property display.

All repeated dispatch such as:

```cpp
if (column == TrackColumn::Artist) { ... }
switch (field) { ... }
```

has been removed from GTK track pages, properties dialogs, inline editing, drag-to-query behavior, and synthetic display fields.

The implemented model is:

```text
TrackField registry (25 compile-time fields)
  -> presentation visible fields
  -> sort/group capabilities
  -> GTK columns
  -> inline editing
  -> Properties dialog editable metadata
  -> Properties dialog read-only technical fields
  -> synthetic display fields
```

Columns are not fields. A column is a GTK presentation surface for a field.

## Non-Goals

- Do not move GTK row value materialization into runtime.
- Do not make runtime depend on gtkmm, Glib, `TrackRowObject`, or GTK widgets.
- Do not create a plugin-style runtime registry. The initial field set is static and compile-time defined.
- Do not keep `TrackColumn` as a second semantic enum in the final state.

## Layer Ownership

### Runtime

Runtime owns frontend-neutral field identity and presentation semantics:

- stable field ids for config and custom views
- field labels suitable for generic menus
- category and value kind
- sort/group/presentation capability metadata
- built-in presentation specs expressed in fields
- projection sort/group behavior

Runtime does **not** own display values.

### GTK

GTK owns field rendering and editing:

- converting `TrackView`/`TrackRowObject` data into display text
- dictionary string resolution for display
- column default width, expansion, and cell style
- inline editor widget behavior
- Properties dialog row creation
- mixed-value aggregation for multi-track edits
- drag-to-query text generation

GTK may use runtime field descriptors, but runtime must not use GTK descriptors.

## Runtime Field Model

The presentation-only field enum has been replaced with a full 25-field track field enum.

Files:

```text
app/runtime/TrackField.h
app/runtime/TrackField.cpp
```

`TrackField` replaced `TrackPresentationField` in presentation specs. The registry is defined in a `constexpr std::to_array<TrackFieldDefinition>` with a `static_assert` enforcing exactly 25 entries.

```cpp
namespace ao::rt
{
  enum class TrackField : std::uint8_t
  {
    Title,
    Artist,
    Album,
    AlbumArtist,
    Genre,
    Composer,
    Work,

    Year,
    DiscNumber,
    TotalDiscs,
    TrackNumber,
    TotalTracks,

    Duration,
    Tags,

    FilePath,
    Codec,
    SampleRate,
    Channels,
    BitDepth,
    Bitrate,
    FileSize,
    ModifiedTime,

    DisplayTrackNumber,
    TechnicalSummary,
    Quality,
  };

  enum class TrackFieldCategory : std::uint8_t
  {
    Metadata,
    Tag,
    Technical,
    Synthetic,
  };

  enum class TrackFieldValueKind : std::uint8_t
  {
    Text,
    Number,
    Duration,
    TagList,
    FilePath,
    TechnicalText,
  };

  struct TrackFieldDefinition final
  {
    TrackField field;
    std::string_view id;
    std::string_view label;
    TrackFieldCategory category;
    TrackFieldValueKind valueKind;

    bool presentable = false;
    bool editable = false;
    bool sortable = false;
    bool groupable = false;
    bool synthetic = false;

    std::optional<TrackSortField> sortField{};
    std::optional<TrackGroupKey> groupKey{};

    std::string_view filterExpressionVariable{};
  };

  std::span<TrackFieldDefinition const> trackFieldDefinitions();
  TrackFieldDefinition const* trackFieldDefinition(TrackField field);
  std::optional<TrackField> trackFieldFromId(std::string_view id);
  std::string_view trackFieldId(TrackField field);
}
```

### Stable IDs

Field ids are persistence-facing. Do not rename them after release without migration.

| Field | ID |
| --- | --- |
| `Title` | `title` |
| `Artist` | `artist` |
| `Album` | `album` |
| `AlbumArtist` | `album-artist` |
| `Genre` | `genre` |
| `Composer` | `composer` |
| `Work` | `work` |
| `Year` | `year` |
| `DiscNumber` | `disc-number` |
| `TotalDiscs` | `total-discs` |
| `TrackNumber` | `track-number` |
| `TotalTracks` | `total-tracks` |
| `Duration` | `duration` |
| `Tags` | `tags` |
| `FilePath` | `file-path` |
| `Codec` | `codec` |
| `SampleRate` | `sample-rate` |
| `Channels` | `channels` |
| `BitDepth` | `bit-depth` |
| `Bitrate` | `bitrate` |
| `FileSize` | `file-size` |
| `ModifiedTime` | `modified-time` |
| `DisplayTrackNumber` | `display-track-number` |
| `TechnicalSummary` | `technical-summary` |
| `Quality` | `quality` |

### Filter Expression Variables

Registry-backed fields support generating valid `lib/expr` filter expressions (e.g., for click-to-filter navigation). The variable name is explicitly defined in the registry to handle naming mismatches (e.g., `AlbumArtist` is `$albumArtist` while its ID is `album-artist`).

| Field | Filter Expression Variable |
| --- | --- |
| `Title` | `$title` |
| `Artist` | `$artist` |
| `Album` | `$album` |
| `AlbumArtist` | `$albumArtist` |
| `Genre` | `$genre` |
| `Composer` | `$composer` |
| `Work` | `$work` |
| `Year` | `$year` |
| `DiscNumber` | `$discNumber` |
| `TotalDiscs` | `$totalDiscs` |
| `TrackNumber` | `$trackNumber` |
| `TotalTracks` | `$totalTracks` |
| `Duration` | `@duration` |
| `SampleRate` | `@sampleRate` |
| `Channels` | `@channels` |
| `BitDepth` | `@bitDepth` |
| `Bitrate` | `@bitrate` |

Fields without a defined variable (e.g., `Tags`, `TechnicalSummary`) do not currently support automated filter generation.

## Presentation Specs

`TrackPresentationSpec` uses `TrackField` directly (file: `app/runtime/TrackPresentationPreset.h`):

```cpp
struct TrackPresentationSpec final
{
  std::string id{};
  TrackGroupKey groupBy = TrackGroupKey::None;
  std::vector<TrackSortTerm> sortBy{};
  std::vector<TrackField> visibleFields{};
  std::vector<TrackField> redundantFields{};

  bool operator==(TrackPresentationSpec const&) const = default;
};
```

`TrackPresentationField` has been fully deleted. No compatibility aliases remain.

## GTK Field UI Model

Files:

```text
app/linux-gtk/track/TrackFieldUi.h
app/linux-gtk/track/TrackFieldUi.cpp
```

GTK field UI definitions are keyed by `rt::TrackField` and provide all display/edit behavior that runtime must not own. The registry is a `static auto const` array (not `constexpr`, because GCC cannot handle lambda-to-function-pointer conversion in `constexpr`).

```cpp
namespace ao::gtk::detail
{
  using Duration = std::chrono::milliseconds;

  // Variant alternatives cover all field value kinds:
  //   std::monostate   for uninitialized / empty
  //   std::string      for text fields (title, track path, codec name, ...)
  //   std::uint16_t    for numeric metadata (year, disc, track numbers)
  //   std::uint32_t    for technical fields (sample rate, bitrate)
  //   std::uint64_t    for FileSize and ModifiedTime
  //   Duration         for duration
  // Comparison operates on raw values (auto-generated by variant).
  using TrackFieldRawValue =
    std::variant<std::monostate, std::string, std::uint16_t, std::uint32_t, std::uint64_t, Duration>;

  // Duration alias.
  using Duration = std::chrono::milliseconds;

  struct TrackFieldEditContext final
  {
    rt::MetadataPatch& patch;
    TrackFieldRawValue const& value;
  };

  using TrackRowTextReader = std::string (*)(TrackRowObject const&, TrackRowCache const&);
  using TrackViewRawReader = TrackFieldRawValue (*)(library::TrackView const&, library::DictionaryStore const&);
  using TrackFieldFormatter = std::string (*)(TrackFieldRawValue const&);
  using TrackFieldPatchWriter = void (*)(TrackFieldEditContext const&);

  struct TrackFieldUiDefinition final
  {
    rt::TrackField field;
    std::int32_t defaultColumnWidth = -1;
    std::string_view dragQueryPrefix{};
    bool columnVisibleByDefault = false;
    bool columnExpands = false;
    bool columnNumeric = false;
    bool columnTagsCell = false;
    bool inlineEditable = false;
    bool propertyDialogEditable = false;
    bool propertyDialogReadonly = false;

    TrackRowTextReader readRowText = nullptr;
    TrackViewRawReader readViewRawValue = nullptr;
    TrackFieldFormatter formatValue = nullptr;
    TrackFieldPatchWriter writePatch = nullptr;
  };
}

namespace ao::gtk
{
  std::span<detail::TrackFieldUiDefinition const> trackFieldUiDefinitions();
  detail::TrackFieldUiDefinition const* trackFieldUiDefinition(rt::TrackField field);
}
```

The registry uses function pointers, not named functions. Lambdas (with no captures) are converted to function pointers via the unary `+` prefix:
```cpp
static auto const defs = std::to_array<TrackFieldUiDefinition>({
  {
    .field = F::Title,
    .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string { ... },
    ...
  },
});
```

Table cells and Properties dialog aggregation intentionally use different read paths:

- table cells read from `TrackRowObject`, because the row cache already owns the table-facing data model;
- Properties dialog aggregation reads raw values from `TrackView`, because mixed-value detection should compare raw values rather than formatted display strings.

A field reader receives the exact source type it supports.

## Column Model

`TrackColumn` has been removed as a semantic enum. No remaining references exist in production code.

GTK columns are controlled by `TrackColumnController` and are keyed by `rt::TrackField`.

Presentation column order comes from `TrackPresentationSpec::visibleFields`. Per-field UI state for manually resized widths lives in `TrackColumnViewState` (file: `app/linux-gtk/track/TrackPresentation.h`):

```cpp
constexpr auto kTrackFieldCount = static_cast<std::size_t>(rt::TrackField::Quality) + 1;

struct TrackColumnViewState final
{
  std::array<std::int32_t, kTrackFieldCount> widths{};

  bool operator==(TrackColumnViewState const&) const = default;
};
```

`TrackColumnDefinition` has been removed. Callers use `TrackFieldUiDefinition` plus runtime `TrackFieldDefinition` directly.

Column ids use `rt::trackFieldId(field)`.
Column titles use `rt::trackFieldDefinition(field)->label`.

`ColumnVisibilityModel` has been fully removed. Visibility is applied directly via `Gtk::ColumnViewColumn::set_visible()` in `TrackColumnController::setupColumns()` (`app/linux-gtk/track/TrackColumnController.cpp:312`).

## Row Value Flow

The table display path is:

```text
Gtk column has TrackField
  -> TrackColumnFactoryBuilder binds row
  -> TrackFieldUiDefinition::readRowText(row, rowCache)
  -> TrackRowObject / TrackRowCache / DictionaryStore as needed
```

`TrackRowObject::getColumnText(TrackColumn)` has been removed.

`TrackRowObject` stores raw table-facing values for registry readers. Field-specific display strings are produced by `TrackFieldUiDefinition::readRowText` / `formatValue`; legacy cached strings may remain only as row-local implementation details and are not the column dispatch surface.

## Inline Editing

Inline editing is field-capability driven. Implemented rules:

1. A field can be inline edited only if `TrackFieldUiDefinition::inlineEditable` is true and `writePatch` is not null.
2. Commit compares old display value to the new editor value through the field definition.
3. Commit writes `rt::MetadataPatch` through `writePatch`.
4. Optimistic row updates are field-based -- no `Title`/`Artist`/`Album` branches remain.
5. Row reactivity uses invalidate-and-reload for fields that cannot be updated optimistically.

## Properties Dialog

`TrackPropertiesDialog` does not own one member per metadata field. It uses a dynamic vector of `FieldEditor` structs (file: `app/linux-gtk/tag/TrackPropertiesDialog.h`):

```cpp
struct FieldEditor final
{
  rt::TrackField field;
  Gtk::Widget* widget = nullptr;
  bool mixed = false;
  detail::TrackFieldRawValue originalRawValue{};
};

std::vector<FieldEditor> _editors;
std::vector<FieldEditor> _readonlyRows;
```

The dialog builds rows by filtering the GTK field UI registry:

- `propertyDialogEditable` for editable metadata rows
- `propertyDialogReadonly` for technical and synthetic read-only rows
- `TrackField::Tags` is explicitly skipped in the metadata tab (handled by `TagEditController`), as seen in `app/linux-gtk/tag/TrackPropertiesDialog.cpp:119`

Mixed-value aggregation is generic and based on raw field values read via `readViewRawValue`.

Save is generic: iterates editable editors and calls `writePatch` for non-mixed changed values.

Widget creation is centralized in helpers (`createEditorWidget`, `createReadonlyWidget`) that map `TrackFieldValueKind` to GTK widgets (`Text` to `Gtk::Entry`, numeric kinds to `Gtk::SpinButton`, and read-only technical values to `Gtk::Label`).

## Synthetic Fields

Synthetic fields are first-class registry entries.

Examples:

- `DisplayTrackNumber`: displays `disc-track` when total discs and disc number require it.
- `TechnicalSummary`: displays a compact summary such as `FLAC · 96 kHz · 24 bit`.
- `Quality`: reserved for runtime playback quality or imported technical quality once available; it is not marked presentable until a real row reader exists.

Synthetic fields may be presentable and read-only when they have a real display reader. They must not have `writePatch`.

Fields can be editable without being visible in built-in table presentations. For example, `TotalDiscs` and `TotalTracks` are Properties-dialog metadata fields and may be presentable for custom columns, but they should not appear in the default built-in column sets unless a presentation explicitly asks for them.

## Drag-To-Query

Drag query generation uses `dragQueryPrefix` from the GTK field UI definition (`app/linux-gtk/track/TrackFieldUi.cpp`).

| Field | Prefix |
| --- | --- |
| Artist | `$a=` |
| Album | `$al=` |
| Genre | `$g=` |

If a field is draggable but has no prefix, drag content generation returns null.

## Verified Invariants

All invariants have been verified and pass:

1. `TrackPresentationSpec::visibleFields` stores `rt::TrackField`. **VERIFIED**
2. GTK column state stores `rt::TrackField`. **VERIFIED**
3. `TrackPresentationField` no longer exists -- zero `rg` matches in `app/ test/`. **VERIFIED**
4. `TrackColumn` no longer exists as a semantic enum -- zero `rg` matches in `app/ test/`. **VERIFIED**
5. Runtime has no GTK dependencies. **VERIFIED**
6. GTK field display/edit behavior is centralized in `TrackFieldUiDefinition`. **VERIFIED**
7. Properties dialog rows are generated from field definitions. **VERIFIED**
8. Inline editing and drag-to-query are field-capability driven. **VERIFIED**
9. Technical and synthetic fields can be columns without becoming editable metadata. **VERIFIED**
10. No feature code performs repeated field dispatch outside the central registries. **VERIFIED**

## Related Files

| File | Description |
| --- | --- |
| `app/runtime/TrackField.h` | `TrackField` enum, `TrackFieldDefinition`, `TrackFieldCategory`, `TrackFieldValueKind`, free functions |
| `app/runtime/TrackField.cpp` | 25-entry `constexpr std::to_array` registry, `trackFieldFromId`, `trackFieldId`, `trackFieldDefinition` |
| `app/runtime/TrackPresentationPreset.h` | `TrackPresentationSpec` using `std::vector<TrackField>` for `visibleFields`/`redundantFields` |
| `app/runtime/TrackPresentationPreset.cpp` | Built-in presets expressed in `TrackField`, normalization |
| `app/linux-gtk/track/TrackFieldUi.h` | `TrackFieldRawValue` variant (6 alternatives), `TrackFieldUiDefinition`, function pointer typedefs |
| `app/linux-gtk/track/TrackFieldUi.cpp` | 25-entry `static auto const` UI registry with `+`-prefixed lambda readers/writers/formatters |
| `app/linux-gtk/track/TrackPresentation.h` | `kTrackFieldCount`, `TrackColumnViewState` with `std::array<std::int32_t, kTrackFieldCount>`, `TrackColumnLayoutModel` |
| `app/linux-gtk/track/TrackPresentation.cpp` | Helper functions delegating to UI registry, `redundantFieldToColumn` |
| `app/linux-gtk/track/TrackColumnController.h` | Column controller using field-based bindings |
| `app/linux-gtk/track/TrackColumnController.cpp` | `setupColumns()` applies `set_visible()` directly per column |
| `app/linux-gtk/track/TrackColumnFactoryBuilder.h` | Field-based column factory builder |
| `app/linux-gtk/track/TrackColumnFactoryBuilder.cpp` | Field-based cell factories, drag, inline edit |
| `app/linux-gtk/track/TrackViewPage.h` | Track view page using field-based columns |
| `app/linux-gtk/track/TrackViewPage.cpp` | Inline edit commit and drag using field capabilities |
| `app/linux-gtk/track/TrackRowObject.h` | Row object storing raw values, no `getColumnText` |
| `app/linux-gtk/track/TrackRowObject.cpp` | Row population storing raw values (strings formatted on demand by UI registry) |
| `app/linux-gtk/track/TrackRowCache.h` | Row cache |
| `app/linux-gtk/track/TrackRowCache.cpp` | Row cache implementation |
| `app/linux-gtk/tag/TrackPropertiesDialog.h` | Dynamic `FieldEditor` vector, no per-field widget members |
| `app/linux-gtk/tag/TrackPropertiesDialog.cpp` | Registry-driven Metadata/Properties tab construction, `Tags` skipped in metadata tab (line 119) |
| `test/unit/runtime/TrackFieldTest.cpp` | Runtime registry tests (id round-trip, 25 entries, no duplicates, preset invariants) |
| `test/unit/runtime/TrackPresentationPresetTest.cpp` | Presentation preset tests using `TrackField` |
| `test/unit/linux-gtk/track/TrackPresentationTest.cpp` | GTK presentation tests (column generation, field defaults) |
| `test/unit/linux-gtk/track/TrackRowCacheTest.cpp` | Row cache tests |
| `test/unit/linux-gtk/track/TrackListAdapterTest.cpp` | Adapter tests |

### Deleted Files

| File | Reason |
| --- | --- |
| `app/linux-gtk/track/ColumnVisibilityModel.h` | Replaced by direct `set_visible()` calls in `TrackColumnController` |
| `app/linux-gtk/track/ColumnVisibilityModel.cpp` | Replaced by direct `set_visible()` calls in `TrackColumnController` |
| `app/linux-gtk/track/TrackColumnDefinition.h` | Removed; callers use `TrackFieldUiDefinition` + `TrackFieldDefinition` |
| `TrackPresentationField` (all references) | Replaced by `TrackField` throughout codebase |
| `TrackColumn` enum | Removed as a semantic enum; no remaining references in `app/` or `test/` |
