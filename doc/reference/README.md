---
id: reference.index
---
# Reference

Use these pages to look up exact commands, languages, formats, compatibility rules, and semantic vocabularies.
They include the meaning needed to use a surface; there is no requirement to find a separate specification for every rule.
For internal object declarations, use the linked headers. Cross-object behavior is grouped in the [system guide](../system/README.md).

## Commands and languages

- [CLI commands and structured output](cli/command.md)
- [TUI commands, keys, and overlays](tui/command.md)
- [Predicate expression language](query/predicate-language.md)
- [Scalar format language](query/format-language.md)
- [Keyboard maps](shell/keymap.md)

## Stored and authored formats

- Library: [database](library/storage/database.md), [portable YAML](library/format/yaml.md), [Track model](library/model/track.md), [List model](library/model/list.md), and [resource descriptors](resource/blob.md).
- Sessions: [playback state](playback/session-state.md), [workspace state](workspace/session-state.md), and [persisted presentation state](presentation/persisted-state.md).
- Managed state: [file locations](persistence/location.md), [application groups and versions](persistence/application-config.md), and [Windows desktop settings and themes](windows/desktop-state.md).
- Layout: [document language](shell/layout-document.md), [component state](shell/layout-state.md), [shared component vocabulary](shell/component-vocabulary.md), [GTK schema and actions](shell/layout-schema.md), and [Windows schema](windows/layout-schema.md).

## Protocols and product vocabularies

- [Linux MPRIS interfaces and mappings](linux/mpris.md)
- [Desktop successor-process protocol](application/desktop-successor-protocol.md)
- [Supported audio files and imported metadata](media/audio-file.md)
- [Runtime track-field vocabulary](library/model/track-field.md)
- [Built-in track presentation presets](presentation/track-preset.md)

Internal lookup material remains useful when it explains a nontrivial contract: see [PCM representation](../system/playback/pcm-format.md), [quality values](../system/playback/quality-values.md), [exception boundaries](../system/failure/exception-carriers.md), and [text catalog semantics](../system/presentation/text-catalog.md).
For contribution tasks, use [development](../development/README.md); for product tasks, use the [user guides](../user/README.md).
