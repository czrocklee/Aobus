---
id: presentation.localization
---
# Interactive localization

## Scope and boundaries

This contract defines locale admission, catalog construction and replacement, message formatting and fallback, and the WinUI MRT projection for interactive Aobus processes.
Locale-aware ordering is a separate service and is not part of this contract.
The [presentation architecture](README.md) owns composition and dependency direction; the [text semantics contract](text-catalog.md) owns domain-value mapping, external values, locale-neutral notation, and Chinese terminology.

The interactive facade is public under `app/include/ao/i18n/`.
Its ICU-backed implementation and canonical assets live under `app/i18n/`, and the build compiler lives under `tool/catalog/`.
GTK, TUI, WinUI, and AppKit construct localization at their composition roots.
The concrete catalog implementation and ICU i18n runtime remain outside Core, application runtime, UIModel, and the CLI link closure.
CLI diagnostics, commands, and machine output remain English and do not construct a catalog.

Interactive catalog output is display text only.
It must never become a persisted identity, query token, grouping key, protocol field, path, URI, or control-flow discriminator.
The [localization workflow](../../development/localization.md) explains how to modify the assets without changing these boundaries.

## Locale model and inventory

A **requested locale** is the strict canonical BCP 47 tag admitted for one interactive process.
A **resource locale** is an authored or generated ICU bundle in the explicit search chain.
A **resolved locale** is the resource that supplied one message; different messages in one catalog may resolve at different fallback levels.
The complete English **root** catalog is exposed publicly as `en` and projected to neutral MRT `en`.
The generated **pseudo locale**, exposed as `qps-ploc`, expands literal spans to reveal clipping, concatenation, and untranslated literals; it is diagnostic rather than a maintained translation.

[`package.lst`](../../../app/i18n/catalog/package.lst) is the single packaged-locale inventory.
For each non-pseudo entry, CMake derives the authored `.txt` input, compiled ICU resource, WinUI resource path and language qualifier, and selectable locale metadata.
`root` maps to public `en`; underscores in authored locale names map to BCP 47 hyphens.
The generated pseudo entry remains in the package but is excluded from selectable language metadata.

The current maintained locales are English, German, Simplified Chinese, Traditional Chinese, Japanese, Spanish, and French (`en`, `de`, `zh-Hans`, `zh-Hant`, `ja`, `es`, and `fr`).
English appears first in `availableCatalogLocales()`, followed by stable tag order.
Display names use ICU native names and the returned metadata has process lifetime.
Only the follow-system choice is frontend-owned.

[`MessageInventory.def`](../../../app/include/ao/i18n/MessageInventory.def) is the canonical typed `MessageId`-to-key inventory.
[`root.txt`](../../../app/i18n/catalog/root.txt) must contain every inventory key exactly once.
Maintained locale files are sparse override tables: omission deliberately falls back and is not filled by copying English into every file.
The build generates pseudo ICU input, compiled `.res`, embedded `.dat`, and WinUI `.resw`; these outputs must not be edited by hand.
Dedicated fallback probes are test assets and must not be normalized into ordinary full-translation coverage during unrelated copy maintenance.

## Catalog lifecycle and ownership

Each interactive composition root resolves one locale at startup and injects the resulting `MessageCatalog` through its UI graph.
Production code has no hidden English/default catalog construction path.
Catalog copies share immutable backing storage.

Construction is all-or-nothing:

1. embedded ICU data is registered once for process lifetime;
2. the requested tag is validated and canonicalized;
3. the explicit resource chain is loaded;
4. every typed id selects a pattern through that chain;
5. each selected pattern is parsed and its formatter constructed; and
6. zero-argument messages are rendered and cached before publication.

A published catalog performs no filesystem I/O, resource loading, or pattern parsing.
Its selected patterns remain immutable.
Concurrent formatting serializes access per cached ICU formatter, while argument conversion and each formatted output are call-owned.

`text(id)` returns a borrowed cached fixed message.
The view remains valid while the catalog or any copy sharing its implementation remains alive, and lookup rejects a message that requires arguments.
`format(id, arguments)` returns owned text and the owned resolved-locale tag; it never returns borrowed formatted text.
String argument views need remain valid only for the call.

TUI Settings may replace its injected catalog on the callback executor.
State that crosses replacement must own its display strings.
A failed replacement leaves the published catalog unchanged and reports a recoverable error.
GTK, WinUI, and AppKit retain their startup selection.
AppKit converts resolved UTF-8 to native strings only at its frontend boundary.

WinUI additionally owns one native lookup state from `configureResourceLanguage()` until `resetResourceLanguage()`.
It configures the requested locale before `InitializeComponent()` and tears the state down only after the window/session graph is gone.
Configuration with the same live tag is idempotent; attempting to replace a live context with another tag returns a conflict.

## Locale admission and fallback

`MessageCatalog::create(tag)` accepts one complete strict UTF-8 BCP 47 tag.
It rejects empty, partial, malformed, or embedded-NUL input rather than repairing it or consulting the ambient C locale.
`root` is admitted as the public English tag `en` for internal compatibility.
`MessageCatalog::createForSystemLocale()` reads the operating-system locale once; if the operating system cannot provide an admissible locale, it constructs English.
An explicitly invalid tag never silently becomes English.

For an admitted request, resolution tries the exact resource locale, ICU likely-subtags expansion, successive parents, and English root in that order, without ICU's ambient default locale entering the chain.
A valid unsupported locale therefore resolves from English root.
Pseudo-localization tries generated `qps_Ploc` and then English root.
The English root is complete, so a successfully published catalog resolves every typed id.

### ICU and MRT compatibility matrix

This matrix is an independently queryable cross-adapter compatibility contract.
“Then `en`” means that a sparse maintained translation may omit an individual message.

| Requested tag | ICU catalog result | WinUI MRT qualifier chain |
|---|---|---|
| `en-GB` | English root | `en` |
| `de-DE` | neutral German, then English root | `de`, then `en` |
| `de-AT` | neutral German, then English root | `de`, then `en` |
| `zh-CN` | Simplified Chinese, then English root | `zh-Hans`, then `en` |
| `zh-Hans` | Simplified Chinese, then English root | `zh-Hans`, then `en` |
| `zh-TW` | Traditional Chinese, then English root | `zh-Hant`, then `en` |
| `zh-Hant` | Traditional Chinese, then English root | `zh-Hant`, then `en` |
| `ja-JP` | Japanese, then English root | `ja`, then `en` |
| `es-ES` | Spanish, then English root | `es`, then `en` |
| `fr-FR` | French, then English root | `fr`, then `en` |
| valid unsupported tag such as `sv-SE` | English root | `en` |
| `qps-ploc` | generated pseudo, then English root | `qps-ploc`, then `en` |

The shared `zh_Hans` and `zh_Hant` assets serve corresponding regional requests rather than duplicating equivalent regional bundles.
This does not change the maintained-locale policy or promise region-specific wording.

## Message and argument contract

`requiredText(catalog, id)` and `requiredFormat(catalog, id, arguments)` are fail-closed operations for governed interactive copy.
A failure means the compiled application and its catalog disagree, so ordinary interactive call sites treat it as fatal.
A fixed message uses `requiredText`; an argument-bearing message uses `requiredFormat` unless a semantic formatter must first select a message or derive selectors.

Patterns use named ICU MessageFormat arguments.
The English pattern defines the signature, and every authored override must preserve the same argument names and argument kinds.
Every `plural`, `select`, and `selectordinal` construct must include an `other` branch.
Arguments may be supplied in any order but must provide every expected name exactly once and no unknown names.
A name reused in one pattern must retain one argument kind.

Accepted argument values are UTF-8 string views, signed 64-bit integers, unsigned 64-bit integers, and doubles.
Bare values and simple formats accept any admitted alternative.
Plural, select-ordinal, and choice arguments require numeric values; select arguments require text.
Booleans and enums are not converted implicitly: presentation code must map them to explicit selectors or message choices.
Unsigned values must fit ICU's signed 64-bit input range.
Every string argument must be valid UTF-8.

Selectors such as `yes`/`no`, source kinds, playback states, and volume states are closed control values supplied by presentation code.
They are not translated.
Inserted paths, names, metadata, key tokens, and other external values remain unchanged even though a locale may reorder their placeholders.
A literal apostrophe in an ICU pattern is written as `''`; preserve braces, selectors, `#`, and apostrophe quoting when translating.

Pseudo-localization transforms literal spans only.
It preserves argument names and syntax, selectors, replacement-number markers, and inserted values.
It is structural coverage, not evidence that wording or grammar is correct.

## Frontend resolution and WinUI projection

GTK, TUI, and AppKit resolve canonical typed ids from the injected `MessageCatalog`.
GTK copies borrowed fixed text when a widget API requires ownership.
TUI feature formatters bind named chrome arguments, leaving command and key identity unchanged.
AppKit converts required and formatted UTF-8 at its native-string boundary.

WinUI binds an explicit MRT `Language` qualifier from the catalog's requested locale.
It does not allow a default `ResourceLoader` context to select another language independently.
The generated resource normally retains the canonical catalog key.
The bounded projection in [`WinUiResourceProjection.h`](../../../app/i18n/WinUiResourceProjection.h) may:

- replace exactly one governed plain named argument with `{0}` for a native `std::vformat` consumer; or
- add a property-qualified alias where XAML `x:Uid` requires one.

Do not add projection entries merely to rename a key or duplicate authored copy.
A positional projection rejects plural/select syntax, multiple arguments, mismatched argument names, and ICU apostrophe-quoted syntax that cannot be represented safely.

ICU `root`, maintained locales, and generated `qps_Ploc` project from the same authored patterns to MRT `en`, maintained BCP 47 qualifiers, and `qps-ploc`.
MakePri uses neutral `en` as its default language.
Maintained-locale `.resw` files contain only authored overrides; a missing projected message is omitted so MRT can resolve neutral English.
English and pseudo projections must contain every governed message.
Checked-in WinUI-only English resources remain limited to product names, symbolic suffixes, and internal diagnostic/protocol text; they are not another store for shared user-facing copy.
The catalog compiler merges them into the single generated neutral `en` PRI input.

Frontend-local layout/action ids and tokens remain untranslated even when their labels are catalog messages.
The complete authored-versus-external boundary and domain-specific pass-through rules are in [presentation text semantics](text-catalog.md#text-ownership-boundary).

## Failure behavior

No partially constructed catalog is published.
Construction reports:

| Condition | Error code |
|---|---|
| Invalid explicit locale tag | `InvalidInput` |
| Incomplete, unreadable, or malformed embedded catalog | `CorruptData` |
| ICU allocation failure | `ResourceExhausted` |
| Other initialization failure | `InitFailed` |

Fixed lookup reports `NotFound` for an out-of-range typed id and `InvalidInput` when the selected message requires arguments.
Formatting reports `NotFound` for an out-of-range typed id and `InvalidInput` for a missing, duplicate, unexpected, mistyped, out-of-range, or invalid-UTF-8 argument.
Catalog construction and WinUI context initialization are fatal during startup because the process cannot provide its governed presentation surface.
Localization has no asynchronous operation or cancellation state beyond TUI's replace-or-preserve Settings transition.

## Persistence and compatibility

TUI stores an explicit language override in the global `preferences` group described by the [application-config reference](../../reference/persistence/application-config.md); an empty override follows the system locale.
It stores the requested language choice, never resolved message text.
GTK, WinUI, and AppKit select the system locale at each startup.

Catalog changes have no library, workspace, session, or interchange schema version.
Changing English root or a maintained translation is nevertheless a user-visible behavior change and requires focused review; wording cleanup must not incidentally rewrite the English baseline or translation tables.
Stable message keys and runtime/Core ids retain their existing compatibility owners.
Changing the message runtime, fallback model, or ICU family is an architectural/dependency change, not ordinary copy maintenance; [Decision 0012](../../decision/0012-adopt-icu-resource-catalogs.md) records the current catalog choice.

## Implementation and test evidence

- [`MessageCatalog.h`](../../../app/include/ao/i18n/MessageCatalog.h) declares the typed facade, argument values, resolved result, and selectable-locale metadata.
- [`MessageCatalog.cpp`](../../../app/i18n/MessageCatalog.cpp) owns admission, explicit fallback, eager construction, lifetime, and formatting errors.
- [`CatalogPattern.cpp`](../../../app/i18n/CatalogPattern.cpp) owns signature validation, pseudo transformation, and generated projection rules.
- [`CatalogCompiler.cpp`](../../../tool/catalog/CatalogCompiler.cpp) validates the root inventory and sparse overrides and generates pseudo and WinUI resources.
- [`app/i18n/CMakeLists.txt`](../../../app/i18n/CMakeLists.txt) derives resources and selectable locales from `package.lst`, compiles and embeds catalogs, and keeps ICU in the interactive leaf.
- [`StringResources.cpp`](../../../app/windows-winui/platform/StringResources.cpp) owns the explicit MRT context.

[`MessageCatalogTest.cpp`](../../../test/unit/i18n/MessageCatalogTest.cpp) protects locale admission, fallback, arguments, ownership, pseudo behavior, and concurrent formatting.
[`CatalogPatternTest.cpp`](../../../test/unit/i18n/CatalogPatternTest.cpp) protects signatures and deterministic ICU/RESW generation.
[`StringResourceTest.cpp`](../../../test/unit/winui/StringResourceTest.cpp) protects canonical WinUI reachability and the bounded projection.
[`WinUiLocalizationProbe.cpp`](../../../test/helper/WinUiLocalizationProbe.cpp) compares native MRT and ICU selection and formatting across the compatibility matrix.
See the [localization workflow](../../development/localization.md#validation) for the required contributor evidence.

## Related documents

- [Presentation architecture](README.md)
- [Presentation text semantics](text-catalog.md)
- [Decision 0012: adopt ICU resource catalogs](../../decision/0012-adopt-icu-resource-catalogs.md)
- [Unicode text operations](../unicode-text.md)
