---
id: presentation.localization
type: spec
status: current
domain: presentation
summary: Defines locale selection, fallback, localization boundaries, and cross-frontend message resolution.
---
# Interactive localization

## Scope

This specification defines startup locale admission, embedded message resolution, formatting, and the WinUI native-resource adapter for interactive Aobus processes.
It covers the catalog foundation, shared semantic copy resolved through feature presentation functions, and frontend-local shell/navigation, playback/output, library/list/filter, preferences/shortcut/presentation-editor, metadata/property, Layout Editor, accessibility, tooltip, empty-state, and recoverable-error copy.

Locale-aware ordering is independent and is not part of this contract.

## Code boundary

The [system architecture](../../architecture/system-overview.md) defines the process layers, and the [presentation architecture](../../architecture/presentation.md) owns localization composition and dependency direction.
The interactive localization facade is public under `app/include/ao/i18n/`; its ICU-backed implementation, canonical resources, and build compiler live under `app/i18n/` and `tool/catalog/`.
GTK, TUI, and WinUI may construct the facade at their process roots.
Core, application runtime, UIModel, and CLI do not depend on the concrete catalog target.

## Terminology

- A **requested locale** is the strict canonical BCP 47 tag admitted for one interactive process.
- A **resource locale** is an authored catalog in the exact-parent-root search chain.
- A **resolved locale** is the resource locale that supplied one message; messages from the same catalog may resolve from different fallback levels.
- **English root** is the complete ICU `root` catalog, exposed publicly as locale `en` and generated for MRT as neutral `en`.
- **Pseudo locale** is the generated `qps-ploc` catalog used to expose clipping, concatenation, and untranslated literals without adding a maintained translation.

## Invariants

- Each interactive composition root resolves one locale at startup and keeps the resulting catalog alive until frontend teardown.
- Each interactive composition root injects that `MessageCatalog` through the UI graph; production code has no hidden English/default construction path.
- Interactive call sites look up required copy with `requiredText` and `requiredFormat`. GTK, TUI, and WinUI use canonical typed `MessageId` values directly; WinUI also consumes generated MRT resources where native lookup is required.
- Explicit locale input must be a complete strict BCP 47 tag; invalid input is never repaired or interpreted through the ambient C locale.
- Resolution tries the exact locale, its ICU likely-subtags expansion, parent sequence, and English root in that order.
- English root contains every typed message id; maintained locale catalogs may contain overrides only.
- Resource loading, fallback selection, pattern parsing, `MessageFormat` construction, and fixed-message rendering finish before a catalog is published.
- Published catalog semantics and patterns are immutable. Concurrent calls serialize access per cached ICU formatter and own argument conversion and output per call.
- Formatting performs no catalog filesystem I/O.
- Translation output is display text only. It is never a persisted identity, query token, grouping key, protocol field, path, URI, or control-flow discriminator.
- Message patterns use named arguments. Every override has the same argument names and argument kinds as English root, and every plural or select construct has an `other` branch.
- Pseudo-localization changes only literal spans; argument syntax, selectors, replacement-number markers, and inserted argument values remain intact.
- TUI command syntax, shortcut names, and key tokens are message arguments rather than translated literals. Localization may reorder them but cannot change what the shell parses or dispatches.
- WinUI lookup uses an explicit MRT `Language` qualifier derived from the catalog's requested locale. It does not independently resolve a language through a default `ResourceLoader` context.
- ICU `root`, `de`, `zh_Hans`, `zh_Hant`, `ja`, `es`, `fr`, and `qps_Ploc` generate MRT `en`, `de`, `zh-Hans`, `zh-Hant`, `ja`, `es`, `fr`, and `qps-ploc` candidates from the same authored patterns; MakePri uses neutral `en` as its default language. Chinese region requests resolve to the corresponding script-qualified candidate instead of duplicating equivalent region and script assets.
- Maintained-locale MRT resources contain only authored overrides; a missing positional or aliased message is omitted so MRT can resolve the neutral-English resource. English-root and pseudo-locale projections require every governed message.
- Generated WinUI resources retain canonical message keys. The generator changes a governed single named argument to `{0}` only for native `std::vformat` consumers and emits property-qualified aliases only where XAML `x:Uid` requires them.
- The concrete localization target and ICU i18n runtime remain outside the CLI source and link closure.

## State model

A catalog has two observable phases:

1. **Constructing** registers the embedded static data package once, admits the requested tag, loads the explicit fallback chain, validates each selected pattern, constructs all formatters, and caches every zero-argument result.
2. **Published** permits borrowed fixed-message lookup, argument-bearing formatting through per-message formatter synchronization, and requested-locale observation until the owning composition root destroys the catalog.

The embedded package has static storage duration and remains registered for the process lifetime.
WinUI additionally owns one configured native lookup state between `configureResourceLanguage()` and `resetResourceLanguage()`.

## Commands and transitions

`MessageCatalog::create(tag)` admits an explicit tag and returns a fully constructed catalog.
`MessageCatalog::createForSystemLocale()` reads the operating-system locale once and uses English when the operating system cannot provide one.
It does not turn an explicitly invalid tag into English.

`MessageCatalog::text(id)` returns the cached result for a zero-argument message.
The view remains valid while the catalog or a copy sharing its implementation remains alive.
It does not invoke `MessageFormat` after publication.

`MessageCatalog::format(id, arguments)` resolves the already selected pattern for `id`, validates the complete argument set, and returns owned UTF-8 text plus the locale that supplied that message.
Arguments may arrive in any order, but names must be unique and exactly match the pattern signature.
String, signed-integral, unsigned-integral, and floating values are deduced by `MessageArgument`; booleans and enums require an explicit semantic mapping rather than implicit numeric conversion.
Unsigned values outside the signed 64-bit ICU input range are rejected.

`requiredText(catalog, id)` and `requiredFormat(catalog, id, arguments)` are required-message operations: failure means the compiled application and its governed catalog disagree, so interactive call sites treat it as fatal.
UIModel retains named semantic functions only where a domain input selects messages, derives selectors, or needs an open-id fallback.

WinUI configures its MRT context before `InitializeComponent()`.
Repeated configuration with the same tag is idempotent; attempting to replace a live context with a different tag reports a conflict.
Frontend teardown releases the MRT context after the window/session graph is gone.

The required cross-adapter outcomes are:

| Request | Catalog resource | MRT qualifier |
|---|---|---|
| `en-GB` | English root | `en` |
| `de-DE` | neutral German when present, otherwise English root | `de`, then `en` |
| `de-AT` | neutral German when present, otherwise English root | `de`, then `en` |
| `zh-CN` | Simplified Chinese when present, otherwise English root | `zh-Hans`, then `en` |
| `zh-Hans` | Simplified Chinese when present, otherwise English root | `zh-Hans`, then `en` |
| `zh-TW` | Traditional Chinese when present, otherwise English root | `zh-Hant`, then `en` |
| `zh-Hant` | Traditional Chinese when present, otherwise English root | `zh-Hant`, then `en` |
| `ja-JP` | Japanese when present, otherwise English root | `ja`, then `en` |
| `es-ES` | Spanish when present, otherwise English root | `es`, then `en` |
| `fr-FR` | French when present, otherwise English root | `fr`, then `en` |
| unsupported valid tag such as `sv-SE` | English root | `en` |
| `qps-ploc` | generated pseudo | `qps-ploc` |

## Failure and cancellation

Catalog construction returns `InvalidInput` for an invalid explicit locale, `CorruptData` for an incomplete or malformed embedded catalog, `ResourceExhausted` for ICU allocation failure, and `InitFailed` for other initialization failures.
Fixed lookup returns `NotFound` for an unknown typed id and `InvalidInput` when the selected message requires arguments.
Formatting returns `NotFound` for an unknown typed id and `InvalidInput` for missing, duplicate, unexpected, mistyped, out-of-range, or invalid-UTF-8 arguments.
No partially constructed catalog is published.

Localization has no asynchronous operation or cancellation state.
An interactive process treats catalog construction or WinUI context initialization failure as startup-fatal because it cannot provide its governed presentation surface.

## Persistence and versioning

Locale selection and resolved messages are not persisted in this tranche.
Catalog changes have no library, workspace, session, or interchange schema version.
The exact ICU family and capabilities are governed by `dependency-contract.json`; changing the message runtime or fallback model requires an architectural decision.

## Frontend observations

GTK, TUI, and WinUI construct the same `MessageCatalog` from their system locale and inject that single cheap handle through their process-owned UI graph.
Shared track-field labels, group and missing-value labels, built-in presentation copy, audio descriptions and profiles, completion roles, structured notifications, library progress and scan results, filter errors, track and selection counts, smart-List state, manual-order and Playlist-membership results, import/export results, language-bearing track-field formatting, now-playing states, transport and volume presentation, and audio-quality semantics resolve through that catalog.
GTK menu copy plus GTK-specific shell and playback accessibility copy, library pickers, import/export and saved-List dialogs, smart-List fields, and List membership/order controls use canonical ids through `gtkText` or GTK-local formatting functions.
Preferences, shortcut-editor chrome and action descriptors, custom-presentation editing, metadata/property controls, Layout Editor vocabulary and validation, accessibility/tooltips, startup wrappers, and recoverable errors use direct canonical ids through the injected `MessageCatalog` because each call maps one message without additional semantic selection.
Layout component types, property names, enum values, action ids, and node ids remain stable document identities; the GTK editor maps known built-in values to localized display text and preserves unknown extension values verbatim.
TUI navigation labels, overlay titles and hints, command-palette metadata, help copy, playback/output empty states, library navigation/filter status, recoverable errors, and remaining accessibility-oriented status copy resolve through `tuiChromeText`, TUI-local formatting functions, or direct canonical ids; presentation-navigation qualifiers use direct ids through `MessageCatalog`.
The frontend helpers and shared semantic functions are stateless over the immutable startup-selected process catalog; render and widget construction perform no resource loading or pattern parsing.
Operating-system device names, track metadata, audio node names, and external application names remain raw arguments.
The audio-quality formatter keeps established technical numbers and symbols locale-neutral while the catalog owns complete lexical and grammatical messages.
Neutral German proves substitutions and plural behavior, and the generated pseudo locale exercises every migrated shared message while preserving runtime arguments.
WinUI consumes generated shared track-field, presentation, shell-navigation, playback/output, static library, tooltip, accessibility, empty-state, and recoverable-error resources from the same canonical ids. Native single-argument formatting and XAML property lookup are generated projections of those ids, not independently authored messages.
The English root is the default presentation baseline owned by the [presentation text catalog reference](../../reference/presentation/text-catalog.md). Deliberate English copy corrections are reviewed as user-visible changes and protected by focused expectations; catalog migration does not otherwise rewrite copy.

CLI does not construct a catalog and retains English command, diagnostic, and machine-output behavior.

## Implementation map

- [`MessageCatalog.h`](../../../app/include/ao/i18n/MessageCatalog.h) defines the typed facade and owned result.
- [`MessageCatalog.cpp`](../../../app/i18n/MessageCatalog.cpp) owns admission, explicit fallback, immutable formatter construction, and formatting.
- [`PresentationText.cpp`](../../../app/uimodel/presentation/PresentationText.cpp) forwards direct typed ids and maps domain inputs only where message selection or fallback is semantic.
- [`GtkTextCatalog.cpp`](../../../app/linux-gtk/i18n/GtkTextCatalog.cpp) and [`TuiTextCatalog.cpp`](../../../app/tui/TuiTextCatalog.cpp) own frontend-local copy helpers and argument binding.
- [`CatalogPattern.cpp`](../../../app/i18n/CatalogPattern.cpp) owns signature validation and structure-aware pseudo transformation.
- [`root.txt`](../../../app/i18n/catalog/root.txt), [`de.txt`](../../../app/i18n/catalog/de.txt), [`zh_Hans.txt`](../../../app/i18n/catalog/zh_Hans.txt), [`zh_Hant.txt`](../../../app/i18n/catalog/zh_Hant.txt), [`ja.txt`](../../../app/i18n/catalog/ja.txt), [`es.txt`](../../../app/i18n/catalog/es.txt), and [`fr.txt`](../../../app/i18n/catalog/fr.txt) are the canonical authored catalogs.
- [`CatalogCompiler.cpp`](../../../tool/catalog/CatalogCompiler.cpp) validates assets and generates pseudo and WinUI resources.
- [`WinUiResourceProjection.h`](../../../app/i18n/WinUiResourceProjection.h) declares the narrow positional and XAML-property projection set.
- [`StringResources.cpp`](../../../app/windows-winui/platform/StringResources.cpp) owns the explicit MRT context.
- [`app/CMakeLists.txt`](../../../app/CMakeLists.txt) owns ICU compilation, common-data packaging, embedding, and the CLI dependency guard.

## Test map

- [`MessageCatalogTest.cpp`](../../../test/unit/i18n/MessageCatalogTest.cpp) protects fallback, formatting, pseudo output, invalid arguments, and concurrent use.
- [`PresentationTextTest.cpp`](../../../test/unit/uimodel/presentation/PresentationTextTest.cpp) protects the complete typed shared surface in English, German, fallback, and pseudo catalogs.
- [`AudioQualityFormatterTest.cpp`](../../../test/unit/uimodel/playback/quality/AudioQualityFormatterTest.cpp) protects the English baseline, localized complete messages, technical-number policy, and external-value pass-through.
- [`MenuControllerTest.cpp`](../../../test/unit/linux-gtk/app/MenuControllerTest.cpp), [`PreferencesWindowTest.cpp`](../../../test/unit/linux-gtk/preference/PreferencesWindowTest.cpp), [`ShortcutEditorWidgetTest.cpp`](../../../test/unit/linux-gtk/preference/ShortcutEditorWidgetTest.cpp), and [`RenderTest.cpp`](../../../test/unit/tui/RenderTest.cpp) protect frontend helper consumption in English, German, and pseudo-localized presentation.
- [`TrackCustomViewDialogTest.cpp`](../../../test/unit/linux-gtk/track/TrackCustomViewDialogTest.cpp), [`TrackPresentationPickerViewModelTest.cpp`](../../../test/unit/uimodel/library/presentation/TrackPresentationPickerViewModelTest.cpp), and [`TrackPresentationNavigationTest.cpp`](../../../test/unit/tui/TrackPresentationNavigationTest.cpp) protect the localized presentation-editor and navigation slice.
- [`LayoutEditorTextTest.cpp`](../../../test/unit/linux-gtk/layout/editor/LayoutEditorTextTest.cpp) protects built-in descriptor and enum display mapping while preserving extension tokens.
- [`ListOrderCapabilitiesTest.cpp`](../../../test/unit/uimodel/library/list/ListOrderCapabilitiesTest.cpp), [`ListMembershipAuthoringSessionTest.cpp`](../../../test/unit/uimodel/library/track/ListMembershipAuthoringSessionTest.cpp), and the GTK List/dialog tests protect localized library authoring and frontend wiring.
- [`CatalogPatternTest.cpp`](../../../test/unit/i18n/CatalogPatternTest.cpp) protects signature validation and deterministic ICU/RESW generation.
- [`StringResourceTest.cpp`](../../../test/unit/winui/StringResourceTest.cpp) protects canonical WinUI resource reachability and the bounded generated projection set.
- [`WinUiLocalizationProbe.cpp`](../../../test/helper/WinUiLocalizationProbe.cpp) runs after native WinUI linking and compares ICU and MRT selection and formatting for the required locale matrix.
- The CLI target-closure check in [`app/CMakeLists.txt`](../../../app/CMakeLists.txt) rejects concrete localization and ICU i18n dependencies below `aobus`.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Presentation text catalog reference](../../reference/presentation/text-catalog.md)
- [Decision 0012: adopt ICU resource catalogs](../../decision/0012-adopt-icu-resource-catalogs.md)
- [Unicode text operations](../text/unicode-text.md)
