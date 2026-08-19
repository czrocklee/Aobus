---
id: text.unicode-operations
type: spec
status: current
domain: utility
summary: Defines strict UTF-8 validation, NFC normalization, caseless keys, and extended grapheme boundaries.
---
# Unicode text operations

## Scope

This specification owns the reusable Aobus operations for strict UTF-8
validation, NFC normalization, locale-independent caseless keys, and the
previous extended grapheme-cluster boundary. It also defines where those
operations may be used.

It does not declare that bytes produced inside every media decoder are valid
before they reach library admission. Each non-library ingestion, grouping, and
sorting owner must still adopt an explicit boundary before making a stronger
claim. It does not own locale-aware collation, transliteration, display
language, message localization, or filesystem path identity.

## Code boundary

The [system architecture](../../architecture/system-overview.md) places the
mechanism in Core. The public facade is
[`include/ao/utility/UnicodeText.h`](../../../include/ao/utility/UnicodeText.h),
implemented by
[`lib/utility/UnicodeText.cpp`](../../../lib/utility/UnicodeText.cpp) inside
the narrow `ao_unicode_text` target.

ICU is private to that implementation. No public header or caller may depend on
an ICU type. Filesystem paths use
[`Path.h`](../../../include/ao/utility/Path.h) and are never passed through a
Unicode normalization operation merely because a UTF-8 rendering is needed.

## Terminology

- **Scalar-valid UTF-8** encodes only Unicode scalar values with the shortest
  legal byte sequence. It excludes overlong sequences, isolated continuation
  bytes, truncated sequences, UTF-16 surrogates, and values above U+10FFFF.
- **NFC text** is Unicode Normalization Form C text.
- A **caseless key** is a derived comparison/search value, never display text.
- An **extended grapheme cluster** is the Unicode user-perceived character
  boundary used for character-oriented editing.
- A **native index** is ICU's index into the supplied encoding. For this
  facade's UTF-8 text it is a byte offset, not a code-point or UTF-16 index.

## Invariants

- Every successful operation accepts and returns scalar-valid UTF-8.
- Validation rejects malformed input; it never repairs or replaces bytes.
- NFC normalization is idempotent.
- `makeUtf8CaselessKey(text)` is
  `NFC(Default_Case_Folding(NFC(text)))`.
- Caseless-key creation is independent of the process locale and current UI
  language.
- Caseless keys are not rendered to users and do not replace the original
  display spelling.
- Grapheme boundaries follow the root Unicode extended-grapheme rules and are
  returned as UTF-8 byte offsets.
- Filesystem paths are not normalized, case-folded, or segmented by this API.
- Unicode operations do not run on a realtime audio thread.
- Behavior is governed by ICU 78.3 with Unicode 17.0 data on both supported
  operating systems.

## State model

The facade exposes no durable or process-global mutable state. ICU's immutable
normalization and case data are shared internally. A grapheme iterator cannot
outlive the call or retain a shallow reference to caller-owned UTF-8 bytes.

Original display text and derived keys are distinct values. A consumer that
stores both owns their lifetime, schema, and invalidation.

## Commands and transitions

`validateUtf8(text)` scans the complete byte sequence. Success establishes
scalar-valid UTF-8; the first malformed sequence rejects the complete input.

`isUtf8Nfc(text)` first establishes valid UTF-8, then reports whether the
input is already NFC without constructing a normalized result.

`normalizeUtf8Nfc(text)` first establishes valid UTF-8. Already-normalized text
is returned unchanged in value. Other valid text is composed into NFC.

`makeUtf8CaselessKey(text)` validates and normalizes the source, applies the
Unicode default case-fold mapping, then normalizes the result again. It does
not apply language-specific lowercasing, accent removal, transliteration, or
collation.

`previousUtf8GraphemeBoundary(text)` validates the complete input and returns
the byte offset immediately before its final extended grapheme cluster. Empty
input returns zero. The returned offset is always a boundary suitable for
`std::string::resize()`.

## Failure and cancellation

All operations are synchronous and have no cancellation point.

| Condition | Result |
|---|---|
| Malformed UTF-8 | `InvalidInput`, with the first invalid byte offset where available. |
| Input or generated intermediate exceeds ICU's signed 32-bit operation limit | `ValueTooLarge`. |
| ICU data or service initialization fails | `InitFailed`. |
| ICU reports allocation failure | `ResourceExhausted`. |

A failed operation returns no partial normalized text, key, or boundary.
Callers may preserve their previous state or apply an explicitly documented
frontend fallback; they may not treat malformed bytes as a successful key.

## Persistence and versioning

The facade persists nothing. Physical library schema version 7 uses a
library-private admission seam around it: dictionary values, inline Track
title and custom-metadata values, and List name and description are validated
and normalized to NFC before persistence. Opaque List filter source is
validated as scalar UTF-8 but retained byte-exact because it can contain URI
literals. Opening a current schema validates the corresponding invariants and
rejects malformed or noncanonical rows as `CorruptData`. Filesystem URI bytes
remain outside the NFC contract.

Caseless keys and future collation keys are versioned derived data because ICU
and Unicode upgrades can change them. A persistent owner must record enough
algorithm/data identity to detect staleness or rebuild all affected keys as one
schema migration. The current library schema does not persist caseless or
collation keys.

## Frontend observations

The TUI command draft removes one complete extended grapheme cluster for each
Backspace. Combining marks, variation selectors, emoji modifiers, regional
indicator pairs, and joined emoji sequences therefore remain intact during
normal editing.

This facade does not prescribe rendered width. Frontends continue to use their
layout engine for cell or pixel measurement.

## Implementation map

- [`UnicodeText.cpp`](../../../lib/utility/UnicodeText.cpp) contains the only
  direct ICU calls for this contract.
- [`TextAdmission.cpp`](../../../lib/library/TextAdmission.cpp) translates the
  reusable Unicode results into library admission and corruption semantics.
- [`ShellInteractionModel.cpp`](../../../app/tui/ShellInteractionModel.cpp)
  consumes the previous-boundary operation for command editing.
- [`dependency-contract.json`](../../../dependency-contract.json),
  [`shell.nix`](../../../shell.nix), and [`vcpkg.json`](../../../vcpkg.json)
  align the ICU version across native resolvers.

## Test map

- [`UnicodeTextTest.cpp`](../../../test/unit/utility/UnicodeTextTest.cpp)
  verifies the governed runtime ICU and Unicode versions and protects
  malformed-sequence rejection, NFC equivalence and idempotence,
  quick checks, multi-code-point case folds, sigma equivalence, and byte-indexed
  grapheme boundaries for combining marks, variation selectors, flags, and ZWJ
  emoji.
- Library builder, Store, and open-integrity tests under
  [`test/unit/library/`](../../../test/unit/library/) protect NFC admission,
  canonical dictionary identity, malformed-input rejection, and persisted-row
  corruption detection.
- [`ShellInteractionModelTest.cpp`](../../../test/unit/tui/ShellInteractionModelTest.cpp)
  protects grapheme-aware Backspace through the frontend consumer.
- Dependency-policy tests and native dependency reports protect the aligned
  ICU release and required imported targets.

## Related documents

- [Decision 0011: adopt ICU for Unicode text](../../decision/0011-adopt-icu-for-unicode-text.md)
- [Dependency version governance](../../development/dependency-governance.md)
- [TUI interaction](../tui/interaction.md)
