---
id: architecture.persistence-versioning
---
# Persisted format versioning

## Scope

This page states how each format that Aobus persists treats data of another version, and which formats belong to each class.
[Decision 0020](../../decision/0020-versioning-and-upgrade-policy.md) records why the policy is what it is.
Exact versions, fields, and rejection rules belong to each format's reference, which the sections below link.

## Version numbers

Each versioned format has one integer version.
Values only increase, and a released value is never reused.
Every versioned format starts at version 1 with the first release; data that development builds wrote before it is unsupported and is not detected.
A format changes version at most once per release: the first change after a release increments it, and later changes in the same cycle amend that unreleased version.

A build reads exactly the versions its format reference lists and refuses a newer one.
No format carries forward-compatibility flags.
Two protected formats carry no version: the AppKit `desktop.plist` and the Windows theme document, whose rules follow below.

## Classes

Every persisted format belongs to one of three classes.
The first two are closed lists, so membership does not follow from how a file is written, and adding a member is a policy change.

### Library data

The [library database](../../reference/library/storage/database.md#compatibility-and-versioning) and [library YAML](../../reference/library/format/yaml.md#compatibility-and-versioning) hold curation that exists nowhere else.
Each accepts only version 1 today and refuses a newer library or document without changing it.
After the first release, every release brings every earlier released version of either format forward without losing curation.

### Protected configuration

These hold deliberate configuration whose loss costs the user real work:

- the `shortcuts` group in GTK, TUI, and Windows;
- the `runtime` group in GTK and TUI;
- the TUI `preferences` group;
- shell layout presets;
- the Windows `desktop` group;
- the Windows theme document;
- the AppKit `desktop.plist`.

The versioned members are [`ConfigStore` protected groups](config-store.md#protected-groups).
A build never reads or overwrites such a group of a newer version: it keeps its defaults for that group for the session, and every save of the group fails without touching the file.
`ConfigStore` refuses to remove a group that a load or save has found newer, and `ShellLayoutStore` refuses to remove a preset file that a newer version saved or whose bytes it cannot read.
Every change to one of these schemas increments its version, an additive one included, and every older version stays readable.
A build recognizes a newer group only as a mapping whose `version` is an unsigned 32-bit integer, so every version of a group keeps that shape.
A `ConfigStore` judges a group by the copy of the file it read first and never rereads it: when a newer version writes the file while this build runs, this build's next save of that file discards what the newer version wrote.
Shell layout presets are not affected, because each save of one rereads its file.
The [application managed-state reference](../../reference/persistence/application-config.md#compatibility-and-versioning) lists each group's version.

The AppKit `desktop.plist` is a Foundation dictionary that every save rewrites whole, keeping top-level keys that the build does not know.
Its rule is that every existing top-level key keeps its value schema, including whole subtrees, and new content uses new top-level keys.

The Windows theme document is authored by the user, and Aobus only reads it.
It gains a version before a release changes its tokens in a way that an earlier theme would fail.

### Losable state and caches

Everything else Aobus persists is losable: for example the GTK `window` and `session` groups, the playback session, the workspace, shell component state, the per-library presentation files, and caches.
A version mismatch drops such state or falls back to defaults, and a later save may replace it.

## Changing a format

The [library format change guide](../../development/library-format-changes.md) covers library data, and the [managed-state schema guide](../../development/managed-state-schemas.md) covers protected configuration and losable state.
The library database and YAML references own which of their changes are incompatible.
