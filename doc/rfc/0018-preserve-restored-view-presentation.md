---
id: rfc.0018.preserve-restored-view-presentation
type: rfc
status: draft
domain: presentation
summary: Proposes keeping an exact restored view presentation instead of replacing it with a list default.
depends-on: none
---
# RFC 0018: Preserve restored view presentation

## Problem

Workspace restore reconstructs each view with its saved `TrackPresentationSpec` and seeds history from the active restored view.
GTK then iterates every open view and calls `ViewService::setPresentation()` with the per-list preference or recommendation.

That pass replaces exact restored state even though list preferences are defined as defaults for newly opened views.
It also bypasses the workspace commit path, so the visible view can disagree with the initial history point.
Playback-session reveal can repeat the same overwrite when it reuses a restored plain view while supplying a list default.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: None.

## Goals

- Keep the exact presentation on every successfully restored workspace view.
- Use a per-list preference only when GTK creates a new view.
- Keep a reused restored view and its navigation history in agreement.
- Preserve current fallback behavior for first-run and playback-created views.

## Non-goals

- Move startup into a shared runtime service.
- Make TUI restore GTK-managed preferences.
- Change workspace, preference, or playback-session file formats.
- Change checkpoint or active-library replacement behavior.

## Proposed design

Remove GTK's post-restore `applyPresentationPreferencesToOpenViews()` pass.
Workspace restore remains the sole source of the presentation for restored entries.

Keep the existing preference resolution at actual creation points:

- first-run All Tracks creation;
- normal list navigation that opens a new view;
- playback restore when no reusable plain view already exists.

When playback restore reuses an existing plain view, navigate without a presentation override so `WorkspaceService` retains that view's exact spec.
Later user presentation changes continue through the normal workspace command and preference-save path.

No new runtime service, state type, startup phase, or persistence field is required.

## Alternatives

### Let the list preference win at every startup

That makes the preference a mirror rather than a default and discards the exact presentation already stored with the workspace view.

### Rebuild workspace history after the GTK overwrite

This would hide one inconsistency while still losing saved view state and bypassing workspace ownership.

### Restore the former interactive-lifecycle proposal

A shared lifecycle service, checkpoint records, readiness phases, and frontend convergence are unrelated to this precedence bug.

## Compatibility and migration

No persisted shape changes.
After the change, existing workspace presentations remain visible at startup; saved list preferences still choose presentations for newly created views.

## Validation

- Restore a view whose exact presentation differs from its saved list preference and prove the exact spec remains active.
- Prove the first history point matches the visible restored presentation.
- Restore playback into an existing plain view and prove its presentation is retained.
- Restore playback without a reusable view and prove the saved preference is used for the new view.
- Keep first-run All Tracks preference and ordinary list-navigation tests passing.

## Promotion plan

Update the interactive-session lifecycle architecture, workspace session specification, and list-preference specification with the corrected GTK startup rule, then delete this RFC after implementation.
