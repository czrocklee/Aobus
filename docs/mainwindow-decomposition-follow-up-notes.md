# MainWindow Decomposition Follow-Up Notes

Date: 2026-05-01

## Context

These notes capture the follow-up architectural ideas that came out of the regression audit for the `MainWindow` decomposition work.

They are intentionally separate from `docs/mainwindow-decomposition-implementation-plan.md` because they are not required to restore correctness. They are optional improvements to consider after the current refactor is behaviorally stable.

## Main Recommendation

Do not add more abstractions as part of the regression-fix pass.

The first priority is to keep the current decomposition working end-to-end:

- sidebar rebuild and selection wiring must stay correct
- import/export flows must remain fully implemented
- playback snapshot propagation must remain behavior-complete

Once that wiring is stable, a small second pass can reduce callback sprawl by introducing narrow host interfaces for the extracted controllers.

## Suggested Next-Step Interfaces

The oracle suggested using tiny host interfaces instead of continuing to grow ad hoc callback bundles.

### `IListSidebarHost`

Purpose:

- handle list selection changes
- request tree and page rebuilds after list mutations
- provide access to per-list membership data when the sidebar needs preview context

Why:

- `ListSidebarController` currently relies on a callback bundle that mixes selection, rebuild, and membership lookup concerns
- a small explicit host interface would make the dependency clearer and easier to evolve

### `IImportExportHost`

Purpose:

- provide access to the current `LibrarySession`
- install a newly created `LibrarySession`
- report status/title/progress updates back to the window shell

Why:

- `ImportExportCoordinator` now needs both read access to the current session and write access to session replacement
- a dedicated host interface would make the open/import/export boundary more explicit than a growing callbacks struct

### Keep `IPlaybackHost`, but keep it small

Current direction is good:

- playback does not reach directly into `MainWindow`
- the host provides page lookup, page switching, and status updates

Guideline:

- only widen `IPlaybackHost` if playback coordination genuinely needs a new host responsibility
- do not turn it into a generic "window services" interface

## When To Introduce These Interfaces

Only do this follow-up if one or more of these conditions become true:

1. controller callback structs keep growing and start mixing unrelated responsibilities
2. the same wiring logic is duplicated across multiple controllers
3. import/export requires more lifetime coordination than a simple callback bundle can express cleanly
4. tests or reviews keep finding bugs caused by ambiguous ownership between coordinator and host

## Constraints For A Follow-Up Refactor

If this is implemented later, keep the scope tight:

- prefer one tiny interface per controller area
- do not add a shared "base host" or service locator
- do not introduce framework-style indirection
- keep `MainWindow` as the concrete shell that implements the host interfaces
- move one controller at a time so behavior remains easy to verify

## Proposed Order

If we decide to do this cleanup later, this order is the safest:

1. introduce `IImportExportHost`
2. introduce `IListSidebarHost`
3. revisit `IPlaybackHost` only if new responsibilities appear

That order keeps the higher-risk library/session ownership boundary explicit first, while leaving the already workable playback split mostly alone.

## Non-Goals

These ideas should not be used as justification for:

- rewriting the current controllers again
- adding generic abstraction layers
- merging unrelated cleanup into behavior-sensitive refactor work
- changing user-visible behavior

## Summary

The oracle's advice was not to add more architecture immediately.

The right near-term move is:

1. keep the extracted controllers behaviorally correct
2. keep callback boundaries understandable
3. introduce tiny host interfaces only if the callback wiring keeps growing and starts obscuring ownership
