---
id: decision.index
type: index
status: current
domain: documentation
summary: Defines and indexes durable Aobus architectural decision records.
---
# Architectural decisions

Decision records preserve why a consequential choice was made, the alternatives considered, and the accepted consequences.
They do not own current behavior; architecture, specifications, and reference remain the current sources of truth.

Use a decision record when reversing the choice would be expensive or when future maintainers are likely to revisit the rejected alternatives.
Do not create retroactive decisions merely to fill this directory.

File names use a four-digit sequence and a concise noun phrase, for example `0001-runtime-uimodel-separation.md`.
Accepted decisions are immutable except for status and links that mark them superseded.
Use the [decision template](../template/decision.md).

## Accepted decisions

- [Decision 0001: unify saved Lists with an independent order overlay](0001-unified-list-ordering.md)
- [Decision 0003: terminate on live library publication fault](0003-terminate-on-library-publication-fault.md)
- [Decision 0004: adopt layout documents for WinUI shell composition](0004-adopt-layout-documents-for-winui-shell-composition.md)
- [Decision 0005: use process restart for WinUI library switching](0005-use-process-restart-for-winui-library-switching.md)
- [Decision 0007: unify fatal diagnostics and abort](0007-unify-fatal-diagnostics-and-abort.md)
- [Decision 0008: close library admission and trust live storage](0008-close-library-admission-and-trust-live-storage.md)
- [Decision 0009: use process restart for GTK library switching](0009-use-process-restart-for-gtk-library-switching.md)
- [Decision 0010: never write to an audio file](0010-never-write-to-audio-files.md)
- [Decision 0011: adopt ICU for Unicode text](0011-adopt-icu-for-unicode-text.md)
- [Decision 0012: adopt ICU resource catalogs](0012-adopt-icu-resource-catalogs.md)
- [Decision 0013: adopt ICU collation](0013-adopt-icu-collation.md)

## Superseded decisions

- [Decision 0002: fail closed at the library integrity boundary](0002-fail-closed-library-integrity.md), superseded by [Decision 0006](0006-validate-open-fail-fast-live-iterator.md).
- [Decision 0006: validate before exposure and fail fast on live iterator faults](0006-validate-open-fail-fast-live-iterator.md), superseded by [Decision 0008](0008-close-library-admission-and-trust-live-storage.md).
