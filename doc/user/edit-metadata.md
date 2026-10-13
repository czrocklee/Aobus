---
id: user.edit-metadata
---
# Edit track metadata

## Outcome

The selected tracks contain the intended curated metadata and tags, and list, query, and presentation surfaces observe the committed change.

## Steps

### Edit from the GTK detail surface

1. Select one track, or select several tracks for a shared edit.
2. Open the detail surface.
3. Use the edit button beside an ordinary built-in metadata value.
   Press Enter or click outside to commit; press Escape to cancel.
   Conductor, Ensemble, and Soloist previews instead open a scoped Credits list editor; the full Credits action edits all four kinds.
4. Use **Add...** in the Metadata section to add custom metadata.
   Custom keys must not duplicate a built-in or existing key.
5. Add or remove tags in the tag-chip editor.
6. For a compact multi-field form, right-click the selection and choose **Properties**.
   Right-click and choose **Edit Tags** for the tag workflow.

`<Multiple Values>` means the selected tracks disagree; it is a display marker and is never written as metadata.
Technical audio properties are read-only.
Deleting a custom value offers undo only when the prior value is unambiguous across the complete selection.
A committed Credits clear in GTK detail can likewise offer Undo for a common nonempty scope, even when another category differs; mixed scopes have no offer.

### Edit from the Windows desktop

1. Select one or more track rows.
2. Right-click a row and choose **Properties...**, choose **More > Properties...** in Modern, choose **View > Properties...** in Classic, or press `Alt+Enter`.
3. Edit ordinary built-in fields that have one value across the selection.
   `<Multiple Values>` identifies a field whose selected tracks disagree; it is not written back as metadata.
   Use a category's edit action or Full Credits for names, kinds, and optional roles; the details pane can open Properties directly at the requested scope.
4. Add or remove tags, and add, edit, or delete custom metadata.
   Tags shown in the dialog are shared by every selected track, and a custom-key change applies to the complete captured selection.
5. Review the read-only audio properties, then choose **Save**.
   Save becomes available only after a valid change and the dialog closes only after the library accepts it.

If the library changes while the dialog is open, Save is disabled and the draft is retained.
Choose Reload and confirm discarding the draft to refresh the captured targets, or close and reopen Properties.
Busy keeps your draft for retry.

### Edit Credits

Credits have four kinds: Conductor, Ensemble, Soloist, and Performer. Artist, Album Artist, and Composer are separate fields.
A category preview shows the first name and `+N` for additional entries, including duplicates; details show every entry and its optional role.
A category editor keeps its kind fixed. Full Credits lets you change kind, which moves the entry to the end of its new category; Up/Down moves only within a category.
Changing a name preserves its role and kind. Blank names prevent Save; a blank role means no role.

For several tracks, a common category loads its complete list even if other categories differ.
A mixed scope starts without a guessed list: choose **Replace this scope on all selected tracks** before authoring a replacement, or use the separate **Clear this scope** action.
Untouched categories retain each track's own entries. An untouched empty mixed draft is not a clear.

In Properties, **Save credits** accepts the child draft but writes nothing yet.
Finish or cancel that child before the parent Save becomes available, even when metadata or tags are already changed.
You can accept several category edits in sequence; the final Properties Save applies those scopes, ordinary metadata, and tags together or none of them.
Cancelling a child leaves previously accepted scopes unchanged.
Only GTK detail offers the scoped clear Undo described above; Windows, TUI, and AppKit Properties do not offer Credits Undo.

### Edit from AppKit or the TUI

In AppKit, choose **Edit Properties** in the inspector, including its compact sheet, then use the category or full Credits controls in Properties.
If the draft becomes stale, confirm discarding it when closing, then reopen Properties; there is no inline Reload.

In the TUI, open Properties with `e`, select a category preview and press Enter, or press `Ctrl+O` on Metadata for all Credits.
`Ctrl+S` accepts the child into Properties; a later parent `Ctrl+S` submits the complete edit. `Esc` cancels only the active child.
See [TUI editing](use-tui.md#select-and-edit-tracks) for controls and confirmed reload.

### Edit from the CLI

1. Preview a bulk mutation:

   ```bash
   aobus -C /music track update --filter '$album == "Kind of Blue"' \
     --album-artist "Miles Davis" --set source=curated --dry-run
   ```

2. Review the matched track count and field changes.
3. Repeat the command without `--dry-run` to commit it.

Use an explicit track id instead of `--filter` when the change must target one known record.

## Verify the result

- Reopen the track detail or run `aobus -C /music track show <id>` and confirm the new values.
- Queries and saved Lists using the changed fields update to the new result.
- The frontend reporting surface reports a failure instead of showing an uncommitted edit as saved.

## Related documents

- [Metadata-editing specification](../system/presentation/metadata-editing.md)
- [GTK track-detail specification](../system/frontend/gtk/track-detail.md)
- [Windows desktop shell specification](../system/frontend/windows.md)
- [Track field reference](../reference/library/model/track-field.md)
- [CLI command reference](../reference/cli/command.md)
