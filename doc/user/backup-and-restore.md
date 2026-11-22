---
id: user.backup-and-restore
---

# Back up and restore library data

## Outcome

You have a portable YAML backup of the library data you chose to save and can use it to restore or merge that data.

## Understand the boundary

Depending on the export mode, a library YAML export can include Aobus metadata, lists, cover references, audio properties, and file details such as size and modification time.
It does not copy the audio files themselves, and it never contains a cover image: a **Full** export records each cover's identity, and Aobus reads the picture back out of your music files.
Back up the music files separately and preserve their paths relative to the music root; without them, a restored library keeps its cover references but may have no picture to show.

`restore` replaces the payload-selected target scope; `merge` preserves target entities absent from the payload and adds imported lists as new lists.
CLI import defaults to merge; committing a restore requires an explicit mode and destructive-confirmation flag.

Backups are version 1 YAML, the first supported format, and only version 1 is accepted.
Libraries and backups made by development builds before the first release are not supported, and no build converts them.
Such a library can be rebuilt by explicitly scanning its music files, which loses curation such as lists and edited metadata.
Opening a library of another version never converts, relabels, or deletes it automatically.
A library or backup whose format is newer than this build supports is refused and left unchanged; open or import it with the version of Aobus that wrote it or a later one.

A disc image that a CUE sheet splits is several tracks with one path. A backup records each track's CUE number and range, so a **Full** restore can rebuild that split without opening the audio file.
**Merge** updates a split track only when the backup's path and CUE number match, and it never turns a whole file into a split file or the reverse. To change that form, remove the tracks and scan, as [Manage a music library](manage-library.md#disc-images-and-cue-sheets) describes.
A **Lists only** backup names a split track by its path and CUE number.

Track paths belong to the selected music root.
When Aobus resolves an audio path on disk, it accepts symlinks to existing targets inside that root and rejects dangling symlinks or links that lead outside the root.
An import does not always need to resolve the path.
For example, a **Full** merge can update an existing track matched by its relative path even if its audio file has been replaced by a dangling symlink: the merge uses the library's saved file record without following the link.

## Steps

### Create a backup

1. In GTK, choose **File → Export Library Data...**. On Windows, choose **More → Export Library Data...** in Modern mode or **File → Export Library Data...** in Classic mode.
2. Choose the payload you need:
   **Full** includes curated metadata, audio properties, file sizes and recorded modification times, cover references, and lists; **Metadata** omits technical statistics and cover references; **Delta** records edits relative to readable files; **Lists only** contains lists without track records.
3. Choose a `.yaml` file and wait for **Library exported successfully**. On Windows, Modern's activity surface names the file while export is running; switching to Classic leaves the transfer running and its status bar reports completion.

**Delta**, **Metadata**, and **Full** carry [deliberately cleared titles](edit-metadata.md#keep-a-title-blank), even when the audio file is missing or its title is already blank. Import remembers those clears for later exports.
An ordinary blank title is included only in **Delta**, when a readable audio file has a nonempty title. Import sets the title to blank without recording a clear.
If a backup omits the title, **Merge** keeps the matching track's title and any recorded clear.

The CLI equivalent for a complete library-data backup is:

```bash
aobus -C /music lib export /backup/library.yaml --mode full
```

### Preview an import

From the CLI, run a dry run in the same mode you intend to apply:

```bash
aobus -C /target -O json lib import /backup/library.yaml --mode restore --dry-run
```

Review `payloadVersion`, `payloadMode`, `targetScope`, the track/list create-update-delete counts, `danglingReferencesIgnored`, and `invalidFiltersKept`.
A nonzero `invalidFiltersKept` counts List filters that do not parse or compile: those Lists restore with their filter text unchanged, and they and the Lists nested under them show no members until the filter is fixed.
Do not continue if the root, mode, or deletion counts are unexpected.

### Apply the import

For a deliberate replacement:

```bash
aobus -C /target lib import /backup/library.yaml --mode restore --confirm-destructive-restore
```

For an additive import that preserves target tracks and lists outside the payload:

```bash
aobus -C /target lib import /backup/library.yaml --mode merge
```

- **GTK:** Choose **File → Import Library Data...** and select a backup. GTK imports only in Restore mode.
  Review the scope and counts, then choose **Restore Library** or **Restore Lists** to apply it.
- **Windows:** Choose **More → Import Library Data...** in Modern mode or **File → Import Library Data...** in Classic mode.
  Choose the import mode, then select a backup.
  - **Merge** is the default. After you choose the file, Aobus applies the merge automatically, without showing a preview or asking for further confirmation.
    To preview a merge, use the CLI dry run above with `--mode merge`.
  - **Restore** shows the scope and counts. Review them, then choose **Restore Library** or **Restore Lists** to apply it.

Canceling the file picker, the restore preview, or the Windows mode dialog leaves the target unchanged.

## Verify the result

- `aobus -C /target lib stats` reports the expected track, list, resource, and storage counts.
- `aobus -C /target lib verify` reports no missing or error-class issues.
- Representative tracks retain the expected curated metadata, tags, covers, List membership, and saved List order.
- The separately restored audio files exist at the paths expected beneath the target music root.

Covers appear without a rescan. A restored cover is shown as soon as some restored audio file still carries it, or when the machine's cover cache still holds it, which is the usual case when you restore onto the machine that produced the backup.
A **Metadata** or **Delta** restore instead takes each track's covers from the audio file itself, so a file you retagged since the backup shows its current art.
A cover with neither a cached copy nor a music file that carries it shows no image; the track keeps its other data, and the picture returns when the file does.

A **Restore** into an existing library gives every restored track and list an ID that library has never used, so numeric IDs in scripts change, and settings that name the old IDs find no record: per-list column layouts and presentation choices and open list views are dropped, and the saved playback position is not resumed.
After a restore made while the app was closed, the **All Tracks** layout and presentation choice name no record and still apply; a restore applied in the running app resets every column layout and presentation choice, **All Tracks** included.
A **Merge** keeps the ID of each existing track it updates; every list it imports gets an ID the library has never used.
A new library numbers its tracks and lists from 1, so after an import its IDs can repeat the numbers in the backup.
Restoring into a new library keeps that library's own **Library ID**, which `aobus lib show` reports; a backup carries no Library ID of its own.
Settings that an earlier library saved, such as those left in the same music root's `.aobus` folder, record that library's ID, so the new library ignores everything in them that names a track or list: open list views, per-list column layouts and presentation choices, and the playback position.
Custom presentation presets and the **All Tracks** layout name no record and still apply.

A track already listed as missing stays missing after a merge. If the merge changes a file's recorded size or modification time, Aobus discards the old audio fingerprint; otherwise it keeps it. Tracks split from one disc image share that fingerprint.
If you rename a file with no fingerprint, a scan may add it as new and leave your edited tracks listed as missing under the old name.

If the fingerprint is pending and you plan to rename the file, take these steps while it is still at its original path:

- If Aobus lists the track as missing or the file's size or modification time differs from the library's record, run `aobus -C /target scan` to refresh the record.
- If the fingerprint is still pending, run `aobus -C /target lib fingerprint --pending`. It only processes files marked available whose size and modification time exactly match the library's record. The command can succeed even if it skips the file.

After renaming, [preview the scan](manage-library.md#preview-and-apply-a-scan-from-the-cli) and check that it will reconnect the existing tracks rather than add new ones.

## Related documents

- [Library YAML transfer specification](../system/library/yaml-transfer.md)
- [Library YAML format reference](../reference/library/format/yaml.md)
- [CLI command reference](../reference/cli/command.md)
