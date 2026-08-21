---
id: decision.0013.adopt-icu-collation
type: decision
status: accepted
domain: presentation
summary: Records transient ICU collation keys as the locale-aware ordering policy for interactive text.
---
# Decision 0013: adopt ICU collation

## Context

The 2026-08-21 locale-ordering review compared the existing UTF-8 byte order
with projection-shaped ICU workloads on Linux and native Windows. The existing
order is deterministic and inexpensive, but it is not an alphabetic order for
multilingual metadata and cannot express differences such as German versus
Swedish placement of `ä`.

[Decision 0011](0011-adopt-icu-for-unicode-text.md) already governs ICU for
locale-independent Unicode mechanics, while [Decision 0012](0012-adopt-icu-resource-catalogs.md)
gives each interactive process one canonical startup locale. The remaining
choice was how to use that locale for presentation order without changing
stored text, queries, source membership, Manual Order, or CLI automation.

The accepted Release/IPO workload included group identity, ordering-key
derivation, arena interning, and final sorting rather than timing the ICU facade
alone. Its worst 50,000-row median was 22.65 ms on the Linux review host and
38.72 ms on the native Windows review host, within the durable budgets in the
[performance-review guide](../development/test/performance.md#locale-aware-ordering-gate).

## Decision

Aobus uses governed ICU collation for textual order in GTK, TUI, and WinUI.
Each interactive composition root constructs one leaf ordering policy from the
same canonical locale used by its message catalog and injects the ICU-free
`TextOrderingPolicy` interface into runtime and UIModel consumers. The concrete
adapter lives in the `ao_app_i18n_ordering` target; runtime, UIModel, Core, and
CLI contain no ICU collation type.

Before collation, text uses locale-independent Unicode default case folding.
Group identity applies that fold to the complete unstripped NFC value. Ordering
applies it after the existing `the`, `a`, or `an` ordering-only article rule.
The configured collator always uses secondary strength, non-ignorable alternate
handling, case level off, and numeric collation off.

Sort keys are length-aware transient binary values materialized outside
comparators and retained only by the owning projection or vocabulary operation.
They are never library, YAML, workspace, session, or query-plan data. Equal ICU
keys use the surface's existing stable identity fallback rather than silently
changing identity or source order.

The CLI constructs no interactive ordering policy and retains its existing
byte-order behavior. This is a deliberate automation contract: the CLI has no
interactive locale owner, and its output must not vary with a desktop locale.

## Alternatives considered

### Retain UTF-8 byte order everywhere

Not selected for interactive surfaces because it preserves the visible
multilingual ordering defect. It remains the non-interactive fallback.

### Use platform-native collators

Not selected because GTK/Unix and Windows would then have different semantics,
failure behavior, fixtures, and upgrade cadence for the same locale.

### Remove accents or transliterate to ASCII

Not selected because accent removal is not locale-aware and is wrong for such
languages as Swedish. Transliteration would also require product policy and
language data far beyond alphabetic collation.

### Compare through ICU inside sort comparators

Not selected because projections compare values repeatedly. Materializing each
derived key once avoids repeating folding and locale comparison throughout an
`O(n log n)` sort and fits the existing arena/cache design.

### Use primary or tertiary strength

Primary strength would erase accent distinctions that remain meaningful.
Tertiary strength would reintroduce distinctions after explicit case folding
and create larger keys without a selected product requirement.

## Consequences

- Interactive order may differ by startup locale and after a governed ICU
  upgrade; the same stored library needs no migration because all keys are
  derived in memory.
- Unicode-caseless spellings such as `MÉTAL`/`métal` and
  `Straße`/`STRASSE` share group identity. The first row in final order supplies
  the visible raw heading.
- Article removal remains ordering-only, so `Doors` and `The Doors` stay
  distinct groups even when they sort together.
- Secondary strength intentionally allows equal sort keys for some width and
  kana distinctions. Raw identity fallbacks keep those values deterministic
  and distinct.
- Default folding is intentionally not Turkic-specific: it equates `I` with
  `i`, not dotless `ı`, in Turkish and Azerbaijani as well as other locales.
  Aobus accepts this exception to keep one identity relation across locales.
- Pinyin, stroke order, kana reading dictionaries, phonebook variants, numeric
  text collation, user rules, and live locale switching remain out of scope.
- Future ICU upgrades must rerun the exact fixtures and the durable
  cross-platform performance gate.

## Current authorities

- [Presentation architecture](../architecture/presentation.md)
- [Track-list presentation](../spec/presentation/track-presentation.md)
- [Track-list projection](../spec/library/projection/track-list.md)
- [Track-field value completion](../spec/presentation/field-completion.md)
- [Metadata editing](../spec/presentation/metadata-editing.md)
- [Performance review](../development/test/performance.md)

## Supersession

Not superseded.
