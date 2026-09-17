# Change messages, translations, and locales

Use this workflow for interactive GTK, TUI, WinUI, or AppKit copy.
The governing contracts are [interactive localization](../system/presentation/localization.md) and [presentation text semantics](../system/presentation/text-catalog.md).
This page explains the contributor task; it does not redefine locale fallback, message lifetimes, or domain mappings.

CLI text is outside the interactive catalog.
Unicode normalization and locale-aware ordering are separate concerns unless the requested change also changes displayed copy.

## Choose the text owner

Before editing, trace the displayed text from its consumer to its semantic owner.
Read the English pattern, the target translation, and the call site or formatter that supplies its arguments.
For music, library, and audio vocabulary, use the [terminology guide](../../.agents/skills/localize-aobus/references/music-audio-terminology.md); Chinese Metadata and List wording is normative in [presentation text semantics](../system/presentation/text-catalog.md#chinese-metadata-terminology).

Use the catalog for authored interactive display text.
Keep stable ids, query or command tokens, user-authored values, paths, external names, and protocol/diagnostic identity outside it as required by the [text ownership boundary](../system/presentation/text-catalog.md#text-ownership-boundary).
Do not localize a token merely because it appears next to localized text.

Equal English wording is not enough to share an id.
Compare meaning, grammatical role, arguments, and frontend behavior before deduplicating.
Conversely, do not add a feature helper for a fixed one-id lookup: use the canonical typed id unless domain state must select a message, derive a selector, or apply an open-id fallback.

## Add or change a message

### 1. Establish meaning and reachability

Identify every state that can render the message, including empty values, plural counts, selection branches, errors, and constrained layouts.
Prefer a complete grammatical message over concatenated fragments.
If the change affects a shared domain mapping, update the owning UIModel formatter and its focused tests rather than implementing the policy independently in each frontend.

Treat the current English root and maintained translations as user-visible behavior.
Do not rewrite unrelated wording, reorder concepts, or fill deliberate fallback omissions as mechanical cleanup.
Preserve dedicated fallback probes and their intentionally sparse coverage unless the task explicitly changes that contract.

### 2. Add the canonical identity

For a new governed message, add one typed id and stable key to [`MessageInventory.def`](../../app/include/ao/i18n/MessageInventory.def).
Use the existing naming neighborhood and keep the key's semantic scope narrow enough that translations can preserve its grammatical role.
Do not edit the generated enum or definition tables; [`MessageCatalog.h`](../../app/include/ao/i18n/MessageCatalog.h) and [`MessageIds.h`](../../app/i18n/MessageIds.h) consume the inventory directly.

For a wording-only change, retain the existing id and key.
Changing English copy does not require a stored-data migration, but it still needs focused user-visible review.

### 3. Author the English pattern

Add the complete English pattern to [`root.txt`](../../app/i18n/catalog/root.txt).
The root must contain every inventory key exactly once.
Use named ICU MessageFormat arguments, never numbered arguments.

- Give arguments semantic names such as `count`, `destination`, or `detail`.
- Include `other` in every `plural`, `select`, and `selectordinal` expression.
- Use one argument kind consistently wherever a name appears in the pattern.
- Encode a literal apostrophe as `''` and preserve ICU quoting around braces or `#`.
- Pass external values as arguments instead of interpolating them into an authored fragment before formatting.
- Keep selectors such as `yes`/`no` or state names stable and untranslated.

The complete argument and failure rules are in the [message contract](../system/presentation/localization.md#message-and-argument-contract).

### 4. Bind the consumer

Use `requiredText` for a fixed no-argument message and `requiredFormat` for an argument-bearing message whose caller already owns the arguments.
Use or extend a semantic formatter only when typed domain state must select among messages, derive hidden selectors, or preserve a documented open-id fallback.
Keep frontend-only native-string conversion and argument binding in the frontend leaf.

When adding a value to a closed enum, update its exhaustive presentation mapping and focused coverage.
When handling an open id, preserve the fallback in [presentation text semantics](../system/presentation/text-catalog.md), including unchanged external values and conservative icon/type behavior.

### 5. Add only required WinUI projection metadata

Most WinUI resources retain the canonical key and need no hand-authored native resource.
Edit [`WinUiResourceProjection.h`](../../app/i18n/WinUiResourceProjection.h) only when an existing native `std::vformat` consumer needs the supported one-argument `{0}` projection or XAML `x:Uid` needs a property-qualified alias.
The projection supports exactly one plain named value; it is not a general converter for plural, select, or multi-argument ICU patterns.

Do not edit generated `.resw` files.
Do not put shared user-facing copy in the WinUI-only English resource list.
A change to projection rules requires native Windows parity evidence.

## Translate a maintained locale

Edit the authored source under [`app/i18n/catalog/`](../../app/i18n/catalog), not generated `.res`, `.dat`, pseudo, or `.resw` output.
A maintained locale is a sparse override table: translate the messages in scope and leave an intentional omission absent so English fallback remains observable.
Do not copy English into every missing key merely to make the table look complete.

For each changed pattern:

1. preserve every English argument name and argument kind;
2. preserve selectors, braces, `#`, `other` branches, and ICU apostrophe escaping;
3. reorder placeholders as target-language grammar requires;
4. keep inserted paths, metadata, application/device names, commands, shortcuts, and key tokens unchanged;
5. render all reachable branches with representative counts and optional values; and
6. verify domain terms against the English concept rather than translating a word without its product meaning.

ICU signature parity proves structure, not grammar.
Check complete sentences for singular and plural counts and for distinctions such as all results versus a bounded preview.
Pseudo-localization exposes structural omissions and clipping but does not validate wording.
A newly maintained language or substantial terminology change needs fluent or domain-informed review.

Do not hide a layout problem by shortening or distorting an otherwise correct translation.
If equal text tempts deduplication, trace all render branches and narrow-layout command access before merging identities.

## Add a maintained locale

Adding a locale changes the supported-language surface; do it as one catalog, packaging, fallback, and native-parity change.
There is no automatic ADR or RFC requirement.
Changes to the runtime or fallback model need proportionate design review and updates to their current contracts.

1. Choose the canonical ICU resource name and public BCP 47 tag.
2. Add `app/i18n/catalog/<icu-locale>.txt` with the matching top-level ICU table and reviewed translations for the intended surface.
3. Add `<icu-locale>.res` to [`package.lst`](../../app/i18n/catalog/package.lst). Keep `root` and generated `qps_Ploc` in their special roles.
4. Let CMake derive the `.txt` input, compiled resource, selectable tag, and WinUI qualifier; do not add parallel manual locale lists.
5. Add focused catalog cases for canonical admission, representative direct translation, intentional English fallback, and selectable metadata.
6. Add the locale to the native [`WinUiLocalizationProbe.cpp`](../../test/helper/WinUiLocalizationProbe.cpp) parity coverage, including regional/script fallback when applicable.
7. Update the [ICU and MRT compatibility matrix](../system/presentation/localization.md#icu-and-mrt-compatibility-matrix) when the supported policy changes.
8. Inspect the new locale in every affected interactive frontend and obtain fluent or domain-informed wording review.

Generated `qps-ploc` remains diagnostic and must not appear in the user-facing language choices.
Do not duplicate script-qualified resources as regional assets unless the locale policy intentionally changes and the fallback contract is updated with it.

## Validation

Follow [test validation and review](test/validation-and-review.md); choose the route by behavior, not by file extension alone.
Run commands from the repository root through `./ao`, or through `ao.bat` on native Windows.

### Existing-locale message changes

For catalog-only wording in a maintained locale:

1. build each affected frontend so generated ICU and native resources are current;
2. run the catalog suite:

   ```bash
   ./ao test --core "[catalog]"
   ```

3. run message-specific UIModel or adapter tests for the changed semantic owner;
4. inspect the affected UI at normal and constrained widths; and
5. inspect native Windows UI when WinUI consumes the changed message.

Unchanged projection rules do not independently require the full Windows locale-parity gate, but displayed WinUI text still needs native inspection.
If documentation changed, also run `./ao docs check`.

### New messages, locales, signatures, or projections

Build every consuming frontend and run the catalog suite plus focused semantic and adapter tests.
A new locale, changed locale selection, changed argument signature, or changed WinUI projection requires native Windows parity evidence from the WinUI localization probe in addition to the applicable implementation gate.
Run the full native `check` and scoped `hygiene` when C++ code, headers, build configuration, or tests change, as required by the validation matrix.

### Visual review

Inspect every affected selection and expanded popup, not only the default screen.
For GTK, include the window-manager minimum width.
For desktop comparisons, record DPI and client size before judging breakpoints.
Confirm the locale actually displayed, and inspect screenshots and layout warnings; UI Automation or character counts alone do not prove correct rendering.
For TUI text, check normal and constrained terminal widths and verify commands and key arguments remain usable.
For AppKit and WinUI, inspect the native surfaces that consume the message.

Report the requested locale, resolved fallback where relevant, commands and focused filters run, frontend sizes inspected, and any untested native surface.
A zero-match test filter or an unbuilt stale catalog is not successful validation.
