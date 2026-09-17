---
id: architecture.index
---
# How Aobus works

Aobus exposes GTK, WinUI, AppKit, TUI, and CLI frontends over shared runtime and Core libraries.
Start with the [system overview](overview.md) for dependency direction and composition roots, or go directly to the topic you are changing.
These pages describe current structure and the contracts implementations and tests must preserve; you do not need to read the whole set.

## Find a subsystem

| Question | Topic |
|---|---|
| How do library reads, mutations, scans, and live views fit together? | [Library](library/README.md) |
| How do playback commands become an audio stream? | [Playback](playback/README.md) |
| How are predicates, formatting, and completion evaluated? | [Track expressions](query/README.md) |
| How are encoded audio files recognized and read? | [Media](media/README.md) |
| How do covers travel from media evidence to frontend images? | [Resource delivery](resource/README.md) |
| Who owns open views, navigation, selection, and sessions? | [Workspace](workspace/README.md) |
| Which behavior belongs to runtime, UIModel, or a frontend? | [Presentation](presentation/README.md) |
| How are layouts, actions, shortcuts, and shell state composed? | [Application shell](shell/README.md) |
| What differs between graphical, terminal, and command-line adapters? | [Frontends](frontend/README.md) |

## Follow a cross-cutting contract

- [Execution](execution/README.md): callback affinity, workers, cancellation, and dedicated threads; [signals](execution/signal.md) define observer delivery and reentrancy.
- [Interactive session lifecycle](session-lifecycle.md): runtime construction, restore, checkpoint, stable identity, and ordered teardown; [desktop library lifecycle](desktop-library-lifecycle.md) defines shared successor-process handoff.
- [Failure and reporting](failure/README.md): result channels, recovery boundaries, fatal containment, and the notification feed.
- [Persistence](persistence/README.md): durable truth, managed state, snapshots, schemas, file replacement, and storage mechanisms.
- [Unicode text](unicode-text.md): UTF-8 admission, normalization, caseless identity, and grapheme boundaries; [interactive localization](presentation/localization.md) separately owns display-language behavior.

Exact commands, languages, and stored formats are reachable from their topics and the [reference guide](../reference/README.md).
Use the [development guide](../development/README.md) for build, test, and maintenance procedures, and [decisions](../decision/README.md) for historical reasons.
