---
id: decision.0011.adopt-icu-for-unicode-text
type: decision
status: accepted
domain: utility
summary: Adopts an exact, cross-platform ICU release behind an Aobus-owned UTF-8 text facade.
---
# Decision 0011: adopt ICU for Unicode text

## Context

The 2026 Unicode text-correctness review found that Aobus had sound UTF-8 path
conversion seams and several frontend-local validators, but no project-wide
implementation for normalization, full case folding, or extended grapheme
boundaries. Handwritten code covered isolated cases, while metadata could
still enter the library as malformed UTF-8 or as canonically equivalent byte
sequences.

Those operations depend on versioned Unicode data. If Linux and Windows use
different data, rebuilding the same music library can produce different
derived search, grouping, and ordering keys. The selected implementation must
therefore align behavior as well as expose the required algorithms.

The review explicitly accepted a governed Unicode dependency and a narrow
project facade. It also constrained the scope to Unicode text mechanics: this
decision does not select a localization catalog, a locale-aware collator, or a
transliteration policy.

## Decision

Aobus uses ICU 78.3 directly for strict UTF-8 processing, NFC normalization,
default Unicode case folding, and extended grapheme-cluster segmentation. ICU
78.3 and Unicode 17.0 are one governed cross-platform dependency contract
resolved by Nix and vcpkg.

ICU is an implementation detail of the narrow Core `ao_unicode_text` target.
Public Aobus headers expose only `std::string`, `std::string_view`, byte
offsets, and `Result`; they expose no ICU type, locale object, allocator, or
exception.

Filesystem paths are outside this contract. They remain native
`std::filesystem::path` values and cross text boundaries only through the
explicit UTF-8 path conversion helpers. Aobus does not normalize path spelling.

Unicode operations are control-plane work. They must not run from a realtime
audio callback.

## Alternatives considered

### Maintain Aobus-owned Unicode tables and algorithms

Rejected. Correct normalization, case folding, and grapheme segmentation need
large generated tables plus continuing conformance work. That would duplicate
the part of ICU we need while making Unicode upgrades harder to audit.

### Use platform-native implementations

Rejected. GLib normalization on Linux and Windows normalization APIs do not
provide one aligned case-fold and grapheme contract, and their data versions
follow different platform release schedules. Platform shims would therefore
preserve the cross-platform behavior drift this decision is intended to remove.

### Use Boost.Locale with its ICU backend

Rejected for this scope. Boost.Locale is a useful higher-level localization
layer, but Aobus currently needs four low-level Unicode operations and no
locale service. The pinned Linux Boost package was built against ICU 76; using
its locale component while governing ICU 78.3 would either load two ICU
families or force a complete Boost rebuild. Calling ICU through the Aobus
facade keeps one behavioral authority without exposing a second abstraction.

The presence of Boost.Locale in a Boost distribution does not make it an
Aobus dependency: Aobus neither discovers nor links the component.

### Accept whichever ICU version each platform provides

Rejected. Unicode data is an observable input to derived keys and text
boundaries. An ungoverned version would make a cross-platform rebuild
nondeterministic at exactly the boundary being repaired.

## Consequences

- Unicode algorithms and data are maintained upstream instead of being copied
  into the repository.
- Nix and vcpkg must resolve the exact ICU release, required imported targets,
  and declared Unicode capabilities.
- ICU remains out of source files that do not implement the facade, and an
  executable only imports ICU when linked code actually uses those operations.
- ICU upgrades require behavior review. Persisted derived keys must either be
  stamped with their algorithm/data version or be rebuilt atomically.
- Locale-aware collation, transliteration, localized article rules, message
  catalogs, plural/select formatting, and pseudo-locales remain separate work.
- TUI Backspace uses grapheme boundaries, physical library schema version 7
  uses strict validation plus NFC admission, and `~` uses Unicode-caseless
  matching for user-visible text while retaining byte-exact URI behavior.

## Current authorities

- [Unicode text operations specification](../spec/text/unicode-text.md)
- [Dependency version governance](../development/dependency-governance.md)
- [TUI interaction specification](../spec/tui/interaction.md)

## Supersession

Not superseded.
