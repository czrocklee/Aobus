---
id: user.manage-library
type: user-guide
status: current
domain: library
summary: Reconciles music files, filters tracks, and creates reusable saved Lists.
---
# Manage a music library

## Outcome

Your library reflects the supported audio files beneath its root, and frequently used searches are available as saved Lists.

## Steps

### Reconcile files from GTK

1. Open the intended root with **File → Open Library...**.
2. Choose **File → Scan Library**.
3. Follow progress in the activity status.

A scan adds new supported files, refreshes technical properties for changed files, reconnects recognized moves when identity is available, and marks unmatched manifest rows as missing.
Curated library metadata remains authoritative after initial import; rescanning a changed file does not replace those edits with file tags.

### Reconcile files from the Windows desktop

1. Open the intended root with **Open Library...**.
2. Choose **Rescan** after files beneath that root change.
3. Follow progress in the activity surface in Modern mode or the status bar in Classic mode.

Windows runs the same transactional reconciliation as GTK. A failed scan keeps the active root open so **Rescan** can retry it, and another **Rescan** while one is active starts no duplicate work.

### Preview and apply a scan from the CLI

1. Inspect the plan without changing the library:

   ```bash
   aobus -C /music scan --dry-run --verbose
   ```

2. Review the `new`, `changed`, `moved`, `missing`, and `errors` counts.
3. Apply the scan only when the plan targets the intended root:

   ```bash
   aobus -C /music scan
   ```

### Filter and save a List on the desktop

1. In GTK or the Windows desktop, enter words in the quick-filter field for a broad search, or enter a query expression for exact matching.
2. Check that the visible rows are the intended result.
3. Use **Create List from current filter**, or right-click All Tracks or a saved source List and choose **New List...**.
4. Give the list a name, adjust its filter or presentation, review the preview, and choose **Create**.

The create action appears only while the current resolved expression is non-empty and valid.
When the source is a saved List, the new List is derived from that parent; creating from All Tracks makes a root List.

Saved Lists keep a predicate rather than a copied membership list, so their results follow later library mutations.
They may also retain an independent manual rank; see [Organize music with Lists and Playlists](organize-with-lists.md).

## Verify the result

- A second dry-run scan reports no actionable file changes after a successful apply.
- Selecting the saved List reproduces the intended filtered result.
- Editing metadata that participates in the predicate updates the List result through the live library change path.

## Related documents

- [CLI command reference](../reference/cli/command.md)
- [Track-filter specification](../spec/presentation/track-filter.md)
- [Predicate language reference](../reference/query/predicate-language.md)
- [Library scan and audio identity](../spec/library/runtime/scan-and-identity.md)
- [Organize music with Lists and Playlists](organize-with-lists.md)
