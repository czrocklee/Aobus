---
id: user.backup-and-restore
type: user-guide
status: current
domain: library
summary: Exports a portable library backup, previews an import, and chooses restore or merge deliberately.
---
# Back up and restore library data

## Outcome

You have a portable YAML backup of the selected library data and can preview and apply it with an explicit restore or merge policy.

## Understand the boundary

A library YAML export contains Aobus metadata, lists, resources, and mode-dependent technical or manifest facts.
It does not copy the audio files themselves.
Back up the music files separately and preserve their paths relative to the music root.

`restore` replaces the payload-selected target scope; `merge` preserves target entities absent from the payload and adds imported lists as new lists.
The current GTK import path uses destructive `restore` by default and does not present a preview-bound confirmation.
Use the CLI preview below before importing, especially into a non-empty library.

## Steps

### Create a backup

1. In GTK, choose **File → Export Library Data...**.
2. Choose the payload you need:
   **Full** includes curated metadata, technical and manifest facts, covers, and lists; **Metadata** omits technical statistics; **Delta** records edits relative to readable files; **List Only** contains lists without track records.
3. Choose a `.yaml` file and wait for **Library exported successfully**.

The CLI equivalent for a complete library-data backup is:

```bash
aobus -C /music lib export /backup/library.yaml --mode full
```

### Preview an import

Run the same mode you intend to commit:

```bash
aobus -C /target -O json lib import /backup/library.yaml --mode restore --dry-run
```

Review `tracksCreated`, `tracksUpdated`, `tracksDeleted`, `listsCreated`, and `listsDeleted`.
Do not continue if the root, mode, or deletion counts are unexpected.

### Apply the import

For a deliberate replacement:

```bash
aobus -C /target lib import /backup/library.yaml --mode restore
```

For an additive import that preserves target tracks and lists outside the payload:

```bash
aobus -C /target lib import /backup/library.yaml --mode merge
```

GTK **File → Import Library Data...** currently performs restore immediately after file selection, so it should be used only when that replacement behavior has already been previewed and accepted.

## Verify the result

- `aobus -C /target lib stats` reports the expected track, list, resource, and storage counts.
- `aobus -C /target lib verify` reports no missing or error-class issues.
- Representative tracks retain the expected curated metadata, tags, covers, and list membership.
- The separately restored audio files exist at the paths expected beneath the target music root.

## Related documents

- [Library YAML transfer specification](../spec/library/runtime/yaml-transfer.md)
- [Library YAML format reference](../reference/library/format/yaml.md)
- [CLI command reference](../reference/cli/command.md)
- [RFC 0001: Safe library transfer](../rfc/0001-safe-library-transfer.md)
