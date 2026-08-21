---
id: decision.0012.adopt-icu-resource-catalogs
type: decision
status: accepted
domain: presentation
summary: Records ICU resource bundles and classic MessageFormat as the single interactive localization stack.
---
# Decision 0012: adopt ICU resource catalogs

## Context

The 2026-08-20 localization spike and review compared the catalog stack before production behavior changed.
Aobus needs stable semantic ids, complete-message plural/select grammar, placeholder validation, deterministic fallback and pseudo-localization, generated WinUI resources, and one aligned Linux/Windows dependency family.

ICU 78.3 is already governed for Unicode text mechanics, but [Decision 0011](0011-adopt-icu-for-unicode-text.md) deliberately did not choose localization.
The new capability requires ICU i18n in interactive applications and must not broaden the existing Core Unicode facade or the CLI link graph.

## Decision

Aobus authors interactive translations as ICU resource-bundle text with stable message keys and complete classic ICU MessageFormat patterns.
The English `root` bundle is complete, neutral `de` owns maintained German copy, and generated pseudo copy uses ICU's canonical `qps_Ploc` spelling.
An Aobus facade performs exact-parent-root lookup with direct bundle opens so ICU's ambient default locale cannot enter the chain.

ICU `MessagePattern` validates argument signatures and drives deterministic pseudo-localization.
The same canonical source deterministically generates WinUI `.resw`; ICU `root`, `de`, and `qps_Ploc` map to neutral MRT `en`, `de`, and `qps-ploc`, and WinUI resolves them through an explicit MRT language context.

The implementation links governed ICU 78.3 i18n only into interactive catalog consumers and focused tests.
Same-version `genrb` compiles locale resources, `pkgdata -m common` builds one `aobus_messages.dat`, and deterministic tooling embeds that aligned common-data image in the localization target.
The target registers it once with `udata_setAppData` before catalog publication; the application ships no loose catalog files.

Catalog construction, resource loading, pattern parsing, and classic `MessageFormat` construction finish single-threaded before publication.
Published catalog semantics and patterns are immutable; each message serializes access to its cached formatter as ICU 78.3 requires, while arguments and output remain call-local.
Core Unicode text and CLI retain their current dependency boundaries.

## Alternatives considered

### GNU gettext PO/MO

Not selected.
Its catalog and translator tooling are mature, but its runtime plural API and source-text identity do not provide the required stable-id, complete-message plural/select contract without Aobus-specific conventions and a second formatter/compiler layer.

### Aobus-owned YAML or JSON messages

Not selected.
The file syntax would be small, but plural/select parsing, argument validation, pseudo-localization, fallback, and native generation would become project-owned internationalization machinery.

### Per-frontend native catalogs

Not selected.
They would preserve separate English authorities and could not give shared UIModel copy one placeholder/fallback gate.

### ICU MessageFormat 2

Not selected for this release because its ICU 78.3 C++ API is technology preview.
The stable classic API supplies the required named arguments, plural, select, and validation model.

## Consequences

- Interactive executables gain the ICU i18n runtime from the same exact ICU family already used for `uc` and `data`; CLI does not.
- Native Windows builds explicitly enable and version-check the matching ICU `tools` feature, but do not deploy ICU tools.
- Translators initially edit ICU resource text and MessageFormat patterns rather than PO/XLIFF files. This is proportionate for English, neutral German, and generated pseudo assets; stable ids and patterns permit a later import/export adapter without changing callers.
- English fallback and placeholder correctness are build/test contracts rather than frontend conventions.
- Embedded `.dat`, pseudo, and `.resw` generation are deterministic repository tooling, not independently authored or deployed loose assets.
- WinUI locale lookup uses explicit MRT context and must prove unpackaged PRI packaging on native Windows.
- WinUI C++ lookups retain canonical message keys. Generated single-argument native patterns use positional `{0}` under those keys, and only XAML property lookup receives a generated property-qualified alias.
- Concurrent formatting follows ICU's explicit synchronization requirement with one lock per cached message formatter and is covered by a focused test plus the first integration's TSan gate; published formatter reconfiguration is forbidden.
- A future move to stable MessageFormat 2 or another authoring format requires a new decision and must preserve typed callers and canonical ids.

## Current authorities

- [Presentation architecture](../architecture/presentation.md)
- [Interactive localization specification](../spec/presentation/localization.md)
- [Presentation text catalog reference](../reference/presentation/text-catalog.md)
- [Unicode text operations](../spec/text/unicode-text.md)

## Supersession

Not superseded.
