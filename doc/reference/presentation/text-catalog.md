---
id: presentation.text-catalog
type: reference
status: current
domain: presentation
summary: Enumerates the shared UIModel presentation-text catalog, its semantic inputs, exact English output, and open-id fallbacks.
---
# Presentation text catalog

## Scope and version

This reference enumerates the current built-in English `ao::uimodel::PresentationTextCatalog`.
The catalog is an in-process, platform-neutral presentation surface with no persisted format version.
Stable ids remain owned by their runtime or Core domains; catalog output is display text and is never persisted, parsed for control flow, or used as an aggregation key.

The catalog is currently a zero-state immutable value.
Default construction selects the only shipped English vocabulary without introducing a locale service, registry, or localization-file format.

## Code boundary

The catalog belongs to UIModel under `app/include/ao/uimodel/presentation/` and `app/uimodel/presentation/`.
Runtime and Core publish stable ids, typed semantic kinds, raw values, and structured arguments.
GTK and TUI consume catalog output and map semantic icon kinds to native icons or terminal rendering.

The following text is not catalog copy:

- persisted stable ids and query-language tokens;
- user-authored list, preset, tag, and metadata text;
- filesystem paths and operating-system device names or descriptions;
- diagnostic error text and command-scoped CLI output; and
- explicitly resolved frontend-local notification or completion text.

## Surface

`PresentationTextCatalog` exposes these lookups:

| Member | Semantic input | Result |
|---|---|---|
| `trackFieldLabel` | `rt::TrackField` | non-owning field label |
| `trackGroupKeyLabel` | `rt::TrackGroupKey` | non-owning group-key label |
| `missingTrackValueLabel` | `rt::MissingTrackValueKind` | non-owning placeholder |
| `builtinTrackPresentation` | stable preset id | optional label and description |
| `createCustomTrackPresentationLabel` | none | non-owning action label |
| `audioBackend` | `audio::BackendId` | owning label, description, short label, device-description fallback, and semantic icon kind |
| `audioProfile` | `audio::ProfileId` | owning label and description |
| `systemDefaultOutputDeviceLabel` | none | non-owning device label |
| `completionDetail` | `rt::CompletionDetail` | owning detail text |
| `notificationMessage` | `rt::NotificationMessage` | owning resolved message |
| `libraryTaskProgressDetail` | typed progress kind and subject | owning detail text |
| `libraryTaskProgressCompact` | typed progress kind and subject | owning compact text |

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
| `list-order` | List Order | Flat list preserving source order. |
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

| Backend id | Label | Description | Short label | Device-description fallback | `AudioIconKind` |
|---|---|---|---|---|---|
| `pipewire` | PipeWire | Modern Linux audio server with low latency | PW | PipeWire | `AudioServer` |
| `alsa` | ALSA | Advanced Linux Sound Architecture (Direct Hardware Access) | ALSA | empty | `OutputDevice` |
| `wasapi` | WASAPI | Windows Audio Session API | WASAPI | WASAPI render endpoint | `OutputDevice` |

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

## Compatibility and fallback

Catalog text is not a persistence or IPC identity.
Changing English copy is a user-visible presentation change and requires corresponding catalog and adapter tests, but it does not migrate stored state.
Stable runtime/Core ids retain their existing compatibility owners.

Closed semantic sets fail at build or focused tests when an unhandled value is added.
Open backend/profile ids remain usable through id-only fallback; Aobus does not accept an unclassified provider-authored marketing name in the Core descriptor.

## Implementation authority

- [`PresentationTextCatalog.h`](../../../app/include/ao/uimodel/presentation/PresentationTextCatalog.h)
- [`PresentationTextCatalog.cpp`](../../../app/uimodel/presentation/PresentationTextCatalog.cpp)
- [`TrackGroupHeadingPresentation.cpp`](../../../app/uimodel/library/presentation/TrackGroupHeadingPresentation.cpp)

## Test authority

- [`PresentationTextCatalogTest.cpp`](../../../test/unit/uimodel/presentation/PresentationTextCatalogTest.cpp) protects closed-set coverage, exact representative copy, open-id fallbacks, typed report expansion, and kind-based progress selection.
- Runtime projection, completion, playback, and notification tests protect the structured inputs before catalog resolution.
- GTK and TUI adapter tests protect consumption without moving native vocabulary into UIModel.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Track presentation presets](track-preset.md)
- [Runtime track field catalog](../library/model/track-field.md)
- [Activity-status surface](activity-status.md)
- [Notification model](../reporting/notification.md)
