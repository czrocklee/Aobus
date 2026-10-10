# AppKit Track Properties and Credits

AppKit's inspector shows complete common credit sections with each entry's kind
and optional role, including duplicates. A short-lived [detail projection](../library/track-detail-projection.md#aggregation)
supplies owning per-kind aggregates, with no read transaction retained while
updating native controls. A mixed category displays “Multiple values”; common
categories remain visible independently.
Missing selected tracks contribute no display values, so surviving tracks still
supply their aggregates; when none resolve, Credits sections are absent. The
inspector retains the requested selection count.

The inspector's **Edit Properties** action, including the compact inspector sheet,
opens the same Properties editor as the library commands. The compact sheet ends
before Properties attaches. The inspector has no direct scoped Credits action,
separate write session, or scalar credit editor. Properties admission still
requires every captured target to exist; display omissions never authorize a
surviving-subset edit.

## One Properties draft

`LibraryEditorModel` captures its authoring binding, ordinary fields, tags and
complete credit sections from the same short-lived library snapshot. The native
`AobusTrackCreditsEditor` is an inline child of that Properties draft, not an
independent database writer.

Properties offers read-only category summaries and scoped edit buttons, plus a
Credits section with all-kind and category entry points. Summaries use the first
name and an additional-entry count, including duplicates. The Credits section
shows the complete accepted draft rather than interpreting a summary as input.
The current native browser has no category columns; it does not cache complete
credits on table rows.

A category session locks kind. An all-kind session exposes a kind popup for each
row. Both use the shared list editor for empty, single-entry and larger lists.
Native controls edit names and optional roles, add and delete rows, and reorder
within a kind. Reclassification appends to the destination kind through the
shared model. Name completion follows the row's kind; role completion uses the
shared role vocabulary.

A mixed scope starts without rows and requires explicit replacement intent.
An empty replacement draft is not an implicit clear: **Clear this scope** is a
separate action beside the visible scope and target count. Invalid rows retain
the draft and expose error text and accessibility help; added and moved rows
receive native focus. Controls live in the scrollable Properties form rather
than expanding the sheet beyond its host.

**Save credits** stages a scoped replacement in Properties. Cancelling the child
leaves previously accepted scopes unchanged. Reopening uses the captured baseline
plus accepted scopes, never a newer library snapshot. While a child is active,
Properties Save is disabled and programmatic Save is rejected, including when
tags or ordinary fields are dirty. Final Properties Save submits the composed
patch through the existing guarded authoring session once.

## Stale drafts and close

Busy submission retains the draft. Any intervening revision makes the session
stale and freezes its controls without changing the visible candidates. AppKit
refreshes a stale authoring baseline by closing and reopening Properties; there
is no in-place Reload action. Closing with ordinary changes or an active Credits
child requires the native discard decision. Keep Open retains the stale child;
confirmed discard detaches completion adapters before retiring the model.

The child shares the existing Properties close transaction and does not attach
another sheet. Child Save and Cancel have native Return and Escape equivalents;
the parent relinquishes those equivalents while the child is active. Lifecycle
close and Quit still join the parent's discard confirmation and pending-save
settlement. AppKit does not offer scoped Credits Undo in this Properties surface.
