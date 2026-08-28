---
id: presentation.text-catalog
type: reference
status: current
domain: presentation
summary: Enumerates the shared UIModel text surface and the interactive localization catalog's exact ids, arguments, assets, and lifetimes.
---
# Presentation text catalogs

## Scope and version

This reference enumerates the locale-selected `ao::i18n::MessageCatalog`, the fail-closed `requiredText` / `requiredFormat` helpers, and the feature-local formatters that map domain values onto that catalog.
They are in-process presentation surfaces with no persisted format version.
Stable ids remain owned by their runtime or Core domains; catalog output is display text and is never persisted, parsed for control flow, or used as an aggregation key.

`MessageCatalog` is the immutable startup-selected ICU catalog.
`requiredText` and `requiredFormat` are the required-message resolvers over that catalog: fixed messages use typed `MessageId` values, while named functions remain only where UIModel must map a domain value or derive message selectors.
The canonical id/key inventory is [`MessageInventory.def`](../../../app/include/ao/i18n/MessageInventory.def).

## Code boundary

The shared typed catalog belongs to UIModel under `app/include/ao/uimodel/presentation/` and `app/uimodel/presentation/`.
The interactive facade belongs to the presentation leaf under `app/include/ao/i18n/` and `app/i18n/`.
Runtime and Core publish stable ids, typed semantic kinds, raw values, and structured arguments.
GTK, TUI, and WinUI composition roots own the locale-selected facade; CLI does not link it.
Each root injects the process `MessageCatalog`; copies share immutable backing storage.
GTK and TUI resolve shell copy through canonical `MessageId` values on that catalog; WinUI resolves generated shell resources through its configured MRT context.
Text returned as `std::string_view` remains valid for the lifetime of the source catalog value; state that crosses that lifetime owns a copy.

The following text is not catalog copy:

- persisted stable ids and query-language tokens;
- user-authored list, preset, tag, and metadata text;
- filesystem paths and operating-system device names or descriptions;
- diagnostic error text and command-scoped CLI output; and
- explicitly resolved frontend-local notification or completion text.

## Interactive localization surface

`MessageCatalog` exposes:

| Member/type | Exact surface |
|---|---|
| `create(localeTag)` | constructs from one explicit strict BCP 47 UTF-8 tag |
| `createForSystemLocale()` | constructs from the operating-system locale, with English fallback when no locale is available |
| `requestedLocale()` | canonical requested tag borrowed for the lifetime of that catalog or a copy sharing its implementation |
| `text(id)` | borrowed UTF-8 text for a message with no runtime arguments |
| `format(id, arguments)` | owned UTF-8 text plus the locale that supplied the selected message |
| `MessageArgumentValue` | `std::string_view`, `std::int64_t`, `std::uint64_t`, or `double` |
| `MessageArgument` | UTF-8 argument name plus a string, integral, or floating value deduced by its constructor |
| `ResolvedMessage` | owned `text` and owned resolved `locale` |

Argument string views are consumed only during `format()` and need not outlive the call.
Boolean and enum arguments are not admitted implicitly; selectors must be named text values and domain enums must be mapped deliberately.
Unsigned values must fit the ICU signed 64-bit integer range.
`text()` rejects argument-bearing messages and its returned view remains valid for the lifetime of the catalog or a copy sharing its implementation.
`format()` returns no borrowed formatted text.

The original integration-probe ids and canonical patterns are:

| `MessageId` | Key | English root pattern | Neutral German override | Signature |
|---|---|---|---|---|
| `PilotLibraryTitle` | `pilot_library_title` | `Music library` | `Musikbibliothek` | none |
| `PilotGreeting` | `pilot_greeting` | `Welcome, {name}` | `Willkommen, {name}` | `name: value` |
| `PilotCopySummary` | `pilot_copy_summary` | `Copied {count} tracks to {destination}` | `Nach {destination} wurden {count} Titel kopiert` | `count: value`, `destination: value` |
| `PilotTrackCount` | `pilot_track_count` | `{count, plural, one {# track} other {# tracks}}` | `{count, plural, one {# Titel} other {# Titel}}` | `count: plural number` |
| `PilotPlaybackState` | `pilot_playback_state` | `{state, select, playing {Playing} paused {Paused} other {Stopped}}` | `{state, select, playing {Wiedergabe} paused {Pausiert} other {Gestoppt}}` | `state: select text` |
| `PilotRegionalFallback` | `pilot_regional_fallback` | `English regional fallback` | `Deutscher Sprach-Fallback` | none |
| `PilotEnglishFallback` | `pilot_english_fallback` | `English message fallback` | absent by design | none |
| `PilotPseudoProbe` | `pilot_pseudo_probe` | `Open {application} music library` | `Öffne die Musikbibliothek von {application}` | `application: value` |

The production shared-id families are:

| Key family | Typed owner |
|---|---|
| `track_field_*`, `track_group_none`, `missing_track_*` | track-field, group, and missing-value labels |
| `track_presentation_*`, `track_presentation_description_*`, `create_custom_track_presentation` | built-in presentation copy, picker label, and shared eligibility reason |
| `audio_backend_*`, `audio_profile_*`, `system_default_output_device` | built-in audio presentation |
| `completion_*` | completion semantic roles |
| `notification_*` | structured playback reports |
| `library_task_*`, `library_scan_*` | structured progress and scan outcomes |
| `library_*`, `track_count`, `track_selection_summary` | shared library state, List fallback labels, and count presentation |
| `track_filter_error` | query diagnostic wrapper |
| `smart_list_*` | smart-List membership and preview state |
| `list_order_*`, `list_membership_*` | manual-order capability/results and Playlist-membership results |
| `track_channel_*`, `track_technical_unknown` | language-bearing track-field formatting |
| `playback_*` | shared now-playing, transport, action, volume, and audio-pipeline copy |
| `audio_node_*`, `audio_format`, `audio_channels_compact` | audio-pipeline node and format presentation |
| `audio_finding_*`, `audio_quality_*` | audio-quality findings and conclusions |
| `gtk_shell_*`, `gtk_playback_*`, `gtk_library_*`, `gtk_list_*`, `gtk_smart_list_*` | GTK shell, playback, and library/List copy through canonical `MessageId` values |
| `gtk_preferences_*`, `gtk_shortcut_*`, `gtk_action_*`, `gtk_custom_view_*`, `gtk_presentation_*` | GTK preferences, shortcut/action, and presentation-editor copy through direct typed ids |
| `gtk_track_*`, `gtk_tag_*`, `gtk_custom_metadata_*`, `gtk_activity_*`, `gtk_manual_order_*` | GTK metadata/property editing, activity, and authoring accessibility copy |
| `gtk_layout_*`, `gtk_edit_value`, `gtk_*_panel`, `gtk_*_details`, `gtk_startup_*` | GTK Layout Editor vocabulary, structural accessibility, and recoverable startup copy |
| `tui_shell_*`, `tui_playback_*`, `tui_library_*` | TUI navigation, help, playback, and library copy through canonical `MessageId` values |
| `tui_presentation_*`, `tui_*_opened`, `tui_*_closed`, `tui_*_failed` | TUI presentation-navigation, accessibility state, and recoverable-error copy |
| `winui_shell_*`, `winui_playback_*`, `winui_library_*`, `winui_*_failed` | WinUI shell, playback, library, native tooltip, empty-state, and recoverable-error copy through MRT |

[`MessageInventory.def`](../../../app/include/ao/i18n/MessageInventory.def) is the exact typed-id-to-key map. `MessageCatalog.h` and `MessageIds.h` include it to emit enumerators and definition entries.
The complete English patterns and maintained localized overrides live in [`root.txt`](../../../app/i18n/catalog/root.txt), [`de.txt`](../../../app/i18n/catalog/de.txt), [`zh_Hans.txt`](../../../app/i18n/catalog/zh_Hans.txt), [`zh_Hant.txt`](../../../app/i18n/catalog/zh_Hant.txt), [`ja.txt`](../../../app/i18n/catalog/ja.txt), [`es.txt`](../../../app/i18n/catalog/es.txt), and [`fr.txt`](../../../app/i18n/catalog/fr.txt); generated WinUI resources use those same keys.

Bare `value` and simple-format arguments accept any `MessageArgumentValue` alternative.
Plural, select-ordinal, and choice arguments accept integer or double values; select arguments accept text.
Arguments are order-independent but must contain every expected name exactly once and no unknown name.

The authored asset set is complete English ICU `root` and maintained locale catalogs (`de`, `zh_Hans`, `zh_Hant`, `ja`, `es`, `fr`).
Build tooling derives `qps_Ploc` by transforming literal spans and maps those assets to WinUI PRI qualifiers `en`, `de`, `zh-Hans`, `zh-Hant`, `ja`, `es`, `fr`, and `qps-ploc`.
MRT resolves `zh-CN` and `zh-TW` requests through the corresponding script-qualified Chinese resource rather than a duplicate regional asset.
The exact fallback behavior belongs to the [interactive localization specification](../../spec/presentation/localization.md).

### Frontend shell catalogs

GTK and TUI resolve shell copy through the process `MessageCatalog` and canonical `MessageId` values.
There is no separate frontend message-id space.
GTK widget APIs that require owned strings copy `requiredText` at the widget boundary.
TUI argument-bearing chrome (help, hints, footers) is formatted through `tuiChromeText`.

The completed frontend migration owns these families:

| Frontend | Typed or native owner | Exact scope |
|---|---|---|
| GTK | Canonical `MessageId` values through `requiredText` / `requiredFormat` | application menus; shell/playback accessibility copy; library and authoring dialogs; preferences and shortcut/action descriptors; presentation and metadata/property editing; Layout Editor display vocabulary; tooltips, empty states, startup, and recoverable errors |
| TUI | Canonical `MessageId` values through `requiredText` / `requiredFormat` / `tuiChromeText` | workspace and overlay copy, status shortcuts, command metadata, help, playback/output empty states, library navigation/filter status, presentation-navigation qualifiers, accessibility states, and recoverable errors |
| WinUI | generated MRT resources | shell menus/actions, playback/output, library, metadata, tooltips, accessibility, empty states, and recoverable native wrappers |

TUI command strings and key names remain shell identity, not translated copy.
Patterns receive values such as `:view <id>`, `/`, `Enter`, `Esc`, and `Ctrl-L` as named arguments, so a locale may reorder but cannot rewrite them.
GTK Gio action names and WinUI action/component identities likewise remain untranslated.
Layout action labels and categories are display text selected from the catalog; their stable action ids remain the only binding identity.
GTK Layout Editor component/property/enum labels follow the same rule: the serialized token remains the combo-row id, known built-ins receive localized display text, and unknown extension values display unchanged.
WinUI-generated resources normally retain the canonical catalog key. Single-argument native wrappers replace the governed named placeholder with `{0}` under that same key, while XAML-only property resources add the required `.Text` alias.

## Surface

Required lookup and semantic mappings:

| Member | Semantic input | Result |
|---|---|---|
| `requiredText` | fixed `i18n::MessageId` | non-owning required message |
| `requiredFormat` | `i18n::MessageId` plus deduced named arguments | owning required message |
| `trackFieldLabel` | `rt::TrackField` | non-owning field label |
| `trackGroupKeyLabel` | `rt::TrackGroupKey` | non-owning group-key label |
| `missingTrackValueLabel` | `rt::MissingTrackValueKind` | non-owning placeholder |
| `builtinTrackPresentation` | stable preset id | optional label and description |
| `audioBackendPresentation` | `audio::BackendId` | owning label, description, short label, device-description fallback, and semantic icon kind |
| `audioProfilePresentation` | `audio::ProfileId` | owning label and description |
| `completionDetail` | `rt::CompletionDetail` | owning detail text |
| `notificationMessage` | `rt::NotificationMessage` | owning resolved message |
| `notificationGroupMessage` | severity and count | owning severity-selected plural message |
| `libraryTaskProgressDetail` | typed progress kind and subject | owning detail text |
| `libraryTaskProgressCompact` | typed progress kind and subject | owning compact text |
| `formatLibraryScanMessage` | typed scan outcome | owning result text |
| `trackSelectionSummary` | count and optional duration | owning selection summary with derived selectors |
| `smartListMembershipEditingText` | membership kind and optional expression | owning membership explanation |
| `smartListPreviewStatus` | validity, source, and count state | owning preview status |
| `formatListMembershipEditNotification` | typed List-membership edit result | owning Playlist-membership result |
| `trackChannelText` | nonzero channel count | owning lexical or pluralized text |
| `transportControlLabel` | typed playback command | non-owning control label and tooltip |
| `playbackActionLabel` | typed playback command | non-owning action-list label |
| `volumeTooltip` | percent plus muted/hardware state | owning volume presentation |

Fixed one-id values such as `LibraryAllTracks` use `requiredText(catalog, id)` directly.
One-pattern values whose callers already own the arguments, such as `TrackCount` or `TrackFilterError`, use `requiredFormat(catalog, id, {{"name", value}})` directly.
A semantic method is retained only when it maps a domain object to one or more ids, selects an id from state, derives hidden select arguments, or applies a documented open-id fallback.

Invalid values in a closed enum return empty text.
The implementation uses exhaustive switches, so adding a closed value requires an explicit catalog decision.

## Track fields

Labels follow `TrackField` raw order:

| Field | Label | Field | Label |
|---|---|---|---|
| `Title` | Title | `Artist` | Artist |
| `Album` | Album | `AlbumArtist` | Album Artist |
| `Genre` | Genre | `Composer` | Composer |
| `Conductor` | Conductor | `Ensemble` | Ensemble |
| `Work` | Work | `Movement` | Movement |
| `Soloist` | Soloist | `Year` | Year |
| `DiscNumber` | Disc | `DiscTotal` | Total Discs |
| `TrackNumber` | Track | `TrackTotal` | Total Tracks |
| `MovementNumber` | Movement No. | `MovementTotal` | Total Movements |
| `Duration` | Duration | `Tags` | Tags |
| `FilePath` | File Path | `Codec` | Codec |
| `SampleRate` | Sample Rate | `Channels` | Channels |
| `BitDepth` | Bit Depth | `Bitrate` | Bitrate |
| `FileSize` | File Size | `ModifiedTime` | Modified |
| `DisplayTrackNumber` | Track # | `TechnicalSummary` | Technical |
| `Quality` | Quality |  |  |

Group-key labels are `None`, `Artist`, `Album`, `Album Artist`, `Genre`, `Composer`, `Conductor`, `Ensemble`, `Work`, and `Year` for the corresponding `TrackGroupKey` values.

### Track-field lexical formatting

`TrackFieldFormatter` receives the injected `MessageCatalog` for language-bearing values only.
Channel counts resolve to `Mono`, `Stereo`, or the catalog's plural `count` pattern; zero remains empty.
The technical placeholder resolves through `requiredText(catalog, MessageId::TrackTechnicalUnknown)` when that placeholder is requested.

ASCII digits, clock durations, ISO-style dates, decimal punctuation, codec symbols, and the fixed `Hz`, `kbps`, `KB`/`MB`/`GB`, `kHz`, and `-bit` unit syntax remain locale-neutral in this tranche.

## Missing group values

Runtime group headings retain `std::string`, `std::uint16_t`, absence, or one of these structured missing kinds:

| Kind | Text |
|---|---|
| `Artist` | Unknown Artist |
| `Album` | Unknown Album |
| `Year` | Unknown Year |
| `Genre` | Unknown Genre |
| `Composer` | Unknown Composer |
| `Conductor` | Unknown Conductor |
| `Ensemble` | Unknown Ensemble |
| `Work` | Unknown Work |

`formatTrackGroupHeading` converts all three heading slots at the UIModel boundary.
An absent slot remains empty, text is copied unchanged, and a numeric year is formatted in decimal.

## Built-in track presentations

| Stable id | Label | Description |
|---|---|---|
| `library` | Library | All tracks in album-artist and album order. |
| `list-order` | Manual Order | Arrange tracks in your own order. |
| `songs` | Songs | Flat list of every track ordered by title. |
| `albums` | Albums | Grouped by album with track-oriented columns. |
| `artists` | Artists | Grouped by album artist with discography ordering. |
| `performers` | Performers | Grouped by track artist, including featured guests. |
| `genres` | Genres | Grouped by genre. |
| `years` | Years | Grouped by year. |
| `classical-composers` | Classical: Composers | Grouped by composer with work-oriented columns. |
| `classical-conductors` | Classical: Conductors | Grouped by conductor with work and ensemble columns. |
| `classical-works` | Classical: Works | Grouped by work with composer-oriented columns. |
| `tagging` | Tagging | Flat list with raw disc/track, genre, year, and tags for curation. |
| `technical` | Technical | Flat list of codec, bitrate, size, and path for file inspection. |

An unknown built-in id returns no value.
Custom-preset labels remain user-authored data owned by the workspace.
The custom-view action label is `Create Custom View...`.

## Audio presentation

Core `BackendDescriptor` and `ProfileDescriptor` retain only stable ids and supported-profile structure.
Operating-system device facts remain raw external data.

Playback presentation also resolves shared now-playing states, transport controls, volume state, and audio-quality semantics through this catalog.
`AudioQualityFormatter` receives structured node formats and quality findings and returns complete localized messages; it does not expose message ids to frontends.
Sample rates, bit counts, channel counts, precision markers, gain values, and the established `Hz`, `kHz`, and `dB` symbols are preformatted locale-neutral arguments in this tranche.
Node names, device names, track metadata, and external application names pass through byte-for-byte as message arguments.

| Backend id | Label | Description | Short label | Device-description fallback | `AudioIconKind` |
|---|---|---|---|---|---|
| `pipewire` | PipeWire | Modern Linux audio server with low latency | PW | PipeWire | `AudioServer` |
| `alsa` | ALSA | Advanced Linux Sound Architecture (Direct Hardware Access) | ALSA | empty | `OutputDevice` |
| `wasapi` | WASAPI | Windows Audio Session API | WASAPI | WASAPI render endpoint | `OutputDevice` |
| `coreaudio` | Core Audio | macOS shared audio output through Core Audio | Core Audio | Core Audio output device | `OutputDevice` |

| Profile id | Label | Description |
|---|---|---|
| `shared` | Shared Mode | System-level mixing with other applications |
| `exclusive` | Exclusive Mode | Direct access to the hardware device |

Unknown backend and profile ids use their stable id as the label.
Unknown backends also use that id as the short label, have no description, and use `OutputDevice` as the conservative semantic icon fallback.
The synthetic empty-id default device is labeled `System Default` only when the provider supplies no external display name.
An external device description wins; otherwise the known backend's catalog fallback is used.

## Completion details

`CompletionItem` retains display and insertion syntax, rank, and one typed `CompletionDetail`.

| Kind | Result |
|---|---|
| `None` | empty |
| `ResolvedText` | supplied frontend-local text |
| `Field` | `field` |
| `Alias` | `alias` |
| `Operator` | `operator` |
| `LogicalOperator` | `logical operator` |
| `Frequency` | decimal `frequency` value |

Runtime query and metadata completers publish semantic roles or counts.
`ResolvedText` is the explicit escape hatch for a frontend-local completion source that shares the transport type; it is not a runtime authored-copy path.

## Structured playback reports

`NotificationMessage` is either already resolved text or a `NotificationReport` carrying one closed template plus typed `trackId`, `subject`, `detail`, and `count` arguments.
Shared runtime playback producers use structured reports; already resolved text remains available for frontend-local notifications.

| Template | Expansion |
|---|---|
| `PlaybackTrackOpenFailed` | `Could not play <track>: <reason>` |
| `PlaybackDecodeFailed` | `Playback failed for <track>: <reason>` |
| `PlaybackRouteActivationFailed` | `Could not start playback: <reason>` |
| `PlaybackDeviceLost` | `Playback device failed: <reason>` |
| `PlaybackSequenceFinished` | `Playback sequence finished` |
| `PlaybackTracksSkipped` | `Skipped 1 unplayable track` or `Skipped <N> unplayable tracks` |
| `PlaybackStoppedAfterFailures` | `Playback stopped after <N> unplayable tracks` |
| `PlaybackStoppedForTrack` | `Playback stopped`, optionally followed by ` for <subject>` and `: <detail>` |

Track selection prefers non-empty `subject`, then `track <id>`, then `playback`.
An empty failure detail becomes `unknown error` where a failure-reason slot is required.
The initial English catalog uses singular/other plural selection only.

## Library-task progress

`LibraryTaskProgressUpdated` carries a typed kind, fraction, and raw subject.
Text prefixes never select behavior.

| Kind | Detail prefix | Compact output |
|---|---|---|
| `Scanning` | Scanning | Scanning library |
| `Updating` | Updating | Updating library |
| `Fingerprinting` | Fingerprinting | complete detail text |
| `IndexingAudioIdentity` | Indexing audio identity | complete detail text |

Detail output is the prefix alone for an empty subject and `<prefix>: <subject>` otherwise.
The subject is raw operation data; text such as `Scanning: literal.flac` remains a subject and does not alter the typed kind.

## Argument-bearing shared messages

The typed resolver supplies these internal message arguments:

| Message family | Arguments |
|---|---|
| track label | `id` |
| playback open/decode failure | `track`, `reason` |
| route/device failure | `reason` |
| skipped/stopped count | `count` |
| stopped-for-track | `hasSubject`, `hasDetail`, `subject`, `detail` |
| progress detail | `hasSubject`, `activity`, `subject` |
| relinked, missing, or unreadable scan result | `count` |
| combined scan changes | `relinked`, `missing` |
| scan result with optional changes/error | `hasChanges`, `changes` or `hasError`, `error` |
| track count | `count` |
| selection summary | `count`, `hasDuration`, `duration` |
| smart-List membership | `expression` when direct |
| smart-List source/preview state | `source`, `count`, or `visible` and `count`, according to state |
| manual-order result | `count` |
| Playlist membership result | `list`, `tag`, and `count`; removal with positions uses `trackCount` and `positionCount` |
| import/export failure | `error` |
| filter error | `diagnostic` |
| general channel count | `count` |
| shortcut conflict prompt | `chord`, `owner` |
| shortcut conflict summary | `chords` |
| shortcut removal tooltip | `chord` |
| shortcut capture hint | `key` |
| custom-presentation copy label | `label` |
| TUI custom-presentation detail or badge | `id` |

The `has*` arguments are closed `yes`/`no` selectors supplied by UIModel; they are not translated values.

## Compatibility and fallback

Catalog text is not a persistence or IPC identity.
Changing English copy is a user-visible presentation change and requires corresponding catalog and adapter tests, but it does not migrate stored state.
Stable runtime/Core ids retain their existing compatibility owners.

Closed semantic sets fail at build or focused tests when an unhandled value is added.
Open backend/profile ids remain usable through id-only fallback; Aobus does not accept an unclassified provider-authored marketing name in the Core descriptor.

## Implementation authority

- [`MessageInventory.def`](../../../app/include/ao/i18n/MessageInventory.def)
- [`MessageCatalog.h`](../../../app/include/ao/i18n/MessageCatalog.h)
- [`MessageIds.h`](../../../app/i18n/MessageIds.h)
- [`PresentationText.h`](../../../app/include/ao/uimodel/presentation/PresentationText.h)
- [`PresentationText.cpp`](../../../app/uimodel/presentation/PresentationText.cpp)
- [`TrackGroupHeadingPresentation.cpp`](../../../app/uimodel/library/presentation/TrackGroupHeadingPresentation.cpp)
- [`root.txt`](../../../app/i18n/catalog/root.txt), [`de.txt`](../../../app/i18n/catalog/de.txt), [`zh_Hans.txt`](../../../app/i18n/catalog/zh_Hans.txt), [`zh_Hant.txt`](../../../app/i18n/catalog/zh_Hant.txt), [`ja.txt`](../../../app/i18n/catalog/ja.txt), [`es.txt`](../../../app/i18n/catalog/es.txt), and [`fr.txt`](../../../app/i18n/catalog/fr.txt)
- [`GtkTextCatalog.h`](../../../app/linux-gtk/i18n/GtkTextCatalog.h) and [`GtkTextCatalog.cpp`](../../../app/linux-gtk/i18n/GtkTextCatalog.cpp)
- [`TuiTextCatalog.h`](../../../app/tui/TuiTextCatalog.h) and [`TuiTextCatalog.cpp`](../../../app/tui/TuiTextCatalog.cpp)
- [`WinUiResourceProjection.h`](../../../app/i18n/WinUiResourceProjection.h)
- [`ShellBuilder.cpp`](../../../app/windows-winui/layout/ShellBuilder.cpp)

## Test authority

- [`PresentationTextTest.cpp`](../../../test/unit/uimodel/presentation/PresentationTextTest.cpp) protects closed-set coverage, exact representative copy, open-id fallbacks, typed report expansion, and kind-based progress selection.
- [`MessageCatalogTest.cpp`](../../../test/unit/i18n/MessageCatalogTest.cpp) protects typed ids, exact fallback, argument kinds, owned results, pseudo output, and concurrent formatting.
- [`CatalogPatternTest.cpp`](../../../test/unit/i18n/CatalogPatternTest.cpp) protects signature validation and deterministic generated assets.
- [`MenuControllerTest.cpp`](../../../test/unit/linux-gtk/app/MenuControllerTest.cpp) protects typed GTK menu copy and German/pseudo resolution.
- [`RenderTest.cpp`](../../../test/unit/tui/RenderTest.cpp) protects typed TUI shell copy, untranslated key arguments, and constrained-width rendering.
- [`PreferencesWindowTest.cpp`](../../../test/unit/linux-gtk/preference/PreferencesWindowTest.cpp), [`ShortcutEditorWidgetTest.cpp`](../../../test/unit/linux-gtk/preference/ShortcutEditorWidgetTest.cpp), and [`TrackCustomViewDialogTest.cpp`](../../../test/unit/linux-gtk/track/TrackCustomViewDialogTest.cpp) protect the fourth GTK slice.
- [`LayoutEditorTextTest.cpp`](../../../test/unit/linux-gtk/layout/editor/LayoutEditorTextTest.cpp) protects localized built-in layout vocabulary and unchanged extension values.
- [`TrackPresentationPickerViewModelTest.cpp`](../../../test/unit/uimodel/library/presentation/TrackPresentationPickerViewModelTest.cpp) and [`TrackPresentationNavigationTest.cpp`](../../../test/unit/tui/TrackPresentationNavigationTest.cpp) protect shared and TUI presentation copy.
- [`StringResourceTest.cpp`](../../../test/unit/winui/StringResourceTest.cpp) protects generated canonical keys and the bounded platform projection map.
- [`WinUiLocalizationProbe.cpp`](../../../test/helper/WinUiLocalizationProbe.cpp) protects generated WinUI resource selection and formatted parity with ICU.
- Runtime projection, completion, playback, and notification tests protect the structured inputs before catalog resolution.
- GTK and TUI adapter tests protect consumption without moving native vocabulary into UIModel.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Interactive localization](../../spec/presentation/localization.md)
- [Decision 0012: adopt ICU resource catalogs](../../decision/0012-adopt-icu-resource-catalogs.md)
- [Track presentation presets](track-preset.md)
- [Runtime track field catalog](../library/model/track-field.md)
- [Activity-status surface](activity-status.md)
- [Notification model](../reporting/notification.md)
