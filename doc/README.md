# Aobus documentation

## Use Aobus

[Get started](user/get-started.md), [play music](user/play-music.md), [manage a library](user/manage-library.md), or [back up and restore](user/backup-and-restore.md).
The [user guides](user/README.md) also cover metadata, Lists, customization, Windows desktop, TUI, and CLI tasks.

## Change Aobus

Start with the [contributor workflow](../CONTRIBUTING.md) and choose a task in the [development guide](development/README.md).

| Task | Start here |
|---|---|
| Build on Linux, macOS, or Windows | [Build and setup](development/README.md#build-and-setup) |
| Change a library command, scan, or live view | [Library](system/library/README.md) |
| Change playback or audio output | [Playback](system/playback/README.md) |
| Diagnose cancellation, callbacks, or teardown | [Execution](system/execution/README.md) and [session lifecycle](system/session-lifecycle.md) |
| Change a stored format | [Persistence](system/persistence/README.md) and the [format reference](reference/README.md#stored-and-authored-formats) |
| Change UI behavior or translations | [Presentation](system/presentation/README.md) and [localization workflow](development/localization.md) |
| Add a command or platform adapter | [Frontends](system/frontend/README.md) and [command reference](reference/README.md#commands-and-languages) |
| Write tests or investigate a lint finding | [Testing](development/test.md) and [linting](development/linting.md) |
| Maintain a lint checker | [Checker development](development/lint/checker-development.md) |

## Understand Aobus

The [system guide](system/README.md) connects the layer map, subsystem structure, and contracts that implementations must preserve.
For an exact language, format, protocol, or vocabulary, use [reference](reference/README.md).
For the reasons behind consequential choices, use [decisions](decision/README.md); they are historical context, not substitutes for current contracts.

## Maintain these documents

[Writing and maintaining documentation](development/documentation.md) explains file boundaries, evidence, proposals, and `./ao docs check`.
You do not need to read it before using the guides above.
