---
id: user.edit-metadata
type: user-guide
status: current
domain: presentation
summary: Edits built-in fields, custom metadata, and tags for one or more tracks.
---
# Edit track metadata

## Outcome

The selected tracks contain the intended curated metadata and tags, and list, query, and presentation surfaces observe the committed change.

## Steps

### Edit from the GTK detail surface

1. Select one track, or select several tracks for a shared edit.
2. Open the detail surface.
3. Use the edit button beside a built-in metadata value.
   Press Enter or click outside to commit; press Escape to cancel.
4. Use **Add...** in the Metadata section to add custom metadata.
   Custom keys must not duplicate a built-in or existing key.
5. Add or remove tags in the tag-chip editor.
6. For a compact multi-field form, right-click the selection and choose **Properties**.
   Right-click and choose **Edit Tags** for the tag workflow.

`<Multiple Values>` means the selected tracks disagree; it is a display marker and is never written as metadata.
Technical audio properties are read-only.
Deleting a custom value offers undo only when the prior value is unambiguous across the complete selection.

### Edit from the Windows desktop

1. Select one or more track rows.
2. Right-click a row and choose **Properties...**, choose **More > Properties...** in Modern, choose **View > Properties...** in Classic, or press `Alt+Enter`.
3. Edit built-in fields that have one value across the selection.
   `<Multiple Values>` identifies a field whose selected tracks disagree; it is not written back as metadata.
4. Add or remove tags, and add, edit, or delete custom metadata.
   Tags shown in the dialog are shared by every selected track, and a custom-key change applies to the complete captured selection.
5. Review the read-only audio properties, then choose **Save**.
   Save becomes available only after a valid change and the dialog closes only after the library accepts it.

If the library changes while the dialog is open, close and reopen Properties from the current selection before trying again.

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

- [Metadata-editing specification](../spec/presentation/metadata-editing.md)
- [GTK track-detail specification](../spec/linux-gtk/track-detail.md)
- [Windows desktop shell specification](../spec/shell/windows-desktop.md)
- [Track field reference](../reference/library/model/track-field.md)
- [CLI command reference](../reference/cli/command.md)
