---
id: user.backup-and-restore
---
# Back up and restore library data

## Outcome

You have a portable YAML backup of the selected library data and can preview and apply it with an explicit restore or merge policy.

## Understand the boundary

A library YAML export contains Aobus metadata, lists, cover references, and mode-dependent technical or manifest facts.
It does not copy the audio files themselves, and it never contains a cover image: a **Full** export records each cover's identity, and Aobus reads the picture back out of your music files.
Back up the music files separately and preserve their paths relative to the music root; without them, a restored library keeps its cover references but may have no picture to show.

`restore` replaces the payload-selected target scope; `merge` preserves target entities absent from the payload and adds imported lists as new lists.
GTK and the Windows desktop prepare a restore preview and require confirmation before changing the library.
CLI import defaults to merge; committing a restore requires an explicit mode and destructive-confirmation flag.

Backups are version 1 YAML, the first supported format, and only version 1 is accepted.
Libraries and backups made by development builds before the first release are not supported, and no build converts them.
Such a library can be rebuilt by explicitly scanning its music files, which loses curation such as lists and edited metadata.
Opening a library of another version never converts, relabels, or deletes it automatically.

A disc image that a CUE sheet splits is several tracks with one path. A backup records each track's CUE number and range, so a **Full** restore can rebuild that split without opening the audio file.
**Merge** updates a split track only when the backup's path and CUE number match, and it never turns a whole file into a split file or the reverse. To change that form, remove the tracks and scan, as [Manage a music library](manage-library.md#disc-images-and-cue-sheets) describes.
A **Lists only** backup names a split track by its path and CUE number.

Track paths belong to the selected music root.
Symlinks whose existing targets remain inside that root are supported, while dangling symlinks and symlinks into another tree are rejected during export, import baselining, scanning, and playback.

## Steps

### Create a backup

1. In GTK, choose **File → Export Library Data...**. On Windows, choose **More → Export Library Data...** in Modern mode or **File → Export Library Data...** in Classic mode.
2. Choose the payload you need:
   **Full** includes curated metadata, technical and manifest facts, cover references, and lists; **Metadata** omits technical statistics and cover references; **Delta** records edits relative to readable files; **Lists only** contains lists without track records.
3. Choose a `.yaml` file and wait for **Library exported successfully**. On Windows, Modern's activity surface names the file while export is running; switching to Classic leaves the transfer running and its status bar reports completion.

The CLI equivalent for a complete library-data backup is:

```bash
aobus -C /music lib export /backup/library.yaml --mode full
```

### Preview an import

Run the same mode you intend to commit:

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

GTK **File → Import Library Data...** and Windows **More/File → Import Library Data...** both use the same prepared-plan contract.
Choose **Merge** to preserve records outside the backup and apply the prepared plan, or **Restore** to preview its scope and counts before explicitly selecting **Restore Library** or **Restore Lists**.
Windows defaults to Merge; canceling its mode dialog or picker, or rejecting its restore preview, leaves the target unchanged.

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

## Related documents

- [Library YAML transfer specification](../system/library/yaml-transfer.md)
- [Library YAML format reference](../reference/library/format/yaml.md)
- [CLI command reference](../reference/cli/command.md)
