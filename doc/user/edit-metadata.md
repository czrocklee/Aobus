---
id: user.edit-metadata
---

# Edit track metadata

## Outcome

The selected tracks contain the intended metadata and tags, and lists, searches, and views reflect the saved change.

## Steps

### Edit from the GTK detail surface

1. Select one track, or select several tracks for a shared edit.
2. Open the detail surface.
3. Use the edit button beside an ordinary built-in metadata value.
   Press Enter or click outside to commit; press Escape to cancel.
   Conductor, Ensemble, and Soloist previews instead open a scoped Credits list editor; the full Credits action edits every kind.
4. Choose the **Custom Metadata** (+) button in the Metadata section, enter a **Key** and **Value**, then choose **Add**.
   Custom keys must not duplicate a built-in or existing key.
5. Add or remove tags in the tag-chip editor.
6. For a compact multi-field form, right-click the selection and choose **Properties**.
   Right-click and choose **Edit Tags** for the tag workflow.

`<Multiple Values>` means the selected tracks disagree; it is a display marker and is never written as metadata.
Technical audio properties are read-only.
Deleting a custom value offers undo only when the prior value is unambiguous across the complete selection.
After you choose **Clear this scope → Save credits** in GTK detail, **Undo** can appear only if the selected tracks all had the same list of credits in that scope, with at least one credit to restore.
Differences outside the cleared scope do not prevent **Undo**.

### Edit from the Windows desktop

1. Select one or more track rows.
2. Right-click a row and choose **Properties...**, choose **More > Properties...** in Modern, choose **View > Properties...** in Classic, or press `Alt+Enter`.
3. Edit ordinary built-in fields that have one value across the selection.
   `<Multiple Values>` identifies a field whose selected tracks disagree; it is not written back as metadata.
   To edit names and optional roles, choose a category button such as **Credits scope: Conductor**. Its kind is fixed; choose **Credits scope: All credit kinds** to change kinds. The details pane can open **Properties** directly at a scope.
4. Add or remove tags, and add, edit, or delete custom metadata.
   The tag list starts with the tags shared by all the tracks selected when you opened **Properties**.
   When you save, custom metadata changes apply to all of those tracks, even if the selection changes afterward.
5. Review the read-only **Audio Properties**, then choose **Save**.
   **Save** becomes available only after a valid change, and saving closes the dialog only after the library accepts the change.

If the library changes while the dialog is open, **Save** is disabled and the dialog keeps your draft.
Choose **Reload**, then **Discard Changes** to load the latest values for the same tracks, or close and reopen **Properties**.
If you see **Library is busy. Try again.**, the dialog keeps your draft so you can retry.

### Edit Credits

Credits have twelve kinds: Conductor, Ensemble, Soloist, Performer, Lyricist, Arranger, Writer, Producer, Engineer, Mixer, DJ-mixer, and Remixer. A kind after Performer appears in Detail only when a selected track has its credits or empty fields are shown; otherwise add the first one from the full Credits action. Artist, Album Artist, and Composer are separate fields.
A category preview shows the first name and `+N` for additional entries, including duplicates; details show every entry and its optional role for a common category, or `<Multiple Values>` for a differing category.
A category editor keeps its kind fixed. Full Credits lets you change kind, which moves the entry to the end of its new category; Up/Down moves only within a category.
Changing a name preserves its role and kind. Blank names prevent Save; a blank role means no role.

For several tracks, a common category loads its complete list even if other categories differ.
A mixed scope starts without a guessed list: choose **Replace this scope on all selected tracks** before authoring a replacement, or use the separate **Clear this scope** action.
Untouched categories retain each track's own entries. An untouched empty mixed draft is not a clear.

In Properties, **Save credits** accepts the child draft but writes nothing yet.
Finish or cancel that child before the parent Save becomes available, even when metadata or tags are already changed.
You can accept several category edits in sequence; the final Properties Save applies those scopes, ordinary metadata, and tags together or none of them.
Cancelling a child leaves previously accepted scopes unchanged.
Only GTK detail offers the scoped clear Undo described above; Windows and TUI Properties do not offer Credits Undo.

### Edit from the TUI

In the TUI, open Properties with `e`, select a category preview and press Enter, or press `Ctrl+O` on Metadata for all Credits.
`Ctrl+S` accepts the child into Properties; a later parent `Ctrl+S` submits the complete edit. `Esc` cancels only the active child.
See [TUI editing](use-tui.md#select-and-edit-tracks) for controls and confirmed reload.

### Edit from the CLI

1. Preview a bulk update:

   ```bash
   aobus -C /music -O json track update --filter '$album = "Kind of Blue"' \
     --album-artist "Miles Davis" --set source=curated --dry-run
   ```

2. Review the matched track count and field changes.
3. Repeat the command without `--dry-run` to commit it.

Use an explicit track id instead of `--filter` when the change must target one known record.

## Verify the result

- Reopen the track detail or run `aobus -C /music track show <id>` and confirm the new values.
- Queries and saved Lists using the changed fields update to the new result.
- If saving fails, the application reports the error instead of showing the unsaved edit as saved.

## Related documents

- [Metadata-editing specification](../system/presentation/metadata-editing.md)
- [GTK track-detail specification](../system/frontend/gtk/track-detail.md)
- [Windows desktop shell specification](../system/frontend/windows.md)
- [Track field reference](../reference/library/model/track-field.md)
- [CLI command reference](../reference/cli/command.md)
