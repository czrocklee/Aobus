---
id: presentation.text-catalog
---
# Presentation text semantics

## Scope and authority

This page defines how presentation code decides what is authored localized copy
and what remains external data or stable identity. It records independently
useful fallback, formatter, notation, and terminology contracts; it is not a
message-id or formatter API inventory.

The exact typed-id-to-key map is
[`MessageInventory.def`](../../../app/include/ao/i18n/MessageInventory.def), and
[`root.txt`](../../../app/i18n/catalog/root.txt) is the complete English message
authority. Maintained translations are the sparse catalog sources selected by
[`package.lst`](../../../app/i18n/catalog/package.lst). Catalog construction,
locale selection, arguments, and fallback belong to
[interactive localization](localization.md); contributor procedure belongs to
the [localization workflow](../../development/localization.md).

Catalog output is in-process display text. It is never persisted, parsed for
control flow, or used as an aggregation, protocol, query, or document identity.

## Text ownership boundary

Runtime and Core publish typed semantic kinds, stable ids, raw values, and
structured report arguments. UIModel maps those values to messages when the
choice is shared across frontends. A fixed message or a pattern whose caller
already owns all arguments uses its canonical `MessageId`; a named semantic
formatter is warranted when it selects messages from typed state, derives
selectors, combines domain values, or supplies a defined fallback for an open
id.

Frontend helpers may bind arguments or convert UTF-8 to native strings, but do
not duplicate shared semantic selection. GTK, TUI, WinUI, and AppKit use the
same canonical message keys. CLI diagnostics and command output remain outside
the interactive catalog.

The following are data or identity, not translatable copy:

- persisted ids and layout, action, component, property, query, and protocol
  tokens;
- user-authored List and preset names, tags, metadata, and track text;
- paths, URIs, operating-system device descriptions, audio-node names, and
  external application names;
- command strings, shortcut names, and key tokens; and
- already resolved frontend-local completion or notification text.

A UI may show a stable token beside a localized label. A token inserted in a
sentence remains unchanged as an argument, although the translation may reorder
it.

## Mapping and fallback contracts

Closed domain values map exhaustively to canonical messages. Adding one requires
an explicit presentation choice and focused coverage. Where a formatter permits
an invalid closed value, it returns empty text; notification and scan formatters
instead use their documented conservative failure/fallback message. Code must
not expose an enum ordinal or incidental C++ spelling.

Open ids preserve forward compatibility:

- unknown audio backend and profile ids use the stable id as the label and have
  no authored description;
- an unknown audio backend uses the conservative output-device icon kind;
- an unknown built-in track-presentation id yields no built-in presentation;
- unknown extension values in surfaces such as the GTK Layout Editor remain
  unchanged rather than being mistaken for catalog keys; and
- an unrecognized structured notification template uses the generic
  notification fallback, while an unknown scan verdict formats as failure
  without inventing an error reason.

External display values win over authored fallbacks. For example, a provider's
nonempty device description is retained; the catalog supplies a known backend's
device fallback only when that external value is absent. The synthetic empty-id
default device receives the localized System Default label only when the
provider supplies no display name.

Known audio backends use these display labels, device fallbacks, and semantic
icon kinds. These current display mappings are not persisted identities or a
separate versioned API; exact localized descriptions remain catalog-owned.

| Backend id | Display label / short label | Device fallback | Icon kind |
|---|---|---|---|
| `pipewire` | PipeWire / PW | PipeWire | `AudioServer` |
| `alsa` | ALSA / ALSA | none | `OutputDevice` |
| `wasapi` | WASAPI / WASAPI | localized WASAPI render endpoint | `OutputDevice` |
| `coreaudio` | Core Audio / Core Audio | localized Core Audio output device | `OutputDevice` |

Unknown backends use their stable id as label and short label, and use `OutputDevice` as the conservative icon kind.

Feature formatters consume structured values rather than parsing display text:
track/group fields and missing-value kinds, completion detail roles, playback
reports, library-task kinds, scan outcomes, authoring results, audio quality,
and volume state all follow this rule. Raw subjects, names, reasons, paths, and
metadata pass through as arguments. Text such as `Scanning: literal.flac`
cannot select a progress kind.

Important formatter semantics include:

- a zero-track selection summary is empty; a nonzero summary includes duration
  only when the supplied preformatted duration is nonempty;
- channel counts map one to Mono, two to Stereo, larger nonzero values to the
  plural count message, and zero to empty at the field boundary;
- completion `ResolvedText` passes through unchanged and frequency is decimal;
- playback report subjects fall back from nonempty subject, to `track <id>`, to
  the localized playback subject, and an empty required failure reason becomes
  localized unknown error;
- notification grouping is selected from typed severity and count;
- scan change counts are combined from structured relinked/missing values;
- Smart List membership distinguishes direct tag-backed from computed membership
  before selecting its message;
- an empty local Smart List expression previews the inherited source (the library
  for All Tracks), distinguishing no tracks from showing that source. A nonempty
  expression instead distinguishes invalid filters, no matches, all matches,
  and a bounded first-results preview;
- List-membership results select messages from operation and typed authoring
  status, distinguishing whether stored manual-order positions were forgotten; and
- muted volume takes precedence over hardware-assisted state when choosing the
  tooltip selector.

The source mappings live in
[`TrackPresentationText.cpp`](../../../app/uimodel/library/presentation/TrackPresentationText.cpp),
[`PlaybackCommandText.cpp`](../../../app/uimodel/playback/command/PlaybackCommandText.cpp),
[`PlaybackOutputText.cpp`](../../../app/uimodel/playback/output/PlaybackOutputText.cpp),
[`ListMembershipAuthoringSession.cpp`](../../../app/uimodel/library/track/ListMembershipAuthoringSession.cpp),
and
[`ActivityPresentationText.cpp`](../../../app/uimodel/status/activity/ActivityPresentationText.cpp).
Those sources, not a duplicate table here, own exact enum/message and id/message
associations.

## Locale-neutral formatting

Localization applies to words and sentence grammar, not automatically to every
number-like value. The current track-field boundary keeps ASCII digits, clock
durations, ISO-style dates, decimal punctuation, codec symbols, and fixed
`Hz`, `kbps`, `KB`/`MB`/`GB`, `kHz`, and `-bit` notation locale-neutral. Group
years and completion frequencies are decimal text.

Audio-quality messages likewise receive preformatted sample rates, bit and
channel counts, precision markers, gain values, and established `Hz`, `kHz`,
and `dB` symbols. PCM container bits and source track bit depth remain distinct
values even when both use `-bit` notation.

These are compatibility rules for the current formatters, not a global rule for
future numeric presentation. Do not broaden or localize them during unrelated
copy changes.

## Chinese metadata terminology

Aobus uses these terms consistently in its maintained Chinese catalogs:

| English concept | `zh_Hans` | `zh_Hant` |
|---|---|---|
| Metadata | 元数据 | 後設資料 |
| Custom Metadata | 自定义元数据 | 自訂後設資料 |

Use the same concept across GTK, TUI, WinUI, and AppKit while allowing sentence
grammar to vary. This applies to headings, editor tabs, export-mode copy, and
future messages for the same concepts. Do not substitute broader Track Detail
or Properties labels, rename Audio Properties to match it, or treat Tags as the
same concept. The export qualifier must continue to describe the
[YAML transfer contract](../library/yaml-transfer.md), which includes curated
text, custom metadata, Tags, and Lists.

In `zh_Hant`, the Aobus domain concept List uses **列表**; **播放清單** names the
narrower Playlist concept. Use 列表 in export descriptions, navigation labels,
and List errors when the broader object is meant. The shared `zh_Hant` catalog
serves requests including `zh-TW` and `zh-HK`; it does not promise each region's
preferred vocabulary.

[Decision 0018](../../decision/0018-chinese-metadata-terminology.md) records the
terminology rationale. The policy never changes message identities or rewrites
user-supplied metadata names and values.

## Compatibility and test authority

Changing catalog copy changes visible behavior but does not migrate stored
state. Open-id fallbacks and the locale-neutral formatting boundaries above are
behavioral compatibility contracts. Exact message ids, patterns, translations,
and argument signatures remain source-owned.

Representative tests are
[`PresentationTextFeaturesTest.cpp`](../../../test/unit/uimodel/presentation/PresentationTextFeaturesTest.cpp)
for semantic selection and open-id fallback,
[`TrackFieldFormatterTest.cpp`](../../../test/unit/uimodel/field/TrackFieldFormatterTest.cpp)
for field notation, and
[`AudioQualityFormatterTest.cpp`](../../../test/unit/uimodel/playback/quality/AudioQualityFormatterTest.cpp)
for complete localized audio messages and external-value pass-through. Use the
[localization workflow](../../development/localization.md) for required
validation.
