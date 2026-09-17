---
id: user.manage-library
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

1. In GTK or the Windows desktop, enter words in the quick-filter field for a broad search, or enter a query expression for field-specific conditions.
2. Check that the visible rows are the intended result.
3. Use **Create List from current filter**, or right-click All Tracks or a saved source List and choose **New List...**.
4. Give the list a name, adjust its filter or presentation, review the preview, and choose **Create**.

The create action appears only while the current resolved expression is non-empty and valid.
When the source is a saved List, the new List is derived from that parent; creating from All Tracks makes a root List.

Saved Lists keep a predicate rather than a copied membership list, so their results follow later library mutations.
They may also retain an independent manual rank; see [Organize music with Lists and Playlists](organize-with-lists.md).

### Understand filtering and suggestions

The same search-text rules apply to GTK, Windows, and the TUI's track filter:

- Plain text uses Quick mode. `Bach cello` requires both terms, but they may match different common metadata fields or tags. Quote a phrase such as `"Massive Attack"` to keep it one term.
- A leading query variable (`$`, `@`, `#`, or `%`) selects Expression mode; opening parentheses and `not` or `!` may precede it. For example, `$composer ~ "Bach"` searches only the composer field. Punctuation inside ordinary names, such as `P!nk`, does not switch modes.
- Quick mode matches metadata substrings without regard to Unicode case differences, but accents remain significant; tags use their exact stored identity. An empty suggestion list does not mean that the filter has no matching tracks.

Suggestions select existing library text. An ASCII prefix of at least three letters or digits can also find supported romanized aliases: typing `zhoujielun` may suggest `周杰倫` when that value exists in the library. Accept the suggestion to insert the original text. Submitting `zhoujielun` without accepting it searches that literal text; filtering itself does not transliterate.

Kana suggestions use romanized Kana; Han-only values use a Mandarin-pinyin alias, not an inferred Japanese Kanji reading. These aliases help selection without changing metadata or saved predicates. Consult the [predicate language](../reference/query/predicate-language.md) for exact operators and field syntax.

## Verify the result

- A second dry-run scan reports no actionable file changes after a successful apply.
- Selecting the saved List reproduces the intended filtered result.
- Editing metadata that participates in the predicate updates the List result through the live library change path.

## Related documents

- [CLI command reference](../reference/cli/command.md)
- [Track-filter specification](../system/presentation/track-filter.md)
- [Predicate language reference](../reference/query/predicate-language.md)
- [Library scan and audio identity](../system/library/scan-and-identity.md)
- [Organize music with Lists and Playlists](organize-with-lists.md)
