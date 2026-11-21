---
id: user.manage-library
---

# Manage a music library

## Outcome

Your library reflects the supported audio files beneath its root, and frequently used searches are available as saved Lists.

## Steps

### Reconcile files from GTK

1. Open the intended root with **File → Open Library...**.
2. If opening the library starts a scan, let it continue. Otherwise, choose **File → Scan Library**.
3. Follow progress in the activity status. If it shows **Indexing audio identity**, Aobus is still identifying files in the background. **Scan Library** stays unavailable until that work finishes; there is no cancel command for this step. If you add files while a scan is running, choose **Scan Library** again when it becomes available. A second request is not queued.

A scan adds new supported files, refreshes technical properties for changed files, reconnects recognized moves when identity is available, and records which files are missing.
Curated library metadata remains authoritative after initial import; rescanning a changed file does not replace those edits with file tags.

For a filename rename that changes only ASCII letter case, the scan can detect a spelling change by confirming that both names refer to the same file.
If neither spelling yields a stable file ID, the scan can instead report a move: only the new name may be listed among those spellings, both lookups must reach regular files without redirection, and the audio fingerprint must uniquely match the missing library entry to the new file.
A pending fingerprint, conflicting name, or ambiguous match does not qualify for this fallback.

### Disc images and CUE sheets

A UTF-8 `.cue` file beside a FLAC or WAV of the same name splits that file into one track per CUE audio track.
`Album.flac.cue` applies only to `Album.flac` and wins over `Album.cue`. The name inside the sheet is ignored.
Save any other encoding as UTF-8 and scan again; Aobus does not guess a legacy encoding, and it does not import the new file until the sheet can be read.
Only FLAC and WAV are split. For another format, remove or rename the sheet to import the file as one track.

A scan never deletes a track, never changes a stored range, and never splits a file that is already one track.
To split that file, or to apply a sheet whose ranges no longer match, remove its tracks and scan again.
Deleting one track of a split file and scanning again creates that CUE track once more while the file itself is unchanged.
Editing titles in the sheet after import does not replace curated metadata.
Track properties show the audio file, the CUE track number, and the start and end time. They do not show "n of m".

The [CUE sheet reference](../reference/media/cue-sheet.md) defines the accepted text. Scan classification belongs to [library scan and audio identity](../system/library/scan-and-identity.md#cue-binding).

### Reconcile files from the Windows desktop

1. Choose **Open Library...** from **More** in Modern mode or **File** in Classic mode and select the intended root.
2. After files beneath that root change, choose **More → Rescan Library** in Modern or **File → Rescan** in Classic.
3. Follow progress in the activity surface in Modern mode or the status bar in Classic mode.

Windows applies scan changes together. A failed scan leaves the same folder open for retry. If another scan is already running, wait for its result before requesting the next scan; a second request is not queued.

If a case-only rename reports `Scan spelling lookup reached a reparse point`, Windows exposed that filename as a reparse point. This can happen with a cloud placeholder, such as a OneDrive file. The scan leaves the existing Track alone rather than guessing that the two names refer to the same file. If practical, restore the filename already stored for that Track, then rescan.

### Preview and apply a scan from the CLI

1. Inspect the plan without changing the library:

   ```bash
   aobus -C /music scan --dry-run --verbose
   ```

2. Review the planned changes and any `errors` or `warnings`. Both `spelling-changed` and `moved` show `old -> new` and reconnect the existing Track without creating a duplicate. A `missing-segments` item means a split file will regain a CUE track that is no longer in the library.
3. Apply the scan only when the plan targets the intended root:

   ```bash
   aobus -C /music scan
   ```

If a case-only rename qualifies for fingerprint matching but the audio fingerprint is pending, check whether the old spelling still opens the intended file.
If it does, run `aobus -C /music lib fingerprint --pending`, then preview the scan again. Look for `moved` from the old name to the new one before applying.
Fingerprinting only processes available entries whose file size and modification time still match the library. Waiting will not fill the fingerprint for a missing entry or a file whose size or modification time has changed.

### Filter and save a List on the desktop

1. In GTK or the Windows desktop, enter words in the quick-filter field for a broad search, or enter a query expression for field-specific conditions.
2. Check that the visible rows are the intended result.
3. Use **Create List from current filter**, or right-click All Tracks or a saved source List and choose **New List...**.
4. Give the list a name, adjust its filter or presentation, review the preview, and choose **Create**.

The create action appears only while the current resolved expression is non-empty and valid.
When the source is a saved List, the new List is derived from that parent; creating from All Tracks makes a root List.

Saved Lists keep a filter rather than a fixed set of tracks, so their results follow later library changes.
They may also retain an independent manual rank; see [Organize music with Lists and Playlists](organize-with-lists.md).

### Understand filtering and suggestions

The same search-text rules apply to GTK, Windows, and the TUI's track filter:

- Plain text uses Quick mode. `Bach cello` requires both terms, but they may match different common metadata fields or tags. Quote a phrase such as `"Massive Attack"` to keep it one term.
- A leading query variable (`$`, `@`, `#`, or `%`) selects Expression mode; opening parentheses and `not` or `!` may precede it. For example, `$composer ~ "Bach"` searches only the composer field. Punctuation inside ordinary names, such as `P!nk`, does not switch modes.
- Quick mode matches metadata substrings without regard to Unicode case differences, but accents remain significant; tags use their exact stored identity. An empty suggestion list does not mean that the filter has no matching tracks.

Suggestions select existing library text. An ASCII prefix of at least three letters or digits can also find supported romanized aliases: typing `zhoujielun` may suggest `周杰倫` when that value exists in the library. Accept the suggestion to insert the original text. Submitting `zhoujielun` without accepting it searches that literal text; filtering itself does not transliterate.

Kana suggestions use romanized Kana; Han-only values use a Mandarin-pinyin alias, not an inferred Japanese Kanji reading. These aliases help selection without changing metadata or saved predicates. Consult the [predicate language](../reference/query/predicate-language.md) for exact operators and field syntax.

## Verify the result

- Run another dry-run scan to check for remaining file changes. If the scan skipped a new-file item because another spelling was registered during the scan, a later scan reevaluates the item and may not import it.
- Selecting the saved List reproduces the intended filtered result.
- After you successfully save a metadata edit that affects a List's filter, its results update automatically; you don't need to reopen the List.

## Related documents

- [CLI command reference](../reference/cli/command.md)
- [Track-filter specification](../system/presentation/track-filter.md)
- [Predicate language reference](../reference/query/predicate-language.md)
- [Library scan and audio identity](../system/library/scan-and-identity.md)
- [CUE sheet](../reference/media/cue-sheet.md)
- [Organize music with Lists and Playlists](organize-with-lists.md)
