---
id: shell.library-scan-report
type: reference
status: current
domain: application-shell
summary: Enumerates the verdicts a finished library scan reduces to, the severity and lifetime each carries, and the sentence every shell reports it with.
---
# Library scan report

## Scope and version

This reference owns what a finished library scan is reported as: the verdict it reduces to, how loudly that verdict is said, how long the report stays reachable, and the sentence itself.

It does not own how the scan runs. Planning, applying, and the deferred audio-identity pass belong to the [scan and identity spec](../../spec/library/runtime/scan-and-identity.md); this reference begins where `runLibraryScan` returns.

The report carries no version of its own. It is an in-process decision, never serialized, so a verdict may be added or a sentence reworded without a compatibility step.

## Code boundary

The report belongs to the **UIModel** layer in the [system architecture](../../architecture/system-overview.md).

`uimodel::runLibraryScan` reduces the private workflow state to a `LibraryScanOutcome`, `uimodel::libraryScanSeverity` and `uimodel::libraryScanLifetime` decide how it is presented, and `uimodel::formatLibraryScanMessage` writes the sentence.

A shell posts the result. It does not decide it.
A scan that lost twelve files is the same event whichever window reports it, and a shell that reaches its own verdict is how two windows come to describe one scan differently.

Deciding also records the diagnostics: the plan summary and any failure reach the log from that one pass, and the unreadable files do when nothing was applied. A shell that wants scan diagnostics gets them by calling the decision, not by writing its own log lines.

An applied plan is the exception: the apply operation reports each failed item as it reaches it, so repeating them here would double every entry. Whichever pass sees an item first is the one that records it.

## Verdicts

| Verdict | Reached when | Severity | Lifetime |
| --- | --- | --- | --- |
| `UpToDate` | The plan found nothing to apply. | Info | Transient |
| `Complete` | The plan applied, losing nothing. | Info | Transient |
| `NeedsReview` | The plan applied, but tracks the library knows about are missing. | Warning | History |
| `CompletedWithErrors` | The plan applied, and some files failed along the way. | Warning | History |
| `Unreadable` | Planning found only unreadable files, so nothing was worth applying. | Error | History |
| `Failed` | The scan did not finish, or reported changes with no result. | Error | History |

Severity decides lifetime: anything a person may have to act on is retained, because whoever has to act on it may not have been looking when it appeared. An outcome that only confirms the library is as they left it passes.

Relinking is not a warning. A moved file that was found again cost the reader nothing, so it is reported at Info with the rest of a clean scan.

## Sentences

| Verdict | Message |
| --- | --- |
| `UpToDate` | `Library is up to date` |
| `Complete` | `Library scan complete`, or `Relinked N moved file(s)` when files moved |
| `NeedsReview` | `N missing file(s) need(s) review`, prefixed with `Relinked N moved file(s); ` when both happened |
| `CompletedWithErrors` | `Scan completed with errors`, followed by `; ` and the change summary when anything moved or went missing |
| `Unreadable` | `Library scan found N unreadable file(s) and no usable changes` |
| `Failed` | `Scan failed: <reason>` |

Every verdict has a sentence. A verdict without one would reach a notification as empty text, which is asserted against rather than left to review.

## Deferred work

A fast-bootstrap scan defers audio-identity indexing and sets `shouldBackfillAudioIdentity` on the outcome. A shell that starts such a scan must honor the flag, or the library stays permanently half-indexed with nothing to say so.

The GTK shell bootstraps this way at startup and starts the backfill. The Windows shell only ever rescans eagerly, so the flag is never set there; a Windows bootstrap scan would have to honor it before it could be added.

## Shells

- The GTK shell posts the outcome from `LibraryImportExportWorkflow::presentScanOutcome`.
- The Windows shell posts it from `LibrarySession::finishActiveScan`, which routes an Error verdict to its failure callback and a Warning verdict to the status line. Only a shell carrying a `status.activity` component presents the notification feed, and the Classic preset does not, so a warning that lived only in the feed would reach those users as a plain ready library. An Info verdict leaves the status line reading the ready library.

## Implementation authority

- [`LibraryScanOutcome.h`](../../../app/include/ao/uimodel/library/task/LibraryScanOutcome.h) and [`LibraryScanOutcome.cpp`](../../../app/uimodel/library/task/LibraryScanOutcome.cpp) own `runLibraryScan`, the verdicts, the severity and lifetime mapping, and the diagnostics.
- [`ActivityPresentationText.cpp`](../../../app/uimodel/status/activity/ActivityPresentationText.cpp) owns `formatLibraryScanMessage`.
- [`LibraryImportExportWorkflow.cpp`](../../../app/linux-gtk/portal/LibraryImportExportWorkflow.cpp) and [`LibrarySession.cpp`](../../../app/windows-winui/app/LibrarySession.cpp) post the decision in their shells.

## Test authority

- [`LibraryScanOutcomeTest.cpp`](../../../test/unit/uimodel/library/task/LibraryScanOutcomeTest.cpp) protects every verdict, its severity and lifetime, its sentence, the deferred-work flag, and that no verdict is silent.
- [`LibraryImportExportWorkflowTest.cpp`](../../../test/unit/linux-gtk/portal/LibraryImportExportWorkflowTest.cpp) protects the GTK shell posting them against a live runtime.

## Related documents

- [Interactive session lifecycle architecture](../../architecture/interactive-session-lifecycle.md)
- [Library architecture](../../architecture/library.md)
- [Scan and identity specification](../../spec/library/runtime/scan-and-identity.md)
- [Shared component vocabulary](component-vocabulary.md)
