---
id: rfc.0019.safe-active-library-replacement
type: rfc
status: draft
domain: linux-gtk
summary: Proposes preparing a replacement GTK library window before releasing the active one.
depends-on: none
---
# RFC 0019: Safe active-library replacement

## Problem

GTK currently checkpoints and destroys the active window/runtime pair before constructing the selected replacement.
If database open, runtime construction, workspace restore, or window initialization fails, the application has no working main window to return to.

The selected path is also saved before replacement construction succeeds.
The next launch can therefore retry a library that never became usable.

Existing callback-scope and idle-registration guards already make late chooser callbacks harmless.
The remaining defect is replacement ordering.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: None.

## Goals

- Keep the current pair usable until a replacement pair is initialized.
- Destroy a failed replacement without changing the current pair or saved selection.
- Save the selected path only after the replacement becomes active.
- Preserve observer-before-runtime destruction for both pairs.
- Cover the complete replacement order with focused GTK tests.

## Non-goals

- Move replacement out of the private GTK composition flow.
- Canonicalize symlink, junction, case, or network-filesystem identity beyond current path policy.
- Retarget a live `AppRuntime` to another root.
- Preserve playback position across a library switch.
- Introduce library-bound session maps, a no-library runtime, or a frontend-neutral lifecycle service.

## Proposed design

Split current `createWindow()` work into two private GTK operations:

- prepare constructs the executor, stores, `AppRuntime`, window, controllers, and restored workspace without adding or presenting the window, restoring playback, or taking over MPRIS;
- activate enables application-wide adapters, adds the prepared window, and presents it.

For a different normalized root, the existing idle callback performs:

1. Prepare the replacement while the current pair remains active.
2. If preparation fails, destroy only the replacement and report through the existing GTK failure boundary.
3. Ask the current window to perform its existing final checkpoint, playback-session discard, and stale-save guard.
4. Activate the replacement and make it the application's active pair.
5. Remove and release the old pair.
6. Save the new selected path best-effort.
7. Start the optional bootstrap scan.

Candidate preparation does not restore a global playback session for the other library.
The replacement starts playback idle, matching the current product decision that playback-position continuity is optional.

`MainContextCallbackScope` and the existing single idle registration remain the lifetime mechanism.

## Alternatives

### Keep destroy-then-create

This minimizes overlap but turns any replacement construction failure into loss of the working application graph.

### Persist selection before activation

That can make restart select a candidate that failed before it was ever usable.
Saving after activation may retain the previous selection if the save fails, but it does not destroy the working in-process pair.

## Compatibility and migration

No persisted schema changes.
The existing normalized path comparison and temporary fallback behavior remain unchanged.

## Validation

- Failure at every replacement preparation step leaves the old window active and the saved path unchanged.
- Successful activation releases old frontend observers before the old runtime.
- Playback-session discard failure aborts replacement and destroys only the prepared pair; existing best-effort checkpoints remain best effort.
- The selected path is saved only after the replacement is active.
- A failed candidate never becomes the MPRIS owner or an application window.
- Late chooser and idle callbacks remain harmless after old-pair and application teardown.
- Same-root selection reuses the current pair and optional scan behavior.
- Replacement starts playback idle and does not interpret another library's stored track/list ids.

## Promotion plan

Update the interactive-session architecture and GTK active-library lifecycle specification with prepare-before-release ordering.
No new runtime surface reference or frontend-neutral lifecycle document is planned.
