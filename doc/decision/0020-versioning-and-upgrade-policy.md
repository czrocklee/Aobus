---
id: decision.0020.versioning-and-upgrade-policy
status: accepted
---
# Decision 0020: version persisted data for explicit upgrade

## Context

Before the first public release, development builds changed persisted formats freely.
The library database reached version 10, library YAML version 8, the playback session version 5, and several state files version 2 or 3; none of them was released.
Open accepts exactly one library version, and no upgrade code exists.

After release that stops being enough. A library holds curation that exists nowhere else, such as tags, Lists, saved order and edited metadata.
It lives beside the music it describes, often on a removable drive that machines with different Aobus builds share.
Users also keep deliberate configuration: key bindings, shell layout presets, the preferred output device and theme.
One maintainer supports four frontends and a CLI, so the policy has to be cheap to follow for every format change and must not need a framework before the first real upgrade.

Some properties cannot be added after release, because every released build must already honor them: how an opener excludes an upgrade in another process, and how a configuration reader treats a version it does not know.

## Decision

### Version numbers

Every versioned persisted format restarts at version 1 with the first release.
Data written by development builds is unsupported and is not detected; it is rebuilt or deleted on the machine that holds it.

Each versioned format has one integer version. Values only increase, and a released value is never reused.
Two formats carry no version: the AppKit `desktop.plist`, whose rule follows below, and the Windows theme document, which the user authors and Aobus only reads.
A format changes version at most once per release: the first change after a release increments it, and later changes in the same cycle amend that unreleased version.
Each library format keeps a last-released constant beside its current one, and a test enforces this rule against it.

Exact version matching stays.
An older build refuses a newer library or document; there are no forward-compatibility flags and no tolerated unknown named databases.
The LMDB file format is part of the library format: LMDB is pinned on every platform, and an LMDB update must open every frozen fixture.

### Three classes of persisted data

- **Library data**, the database and its YAML: every release brings every earlier released library forward without losing curation.
- **Protected configuration**, the `shortcuts` and `runtime` groups, TUI preferences, shell layout presets, the Windows desktop settings, the Windows theme document and the AppKit `desktop.plist`: every older version stays readable, and an older build never overwrites or removes content of a newer version.
  It uses defaults for that member for the session and shows one notice when it loads it. The AppKit dictionary instead keeps every existing key's value schema and adds new keys.
  The theme document gains a version before a release changes its tokens in a way an earlier theme would fail.
- **Losable state and caches**: everything else Aobus persists, such as the GTK `window` group, the playback session, workspace and per-library presentation files. It is dropped or reset on a version mismatch, as before.

The first two classes are closed lists, not decided by how a file is written; adding a member to them is a policy change.

### Database upgrade

An upgrade is explicit and never silent. Each frontend asks for confirmation, and the CLI has `aobus lib upgrade`.
It runs one step per version, each with a frozen decoder that checks the old records it reads.
The whole chain and the new version stamp run in one LMDB write transaction, and the current open validation runs on the result before commit.
A failure before commit leaves the library unchanged. A commit error is treated as an uncertain outcome, so the upgrader reopens the library and checks it before reporting.
The library id, every Track and List id, the id high-water marks and the revision are preserved.

Steps are never removed. Each is self-contained: it writes the form of its own target version forever, depends only on the storage adapter and its own frozen structures, and never opens an audio file.

Every upgrade first keeps an unmodified copy of the library, which stays until the user deletes it.
`aobus lib rollback` puts such a copy back in every later release: it gives the copy a fresh library id, first keeps the current `data.mdb` as a copy that a later rollback also accepts, and then replaces only `data.mdb` by an atomic rename.
Rollback restores a different file, so it cannot publish through LMDB's own commit as an upgrade does.
Both copies are synced before the rename; if a power loss on exFAT then leaves no `data.mdb`, running the rollback again puts the copy back, and it keeps an empty library that an open created there meanwhile as one more copy.
Reinstalling an older release is not promised.

Version 1 must add a shared open lock, taken by every process that opens a library and held exclusively by upgrade and rollback, beside the existing writer lease.
No operation ever moves the database directory or a lock file.

### YAML

Import accepts every released version through tree steps, and export writes only the current version.
A released document keeps its meaning: a later build imports it in every mode without losing curation, and no value in it depends on the host that wrote it.
Each mode keeps its baseline rules, so a full import opens no audio file, while a delta or metadata import still takes the fields it omits from the files it names.
The tree steps are pure document conversions and never open an audio file.
A full export restored into a new library is the last line of defense when an in-place upgrade is not possible. There is no automatic snapshot.

### Filter grammar

The predicate grammar is owned by the library version.
A change that alters how text the current language accepts parses or evaluates is a library format change, and its steps rewrite stored filters.
Syntax that only gives meaning to text rejected today needs no version, and an older build keeps such a filter verbatim.
Moving, resetting and forgetting hidden positions of a saved order refuse while the List's track source has an error, so an older build never erases an order it cannot evaluate.

## Alternatives considered

### Forward-compatible newer libraries

Rejected.
An older build cannot maintain a newer store's invariants when it writes; deleting a Track would leave the newer statistics row behind.
The most it could offer is a read-only open, which a player gains little from.

### Upgrade automatically on open

Rejected.
The library travels with the music folder, and an automatic upgrade would lock out every other machine that still runs an older build.

### Publish an upgrade by renaming a rebuilt database

Rejected.
A rename publication is not crash-safe on exFAT, the usual format of a shared removable drive, while LMDB's own commit is.
Map size and the number of named databases are open parameters, so neither needs a rebuild.

### Remove old steps after a support window

Rejected.
An old library would then be recoverable only through an old binary, which may no longer run on a later operating system.
Pruning would also need an automatic full snapshot to ship first.

### Treat all application state as losable

Rejected for deliberate configuration.
Losing key bindings, authored layout presets or the preferred output device on every version change costs the user real work, and some frontends already protected the same choices.

### One lock file for both roles

Rejected.
The writer lease locks its whole file, with `flock` on POSIX and a full-range `LockFileEx` on Windows, so the same file cannot also carry a shared open lock.
POSIX byte-range locks could split one file, but a process loses them when it closes any descriptor of that file.

## Consequences

- Development-era libraries, documents and state are rebuilt or deleted at the reset.
- Before the first release, version 1 still needs the open lock, a distinct error for a newer version, the protected-configuration rules, and the release guard.
- Each later format change costs a step, a YAML tree step, and frozen fixtures on every platform. Each fixture also gets a frozen dump of every named database's keys and values after the step, which the step must reproduce from that platform's previous-version fixture.
- The first format change after release builds the upgrade machinery: the step runner, the pre-upgrade copy, rollback and the confirmations.
- State bound to the old library id after a rollback is left unused rather than deleted.
- Data written by a development build at an unreleased version stays unsupported, so dogfooding upgrades a copy of a released library.
