---
id: rfc.0030.structured-presentation-vocabulary
type: rfc
status: implemented
domain: presentation
summary: Replaced shared authored display strings in Core/runtime with stable semantic values, structured arguments, and one typed UIModel English catalog.
depends-on: none
---
# RFC 0030: Structured presentation vocabulary

## Disposition

Implemented on 2026-07-19.

The implementation:

- adds a zero-state immutable `PresentationTextCatalog` as the single owner of the initial shared English vocabulary, without selecting a locale service, registry, or localization-file format;
- removes field labels and built-in preset labels/descriptions from runtime structural catalogs while preserving every stable id and persisted presentation shape;
- keeps runtime group-heading slots as raw text, numeric years, absence, or exhaustive missing-value kinds until UIModel resolves them;
- reduces Core backend/profile descriptors to machine ids and supported-profile structure, preserves operating-system device facts as external data, and maps known ids plus unknown-id fallback in UIModel;
- replaces native audio icon names below GTK with `AudioIconKind` and maps that semantic value at the GTK adapter;
- replaces completion detail prose with typed roles, frequency arguments, and an explicit resolved-text escape hatch for frontend-local completion sources;
- removes the inert notification template field, carries shared playback reports as closed templates plus typed arguments through immutable feed snapshots, and resolves them once in the activity projection;
- replaces library-progress prefix parsing with `LibraryTaskProgressKind`, a raw subject, and catalog-selected compact/detail text;
- adds a current [presentation text catalog reference](../reference/presentation/text-catalog.md), authored-copy classification guidance, and focused closed-set/fallback tests; and
- preserves stable ids, query syntax, user-authored text, metadata, paths, external device facts, diagnostics, and CLI contracts under their existing owners.

The concrete catalog is deliberately default-constructible and stateless while one vocabulary exists.
View models may own that value without synchronization or lifetime coupling; a future second vocabulary must preserve the typed coverage before introducing injection or storage machinery.
Unknown backend/profile ids use id-only fallback, and the initial English formatter needs only singular/other plural selection.

The [presentation architecture](../architecture/presentation.md), [playback architecture](../architecture/playback.md), [track expression architecture](../architecture/track-expression.md), [failure and reporting architecture](../architecture/failure-and-reporting.md), and linked specifications/references own current behavior and supersede the proposal language below.
No separate decision record was needed because implementation followed the RFC's semantic-value/catalog boundary without choosing a localization technology.

## Problem

The current layer model assigns platform-neutral presentation policy to UIModel and platform rendering vocabulary to frontends, but authored display text is still embedded across core audio and application runtime values.

[`TrackFieldDefinition`](../../app/include/ao/rt/TrackField.h) combines stable field identity and machine capabilities with about thirty English labels.
Built-in [`TrackPresentationPreset`](../../app/include/ao/rt/TrackPresentation.h) values combine runtime sort/group specifications with English labels and descriptions.
[`LiveTrackListProjection`](../../app/runtime/projection/LiveTrackListProjection.cpp) writes English `Unknown Artist`, `Unknown Album`, `Unknown Year`, and related placeholders into group snapshots instead of retaining the semantic reason that a value is absent.

Core [`BackendProvider::BackendDescriptor`](../../include/ao/audio/BackendProvider.h) carries authored backend descriptions and GTK icon names such as `audio-card-symbolic` and `media-playback-start-symbolic`.
That makes the core audio boundary name one frontend's icon vocabulary and gives runtime no way to distinguish provider facts from application marketing copy.
Operating-system device names and descriptions are legitimate external data; built-in backend prose and toolkit icon names are not.

[`QueryExpressionCompleter`](../../app/runtime/completion/QueryExpressionCompleter.cpp) encodes completion roles as English detail strings such as `field`, `alias`, `operator`, and `logical operator`.
The insertion and display tokens are query syntax and may remain runtime/query data, but the role description is presentation copy.

Reporting exposes the same missing boundary in a more consequential form.
Runtime notification state carries a `templateId`, but the activity projection ignores it and copies already-formatted title, message, action labels, progress labels, and icon names.
The current projection also recognizes library-task kinds by testing whether English text begins with `Scanning:` or `Updating:`.
[RFC 0013](0013-coherent-application-reporting-policy.md) proposes typed report intent, but it needs one concrete presentation-text owner if template ids and typed arguments are to become more than unused fields.

These strings are individually harmless, but the aggregate boundary has four costs:

- changing or translating presentation text requires edits in core/runtime owners that should not know presentation policy;
- GTK and TUI can drift because they receive some preformatted text and independently recreate other text;
- missing-value, completion-role, and report-template semantics are lost before UIModel can format them; and
- toolkit icon names can leak downward because a plain `std::string` does not reveal which layer owns the vocabulary.

Moving strings one at a time would preserve the underlying ambiguity.
The system needs stable semantic ids, structured arguments, and a typed UIModel text catalog before the individual call sites move.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0011](0011-executor-affine-reporting-feed.md), [RFC 0013](0013-coherent-application-reporting-policy.md).

RFC 0013 defines which operation owns a report and which typed facts cross into presentation.
RFC 0011 defines the authoritative feed snapshot that retains accepted report content.
This RFC defines where shared interactive copy and template expansion live; joint implementations must carry report-template ids and argument types through the feed to this catalog boundary without retaining duplicate resolved and unresolved authority.

## Goals

- Keep stable identities, machine capabilities, raw domain values, and structured absence in core/runtime while moving authored shared display copy to UIModel.
- Introduce one composition-owned, immutable UIModel text catalog with typed feature-specific keys rather than a flat process-global string map.
- Preserve query syntax, user-authored labels, metadata, filesystem paths, and operating-system device descriptions as data rather than misclassifying them as application copy.
- Represent missing track-group values, completion roles, backend/profile presentation, and shared report summaries with closed semantic kinds and structured arguments.
- Make GTK and TUI consume the same shared labels and descriptions while retaining platform-specific layout, markup, icon, and accessibility choices.
- Make every closed catalog exhaustive and give open external ids an explicit fallback.
- Prevent localized or rendered text from becoming persisted identity, a recovery switch, an aggregation key, or a protocol token.
- Establish a path to localization without requiring a localization framework in the first implementation.

## Non-goals

- Translate Aobus or select supported locales in this RFC.
- Move command-scoped CLI prose into UIModel; the CLI remains a runtime adapter with its own output contract.
- Replace query-language tokens, metadata values, user-authored custom-preset labels, device names supplied by the operating system, or diagnostic error messages with catalog ids.
- Standardize GTK and TUI typography, icon assets, widgets, terminal glyphs, or layout.
- Create one universal application message bus or permit recovery and control flow to depend on text lookup.
- Move every frontend-only literal into a shared catalog when it has no cross-frontend semantic consumer.

## Proposed design

### Semantic values before text

Every shared presentation input crosses into UIModel as a typed semantic value plus raw arguments.
Stable persisted or protocol ids remain owned by their existing domain authorities and are never replaced by localized text.

The first migration introduces feature-specific values equivalent to:

```text
TrackField                         existing runtime enum and stable id
BuiltInTrackPresentationId         existing stable preset id
MissingTrackValueKind              artist, album, year, genre, composer, ...
CompletionDetailKind               field, alias, operator, logical-operator, frequency
AudioBackendPresentationKey        BackendId plus optional ProfileId
AudioIconKind                      output-device, shared-output, exclusive-output, ...
ReportTemplateId + typed arguments RFC 0013 integration surface
```

Closed semantic sets use enums or exhaustive typed tables.
Extensible ids remain strong or string ids with an explicit unknown-id path.
No caller switches on catalog output text.

### UIModel text catalog

UIModel owns an immutable `PresentationTextCatalog` selected by an interactive composition root.
The initial implementation supplies one built-in English catalog.
Its public surface is typed by feature and returns small presentation values such as a label, description, or formatted message; it does not expose one unstructured `map<string, string>`.

Feature tables remain beside their UIModel capsule and are composed through the catalog facade:

- track-field labels and related short headings live with track presentation policy;
- built-in presentation labels and descriptions live with `TrackPresentationCatalog`;
- missing-value text lives with track-list/group presentation;
- completion detail labels live with completion presentation;
- backend/profile labels, descriptions, and semantic icon kinds live with playback-output presentation; and
- report templates and plural-aware summaries live with reporting presentation when RFC 0013 is implemented.

The catalog is immutable after construction so view models may borrow it without synchronization.
Runtime and core never receive it.
GTK and TUI may use a shared default instance, while a future localized composition may construct another catalog with the same typed coverage.

### Ownership matrix

| Current content | Core/runtime after migration | UIModel owner | Frontend owner |
|---|---|---|---|
| Track-field definition | Stable id, category, value kind, query/sort/group capabilities | Label and short presentation text | Column/header rendering |
| Built-in presentation preset | Stable id and structural `TrackPresentationSpec` | Label and description | Picker/menu rendering |
| Group heading | Sort/group identity, raw values, structured missing kinds | Primary/secondary/tertiary text | Row markup and geometry |
| Audio backend/profile | Stable ids, capabilities, external device facts | Built-in label, description, semantic icon kind | Native icon name or terminal glyph |
| Completion item | Insert/display syntax, rank, typed detail kind and arguments | Detail label/formatting | Popup or command-palette layout |
| Shared operation report | Operation/template id, disposition, typed arguments | User summary, pluralization, action label | Notification widget/panel and platform icon |

### Track fields and presentation presets

`TrackFieldDefinition` drops its authored `label` but retains the stable id and every machine-readable capability used by runtime/query code.
UIModel exposes exhaustive `trackFieldLabel(TrackField)` or equivalent catalog lookup and continues to own sizing, alignment, editor options, and column titles.

Runtime built-in presets retain only structural specs and stable ids.
UIModel joins those specs with catalog text when building `TrackPresentationCatalog` entries.
Custom preset labels remain user-authored persisted data and are never translated or replaced by a built-in catalog entry.

### Structured group headings

Runtime grouping must not flatten absence into English text.
Its projection publishes a group-heading value containing the group key, available raw components, and explicit missing kinds for each presentation slot.
Numeric values such as a year remain typed until UIModel formatting.

UIModel turns that value into the current primary, secondary, and tertiary strings.
Sorting and group identity continue to use normalized runtime keys, so changing copy or locale cannot change ordering, membership, section identity, or cache keys.

The same missing-value catalog is reusable by now-playing and detail presentation, but each view model still chooses whether absence is shown, hidden, or combined with another value.

### Audio descriptor split

Core audio descriptors retain backend/profile ids, capabilities, supported profiles, and device facts supplied by the platform.
They no longer carry GTK icon names or built-in promotional descriptions.

UIModel maps known backend/profile ids to default labels, descriptions, and an `AudioIconKind`.
GTK maps that kind to a native symbolic icon, and TUI maps it to terminal presentation.
An unknown backend uses an explicit fallback based on its stable id and any external provider name; it never fabricates a known-backend description.

Device display names and descriptions discovered from PipeWire, ALSA, or WASAPI remain raw external facts because they identify user-selectable operating-system resources rather than authored Aobus copy.

### Completion roles

`CompletionItem::detail` becomes a semantic detail kind plus typed optional data such as a usage count.
Query syntax remains in `displayText` and `insertText` because those strings are language tokens, not translated prose.

UIModel resolves the detail kind into short text and pluralizes structured counts.
No frontend infers a role by comparing a detail string.

### Report templates and structured arguments

When integrated with RFC 0013, shared runtime operations publish a stable report-template id and a closed typed argument payload.
UIModel resolves that pair through the catalog exactly once for compact/detail presentation.
The runtime feed retains the accepted structured intent according to RFC 0011's snapshot model; it does not resolve UIModel copy or retain a duplicate resolved form.
An inert `templateId` with no consumer is not an allowed end state.

Library progress carries an operation kind rather than requiring `starts_with("Scanning:")` or `starts_with("Updating:")`.
Scan summaries carry counts and disposition.
Playback skip summaries carry skipped count and whether the failure threshold stopped playback.
Pluralization and shared user wording occur in UIModel, while diagnostic paths retain their own detailed text.

### Fallback and completeness

Every closed catalog lookup is total.
Tests iterate every enum and built-in id and fail when an entry is missing, duplicated, or empty where text is required.

Open ids use a documented fallback:

- unknown backend/profile ids use a provider name when available, otherwise the stable id;
- unknown report-template ids produce one generic bounded message and a diagnostic containing the id, never raw format-string execution; and
- unknown persisted presentation ids continue through the existing catalog fallback policy rather than becoming translated identity.

Catalog arguments are typed and bounded before formatting.
They cannot carry toolkit markup by default; a frontend that adds markup escapes catalog and user/external text at its own boundary.

### Migration sequence

1. Add the immutable catalog facade, feature tables, exhaustive tests, and composition wiring without changing producers.
2. Move track-field and built-in-preset copy while preserving stable ids and current English output.
3. Introduce structured group headings and migrate every consumer of runtime grouping text.
4. Split audio machine descriptors from UIModel/backend presentation and replace native icon strings with semantic icon kinds.
5. Replace completion detail strings with typed roles and arguments.
6. Integrate report templates, progress kinds, scan summaries, and playback summaries with RFC 0013.
7. Add boundary guardrails after legitimate raw external and diagnostic text paths are classified.

Each slice must preserve current English behavior unless it deliberately fixes a documented inconsistency.
Temporary adapters are private to the migration and are removed before the slice completes.

## Alternatives

### Move every string directly to GTK

This removes runtime copy but duplicates shared policy in GTK and TUI and gives no shared owner for missing values, completion roles, or report summaries.

### Keep strings in runtime and add translation keys beside them

Parallel English and key fields would let consumers disagree and would retain an easy path for new runtime formatting.
The semantic key and typed arguments must be the authoritative shared input.

### Use one global string map

A flat string-keyed map loses exhaustiveness, argument types, ownership, and discoverability.
Feature-specific typed tables composed through one facade provide the same locale selection without becoming a service locator.

### Treat every string as presentation copy

Query syntax, user labels, metadata, filesystem paths, and operating-system device names have identity or source semantics.
Translating or replacing them would corrupt data rather than improve the layer boundary.

### Introduce a complete localization framework first

The current defect is ownership and structure, not message-file tooling.
An immutable English catalog establishes the necessary seam and allows localization format/storage to be selected later with evidence.

## Compatibility and migration

This proposal changes in-process C++ APIs across core audio, runtime, UIModel, GTK, and TUI.
There is no source-compatibility obligation between those layers.

Existing stable track-field, presentation, backend, profile, action, and query tokens do not change.
No localized or catalog-resolved text is persisted.
User-authored custom-preset labels and external metadata/device text remain byte-preserving data under their current owners.

The initial English catalog must reproduce current shared text where current behavior is coherent.
Deliberate corrections, such as unifying `Unknown Artist` use or pluralization, are specification changes and receive explicit tests.

No migration reader is required because the proposal does not change a durable document shape unless a later report-template format is persisted by another RFC.

## Validation

- Exhaustive UIModel tests cover every track field, built-in presentation, missing-value kind, completion detail kind, known backend/profile, semantic icon kind, and report template introduced by the migration.
- Runtime tests prove stable ids, sorting, grouping, source membership, and query insertion are unchanged by catalog text.
- Group-projection tests prove absence is structured and changing catalog text cannot change group keys or ordering.
- Audio tests prove core descriptors contain no toolkit icon vocabulary and unknown providers retain usable fallback identity.
- Completion tests prove item roles and counts remain typed through UIModel formatting.
- Reporting tests prove progress and summary selection uses typed operation kinds rather than English prefix or message matching.
- GTK and TUI tests prove equivalent semantic labels while allowing different native icons and layout.
- Structured CLI output continues to use stable ids and never gains localized values in machine-readable fields.
- Repository guardrails reject known toolkit icon suffixes and designated authored-copy fields in core/runtime surfaces after each migration slice.
- The implementation passes `./ao check` and `./ao docs check`.

## Resolved questions

- The first catalog is a concrete zero-state English value; localization injection is deferred until a second vocabulary provides evidence for the required construction API.
- Unknown extension backends and profiles use id-only fallback. Operating-system device names remain external facts, while unclassified provider marketing names do not enter Core descriptors.
- The current English playback summaries require singular/other only. A future locale source must expand the typed formatting contract before claiming more plural categories.

## Promotion plan

The [presentation architecture](../architecture/presentation.md) now owns the catalog, structured group-heading, audio-presentation, and completion-role boundaries.
The [playback architecture](../architecture/playback.md), [track-expression architecture](../architecture/track-expression.md), and [failure and reporting architecture](../architecture/failure-and-reporting.md) record the data-flow changes; system and audio-quality dependency maps did not change.

The track-field, track-presentation, activity-status, completion, notification, and catalog specifications/references now record implemented semantic kinds, fallback behavior, and exact public surfaces.
The UIModel organization guide classifies authored copy versus user, external, protocol, and diagnostic text, while executable boundary checks preserve platform-vocabulary constraints.
