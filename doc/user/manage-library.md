---
id: user.manage-library
type: user-guide
status: current
domain: library
summary: Reconciles music files, filters tracks, and creates reusable Smart Lists.
---
# Manage a music library

## Outcome

Your library reflects the supported audio files beneath its root, and frequently used searches are available as Smart Lists.

## Steps

### Reconcile files from GTK

1. Open the intended root with **File → Open Library...**.
2. Choose **File → Scan Library**.
3. Follow progress in the activity status.

A scan adds new supported files, refreshes technical properties for changed files, reconnects recognized moves when identity is available, and marks unmatched manifest rows as missing.
Curated library metadata remains authoritative after initial import; rescanning a changed file does not replace those edits with file tags.

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

### Filter and save a Smart List in GTK

1. Enter words in the quick-filter field for a broad search, or enter a query expression for exact matching.
2. Check that the visible rows are the intended result.
3. Use **Create smart list from current filter**, or right-click an eligible list and choose **New Smart List...**.
4. Give the list a name, adjust its filter or presentation, review the preview, and choose **Create**.

Smart Lists keep a predicate rather than a copied membership list, so their results follow later library mutations.

## Verify the result

- A second dry-run scan reports no actionable file changes after a successful apply.
- Selecting the Smart List reproduces the intended filtered result.
- Editing metadata that participates in the predicate updates the Smart List result through the live library change path.

## Related documents

- [CLI command reference](../reference/cli/command.md)
- [Track-filter specification](../spec/presentation/track-filter.md)
- [Predicate language reference](../reference/query/predicate-language.md)
- [Library scan and audio identity](../spec/library/runtime/scan-and-identity.md)
- [Derived track-view RFC](../rfc/0006-coherent-derived-track-views.md)
