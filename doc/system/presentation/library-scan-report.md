---
id: shell.library-scan-report
---
# Library scan report

## Scope

This contract starts when `runLibraryScanAsync` finishes. It defines the shared
verdict, severity, retention, diagnostics responsibility, localized formatting,
and deferred-work handoff. Scan planning and application belong to the
[scan and identity specification](../library/scan-and-identity.md).

The outcome is an in-process UIModel value and has no serialization version. A
shell posts the decision; it does not derive a second verdict or sentence.

## Outcome contract

`runLibraryScanAsync` reduces workflow state to `LibraryScanOutcome`.
`libraryScanSeverity` and `libraryScanLifetime` decide presentation strength and
retention, while `formatLibraryScanMessage` formats the typed outcome through
the injected catalog.

| Verdict | Condition | Severity | Lifetime |
| --- | --- | --- | --- |
| `UpToDate` | Planning found nothing to apply. | Info | Transient |
| `Complete` | A plan applied without missing or failed items. Stale evidence left for a later scan is not an error. | Info | Transient |
| `NeedsReview` | The plan applied, but known tracks are missing. | Warning | History |
| `CompletedWithErrors` | The plan applied and one or more items failed. | Warning | History |
| `Unreadable` | Planning found errors only, so nothing was applied. | Error | History |
| `Failed` | The workflow failed, or an actionable plan returned no apply result. | Error | History |

Anything requiring attention remains in history; a clean confirmation is
transient. Relinking moved files is informational rather than a warning.
Application failures outrank missing files when choosing a verdict, while both
missing and relinked counts remain available to the formatter. Every verdict
must produce nonempty text. Exact English and translated sentences belong to
the [catalog assets](../../../app/i18n/catalog/root.txt), not to a second table
here.

An unknown verdict follows conservative failure formatting without inventing an
error reason. Paths and diagnostic reasons remain external values passed into
a catalog pattern.

## Diagnostics ownership

The decision pass logs the plan summary and workflow failure. When planning
finds only unreadable items, it also logs those issues because no apply pass saw
them. For an actionable plan, application already reports each failed item, so
the outcome pass must not duplicate those entries.

This is one ownership rule: a shell obtains scan diagnostics by invoking the
shared workflow and must not recreate the same log decisions.

## Deferred audio identity work

Fast-bootstrap mode may defer audio-identity indexing. The outcome carries
`shouldBackfillAudioIdentity`; a shell that requests this mode must honor the
flag after presenting the scan result.

GTK uses fast bootstrap at startup and starts the backfill from
`LibraryImportExportWorkflow::presentScanOutcome`. WinUI, TUI, and AppKit
currently request eager scans, so their outcomes do not request the backfill.
Any new fast-bootstrap caller must consume the flag rather than infer it from
shell state.

## Shell presentation

GTK posts the shared severity, formatted text, and lifetime from
`LibraryImportExportWorkflow::presentScanOutcome`.

WinUI does the same in `LibrarySession::finishActiveScan`. It additionally sends
an Error outcome to its failure surface and a Warning outcome to its status
line. This matters because only shells containing `status.activity` display the
notification feed, and the shipped Classic preset lacks that component. Info
outcomes restore the ready-library status.

TUI's `LibraryScanController` and AppKit's `LibrarySession` likewise post the
shared severity, message, and lifetime to runtime notifications. These are
placement decisions, not alternate scan verdicts.

## Evidence

- [`LibraryScanOutcome.h`](../../../app/include/ao/uimodel/library/task/LibraryScanOutcome.h)
  defines the outcome and public decision functions.
- [`LibraryScanOutcome.cpp`](../../../app/uimodel/library/task/LibraryScanOutcome.cpp)
  owns reduction, severity, lifetime, and diagnostics.
- [`ActivityPresentationText.cpp`](../../../app/uimodel/status/activity/ActivityPresentationText.cpp)
  formats the typed outcome.
- [`LibraryImportExportWorkflow.cpp`](../../../app/linux-gtk/portal/LibraryImportExportWorkflow.cpp)
  and [`LibrarySession.cpp`](../../../app/windows-winui/app/LibrarySession.cpp)
  own shell posting and placement. [`LibraryScanController.cpp`](../../../app/tui/LibraryScanController.cpp)
  and AppKit [`LibrarySession.cpp`](../../../app/macos-appkit/LibrarySession.cpp)
  own the other interactive consumers.
- [`LibraryScanOutcomeTest.cpp`](../../../test/unit/uimodel/library/task/LibraryScanOutcomeTest.cpp)
  protects verdict precedence, retention, formatting completeness, and deferred
  work; [`LibraryImportExportWorkflowTest.cpp`](../../../test/unit/linux-gtk/portal/LibraryImportExportWorkflowTest.cpp)
  protects GTK posting.

## Related documents

- [Interactive session lifecycle](../session-lifecycle.md)
- [Library architecture](../library/structure.md)
- [Scan and identity](../library/scan-and-identity.md)
- [Activity status](activity-status.md)
